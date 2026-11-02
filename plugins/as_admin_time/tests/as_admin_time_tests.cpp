// as_admin_time tests — no engine: the plugin TU is compiled in (its handlers live in an anonymous
// namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake.
//
// Covered:
//   * src/config.hpp — the self-contained KV1 reader, the production admin_time.ini files (with
//     their comments, the missing "spec_time" key of the live file, the "Config" block merge), the
//     candidate paths <configs>/admin_system then <configs>/as_admin_time, the fallback walk, the
//     original's GetInt/GetBool semantics and the built-in defaults;
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, between
//     OnMapEnd and the next OnMapStart, or when the plugin was loaded into the map-change window;
//   * PITFALLS C2 — slot range, connected/SourceTV checks before GetClientName / GetClientSteamID64
//     (deliberately without IsClientInGame: the admin core can report a loaded admin before the
//     first spawn), the controller check of the name, "<unknown>" -> "", a bot's id is 0;
//   * the "connecting admin" path — OnAdminConnected with a connected but not-in-game client still
//     writes the session row (id + SQL), while out-of-range slots and the map-change window do
//     not;
//   * PITFALLS C12 — the "player_disconnect" Pre hook is attached in OnPluginStart and removed in
//     OnPluginEnd; the handler answers Continue and closes the row (also with the steam id cached
//     before a map change);
//   * PITFALLS C11 — "jointeam" is reached through the global listener and filtered by name, the
//     chat path and the server console are ignored, and the listener is removed on unload; the
//     spec_time = 0 tracking and the stored played_time;
//   * the database half against a real SQLite file through plgdb: the CREATE TABLE spelling, the
//     crash-recovery reset of this server's open rows (and only of this server's), the INSERT on
//     OnAdminConnected and the two "close the row" statements.
//
// Run by ./test.sh (build/tests/as_admin_time_tests). No pisexcompat and no s2guard are involved.

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

// The optional second CHECK argument is the human-readable description of the check.
inline void CheckMessage() {}

inline void CheckMessage(std::string_view message) {
	std::printf("  (%s)\n", std::string(message).c_str());
}

#define CHECK(expr, ...)                                                                       \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
			CheckMessage(__VA_ARGS__);                                                         \
		}                                                                                      \
	} while (false)

void CheckEq(std::string_view actual, std::string_view expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got '%s', expected '%s'\n", what, std::string(actual).c_str(),
					std::string(expected).c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got %lld, expected %lld\n", what, static_cast<long long>(actual),
					static_cast<long long>(expected));
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- the fake plugify host ------------------------------------------------------------------

std::string g_configsDir;

plg::string FakeGetConfigsDir() {
	return plg::string(g_configsDir.c_str());
}

struct FakeLogEntry {
	plg::Severity severity = plg::Severity::Info;
	std::string message;
};

std::vector<FakeLogEntry> g_logs;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logs.push_back(FakeLogEntry{severity, std::string(message)});
}

bool Logged(plg::Severity severity, std::string_view needle) {
	for (const FakeLogEntry& entry : g_logs) {
		if (entry.severity == severity && entry.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake map lifecycle -------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client layer --------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool hasController = false;
	std::string name;
	uint64_t steamid64 = 0;
};

std::vector<FakeClient> g_clients(64);

int g_clientCalls = 0; // every fake below that is a client query counts into this
int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTvCalls = 0;
int g_controllerCalls = 0;
int g_nameCalls = 0;
int g_steamIdCalls = 0;

const FakeClient& Slot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

void ResetClientCounters() {
	g_clientCalls = 0;
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTvCalls = 0;
	g_controllerCalls = 0;
	g_nameCalls = 0;
	g_steamIdCalls = 0;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	++g_connectedCalls;
	return Slot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	++g_inGameCalls;
	return Slot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	++g_sourceTvCalls;
	return Slot(slot).sourceTV;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	++g_controllerCalls;
	if (!Slot(slot).hasController) {
		return nullptr;
	}
	// Never dereferenced by the plugin: it only asks whether a controller exists.
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000) + static_cast<uintptr_t>(slot));
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	++g_nameCalls;
	// s2sdk answers "<unknown>" when it cannot resolve the client name.
	return plg::string(Slot(slot).name.empty() ? "<unknown>" : Slot(slot).name.c_str());
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	++g_steamIdCalls;
	return Slot(slot).steamid64;
}

// --- the fake command listener layer ------------------------------------------------------------

struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeListener> g_addedListeners;
std::vector<FakeListener> g_removedListeners;

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_addedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	g_removedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

int CountListeners(const std::string& name) {
	int count = 0;
	for (const FakeListener& listener : g_addedListeners) {
		count += listener.name == name ? 1 : 0;
	}
	return count;
}

const FakeListener* FindListener(const std::string& name) {
	for (const FakeListener& listener : g_addedListeners) {
		if (listener.name == name) {
			return &listener;
		}
	}
	return nullptr;
}

bool RemovedListener(const std::string& name, s2sdk::ConCommandCallback callback,
					 s2sdk::HookMode mode) {
	for (const FakeListener& listener : g_removedListeners) {
		if (listener.name == name && listener.callback == callback && listener.mode == mode) {
			return true;
		}
	}
	return false;
}

// --- the fake event layer -----------------------------------------------------------------------

struct FakeEventHook {
	std::string name;
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

s2sdk::EventCallback g_disconnectHook = nullptr;
std::vector<FakeEventHook> g_hookedEvents;
std::vector<FakeEventHook> g_unhookedEvents;
s2sdk::EventHookError g_hookEventResult = s2sdk::EventHookError::Okay;
int g_hookEventCalls = 0;
int g_unhookEventCalls = 0;

// The fake event object: a real address so the handler can pass it back to GetEventPlayerSlot.
int g_fakeEventStorage = 0;

int32_t g_eventSlot = -1;
std::string g_eventKey;
int g_eventSlotCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	++g_hookEventCalls;
	g_hookedEvents.push_back(FakeEventHook{ToStd(name), callback, mode});
	if (g_hookEventResult != s2sdk::EventHookError::Okay) {
		return g_hookEventResult;
	}
	if (ToStd(name) == "player_disconnect" && mode == s2sdk::HookMode::Pre) {
		g_disconnectHook = callback;
	}
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	++g_unhookEventCalls;
	g_unhookedEvents.push_back(FakeEventHook{ToStd(name), callback, mode});
	if (g_disconnectHook == callback) {
		g_disconnectHook = nullptr;
	}
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	++g_eventSlotCalls;
	g_eventKey = ToStd(key);
	if (event != &g_fakeEventStorage) {
		return -1;
	}
	return g_eventSlot;
}

s2sdk::ResultType FireDisconnect(int32_t slot) {
	g_eventSlot = slot;
	if (g_disconnectHook == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return g_disconnectHook(plg::string("player_disconnect"), &g_fakeEventStorage, false);
}

// --- the fake admin_system bindings --------------------------------------------------------------

std::array<std::string, 7> g_adminDb{"sqlite", "", "", "", "", "", ""}; // driver, host, port, user, pass, db, prefix
bool g_fakeCoreLoaded = true;
std::set<int32_t> g_admins;
std::vector<int32_t> g_adminChecks;

admin_system::CoreLoadedCallback g_coreLoadedCallback = nullptr;
admin_system::AdminConnectedCallback g_adminConnectedCallback = nullptr;
int g_coreLoadedRegisters = 0;
int g_coreLoadedUnregisters = 0;
int g_adminConnectedRegisters = 0;
int g_adminConnectedUnregisters = 0;

float FakeGetPluginVersion() {
	return 1.0f;
}

plg::vector<plg::string> FakeGetDatabaseConfig() {
	plg::vector<plg::string> out;
	for (const std::string& value : g_adminDb) {
		out.push_back(plg::string(value.c_str()));
	}
	return out;
}

bool FakeIsCoreLoaded() {
	return g_fakeCoreLoaded;
}

bool FakeIsAdmin(int32_t slot) {
	g_adminChecks.push_back(slot);
	return g_admins.count(slot) != 0;
}

void FakeOnCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	++g_coreLoadedRegisters;
	g_coreLoadedCallback = callback;
}

void FakeOnCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	++g_coreLoadedUnregisters;
	if (g_coreLoadedCallback == callback) {
		g_coreLoadedCallback = nullptr;
	}
}

void FakeOnAdminConnectedRegister(admin_system::AdminConnectedCallback callback) {
	++g_adminConnectedRegisters;
	g_adminConnectedCallback = callback;
}

void FakeOnAdminConnectedUnregister(admin_system::AdminConnectedCallback callback) {
	++g_adminConnectedUnregisters;
	if (g_adminConnectedCallback == callback) {
		g_adminConnectedCallback = nullptr;
	}
}

// Through the registered trampoline, i.e. the path the admin core uses.
void FireAdminConnected(int32_t slot) {
	if (g_adminConnectedCallback != nullptr) {
		g_adminConnectedCallback(slot);
	}
}

void FireCoreLoaded() {
	if (g_coreLoadedCallback != nullptr) {
		g_coreLoadedCallback();
	}
}

// --- fakes installation ---------------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_GetDatabaseConfig = &FakeGetDatabaseConfig;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_IsAdmin = &FakeIsAdmin;
	__admin_system_OnCoreLoaded_Register = &FakeOnCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeOnCoreLoadedUnregister;
	__admin_system_OnAdminConnected_Register = &FakeOnAdminConnectedRegister;
	__admin_system_OnAdminConnected_Unregister = &FakeOnAdminConnectedUnregister;

	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::Log = &FakeLog;
}

// --- the throw-away workspace ---------------------------------------------------------------------

std::string g_baseDir;

std::string AdminSystemConfigPath() {
	return g_baseDir + "/configs/admin_system/admin_time.ini";
}

std::string PluginConfigPath() {
	return g_baseDir + "/configs/as_admin_time/admin_time.ini";
}

void RemoveFile(const std::string& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
}

void WriteFile(const std::string& path, std::string_view text) {
	std::filesystem::create_directories(std::filesystem::path(path).parent_path());
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		std::printf("FAIL: could not write %s\n", path.c_str());
		++g_failures;
		return;
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::fclose(file);
}

void RemoveConfigFiles() {
	RemoveFile(AdminSystemConfigPath());
	RemoveFile(PluginConfigPath());
}

// A fresh SQLite file for the module's own connection; the probe opens a second connection to the
// same file to read what the plugin wrote (WAL is on, so the reader never blocks the writer).
std::string g_dbPath;

void UseDatabase(const char* name) {
	g_dbPath = g_baseDir + "/db_" + name + ".sqlite";
	RemoveFile(g_dbPath);
	RemoveFile(g_dbPath + "-wal");
	RemoveFile(g_dbPath + "-shm");
	g_adminDb = {"sqlite", g_dbPath, "", "", "", "", ""};
}

// --- the fake world reset ------------------------------------------------------------------------

void ResetWorld() {
	g_clients.assign(64, FakeClient{});
	g_admins.clear();
	g_adminChecks.clear();
	ResetClientCounters();

	g_addedListeners.clear();
	g_removedListeners.clear();

	g_disconnectHook = nullptr;
	g_hookedEvents.clear();
	g_unhookedEvents.clear();
	g_hookEventResult = s2sdk::EventHookError::Okay;
	g_hookEventCalls = 0;
	g_unhookEventCalls = 0;
	g_eventSlot = -1;
	g_eventKey.clear();
	g_eventSlotCalls = 0;

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_fakeCoreLoaded = true;
	g_coreLoadedCallback = nullptr;
	g_adminConnectedCallback = nullptr;
	g_coreLoadedRegisters = 0;
	g_coreLoadedUnregisters = 0;
	g_adminConnectedRegisters = 0;
	g_adminConnectedUnregisters = 0;

	g_logs.clear();
	RemoveConfigFiles();

	g_configsDir = g_baseDir + "/configs";
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::Log = &FakeLog;

	// The config the module starts from (the built-in defaults).
	g_config = as_admin_time::Config{};
}

void PrepareTest() {
	// Stops whatever a previous test left behind (OnPluginEnd is safe when nothing is registered)
	// and starts the next test from an empty fake world.
	g_asAdminTimePlugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_asAdminTimePlugin.OnPluginEnd();
	ResetWorld();
}

// --- the database probe ---------------------------------------------------------------------------

class Probe {
public:
	explicit Probe(const std::string& path) {
		plgdb::Config config;
		config.driver = plgdb::Driver::SQLite;
		config.sqlitePath = path;
		m_db = plgdb::Database::Create(config);
	}

	plgdb::Result Query(const std::string& sql, plgdb::Params params = {}) {
		if (!m_db) {
			plgdb::Result result;
			result.error = "no probe database";
			return result;
		}
		return m_db->QuerySync(sql, std::move(params));
	}

private:
	std::unique_ptr<plgdb::Database> m_db;
};

struct Scalar {
	bool ok = false;
	int64_t value = 0;
	std::string error;
};

Scalar QueryScalar(Probe& probe, const std::string& sql, const plgdb::Params& params) {
	Scalar out;
	const plgdb::Result result = probe.Query(sql, params);
	if (!result.Ok()) {
		out.error = result.Error();
		return out;
	}
	if (result.RowCount() == 0 || result.ColumnCount() == 0) {
		out.error = "no rows";
		return out;
	}
	out.ok = true;
	out.value = result.GetInt(0, 0);
	return out;
}

struct TextValue {
	bool ok = false;
	std::string value;
	std::string error;
};

TextValue QueryText(Probe& probe, const std::string& sql, const plgdb::Params& params) {
	TextValue out;
	const plgdb::Result result = probe.Query(sql, params);
	if (!result.Ok()) {
		out.error = result.Error();
		return out;
	}
	if (result.RowCount() == 0 || result.ColumnCount() == 0) {
		out.error = "no rows";
		return out;
	}
	out.ok = true;
	out.value = result.GetString(0, 0);
	return out;
}

// Polls the plugin (which also runs the plgdb callbacks) until `done` answers true.
bool WaitUntil(const std::function<bool()>& done, int timeoutMs = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (true) {
		g_asAdminTimePlugin.OnPluginUpdate(std::chrono::milliseconds(0));
		if (done()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

bool WaitForScalar(Probe& probe, const std::string& sql, const plgdb::Params& params,
				   int64_t expected, const char* what) {
	Scalar last;
	const bool done = WaitUntil([&] {
		last = QueryScalar(probe, sql, params);
		return last.ok && last.value == expected;
	});
	if (!done) {
		std::printf("  (%s: last %s)\n", what,
					last.ok ? std::to_string(last.value).c_str() : last.error.c_str());
	}
	return done;
}

bool WaitForScalarBetween(Probe& probe, const std::string& sql, const plgdb::Params& params,
						  int64_t minValue, int64_t maxValue, const char* what) {
	Scalar last;
	const bool done = WaitUntil([&] {
		last = QueryScalar(probe, sql, params);
		return last.ok && last.value >= minValue && last.value <= maxValue;
	});
	if (!done) {
		std::printf("  (%s: last %s, expected %lld..%lld)\n", what,
					last.ok ? std::to_string(last.value).c_str() : last.error.c_str(),
					static_cast<long long>(minValue), static_cast<long long>(maxValue));
	}
	return done;
}

int64_t NowSeconds() {
	return static_cast<int64_t>(std::time(nullptr));
}

// The module's SQLite DDL, byte for byte the statement it executes (used to pre-populate the file
// before the plugin starts).
constexpr char kTestCreateTable[] =
	"CREATE TABLE IF NOT EXISTS `as_admin_time` ("
	"`id` INTEGER PRIMARY KEY AUTOINCREMENT,"
	"`admin_id` VARCHAR(32) NOT NULL,"
	"`admin_name` VARCHAR(64) NOT NULL,"
	"`connect_time` INT NOT NULL,"
	"`disconnect_time` INT NOT NULL DEFAULT -1,"
	"`played_time` INT NOT NULL DEFAULT 0,"
	"`server_id` VARCHAR(32) NOT NULL"
	");";

// --- the config reader --------------------------------------------------------------------------

void TestConfigReader() {
	std::printf("-- KV1 reader\n");

	// A hand-edited admin_time.ini: both comment styles, a trailing comment, an unquoted value.
	const std::string_view text = R"(
// The Pisex file, with the comments an admin leaves behind.
"Config"
{
    "server_id"     "2"     // server 2
    /* the next two entries are unquoted on purpose */
    reset           1
    "spec_time"     "0"
})";

	as_admin_time::Config config;
	CHECK(as_admin_time::LoadConfigFromText(text, config));
	CheckEqInt(config.serverId, 2, "server_id");
	CHECK(config.reset);
	CHECK(!config.specTime);

	// Escapes, a BOM, and the case-insensitive lookup of the keys.
	const std::string bom = std::string("\xEF\xBB\xBF") + R"("Config" { "server_id" "7" "reset" "off" })";
	config = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText(bom, config));
	CheckEqInt(config.serverId, 7, "server_id after BOM");
	CHECK(!config.reset);
	CHECK(config.specTime); // missing key keeps the built-in default

	// #base / #include / #define lines are skipped, not followed.
	const std::string_view directives = R"(#base "admin_system/base.ini"
#include "shared.ini"
#define ADMINTIME
"Config" { "server_id" "5" })";
	config = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText(directives, config));
	CheckEqInt(config.serverId, 5, "server_id after preprocessor lines");

	// Top-level keys (no "Config" block) are read directly: the merge only happens for a single
	// top-level block.
	config = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText(R"("server_id" "9" "reset" "0")", config));
	CheckEqInt(config.serverId, 9, "top-level server_id");
	CHECK(!config.reset);

	// Two top-level blocks: nothing is merged, so the keys stay at their defaults (Valve's
	// loader only merges a *single* root block).
	config = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText(
		R"("Config" { "server_id" "4" } "Other" { "server_id" "6" })", config));
	CheckEqInt(config.serverId, 1, "two blocks keep the default");
}

void TestConfigGetters() {
	std::printf("-- KV1 getter semantics\n");

	as_admin_time::Config config;
	// GetInt: from_chars, then the original's atoi fallback; "" keeps the default.
	config = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText(
		R"("Config" { "a" "12" "b" "1.5" "c" "abc" "d" "-3" "e" "" })", config));
	const as_admin_time::KvNode root = *as_admin_time::ParseKv1(
		R"("Config" { "a" "12" "b" "1.5" "c" "abc" "d" "-3" "e" "" })");
	CheckEqInt(root.GetInt("a", 99), 12, "GetInt plain");
	CheckEqInt(root.GetInt("b", 99), 1, "GetInt falls back to atoi");
	CheckEqInt(root.GetInt("c", 99), 0, "GetInt non-numeric");
	CheckEqInt(root.GetInt("d", 99), -3, "GetInt negative");
	CheckEqInt(root.GetInt("e", 99), 99, "GetInt empty value keeps the default");
	CheckEqInt(root.GetInt("missing", 99), 99, "GetInt missing key");
	CheckEqInt(root.GetInt("a", 99), 12, "GetInt case-insensitive key");
	CheckEqInt(root.GetInt("A", 99), 12, "GetInt upper-case key");

	// GetBool: true/yes/on and false/no/off before the integer test, like pisexcompat's KeyValues.
	const as_admin_time::KvNode bools = *as_admin_time::ParseKv1(
		R"("Config" {
			"t" "true" "y" "YES" "o" "on"
			"f" "false" "n" "No" "x" "off"
			"two" "2" "zero" "0" "junk" "abc" "empty" ""
		})");
	CHECK(bools.GetBool("t", false));
	CHECK(bools.GetBool("y", false));
	CHECK(bools.GetBool("o", false));
	CHECK(!bools.GetBool("f", true));
	CHECK(!bools.GetBool("n", true));
	CHECK(!bools.GetBool("x", true));
	CHECK(bools.GetBool("two", false));
	CHECK(!bools.GetBool("zero", true));
	CHECK(!bools.GetBool("junk", true));
	CHECK(bools.GetBool("empty", true), "an empty value keeps the default");
	CHECK(!bools.GetBool("empty", false));
	CHECK(bools.GetBool("missing", true), "a missing key keeps the default");

	// The string getter: value, default for a missing key, default when the key carries no value.
	CheckEq(bools.GetString("missing", "def"), "def", "GetString default");
	CheckEq(bools.GetString("t"), "true", "GetString value");
	const as_admin_time::KvNode blockOnly = *as_admin_time::ParseKv1(R"("Config" { "sub" { "k" "v" } })");
	CheckEq(blockOnly.GetString("sub", "def"), "def", "a block child has no value");
}

void TestConfigMalformed() {
	std::printf("-- KV1 reader: malformed files are rejected whole\n");

	// Whatever `out` held before must survive a rejected file: the caller then tries the next
	// candidate config instead of running on half a table.
	as_admin_time::Config config;
	config.serverId = 42;
	config.reset = false;
	config.specTime = false;

	CHECK(!as_admin_time::LoadConfigFromText(R"("Config" { "server_id" "1")", config)); // no '}'
	CHECK(!as_admin_time::LoadConfigFromText(R"("Config" { "server_id" "1 })", config)); // open quote
	CHECK(!as_admin_time::LoadConfigFromText(R"("Config" { "server_id" "1" } })", config)); // stray '}'
	CheckEqInt(config.serverId, 42, "config untouched by a rejected file");
	CHECK(!config.reset);
	CHECK(!config.specTime);

	// Empty input parses as an empty root (no keys), i.e. it is *not* malformed - the defaults
	// stay, exactly like an empty KeyValues file.
	as_admin_time::Config empty = as_admin_time::Config{};
	CHECK(as_admin_time::LoadConfigFromText("", empty));
	CheckEqInt(empty.serverId, 1, "empty file keeps the default");
}

void TestConfigCandidates() {
	std::printf("-- config candidates\n");

	const std::vector<std::string> candidates =
		as_admin_time::ConfigCandidates("/srv/cs2/game/csgo/addons/plugify/configs");
	CHECK(candidates.size() == 2);
	// The Pisex layout first: every admin module shares configs/admin_system, and production
	// keeps admin_time.ini there.
	CheckEq(candidates[0], "/srv/cs2/game/csgo/addons/plugify/configs/admin_system/admin_time.ini",
			"primary candidate");
	// The plugin's own folder (PLUGIN_STANDARD §3) is the fallback.
	CheckEq(candidates[1], "/srv/cs2/game/csgo/addons/plugify/configs/as_admin_time/admin_time.ini",
			"fallback candidate");

	const std::vector<std::string> trailing = as_admin_time::ConfigCandidates("/srv/configs/");
	CHECK(trailing.size() == 2);
	CheckEq(trailing[0], "/srv/configs/admin_system/admin_time.ini", "trailing slash primary");
	CheckEq(trailing[1], "/srv/configs/as_admin_time/admin_time.ini", "trailing slash fallback");

	CHECK(as_admin_time::ConfigCandidates("").empty(), "no configs directory -> no candidates");

	CheckEq(as_admin_time::JoinPath("", "admin_time.ini"), "admin_time.ini", "JoinPath empty base");
	CheckEq(as_admin_time::JoinPath("/a/b", ""), "/a/b", "JoinPath empty name");
	CheckEq(as_admin_time::JoinPath("/a/b", "admin_time.ini"), "/a/b/admin_time.ini", "JoinPath plain");
	CheckEq(as_admin_time::JoinPath("/a/b/", "admin_time.ini"), "/a/b/admin_time.ini",
			"JoinPath trailing slash");
	CheckEq(as_admin_time::JoinPath("/a/b", "/admin_time.ini"), "/a/b/admin_time.ini",
			"JoinPath leading slash");
}

void TestShippedConfigs() {
	std::printf("-- shipped admin_time.ini files\n");

	const std::string dataDir = AS_ADMIN_TIME_TEST_DATA_DIR;

	// The live production file: no "spec_time" key (the default is on), a "Config" block and
	// Russian // comments after every value.
	const std::string prodPath = dataDir + "/admin_time_prod.ini";
	CHECK(as_admin_time::FileExists(prodPath));
	as_admin_time::Config prod;
	CHECK(as_admin_time::LoadConfigFromFile(prodPath, prod));
	CheckEqInt(prod.serverId, 1, "prod server_id");
	CHECK(prod.reset);
	CHECK(prod.specTime);

	// The module repository's file: spec_time is spelled out.
	const std::string modulePath = dataDir + "/admin_time_module.ini";
	CHECK(as_admin_time::FileExists(modulePath));
	as_admin_time::Config module;
	CHECK(as_admin_time::LoadConfigFromFile(modulePath, module));
	CheckEqInt(module.serverId, 1, "module server_id");
	CHECK(module.reset);
	CHECK(module.specTime);

	// The spectator-time configuration (spec_time = 0, reset off, server 3).
	const std::string specOffPath = dataDir + "/admin_time_spec_off.ini";
	CHECK(as_admin_time::FileExists(specOffPath));
	as_admin_time::Config specOff;
	CHECK(as_admin_time::LoadConfigFromFile(specOffPath, specOff));
	CheckEqInt(specOff.serverId, 3, "spec_off server_id");
	CHECK(!specOff.reset);
	CHECK(!specOff.specTime);

	// A missing file is not an error for the caller: false and the values stay untouched.
	as_admin_time::Config untouched = as_admin_time::Config{};
	untouched.serverId = 11;
	CHECK(!as_admin_time::LoadConfigFromFile("/nonexistent/as_admin_time/admin_time.ini", untouched));
	CheckEqInt(untouched.serverId, 11, "config untouched by a missing file");
}

// The plugin's own LoadConfig(): the candidate walk, the fallback and the defaults.
void TestLoadConfigWalk() {
	std::printf("-- LoadConfig() candidate walk\n");

	const std::string dataDir = AS_ADMIN_TIME_TEST_DATA_DIR;

	// 1. The Pisex layout wins when both files exist.
	PrepareTest();
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "2" })");
	WriteFile(PluginConfigPath(), R"("Config" { "server_id" "9" })");
	LoadConfig();
	CheckEqInt(g_config.serverId, 2, "the admin_system file wins");
	CHECK(Logged(plg::Severity::Info, AdminSystemConfigPath()));

	// 2. Only the plugin folder: the fallback is used.
	PrepareTest();
	WriteFile(PluginConfigPath(), R"("Config" { "server_id" "9" "spec_time" "0" })");
	LoadConfig();
	CheckEqInt(g_config.serverId, 9, "the plugin file is the fallback");
	CHECK(!g_config.specTime);
	CHECK(Logged(plg::Severity::Info, PluginConfigPath()));

	// 3. A malformed primary is reported and the fallback is loaded.
	PrepareTest();
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "2")");
	WriteFile(PluginConfigPath(), R"("Config" { "server_id" "8" })");
	LoadConfig();
	CheckEqInt(g_config.serverId, 8, "fallback after a malformed primary");
	CHECK(Logged(plg::Severity::Error, "Failed to load " + AdminSystemConfigPath()));

	// 4. A malformed fallback too: the built-in defaults stay and the failure is logged.
	PrepareTest();
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "2")");
	WriteFile(PluginConfigPath(), R"("Config" { "server_id" "8")");
	LoadConfig();
	CheckEqInt(g_config.serverId, 1, "defaults after two malformed files");
	CHECK(g_config.reset);
	CHECK(g_config.specTime);
	CHECK(Logged(plg::Severity::Error, "using the built-in defaults"));

	// 5. No file at all: defaults, and the "not found" log names both paths.
	PrepareTest();
	LoadConfig();
	CheckEqInt(g_config.serverId, 1, "defaults without a file");
	CHECK(Logged(plg::Severity::Error, "admin_time.ini not found (looked at " + AdminSystemConfigPath() +
										   ", " + PluginConfigPath() + ")"));

	// 6. The host reports no configs directory at all.
	PrepareTest();
	plg::GetConfigsDir = nullptr;
	LoadConfig();
	CheckEqInt(g_config.serverId, 1, "defaults without a configs directory");
	CHECK(Logged(plg::Severity::Error, "the plugify configs directory is unavailable"));

	// 7. The live production file, through the same walk.
	PrepareTest();
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "1" })");
	g_config = as_admin_time::Config{};
	LoadConfig();
	CheckEqInt(g_config.serverId, 1, "production server_id");
	CHECK(g_config.specTime);
	CHECK(as_admin_time::FileExists(dataDir + "/admin_time_prod.ini"));

	FinishTest();
}

// --- A. the map gate (PITFALLS B1/B2) --------------------------------------------------------------

void TestStartRegistrations() {
	std::printf("-- OnPluginStart registrations\n");

	PrepareTest();
	UseDatabase("start");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "4" "reset" "1" "spec_time" "1" })");

	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	CheckEqInt(g_mapStartRegisters, 1, "the map start listener is registered");
	CheckEqInt(g_mapEndRegisters, 1, "the map end listener is registered");
	CHECK(g_mapListeners);
	CHECK(!g_mapReady, "the gate is shut on plugin start");

	// PITFALLS C12: the disconnect hook, Pre, this plugin's handler.
	CHECK(g_disconnectHooked);
	CheckEqInt(g_hookEventCalls, 1, "one event hook");
	CHECK(g_hookedEvents.size() == 1);
	CheckEq(g_hookedEvents[0].name, "player_disconnect", "hooked event name");
	CheckEqInt(static_cast<int64_t>(g_hookedEvents[0].mode), static_cast<int64_t>(s2sdk::HookMode::Pre),
			   "the event hook is Pre");
	CHECK(g_hookedEvents[0].callback == &OnPlayerDisconnectEvent, "the event hook runs this plugin's handler");

	// spec_time = 1: no jointeam listener (the original registered it only for spec_time = 0).
	CHECK(!g_joinTeamListening);
	CheckEqInt(CountListeners(""), 0, "no global command listener with spec_time = 1");
	CheckEqInt(CountListeners("jointeam"), 0, "jointeam is never registered as a command");

	// The admin core listeners.
	CheckEqInt(g_coreLoadedRegisters, 1, "OnCoreLoaded is registered");
	CheckEqInt(g_adminConnectedRegisters, 1, "OnAdminConnected is registered");

	// The config and the database.
	CheckEqInt(g_config.serverId, 4, "the config was loaded");
	CHECK(g_coreLoaded);
	CHECK(g_db != nullptr, "the database is open (the fake core reports IsCoreLoaded)");
	CHECK(Logged(plg::Severity::Info, "ready (server_id 4, reset on, spec_time on)"));

	FinishTest();
}

void TestMapGate() {
	std::printf("-- the map gate\n");

	PrepareTest();
	UseDatabase("gate");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);

	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};

	// --- before the first map: not one client call may reach s2sdk ----------------------------
	ResetClientCounters();
	CHECK(!CanQueryClient(3), "CanQueryClient is false before the gate opens");
	CHECK(!HasClient(3), "HasClient is false before the gate opens");
	CHECK(!HasController(3), "HasController is false before the gate opens");
	CHECK(ClientSteamID64(3) == 0, "the SteamID64 is 0 before the gate opens");
	CHECK(ClientName(3).empty(), "the name is empty before the gate opens");
	CheckEqInt(g_clientCalls, 0, "not one s2sdk client call before the gate opens");

	// An admin connect in that window must not insert a row either.
	FireAdminConnected(3);
	CheckEqInt(g_clientCalls, 0, "no client call for an admin connect before the map");

	// --- OnMapStart latches the gate immediately ---------------------------------------------
	MapStart();
	CHECK(g_mapReady, "OnMapStart opens the gate");
	ResetClientCounters();
	CHECK(HasClient(3), "the live client is reachable");
	CHECK(HasController(3), "the client has a controller");
	CHECK(ClientName(3) == "Alice", "the name is read");
	CHECK(ClientSteamID64(3) == 76561198000000003ULL, "the SteamID64 is read");
	CHECK(g_clientCalls > 0, "the client calls reach s2sdk after the map start");

	// --- after OnMapEnd: not one client call may reach s2sdk ---------------------------------
	MapEnd();
	CHECK(!g_mapReady, "OnMapEnd drops readiness");
	g_asAdminTimePlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_mapReady, "an update after OnMapEnd does not re-open the gate");
	ResetClientCounters();
	CHECK(!HasClient(3), "HasClient is false after OnMapEnd");
	CHECK(!HasController(3), "HasController is false after OnMapEnd");
	CHECK(ClientName(3).empty(), "the name is empty after OnMapEnd");
	CHECK(ClientSteamID64(3) == 0, "the SteamID64 is 0 after OnMapEnd");
	CheckEqInt(g_clientCalls, 0, "not one s2sdk client call after OnMapEnd");

	// --- the next OnMapStart opens the gate again -------------------------------------------
	MapStart();
	CHECK(g_mapReady, "OnMapStart latches the gate again");
	CHECK(HasClient(3) && ClientName(3) == "Alice", "the client is reachable again");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: only the next OnMapStart opens the gate.
void TestMapChangeWindowStart() {
	std::printf("-- plugin loaded into the map-change window\n");

	PrepareTest();
	UseDatabase("window");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" })");
	g_asAdminTimePlugin.OnPluginStart();
	MapEnd();
	g_asAdminTimePlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");

	g_clients[2] = FakeClient{true, true, false, true, "Bob", 76561198000000002ULL};
	ResetClientCounters();
	CHECK(ClientSteamID64(2) == 0, "no client id in the map-change window");
	CheckEqInt(g_clientCalls, 0, "no client call in the map-change window");

	MapStart();
	CHECK(g_mapReady, "the next OnMapStart opens the gate");
	CHECK(HasClient(2) && ClientName(2) == "Bob", "the client is reachable after the map start");

	FinishTest();
}

// --- B. the client guards (PITFALLS C2) -------------------------------------------------------------

void TestClientGuards() {
	std::printf("-- client guards\n");

	PrepareTest();
	UseDatabase("guards");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" })");
	g_asAdminTimePlugin.OnPluginStart();
	MapStart();
	CHECK(g_mapReady);

	// The id only needs a connected client (the Pisex GetSteamID64 guard); a client that is still
	// connecting has no controller yet, so it has no name yet.
	g_clients[7] = FakeClient{true, false, false, true, "Dave", 76561198000000007ULL}; // connected, no map spawn
	g_clients[8] = FakeClient{true, true, false, true, "", 76561198000000008ULL};     // "<unknown>" name
	g_clients[9] = FakeClient{true, true, true, true, "SourceTV", 0};                // the proxy
	g_clients[10] = FakeClient{true, false, false, false, "BOT Rick", 0};            // connecting, no controller
	g_clients[11] = FakeClient{true, true, false, true, "Carol", 76561198000000011ULL};
	g_clients[12] = FakeClient{false, false, false, true, "Gone", 76561198000000012ULL}; // disconnected

	// Out-of-range slots never reach s2sdk.
	ResetClientCounters();
	CHECK(!CanQueryClient(-1) && !CanQueryClient(64), "slots -1 and 64 cannot be queried");
	CHECK(!HasClient(-1) && !HasClient(64), "out-of-range slots have no client");
	CHECK(!HasController(-1) && !HasController(64), "out-of-range slots have no controller");
	CHECK(ClientName(-1).empty() && ClientName(64).empty(), "out-of-range slots have no name");
	CHECK(ClientSteamID64(-1) == 0 && ClientSteamID64(64) == 0, "out-of-range slots have no id");
	CheckEqInt(g_clientCalls, 0, "slots -1 and 64 never reach a client binding");

	// A connected client that is NOT in game yet: the id is read (the old GetSteamID64 did, and
	// OnAdminConnected can arrive before the first spawn), the name is read via its controller.
	ResetClientCounters();
	CHECK(HasClient(7), "a connected client is a client even before it is in game");
	CHECK(HasController(7), "the connecting client already has a controller");
	CHECK(ClientSteamID64(7) == 76561198000000007ULL, "the id of a connected client is read");
	CHECK(ClientName(7) == "Dave", "the name of a connecting client with a controller is read");
	CheckEqInt(g_inGameCalls, 0, "IsClientInGame is not consulted at all");
	CheckEqInt(g_steamIdCalls, 1, "GetClientSteamID64 was called once");
	CheckEqInt(g_nameCalls, 1, "GetClientName was called once");

	// A disconnected slot has neither an id nor a name.
	ResetClientCounters();
	CHECK(!HasClient(12), "a slot without a client has no client");
	CHECK(!HasController(12), "a slot without a client has no controller");
	CHECK(ClientSteamID64(12) == 0, "no id for a disconnected slot");
	CHECK(ClientName(12).empty(), "no name for a disconnected slot");
	CheckEqInt(g_steamIdCalls, 0, "GetClientSteamID64 is not called for a disconnected slot");
	CheckEqInt(g_nameCalls, 0, "GetClientName is not called for a disconnected slot");
	CheckEqInt(g_controllerCalls, 0, "PlayerSlotToEntPointer is not called without a client");

	// s2sdk's "<unknown>" placeholder becomes "".
	ResetClientCounters();
	CHECK(HasClient(8) && HasController(8), "a connected human has a client and controller");
	CHECK(ClientName(8).empty(), "the \"<unknown>\" placeholder becomes an empty name");
	CheckEqInt(g_nameCalls, 1, "GetClientName was called once");

	// The SourceTV proxy is not a client: no id, no name, no controller query.
	ResetClientCounters();
	CHECK(!HasClient(9), "the SourceTV proxy is not a client");
	CHECK(!HasController(9), "the SourceTV proxy has no controller");
	CHECK(ClientSteamID64(9) == 0, "the SourceTV proxy has no id");
	CHECK(ClientName(9).empty(), "the SourceTV proxy has no name");
	CheckEqInt(g_steamIdCalls, 0, "GetClientSteamID64 is not called for the proxy");
	CheckEqInt(g_nameCalls, 0, "GetClientName is not called for the proxy");

	// No controller: the name is not read, the id still is (this is the admin-name "" case).
	ResetClientCounters();
	CHECK(HasClient(10), "a connecting client without a controller is still a client");
	CHECK(!HasController(10), "a slot without a controller entity has no controller");
	CHECK(ClientName(10).empty(), "no name without a controller");
	CheckEqInt(g_nameCalls, 0, "GetClientName is not called without a controller");
	CHECK(ClientSteamID64(10) == 0, "the id of this bot is 0");
	CheckEqInt(g_steamIdCalls, 1, "the id is still asked for (and answered 0)");

	// A human in game.
	CHECK(HasController(11) && ClientName(11) == "Carol", "a human's name is read");
	CHECK(ClientSteamID64(11) == 76561198000000011ULL, "a human's id is read");

	FinishTest();
}

// --- C. OnAdminConnected -> INSERT -------------------------------------------------------------

void TestAdminConnectInsertsRow() {
	std::printf("-- OnAdminConnected inserts the online row\n");

	PrepareTest();
	UseDatabase("insert");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	g_asAdminTimePlugin.OnPluginStart();
	MapStart();

	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	const int64_t before = NowSeconds();

	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));

	const TextValue adminId =
		QueryText(probe, "SELECT `admin_id` FROM `as_admin_time`", {});
	CHECK(adminId.ok);
	CheckEq(adminId.value, "76561198000000003", "admin_id is the steamid64 as text");
	const TextValue adminName = QueryText(probe, "SELECT `admin_name` FROM `as_admin_time`", {});
	CHECK(adminName.ok);
	CheckEq(adminName.value, "Alice", "admin_name is the player name");
	const TextValue serverId = QueryText(probe, "SELECT `server_id` FROM `as_admin_time`", {});
	CHECK(serverId.ok);
	CheckEq(serverId.value, "1", "server_id is the configured one");
	CHECK(QueryScalar(probe, "SELECT `disconnect_time` FROM `as_admin_time`", {}).value == -1);
	CHECK(QueryScalar(probe, "SELECT `played_time` FROM `as_admin_time`", {}).value == 0);
	const Scalar connectTime = QueryScalar(probe, "SELECT `connect_time` FROM `as_admin_time`", {});
	CHECK(connectTime.ok);
	CHECK(connectTime.value >= before && connectTime.value <= NowSeconds() + 1);

	// The schema is the original's SQLite spelling.
	const TextValue schema =
		QueryText(probe, "SELECT `sql` FROM `sqlite_master` WHERE `name` = 'as_admin_time'", {});
	CHECK(schema.ok);
	CHECK(schema.value.find("AUTOINCREMENT") != std::string::npos);
	CHECK(schema.value.find("`disconnect_time` INT NOT NULL DEFAULT -1") != std::string::npos);

	// A non-admin slot: no row (checked by IsAdmin before anything else).
	g_clients[4] = FakeClient{true, true, false, true, "Bob", 76561198000000004ULL};
	FireAdminConnected(4);
	CHECK(QueryScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}).value == 1);
	CHECK(std::find(g_adminChecks.begin(), g_adminChecks.end(), 4) != g_adminChecks.end());

	// A missing controller: the row is still opened, with the empty name (the original's
	// GetPlayerName answer for a client without a controller).
	g_clients[5] = FakeClient{true, true, false, false, "", 76561198000000005ULL};
	g_admins.insert(5);
	FireAdminConnected(5);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 2, "the second row"));
	CheckEq(QueryText(probe, "SELECT `admin_name` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000005")})
				.value,
			"", "an unknown name is stored empty");

	// A slot without a steam id (a bot / a slot that is not connected): no row.
	g_clients[6] = FakeClient{true, true, false, true, "Bot", 0};
	g_admins.insert(6);
	FireAdminConnected(6);
	CHECK(QueryScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}).value == 2);

	// Before the map the id must not even be read, so no row is inserted (PITFALLS B1).
	PrepareTest(); // ends the plugin (db closed) and resets the world
	UseDatabase("insert2");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe2(g_dbPath);
	ResetClientCounters();
	FireAdminConnected(3);
	CheckEqInt(g_clientCalls, 0, "no client query before the first map");
	// The table exists (CREATE TABLE ran) but holds no row; wait for the table first.
	CHECK(WaitUntil([&] {
		const Scalar count = QueryScalar(probe2, "SELECT COUNT(*) FROM `as_admin_time`", {});
		return count.ok;
	}));
	CHECK(QueryScalar(probe2, "SELECT COUNT(*) FROM `as_admin_time`", {}).value == 0);

	FinishTest();
}

// --- C2. OnAdminConnected while the client is still connecting -------------------------------------

// The admin core starts its LoadAdmin from the connect/authenticate path, so OnAdminConnected can be
// delivered while the client is connected but NOT yet in game. The session row must be created then:
// the original's pisex::GetSteamID64 answered for every connected slot, and requiring
// IsClientInGame would silently lose that admin's session (the regression this test guards).
void TestAdminConnectWhileConnecting() {
	std::printf("-- OnAdminConnected while the client is still connecting\n");

	PrepareTest();
	UseDatabase("connecting");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	MapStart();

	Probe probe(g_dbPath);

	// Connected, authenticated, valid steamid, NOT in game, controller already exists.
	g_clients[6] = FakeClient{true, false, false, true, "Alice", 76561198000000006ULL};
	g_admins = {6};
	ResetClientCounters();
	FireAdminConnected(6);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1,
						"the row of a connecting admin"));
	CheckEq(QueryText(probe, "SELECT `admin_id` FROM `as_admin_time`", {}).value,
			"76561198000000006", "the id of a not-in-game client is stored");
	CheckEq(QueryText(probe, "SELECT `admin_name` FROM `as_admin_time`", {}).value, "Alice",
			"the name is stored (the controller exists)");
	CheckEq(QueryText(probe, "SELECT `server_id` FROM `as_admin_time`", {}).value, "1",
			"the server_id is stored");
	CHECK(QueryScalar(probe, "SELECT `disconnect_time` FROM `as_admin_time`", {}).value == -1);
	CheckEqInt(g_inGameCalls, 0, "IsClientInGame is never consulted for the id");
	CheckEqInt(g_steamIdCalls, 1, "the id is read once from the connected client");

	// Connected, no controller yet (a very early authenticate): the row is still opened, with the
	// empty name the original's GetPlayerName answered for such a slot.
	g_clients[7] = FakeClient{true, false, false, false, "", 76561198000000007ULL};
	g_admins.insert(7);
	FireAdminConnected(7);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 2,
						"the row of the second connecting admin"));
	CheckEq(QueryText(probe, "SELECT `admin_name` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000007")})
				.value,
			"", "no controller yet -> empty name");

	// Out-of-range slots are refused before any client call and before IsAdmin.
	ResetClientCounters();
	g_adminChecks.clear();
	FireAdminConnected(-1);
	FireAdminConnected(64);
	CheckEqInt(g_clientCalls, 0, "out-of-range slots make no client call");
	CHECK(g_adminChecks.empty(), "out-of-range slots never reach IsAdmin");
	CHECK(QueryScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}).value == 2);

	// The map gate still wins: between OnMapEnd and the next OnMapStart the id must not be read and
	// no session row may be opened.
	MapEnd();
	ResetClientCounters();
	FireAdminConnected(6);
	CheckEqInt(g_clientCalls, 0, "no client query in the map-change window");
	CHECK(QueryScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}).value == 2);

	// The next map: the same admin's new connection opens a new row.
	MapStart();
	FireAdminConnected(6);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 3,
						"the row of the new map"));

	FinishTest();
}

// --- D. player_disconnect closes the row (PITFALLS C12) ------------------------------------------

void TestPlayerDisconnectClosesRow() {
	std::printf("-- player_disconnect closes the row (spec_time = 1)\n");

	PrepareTest();
	UseDatabase("disconnect");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	g_asAdminTimePlugin.OnPluginStart();
	MapStart();

	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));

	// Age the row so "played_time = now - connect_time" is distinguishable from 0.
	const int64_t now = NowSeconds();
	CHECK(probe.Query("UPDATE `as_admin_time` SET `connect_time` = ? WHERE `admin_id` = ?",
					  {now - 100, std::string("76561198000000003")})
			  .Ok());

	ResetClientCounters();
	g_eventSlotCalls = 0;
	g_eventKey.clear();
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue, "the handler answers Continue");
	CheckEqInt(g_eventSlotCalls, 1, "the slot is read from the event");
	CheckEq(g_eventKey, "userid", "from the event's userid");

	CHECK(WaitForScalarBetween(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
							   {std::string("76561198000000003")}, 95, 105, "played_time"));
	const Scalar disconnectTime = QueryScalar(
		probe, "SELECT `disconnect_time` FROM `as_admin_time` WHERE `admin_id` = ?",
		{std::string("76561198000000003")});
	CHECK(disconnectTime.ok);
	CHECK(disconnectTime.value >= now && disconnectTime.value <= NowSeconds() + 1);
	CHECK(g_openRowSteam[3] == 0, "the cached steam id is forgotten");

	// A second event for the same slot: the row is already closed, nothing happens.
	const Scalar playedBefore = QueryScalar(
		probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
		{std::string("76561198000000003")});
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue);
	WaitUntil([] { return false; }, 100); // give the (absent) statement time to run
	const Scalar playedAfter = QueryScalar(
		probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
		{std::string("76561198000000003")});
	CHECK(playedAfter.ok && playedBefore.ok && playedAfter.value == playedBefore.value);

	// An unknown slot is logged and ignored; an event without a slot id (-1) too.
	g_logs.clear();
	g_eventSlot = 64;
	CHECK(FireDisconnect(-1) == s2sdk::ResultType::Continue);
	CHECK(Logged(plg::Severity::Error, "could not resolve the player slot"));

	// A slot that never had a row: no statement, no crash.
	g_eventSlot = 20;
	CHECK(FireDisconnect(20) == s2sdk::ResultType::Continue);

	FinishTest();
}

// At a map change the clients are gone before player_disconnect arrives: the steam id cached at
// connect time must still close the row (PITFALLS B2 + the original's P3 leak).
void TestDisconnectInMapChangeWindow() {
	std::printf("-- player_disconnect in the map-change window\n");

	PrepareTest();
	UseDatabase("window_disconnect");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");
	g_asAdminTimePlugin.OnPluginStart();
	MapStart();

	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));
	CHECK(g_openRowSteam[3] == 76561198000000003ULL);

	// The map ends, the engine drops the clients, then delivers the disconnects.
	MapEnd();
	g_clients[3] = FakeClient{};
	const int64_t now = NowSeconds();
	CHECK(probe.Query("UPDATE `as_admin_time` SET `connect_time` = ? WHERE `admin_id` = ?",
					  {now - 60, std::string("76561198000000003")})
			  .Ok());

	ResetClientCounters();
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue);
	CheckEqInt(g_clientCalls, 0, "no client query in the map-change window");
	CHECK(WaitForScalarBetween(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
							   {std::string("76561198000000003")}, 55, 65,
							   "played_time from the cached steam id"));

	FinishTest();
}

// --- E. spec_time = 0: the global jointeam listener (PITFALLS C11) --------------------------------

plg::vector<plg::string> Args(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

s2sdk::ResultType DispatchGlobal(int32_t caller, s2sdk::ConCommandContext context,
								 std::initializer_list<const char*> words) {
	const FakeListener* listener = FindListener("");
	if (listener == nullptr) {
		std::printf("FAIL: the global command listener is not installed\n");
		++g_failures;
		return s2sdk::ResultType::Continue;
	}
	return listener->callback(caller, context, Args(words));
}

void TestJoinTeamTracking() {
	std::printf("-- spec_time = 0: the jointeam tracking\n");

	PrepareTest();
	UseDatabase("jointeam");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "3" "reset" "0" "spec_time" "0" })");
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);

	// The listener: the *global* one (empty name), Pre, this plugin's handler. "jointeam" itself
	// is never registered as a console command and never listened to by name.
	CHECK(g_joinTeamListening);
	CheckEqInt(CountListeners(""), 1, "one global command listener");
	CheckEqInt(CountListeners("jointeam"), 0, "jointeam is not listened to by name");
	const FakeListener* listener = FindListener("");
	CHECK(listener != nullptr && listener->callback == &OnGlobalCommand);
	CHECK(listener != nullptr && listener->mode == s2sdk::HookMode::Pre);
	CheckEqInt(g_config.serverId, 3, "server_id 3 from the config");

	// Leaving the spectator team starts the counter.
	g_timeJoined[3] = 0;
	g_time[3] = 0;
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Console, {"jointeam", "2"}) ==
			  s2sdk::ResultType::Continue,
		  "jointeam continues to the engine");
	CHECK(g_timeJoined[3] > 0, "the join time is remembered");
	CHECK(g_time[3] == 0);

	// Already counting: another non-spectator team does not restart it.
	const int joined = g_timeJoined[3];
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Console, {"jointeam", "3"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], joined, "the join time is not restarted");

	// Time travel: the admin spent two minutes outside the spectator team.
	g_timeJoined[3] -= 120;
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Console, {"joinTEAM", "1"}) ==
			  s2sdk::ResultType::Continue,
		  "the command name is matched case-insensitively");
	CHECK(g_time[3] >= 120 && g_time[3] <= 122, "the time outside the spectator team is added");
	CheckEqInt(g_timeJoined[3], 0, "the join time is cleared");

	// The command name filter, the chat path and the server console are all ignored.
	const int timeBefore = g_time[3];
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Console, {"joingame", "1"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], 0, "another command does not start the counter");
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Chat, {"jointeam", "2"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], 0, "a chat spelling is not a team change");
	CHECK(DispatchGlobal(-1, s2sdk::ConCommandContext::Console, {"jointeam", "2"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], 0, "the server console has no slot to track");
	CHECK(DispatchGlobal(3, s2sdk::ConCommandContext::Console, {"jointeam"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], 0, "a missing team argument is ignored");
	CHECK(DispatchGlobal(70, s2sdk::ConCommandContext::Console, {"jointeam", "2"}) ==
			  s2sdk::ResultType::Continue);
	CheckEqInt(g_timeJoined[3], 0, "an out-of-range slot is ignored");
	CheckEqInt(g_time[3], timeBefore, "the ignored calls change nothing");

	// The stored played_time is the tracked time, not the wall-clock time since connect.
	g_timeJoined[3] = static_cast<int>(NowSeconds()) - 60; // counting again, 60 s so far
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));

	// The row says the admin connected 500 s ago; the tracked time is g_time[3] (~the 120 s above
	// plus the 60 s being counted when the disconnect arrives).
	const int64_t connectTime = NowSeconds() - 500;
	CHECK(probe.Query("UPDATE `as_admin_time` SET `connect_time` = ? WHERE `admin_id` = ?",
					  {connectTime, std::string("76561198000000003")})
			  .Ok());
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue);
	CHECK(WaitForScalarBetween(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
							   {std::string("76561198000000003")}, 175, 185,
							   "played_time counts only the non-spectator time"));
	CheckEq(QueryText(probe, "SELECT `server_id` FROM `as_admin_time`", {}).value, "3",
			"server_id 3 is stored");

	FinishTest();
}

// --- F. the config fallback through the plugin (no admin core yet) ------------------------------

void TestLateCoreAndNoCore() {
	std::printf("-- the core loads later / never\n");

	// The plugin is loaded before the core finished loading: no database until the callback.
	PrepareTest();
	UseDatabase("late_core");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" })");
	g_fakeCoreLoaded = false;
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_coreLoaded);
	CHECK(g_db == nullptr, "no database before the core reports it is loaded");

	FireCoreLoaded();
	CHECK(g_coreLoaded);
	CHECK(g_db != nullptr, "the core-loaded callback opens the database");

	// The callback is idempotent: a second call does not reopen the connection.
	plgdb::Database* const first = g_db.get();
	FireCoreLoaded();
	CHECK(g_db.get() == first, "the core-loaded callback runs once");

	// An admin connect now works.
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));

	FinishTest();
}

// --- G. the crash-recovery reset ------------------------------------------------------------------

void TestResetOnCoreLoaded() {
	std::printf("-- the online-row reset (reset = 1)\n");

	PrepareTest();
	UseDatabase("reset");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "1" "spec_time" "1" })");

	// A crash left two rows of this server open, plus one of server 2 and one already closed row.
	const int64_t now = NowSeconds();
	Probe probe(g_dbPath);
	CHECK(probe.Query(kTestCreateTable, {}).Ok());
	CHECK(probe.Query("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, "
					  "`disconnect_time`, `played_time`, `server_id`) VALUES (?, ?, ?, -1, 0, ?)",
					  {std::string("76561198000000001"), std::string("Alice"), now - 100,
					   std::string("1")})
			  .Ok());
	CHECK(probe.Query("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, "
					  "`disconnect_time`, `played_time`, `server_id`) VALUES (?, ?, ?, -1, 0, ?)",
					  {std::string("76561198000000002"), std::string("Bob"), now - 100,
					   std::string("2")})
			  .Ok());
	CHECK(probe.Query("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, "
					  "`disconnect_time`, `played_time`, `server_id`) VALUES (?, ?, ?, ?, 50, ?)",
					  {std::string("76561198000000003"), std::string("Carol"), now - 500, now - 450,
					   std::string("1")})
			  .Ok());

	g_asAdminTimePlugin.OnPluginStart();

	// This server's open row is closed with played_time = now - connect_time (~100 s).
	CHECK(WaitForScalarBetween(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
							   {std::string("76561198000000001")}, 95, 105, "the reset played_time"));
	const Scalar closedDisconnect = QueryScalar(
		probe, "SELECT `disconnect_time` FROM `as_admin_time` WHERE `admin_id` = ?",
		{std::string("76561198000000001")});
	CHECK(closedDisconnect.ok && closedDisconnect.value != -1, "the reset closes the row");

	// The other server's open row and the already closed row are untouched.
	CHECK(QueryScalar(probe, "SELECT `disconnect_time` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000002")})
			  .value == -1,
		  "another server's open row is not closed");
	CHECK(QueryScalar(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000002")})
			  .value == 0);
	CHECK(QueryScalar(probe, "SELECT `played_time` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000003")})
			  .value == 50,
		  "an already closed row is not rewritten");
	CHECK(QueryScalar(probe, "SELECT `disconnect_time` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000003")})
			  .value == now - 450);

	FinishTest();
}

void TestNoResetWhenDisabled() {
	std::printf("-- reset = 0 keeps the open rows\n");

	PrepareTest();
	UseDatabase("noreset");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "1" })");

	const int64_t now = NowSeconds();
	Probe probe(g_dbPath);
	CHECK(probe.Query(kTestCreateTable, {}).Ok());
	CHECK(probe.Query("INSERT INTO `as_admin_time` (`admin_id`, `admin_name`, `connect_time`, "
					  "`disconnect_time`, `played_time`, `server_id`) VALUES (?, ?, ?, -1, 0, ?)",
					  {std::string("76561198000000001"), std::string("Alice"), now - 100,
					   std::string("1")})
			  .Ok());

	g_asAdminTimePlugin.OnPluginStart();

	// Ordering proof: the freshly opened row is inserted after the (absent) reset statement, so
	// once it is visible every statement the plugin queued at start has completed.
	MapStart();
	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 2, "the new online row"));
	CHECK(QueryScalar(probe, "SELECT `disconnect_time` FROM `as_admin_time` WHERE `admin_id` = ?",
					  {std::string("76561198000000001")})
			  .value == -1,
		  "the old row is left open");

	FinishTest();
}

// --- H. the manifest / binding degradation paths ---------------------------------------------------

void TestMissingBindings() {
	std::printf("-- missing s2sdk bindings\n");

	PrepareTest();
	UseDatabase("bindings");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" })");

	__s2sdk_HookEvent = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	g_logs.clear();

	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart survives missing bindings");
	CHECK(!g_disconnectHooked, "no event hook without the binding");
	CHECK(!g_mapListeners, "no map listeners without the binding");
	CHECK(Logged(plg::Severity::Error, "the s2sdk map listeners are unavailable"));
	CHECK(Logged(plg::Severity::Error, "\"player_disconnect\" is not hooked"));
	g_asAdminTimePlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CHECK(!g_mapReady, "the gate can never open without the map listeners");

	g_asAdminTimePlugin.OnPluginEnd();
	CheckEqInt(g_unhookEventCalls, 0, "no unhook without a hook");
	CheckEqInt(g_mapStartUnregisters, 0, "no map unregister without a registration");

	// A hook that the event system refuses is logged, not fatal.
	InstallFakes();
	PrepareTest();
	UseDatabase("bindings2");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" })");
	g_hookEventResult = s2sdk::EventHookError::InvalidEvent;
	g_logs.clear();
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(!g_disconnectHooked);
	CHECK(Logged(plg::Severity::Error, "could not hook \"player_disconnect\""));

	FinishTest();
}

// --- I. a clean unload (PITFALLS A7/C11/C12) -------------------------------------------------------

void TestUnload() {
	std::printf("-- OnPluginEnd removes everything\n");

	PrepareTest();
	UseDatabase("unload");
	WriteFile(AdminSystemConfigPath(), R"("Config" { "server_id" "1" "reset" "0" "spec_time" "0" })");
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	MapStart();
	CHECK(g_mapReady);
	CHECK(g_disconnectHooked && g_joinTeamListening);

	const s2sdk::EventCallback disconnected = g_disconnectHook;
	const FakeListener* listener = FindListener("");
	CHECK(listener != nullptr);
	if (listener == nullptr) {
		FinishTest();
		return;
	}
	const s2sdk::ConCommandCallback global = listener->callback;

	g_clients[3] = FakeClient{true, true, false, true, "Alice", 76561198000000003ULL};
	g_admins = {3};
	Probe probe(g_dbPath);
	FireAdminConnected(3);
	CHECK(WaitForScalar(probe, "SELECT COUNT(*) FROM `as_admin_time`", {}, 1, "the online row"));

	g_asAdminTimePlugin.OnPluginEnd();

	// The event hook is removed with the same name/callback/mode it was added with.
	CheckEqInt(g_unhookEventCalls, 1, "one unhook");
	CHECK(g_unhookedEvents.size() == 1);
	CheckEq(g_unhookedEvents[0].name, "player_disconnect", "unhooked event name");
	CHECK(g_unhookedEvents[0].callback == &OnPlayerDisconnectEvent);
	CheckEqInt(static_cast<int64_t>(g_unhookedEvents[0].mode), static_cast<int64_t>(s2sdk::HookMode::Pre),
			   "the unhook is Pre");
	CHECK(g_disconnectHook == nullptr, "the hook is gone from s2sdk");

	// The global listener is removed with the empty name and the same callback; "jointeam" was
	// never registered as a command, so RemoveCommand is not part of the port at all.
	CHECK(RemovedListener("", &OnGlobalCommand, s2sdk::HookMode::Pre),
		  "the global listener is detached with RemoveCommandListener");
	CheckEqInt(CountListeners("jointeam"), 0, "jointeam is never a command of this plugin");

	// The map listeners and the admin core listeners.
	CheckEqInt(g_mapStartUnregisters, 1, "the map start listener is removed");
	CheckEqInt(g_mapEndUnregisters, 1, "the map end listener is removed");
	CHECK(g_onMapStart == nullptr && g_onMapEnd == nullptr);
	CheckEqInt(g_coreLoadedUnregisters, 1, "OnCoreLoaded is unregistered");
	CheckEqInt(g_adminConnectedUnregisters, 1, "OnAdminConnected is unregistered");

	CHECK(!g_mapReady, "the gate is shut after the unload");
	CHECK(g_db == nullptr, "the database is closed");
	CHECK(g_pAdminApi == nullptr);
	CHECK(g_openRowSteam[3] == 0 && g_time[3] == 0 && g_timeJoined[3] == 0, "the slot state is cleared");

	// A stale callback (the engine may still hold the pointer for a moment) is harmless: no
	// client query, no database statement.
	ResetClientCounters();
	CHECK(disconnected(plg::string("player_disconnect"), &g_fakeEventStorage, false) ==
			  s2sdk::ResultType::Continue,
		  "the stale event callback answers Continue");
	g_eventSlot = 3;
	CHECK(FireDisconnect(3) == s2sdk::ResultType::Continue);
	CHECK(global(3, s2sdk::ConCommandContext::Console, Args({"jointeam", "2"})) ==
			  s2sdk::ResultType::Continue,
		  "the stale command callback answers Continue");
	CheckEqInt(g_clientCalls, 0, "a callback after OnPluginEnd makes no client call");

	// plg reload: a fresh start after the end registers everything exactly once again.
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CheckEqInt(g_mapStartRegisters, 2, "a reload registers the map listeners again");
	CheckEqInt(g_mapEndRegisters, 2, "a reload registers the map listeners again");
	CheckEqInt(g_hookEventCalls, 2, "a reload hooks player_disconnect again");
	CheckEqInt(g_coreLoadedRegisters, 2, "a reload registers OnCoreLoaded again");
	CheckEqInt(g_adminConnectedRegisters, 2, "a reload registers OnAdminConnected again");
	CheckEqInt(CountListeners(""), 2, "a reload attaches the global listener again");
	CHECK(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	CHECK(g_mapReady, "the reloaded plugin opens the gate on OnMapStart");

	FinishTest();
}

// The production file has no "spec_time" key: the module must not install the jointeam listener.
void TestSpecTimeDefaultSkipsListener() {
	std::printf("-- spec_time defaults to on (production file)\n");

	PrepareTest();
	UseDatabase("prod");
	std::filesystem::copy_file(std::string(AS_ADMIN_TIME_TEST_DATA_DIR) + "/admin_time_prod.ini",
							   AdminSystemConfigPath(),
							   std::filesystem::copy_options::overwrite_existing);
	CHECK(g_asAdminTimePlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);
	CHECK(g_config.specTime, "the missing key defaults to on");
	CHECK(!g_joinTeamListening, "no jointeam listener with the default");
	CheckEqInt(CountListeners(""), 0, "no global listener with the default");

	FinishTest();
}

} // namespace

int main() {
	std::printf("as_admin_time_tests (fake s2sdk/admin_system bindings, real SQLite through plgdb)\n");

	g_baseDir = "/tmp/as_admin_time_tests_" + std::to_string(static_cast<long long>(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(g_baseDir, error);
	std::filesystem::create_directories(g_baseDir + "/configs/admin_system", error);
	std::filesystem::create_directories(g_baseDir + "/configs/as_admin_time", error);

	InstallFakes();
	ResetWorld();

	TestConfigReader();
	TestConfigGetters();
	TestConfigMalformed();
	TestConfigCandidates();
	TestShippedConfigs();
	TestLoadConfigWalk();
	TestStartRegistrations();
	TestMapGate();
	TestMapChangeWindowStart();
	TestClientGuards();
	TestAdminConnectInsertsRow();
	TestAdminConnectWhileConnecting();
	TestPlayerDisconnectClosesRow();
	TestDisconnectInMapChangeWindow();
	TestJoinTeamTracking();
	TestLateCoreAndNoCore();
	TestResetOnCoreLoaded();
	TestNoResetWhenDisabled();
	TestMissingBindings();
	TestUnload();
	TestSpecTimeDefaultSkipsListener();

	g_asAdminTimePlugin.OnPluginEnd();
	std::filesystem::remove_all(g_baseDir, error);

	std::printf("as_admin_time_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
