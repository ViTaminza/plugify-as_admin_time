// as_admin_time — Plugify port of the Pisex admin_system module "AdminTime" ([AS] Admin Time):
// records how long each admin is online in the shared admin database.
//
// Rows go into `as_admin_time` (the literal table name of the original — no prefix — so a
// Metamod and a Plugify server read the same audit trail):
//
//   admin_id VARCHAR(32)   steamid64
//   admin_name             player name at connect time
//   connect_time           unix time of the connect
//   disconnect_time        unix time of the disconnect, -1 while the admin is online
//   played_time            seconds played
//   server_id              the "server_id" of admin_time.ini
//
// Behaviour of the original, kept:
//   * "reset" (default 1): on core load every row of this server that is still marked online
//     (-1) is closed with played_time = now - connect_time (crash/restart recovery).
//   * "spec_time" (default 1): played_time is the wall-clock time between connect and
//     disconnect; with 0 the plugin tracks the time the admin is NOT in spectator team by
//     hooking the "jointeam" command and stores that.
//   * OnAdminConnected (from the admin core) inserts the "online" row; player_disconnect closes
//     it.
//
// Pure s2sdk + plgdb, no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7). The guards the
// other modules got from those libraries live in this file:
//   * PITFALLS B1/B2 — the map gate g_mapReady. s2sdk client functions dereference null before the
//     first map and, worse, an emptied client list between OnMapEnd and the next OnMapStart. The
//     map listeners are registered in OnPluginStart; readiness is latched by OnMapStart and by the
//     first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd was seen
//     since OnPluginStart), and dropped by OnMapEnd. No client call is made while it is closed.
//   * PITFALLS C2 — every client query goes through CanQueryClient/HasClient/HasController: slot
//     0..63, IsClientConnected && !IsClientSourceTV, and a controller entity for the name. The
//     in-game state is deliberately NOT required (see HasClient: OnAdminConnected can arrive
//     before the first spawn, and the original's GetSteamID64 answered for every connected slot);
//     s2sdk's "<unknown>" name becomes "".
//   * PITFALLS C12 — "player_disconnect" is hooked with s2sdk::HookEvent(HookMode::Pre) in
//     OnPluginStart and unhooked in OnPluginEnd; the handler answers Continue on every path
//     (Handled would swallow the event for the engine and every other plugin) and reads the slot
//     with s2sdk::GetEventPlayerSlot(event, "userid") when the event arrives.
//   * PITFALLS C11 — CS2's "jointeam" is not a ConCommand of the game (the game parses it in its
//     ClientCommand path), so it is reached through s2sdk's *global* command listener
//     (AddCommandListener(plg::string(), ...)), exactly like pisexcompat did for the Pisex
//     modules; the handler filters the command name itself and is removed in OnPluginEnd.
//   * PITFALLS A1/A7 — OnPluginStart never returns Failed (that stops every Plugify plugin),
//     OnPluginEnd removes the event hook, the command listener and the map listeners, so
//     `plg reload` leaves nothing that points into the unloaded library.
//
// Porting notes:
//   * the IMySQLConnection of the original is replaced by a private plgdb::Database opened with
//     the settings the admin core exports (GetDatabaseConfig); every statement is asynchronous
//     and every callback runs from the plugin's own plgdb Poll(), i.e. on the game thread;
//   * the SQL is parameterised (the original interpolated the player name and the steam id);
//     the column types stay exactly as the Metamod plugin created them, and the CREATE TABLE
//     statement keeps MySQL's spelling for MySQL and uses SQLite's AUTOINCREMENT otherwise;
//   * player_disconnect's "userid" is translated to a player slot with GetEventPlayerSlot (the
//     raw user id is not a slot in CS2);
//   * the jointeam handler of the original called std::stoi() on the command *name* ("jointeam")
//     and would have thrown into the engine; the port reads the first argument after the command
//     name, which is what the original meant to do, and answers Continue so the engine keeps
//     handling the team change;
//   * admin_time.ini is read by the self-contained KV1 reader in src/config.hpp: the production
//     file is <GetConfigsDir()>/admin_system/admin_time.ini (the Pisex layout), with
//     <GetConfigsDir()>/as_admin_time/admin_time.ini as the plugin-local fallback.

#include <plgdb/db.hpp>

#include <admin_api.hpp>

#include "config.hpp"

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr char kLogTag[] = "ATime";
constexpr int32_t kMaxSlots = 64;
// Team 1 is the spectator team in CS2 (the original's `iTeam == 1`).
constexpr int kSpectatorTeam = 1;

// The object the ported module uses (the Pisex variable, now the consumer wrapper).
admin_system::AdminApi* g_pAdminApi = nullptr;

// The original's per-server key and the database handle built from the admin core's settings.
as_admin_time::Config g_config;
std::unique_ptr<plgdb::Database> g_db;
bool g_coreLoaded = false;

// The original g_iTime / g_iTimeJoined (spec_time = 0 tracking).
std::array<int, kMaxSlots> g_time{};
std::array<int, kMaxSlots> g_timeJoined{};

// The steamid that owns the still-open row of the slot. Remembered because at a map change the
// engine has already dropped the clients when player_disconnect is delivered, GetSteamID64
// answers 0 there, and the row would stay open ("disconnect_time = -1") for good.
std::array<uint64_t, kMaxSlots> g_openRowSteam{};

// --- PITFALLS B1/B2: the map gate ----------------------------------------------------------------

bool g_mapReady = false;           // a map is up: s2sdk client/entity calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided
bool g_mapListeners = false;       // OnMapStart/OnMapEnd are registered with s2sdk

// --- registrations the unload has to take back (PITFALLS A7) -------------------------------------

bool g_disconnectHooked = false;  // the "player_disconnect" Pre hook is attached
bool g_joinTeamListening = false; // the global "jointeam" listener is attached

// --- logging ---------------------------------------------------------------------------------

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(std::string("[") + kLogTag + "] " + std::string(message), severity,
				 plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	Log(message, plg::Severity::Error);
}

void LogInfo(std::string_view message) {
	Log(message, plg::Severity::Info);
}

// --- small helpers ---------------------------------------------------------------------------

std::string ToString(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// ASCII lower-case copy; the command names s2sdk hands out keep the client's spelling.
std::string ToLower(std::string_view text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

// The original read the team with std::atoi(); the command argument is parsed the same way
// ("2" -> 2, "2 x" -> 2, "" -> 0) without the exception std::stoi threw into the engine.
int ParseCommandInt(const plg::string& value) {
	const std::string text(value.data(), value.size());
	return std::atoi(text.c_str());
}

int64_t Now() {
	return static_cast<int64_t>(std::time(nullptr));
}

// The original's per-slot state, cleared on start, unload and after a closed row. State only: the
// map-end path must not touch a client (PITFALLS B2).
void ResetSlotState() {
	g_time.fill(0);
	g_timeJoined.fill(0);
	g_openRowSteam.fill(0);
}

// --- map listeners (PITFALLS B1/B2) -------------------------------------------------------------

// s2sdk listener callbacks are plain function pointers called on the game thread. They must not
// throw, and they must not touch a client: OnMapEnd only clears state.

void OnMapStartListener() {
	// From here on the clients and entities of the new map are in place.
	g_mapReady = true;
}

void OnMapEndListener() {
	// From here until the next map start every s2sdk client query can crash (PITFALLS B2).
	// g_openRowSteam is deliberately kept: the disconnects of this map change still have to close
	// their rows, and the steam id can no longer be read once the clients are gone.
	g_mapReady = false;
	g_mapEndedSinceStart = true;
}

// --- PITFALLS C2: the client guards -------------------------------------------------------------

// The map gate and the slot range are the only requirements for a function that addresses a
// client without reading its state.
bool CanQueryClient(int32_t slot) {
	return g_mapReady && slot >= 0 && slot < kMaxSlots;
}

// A connected slot with a client object that is not the SourceTV proxy. This is exactly
// pisexcompat's ClientExists() - the guard of the original's GetSteamID64 - and it deliberately
// does NOT require IsClientInGame: the admin core starts LoadAdmin from its connect/authenticate
// path, so OnAdminConnected can arrive before the first spawn, and an in-game requirement would
// silently drop that admin's session row (the Pisex wrapper answered for every connected slot).
// The PITFALLS C2 safety still holds: the map gate is closed before the first map and around
// OnMapEnd, the slot is range checked, and GetClientSteamID64 only needs the client object
// (Player::GetSteamId falls back to the unauthenticated id the client connected with).
bool HasClient(int32_t slot) {
	if (!CanQueryClient(slot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientSourceTV) {
		return false;
	}
	return s2sdk::IsClientConnected(slot) && !s2sdk::IsClientSourceTV(slot);
}

// ... and the slot has a controller entity, i.e. a name to read: pisexcompat's ControllerExists(),
// the guard of the original's GetPlayerName. A client that is still connecting has no controller
// yet; the original stored an empty name for it, and so does the port.
bool HasController(int32_t slot) {
	if (!HasClient(slot) || !__s2sdk_PlayerSlotToEntPointer) {
		return false;
	}
	return s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
}

// The SteamID64 of the original's GetSteamID64(): read for every connected slot (in game or not),
// 0 for the SourceTV proxy, an empty slot or the window between two maps, where the id must not be
// read at all.
uint64_t ClientSteamID64(int32_t slot) {
	if (!HasClient(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(slot);
}

// The player name of the original's GetPlayerName(): read only with a controller entity (a client
// that is still connecting has none, so the row is opened with an empty name, exactly like the
// Pisex module), "" for s2sdk's "<unknown>" placeholder.
std::string ClientName(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_GetClientName) {
		return {};
	}
	const plg::string name = s2sdk::GetClientName(slot);
	std::string out(name.data(), name.size());
	if (out == "<unknown>") {
		out.clear(); // s2sdk's placeholder for a client without a name
	}
	return out;
}

// --- the database ------------------------------------------------------------------------------

// database.cpp-style DDL pair: MySQL verbatim, SQLite with its own auto increment spelling.
constexpr char kCreateTableMysql[] =
	"CREATE TABLE IF NOT EXISTS `as_admin_time` ("
	"`id` INT PRIMARY KEY AUTO_INCREMENT,"
	"`admin_id` VARCHAR(32) NOT NULL,"
	"`admin_name` VARCHAR(64) NOT NULL,"
	"`connect_time` INT NOT NULL,"
	"`disconnect_time` INT NOT NULL DEFAULT -1,"
	"`played_time` INT NOT NULL DEFAULT 0,"
	"`server_id` VARCHAR(32) NOT NULL"
	");";

constexpr char kCreateTableSqlite[] =
	"CREATE TABLE IF NOT EXISTS `as_admin_time` ("
	"`id` INTEGER PRIMARY KEY AUTOINCREMENT,"
	"`admin_id` VARCHAR(32) NOT NULL,"
	"`admin_name` VARCHAR(64) NOT NULL,"
	"`connect_time` INT NOT NULL,"
	"`disconnect_time` INT NOT NULL DEFAULT -1,"
	"`played_time` INT NOT NULL DEFAULT 0,"
	"`server_id` VARCHAR(32) NOT NULL"
	");";

// Every statement is asynchronous; the callback runs from OnPluginUpdate's Poll() on the game
// thread (plgdb's contract) and only reports the failure.
void RunQuery(std::string sql, plgdb::Params params, const char* what) {
	if (!g_db) {
		return;
	}
	const std::string label(what);
	g_db->Query(std::move(sql), std::move(params), [label](const plgdb::Result& result) {
		if (!result.Ok()) {
			LogError(label + " failed: " + result.Error());
		}
	});
}

// OnAdminCoreLoaded() of the original: the core is up (or was already up when this plugin loaded,
// see OnPluginStart), so the module opens its own connection with the exported settings, creates
// its table and closes the rows a crash/restart left open.
void OnAdminCoreLoaded() {
	if (g_coreLoaded || g_pAdminApi == nullptr) {
		return;
	}
	g_coreLoaded = true;

	const admin_system::DatabaseConfig config = g_pAdminApi->GetDatabaseConfig();
	if (config.IsSqlite() && config.host.empty()) {
		LogError("the admin core uses SQLite but reported no database path");
	}
	// The core hands the SQLite file path in the host slot (admin_system/API.md).
	plgdb::Config dbConfig =
		admin_system::MakeDatabaseConfig(config, config.IsSqlite() ? config.host : std::string());
	g_db = plgdb::Database::Create(dbConfig);
	if (!g_db) {
		LogError("could not create the database connection (driver " + config.driver + ")");
		return;
	}

	RunQuery(config.IsSqlite() ? kCreateTableSqlite : kCreateTableMysql, {},
			 "CREATE TABLE as_admin_time");

	if (g_config.reset) {
		// The original reset this server's still-online rows the same way.
		const int64_t now = Now();
		RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? - `connect_time` "
				 "WHERE `disconnect_time` = -1 AND `server_id` = ?;",
				 {now, now, std::to_string(g_config.serverId)}, "online-row reset");
	}
}

// OnAdminConnect() of the original: an admin was just loaded by the core, open his "online" row.
void OnAdminConnected(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!g_db) {
		return; // the core is not loaded (or has no database) yet
	}
	if (g_pAdminApi == nullptr || !g_pAdminApi->IsAdmin(slot)) {
		return;
	}
	// A connected client answers the id whether or not it is in game yet: the core starts its
	// admin load from the connect/authenticate path, and OnAdminConnected can be delivered from
	// the SQL callback before the player spawned (see HasClient). Only the map gate can refuse
	// here, and then the map is not up at all - no admin session of this map is lost.
	const uint64_t steamid = ClientSteamID64(slot);
	if (steamid == 0) {
		return;
	}
	const auto index = static_cast<std::size_t>(slot);
	g_openRowSteam[index] = steamid;
	// The original inserted the online row the same way.
	RunQuery("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, `server_id`) "
			 "VALUES (?, ?, ?, ?);",
			 {std::to_string(steamid), ClientName(slot), Now(), std::to_string(g_config.serverId)},
			 "insert online row");
}

// --- the hooked event (PITFALLS C12) -------------------------------------------------------------

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*name*/, void* event,
										  bool /*dontBroadcast*/) {
	try {
		if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The original read "userid" and used it as the player slot; in CS2 it has to be
		// translated.
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, "userid");
		if (slot < 0 || slot >= kMaxSlots) {
			LogError("player_disconnect: could not resolve the player slot (userid)");
			return s2sdk::ResultType::Continue;
		}
		if (!g_db) {
			return s2sdk::ResultType::Continue;
		}
		const auto index = static_cast<std::size_t>(slot);
		// At a map change every client is disconnected before the event is delivered, the steam id
		// cannot be looked up any more; the id remembered when the admin connected names the row.
		const uint64_t steamid = ClientSteamID64(slot);
		const uint64_t rowSteam = steamid != 0 ? steamid : g_openRowSteam[index];
		if (rowSteam == 0) {
			return s2sdk::ResultType::Continue;
		}
		g_openRowSteam[index] = 0;

		const int64_t now = Now();
		if (!g_config.specTime) {
			// Time spent outside the spectator team (tracked by the jointeam handler).
			if (g_timeJoined[index] > 0) {
				g_time[index] += static_cast<int>(now) - g_timeJoined[index];
				g_timeJoined[index] = 0;
			}
			RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? WHERE "
					 "`admin_id` = ? AND `disconnect_time` = -1 AND `server_id` = ?;",
					 {now, static_cast<int64_t>(g_time[index]), std::to_string(rowSteam),
					  std::to_string(g_config.serverId)},
					 "close online row");
		} else {
			RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? - "
					 "`connect_time` WHERE `admin_id` = ? AND `disconnect_time` = -1 AND "
					 "`server_id` = ?;",
					 {now, now, std::to_string(rowSteam), std::to_string(g_config.serverId)},
					 "close online row");
		}

		g_time[index] = 0;
		g_timeJoined[index] = 0;
	} catch (const std::exception& ex) {
		LogError(std::string("player_disconnect handler threw: ") + ex.what());
	} catch (...) {
		LogError("player_disconnect handler threw an unknown exception");
	}
	// Continue: the engine's own disconnect handling (and every other plugin's hook) must run.
	return s2sdk::ResultType::Continue;
}

// --- the hooked command (PITFALLS C11) -----------------------------------------------------------

// "jointeam" is not a ConCommand in CS2, so it is dispatched through the *global* listener: the
// handler gets every client command (and the server console ones) and filters by name itself.
// Only the spectator-time mode (spec_time = 0) needs it, and only it is registered.
s2sdk::ResultType OnGlobalCommand(int32_t caller, s2sdk::ConCommandContext context,
								  const plg::vector<plg::string>& args) {
	try {
		// A client console command; the chat spelling ("!jointeam") is not a team change.
		if (context != s2sdk::ConCommandContext::Console) {
			return s2sdk::ResultType::Continue;
		}
		if (args.empty() || ToLower(args[0]) != "jointeam") {
			return s2sdk::ResultType::Continue;
		}
		// The server console (caller -1) has no slot to track; the original bailed out there too.
		if (caller < 0 || caller >= kMaxSlots) {
			return s2sdk::ResultType::Continue;
		}
		if (args.size() < 2) {
			return s2sdk::ResultType::Continue; // no team argument: nothing to track
		}
		const auto index = static_cast<std::size_t>(caller);
		const int team = ParseCommandInt(args[1]);
		if (g_timeJoined[index] > 0 && team == kSpectatorTeam) {
			// Back from the spectator team: add the time spent there.
			g_time[index] += static_cast<int>(Now()) - g_timeJoined[index];
			g_timeJoined[index] = 0;
		} else if (team != kSpectatorTeam && g_timeJoined[index] == 0) {
			// Leaving the spectator team (or joining a real one): start counting.
			g_timeJoined[index] = static_cast<int>(Now());
		}
	} catch (const std::exception& ex) {
		LogError(std::string("jointeam handler threw: ") + ex.what());
	} catch (...) {
		LogError("jointeam handler threw an unknown exception");
	}
	// Continue: the engine's own team change must proceed as if we had never listened.
	return s2sdk::ResultType::Continue;
}

// --- config ----------------------------------------------------------------------------------

// LoadConfig() of the original, now through the self-contained KV1 reader: the production file is
// <configs>/admin_system/admin_time.ini, with <configs>/as_admin_time/admin_time.ini as the
// fallback (PLUGIN_STANDARD §3). A missing/malformed file keeps the built-in defaults and logs,
// exactly like the original's failed LoadFromFile did.
void LoadConfig() {
	std::string configsDir;
	if (plg::GetConfigsDir) {
		configsDir = ToString(plg::GetConfigsDir());
	}
	const std::vector<std::string> candidates = as_admin_time::ConfigCandidates(configsDir);
	if (candidates.empty()) {
		LogError("the plugify configs directory is unavailable - using the built-in defaults");
		return;
	}

	std::string lookedAt;
	bool sawFile = false;
	for (const std::string& path : candidates) {
		if (!lookedAt.empty()) {
			lookedAt += ", ";
		}
		lookedAt += path;
		if (!as_admin_time::FileExists(path)) {
			continue;
		}
		sawFile = true;
		if (as_admin_time::LoadConfigFromFile(path, g_config)) {
			LogInfo("loaded " + path);
			return;
		}
		// g_pUtils->ErrorLog("[%s] Failed to load %s", GetLogTag(), pszPath) of the original.
		LogError("Failed to load " + path);
	}

	if (sawFile) {
		LogError("Failed to load admin_time.ini (looked at " + lookedAt + ") - using the built-in defaults");
	} else {
		LogError("admin_time.ini not found (looked at " + lookedAt + ") - using the built-in defaults");
	}
}

// --- lifecycle -------------------------------------------------------------------------------

class AsAdminTimePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			// A reload (plg reload) starts from a clean sheet; OnPluginEnd already unregistered
			// everything, so the flags are simply re-armed here (PITFALLS A7).
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;
			g_coreLoaded = false;
			ResetSlotState();

			// File I/O only: no client, entity or schema call happens in OnPluginStart
			// (PITFALLS B1).
			LoadConfig();

			// PITFALLS B1: registering the map listeners touches no client. Without them nothing
			// could ever close the gate again, so a missing binding keeps it shut (CanQueryClient
			// stays false) instead of opening it blindly.
			if (!g_mapListeners && __s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
				s2sdk::OnMapStart_Register(&OnMapStartListener);
				s2sdk::OnMapEnd_Register(&OnMapEndListener);
				g_mapListeners = true;
			} else if (!g_mapListeners) {
				LogError("the s2sdk map listeners are unavailable - client calls stay disabled");
			}

			// PITFALLS C12: one Pre hook on "player_disconnect"; the handler answers Continue on
			// every path, so the engine's own disconnect handling is untouched.
			if (!g_disconnectHooked && __s2sdk_HookEvent) {
				if (s2sdk::HookEvent("player_disconnect", &OnPlayerDisconnectEvent,
									 s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay) {
					g_disconnectHooked = true;
				} else {
					LogError("could not hook \"player_disconnect\" - the online rows will not be closed");
				}
			} else if (!g_disconnectHooked) {
				LogError("the s2sdk event bindings are unavailable - \"player_disconnect\" is not hooked");
			}

			// PITFALLS C11: "jointeam" is an engine/client command, so only a listener is
			// attached to it (never AddConsoleCommand, which would shadow the game command). Only
			// the spec_time = 0 mode needs it, exactly like the original.
			if (!g_config.specTime) {
				if (!g_joinTeamListening && __s2sdk_AddCommandListener) {
					g_joinTeamListening =
						s2sdk::AddCommandListener(plg::string(), &OnGlobalCommand, s2sdk::HookMode::Pre);
					if (!g_joinTeamListening) {
						LogError("could not listen on the engine's \"jointeam\" command - "
								 "spec_time = 0 cannot track the spectator time");
					}
				} else if (!g_joinTeamListening) {
					LogError("s2sdk AddCommandListener is unavailable - "
							 "spec_time = 0 cannot track the spectator time");
				}
			}

			// The database settings only exist once the core has finished loading. The original
			// registered OnCoreLoaded and got the connection inside the callback; a plugin that is
			// loaded after the core has to run the same startup immediately, because the event does
			// not fire twice (see admin_system/API.md).
			if (g_pAdminApi != nullptr) {
				g_pAdminApi->OnCoreLoaded(&OnAdminCoreLoaded);
				if (g_pAdminApi->IsCoreLoaded()) {
					OnAdminCoreLoaded();
				}
				g_pAdminApi->OnAdminConnected(&OnAdminConnected);
			}

			LogInfo("ready (server_id " + std::to_string(g_config.serverId) +
					", reset " + (g_config.reset ? "on" : "off") +
					", spec_time " + (g_config.specTime ? "on" : "off") + ")");
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1: that stops every plugify plugin)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			// A plugin loaded into a running map never saw OnMapStart: its first update is the "a
			// map is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the
			// server is between maps, so the latch waits for the next OnMapStart instead.
			if (!g_firstUpdateDone) {
				g_firstUpdateDone = true;
				if (g_mapListeners && !g_mapEndedSinceStart) {
					g_mapReady = true;
				}
			}
			if (g_db) {
				g_db->Poll(); // runs the completed database callbacks on the game thread
			}
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginUpdate failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// The event hook points into this library, which is about to go away (PITFALLS A7).
			if (g_disconnectHooked && __s2sdk_UnhookEvent) {
				s2sdk::UnhookEvent("player_disconnect", &OnPlayerDisconnectEvent,
								   s2sdk::HookMode::Pre);
			}
			g_disconnectHooked = false;

			// The global listener is ours: RemoveCommandListener with the same empty name and
			// callback it was added with (never RemoveCommand: "jointeam" belongs to the game).
			if (g_joinTeamListening && __s2sdk_RemoveCommandListener) {
				s2sdk::RemoveCommandListener(plg::string(), &OnGlobalCommand, s2sdk::HookMode::Pre);
			}
			g_joinTeamListening = false;

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			// The admin core listeners point into this library as well.
			if (g_pAdminApi != nullptr) {
				g_pAdminApi->OnCoreLoadedUnregister();
				g_pAdminApi->OnAdminConnectedUnregister();
			}
			g_pAdminApi = nullptr;

			g_db.reset(); // flushes pending statements and joins the plgdb worker

			// A stale callback (the engine may still hold the pointer for a moment) can no
			// longer reach a client or the database.
			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
			g_coreLoaded = false;
			ResetSlotState();
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsAdminTimePlugin g_asAdminTimePlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asAdminTimePlugin)
