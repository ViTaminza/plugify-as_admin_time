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
// Porting notes:
//   * the IMySQLConnection of the original is replaced by a private plgdb::Database opened with
//     the settings the admin core exports (GetDatabaseConfig); every statement is asynchronous
//     and every callback runs from the plugin's own plgdb Poll(), i.e. on the game thread;
//   * the SQL is parameterised (the original interpolated the player name and the steam id);
//     the column types stay exactly as the Metamod plugin created them, and the CREATE TABLE
//     statement keeps MySQL's spelling for MySQL and uses SQLite's AUTOINCREMENT otherwise;
//   * the event's "userid" is translated to a player slot with s2sdk's GetEventPlayerSlot
//     (the raw user id is not a slot in CS2);
//   * the jointeam handler of the original called std::stoi() on the command *name*
//     ("jointeam") and would have thrown into the engine; the port reads the first argument
//     after the command name, which is what the original meant to do.

#include <admin_api.hpp>

#include <pisex/pisex.hpp>

#include <plgdb/db.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

namespace {

// The object the ported module uses (the Pisex variable, now the consumer wrapper).
admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

constexpr char kLogTag[] = "ATime";
constexpr int kMaxSlots = 64;
constexpr char kConfigPath[] = "addons/configs/admin_system/admin_time.ini";
// The original hard-codes "as_admin_time" (GetMySQLConnection() users had no prefix), so the
// prefix of core.ini is deliberately not applied here.
constexpr char kTable[] = "as_admin_time";

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

class AsAdminTimePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// --- admin_time.ini ------------------------------------------------------------------------
	int m_serverId = 1;
	bool m_reset = true;
	bool m_specTime = true;

	// The original g_iTime / g_iTimeJoined (spec_time = 0 tracking).
	std::array<int, kMaxSlots> m_time{};
	std::array<int, kMaxSlots> m_timeJoined{};

	// The steamid that owns the still-open row of the slot. Remembered because at a map change the
	// engine has already dropped the clients when player_disconnect is delivered,
	// GetSteamID64 answers 0 there, and the row would stay open ("disconnect_time = -1") for good.
	std::array<uint64_t, kMaxSlots> m_openRowSteam{};

	std::unique_ptr<plgdb::Database> m_db;
	bool m_coreLoaded = false;

	void LoadConfig();
	void OnCoreLoaded(); // the original OnAdminCoreLoaded
	void OnAdminConnect(int iSlot);
	void OnPlayerDisconnect(const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast);
	void RunQuery(std::string sql, plgdb::Params params, const char* what);
};

AsAdminTimePlugin g_asAdminTimePlugin;

// --- the ported module ------------------------------------------------------------------------

void AsAdminTimePlugin::RunQuery(std::string sql, plgdb::Params params, const char* what) {
	if (!m_db) {
		return;
	}
	const std::string label(what);
	m_db->Query(std::move(sql), std::move(params), [label](const plgdb::Result& result) {
		if (!result.Ok()) {
			pisex::g_pUtils->ErrorLog("[%s] %s failed: %s", kLogTag, label.c_str(), result.Error().c_str());
		}
	});
}

void AsAdminTimePlugin::LoadConfig() {
	pisex::KeyValues::AutoDelete kv("Config");
	if (!kv->LoadFromFile(kConfigPath)) {
		pisex::g_pUtils->ErrorLog("[%s] Failed to load %s", kLogTag, kConfigPath);
		return; // defaults: server_id 1, reset on, spec_time on
	}
	m_serverId = kv->GetInt("server_id", m_serverId);
	m_reset = kv->GetBool("reset", m_reset);
	m_specTime = kv->GetBool("spec_time", m_specTime);
}

void AsAdminTimePlugin::OnCoreLoaded() {
	if (m_coreLoaded) {
		return;
	}
	m_coreLoaded = true;

	const admin_system::DatabaseConfig config = g_pAdminApi->GetDatabaseConfig();
	if (config.IsSqlite() && config.host.empty()) {
		pisex::g_pUtils->ErrorLog("[%s] The admin core uses SQLite but reported no database path", kLogTag);
	}
	plgdb::Config dbConfig =
		admin_system::MakeDatabaseConfig(config, config.IsSqlite() ? config.host : std::string());
	m_db = plgdb::Database::Create(dbConfig);
	if (!m_db) {
		pisex::g_pUtils->ErrorLog("[%s] Could not create the database connection (driver %s)", kLogTag,
								  config.driver.c_str());
		return;
	}

	RunQuery(config.IsSqlite() ? kCreateTableSqlite : kCreateTableMysql, {}, "CREATE TABLE as_admin_time");

	if (m_reset) {
		// The original reset this server's still-online rows the same way.
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? - `connect_time` WHERE "
				 "`disconnect_time` = -1 AND `server_id` = ?;",
				 {now, now, std::to_string(m_serverId)}, "online-row reset");
	}
}

void AsAdminTimePlugin::OnAdminConnect(int iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	if (!m_db) {
		return; // the core is not loaded (or has no database) yet
	}
	if (!g_pAdminApi->IsAdmin(iSlot)) {
		return;
	}
	const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (steamid == 0) {
		return;
	}
	m_openRowSteam[static_cast<std::size_t>(iSlot)] = steamid;
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	// The original inserted the online row the same way.
	RunQuery("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, `server_id`) VALUES (?, ?, ?, "
			 "?);",
			 {std::to_string(steamid), std::string(pisex::g_pPlayers->GetPlayerName(iSlot)), now,
			  std::to_string(m_serverId)},
			 "insert online row");
}

void AsAdminTimePlugin::OnPlayerDisconnect(const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast) {
	(void)szName;
	(void)bDontBroadcast;
	if (!pEvent) {
		return;
	}
	// The original read "userid" and used it as the player slot; in CS2 it has to be translated.
	const int iSlot = pEvent->GetPlayerSlot("userid");
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		pisex::g_pUtils->ErrorLog("[%s] player_disconnect: could not resolve the player slot (userid)", kLogTag);
		return;
	}
	if (!m_db) {
		return;
	}
	const auto slot = static_cast<std::size_t>(iSlot);
	// At a map change every client is disconnected before the event is delivered, the steam id
	// cannot be looked up any more; the id remembered when the admin connected names the row.
	const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
	const uint64_t rowSteam = steamid != 0 ? steamid : m_openRowSteam[slot];
	if (rowSteam == 0) {
		return;
	}
	m_openRowSteam[slot] = 0;

	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (!m_specTime) {
		// Time spent outside the spectator team (tracked by the jointeam handler).
		if (m_timeJoined[slot] > 0) {
			m_time[slot] += static_cast<int>(now) - m_timeJoined[slot];
			m_timeJoined[slot] = 0;
		}
		RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? WHERE `admin_id` = ? AND "
				 "`disconnect_time` = -1 AND `server_id` = ?;",
				 {now, static_cast<int64_t>(m_time[slot]), std::to_string(rowSteam),
				  std::to_string(m_serverId)},
				 "close online row");
	} else {
		RunQuery("UPDATE `as_admin_time` SET `disconnect_time` = ?, `played_time` = ? - `connect_time` WHERE "
				 "`admin_id` = ? AND `disconnect_time` = -1 AND `server_id` = ?;",
				 {now, now, std::to_string(rowSteam), std::to_string(m_serverId)}, "close online row");
	}

	m_time[slot] = 0;
	m_timeJoined[slot] = 0;
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult AsAdminTimePlugin::OnPluginStart() {
	try {
		// "admin_system" makes pisexcompat resolve "addons/configs/admin_system/..." through the
		// shared <configs>/admin_system directory every admin plugin uses.
		pisex::Init("admin_system");
		LoadConfig();

		// The database settings only exist once the core has finished loading. The original
		// registered OnCoreLoaded and got the connection inside the callback; a plugin that is
		// loaded after the core has to run the same startup immediately, because the event does
		// not fire twice (see API.md).
		g_pAdminApi->OnCoreLoaded([] { g_asAdminTimePlugin.OnCoreLoaded(); });
		if (g_pAdminApi->IsCoreLoaded()) {
			OnCoreLoaded();
		}

		g_pAdminApi->OnAdminConnected([](int iSlot) { g_asAdminTimePlugin.OnAdminConnect(iSlot); });

		// The original hooked the event with the Pisex utils; pisexcompat's HookEvent is the same
		// pre-fire point.
		pisex::g_pUtils->HookEvent("player_disconnect", [](const char* szName, pisex::IGameEvent* pEvent,
															bool bDontBroadcast) {
			g_asAdminTimePlugin.OnPlayerDisconnect(szName, pEvent, bDontBroadcast);
		});

		if (!m_specTime) {
			// Only needed when the spectator time is excluded: tracks when the admin leaves the
			// spectator team. The original registered this command with the console name
			// "jointeam" and returned false (let the engine continue).
			pisex::g_pUtils->RegCommand({"jointeam"}, {}, [this](int iSlot, const char* szContent) {
				if (iSlot < 0 || iSlot >= kMaxSlots) {
					return false;
				}
				// The content is "<command> <args...>"; the original took the first token, which
				// is the command name - reading the argument keeps the intended behaviour.
				pisex::CommandArgs args;
				args.Tokenize(szContent);
				if (args.ArgC() > 1) {
					const int iTeam = std::atoi(args.Arg(1));
					const auto slot = static_cast<std::size_t>(iSlot);
					if (m_timeJoined[slot] > 0 && iTeam == 1) {
						m_time[slot] += static_cast<int>(std::time(nullptr)) - m_timeJoined[slot];
						m_timeJoined[slot] = 0;
					} else if (iTeam != 1 && m_timeJoined[slot] == 0) {
						m_timeJoined[slot] = static_cast<int>(std::time(nullptr));
					}
				}
				return false;
			});
		}

		return {};
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsAdminTimePlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		pisex::Poll();
		if (m_db) {
			m_db->Poll(); // runs the completed database callbacks on the game thread
		}
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsAdminTimePlugin::OnPluginEnd() {
	try {
		// Drop the admin core listeners first: they point into this library, which is about to go
		// away. (pisexcompat only owns its own registrations.)
		g_pAdminApi->OnCoreLoadedUnregister();
		g_pAdminApi->OnAdminConnectedUnregister();
		pisex::Shutdown();
		m_db.reset(); // flushes and joins the plgdb worker
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asAdminTimePlugin)
