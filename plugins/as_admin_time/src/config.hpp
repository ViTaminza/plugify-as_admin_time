// as_admin_time — the config half of the module: where admin_time.ini is read from and the
// self-contained Valve KeyValues (KV1) reader that parses it.
//
// Self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library). The
// original module loaded the file through pisexcompat's hl2sdk KeyValues shim; the port keeps the
// file format and the getter semantics, so the production .ini files are read unchanged.
//
// 1. The file. Production keeps the Pisex file, comments included:
//
//        "Config"
//        {
//            "server_id"     "1"     // Server ID который будет в базе данных
//            "reset"         "1"     // ...
//            "spec_time"     "1"     // ...
//        }
//
//    ConfigCandidates() lists where it is looked up, in order: <configs>/admin_system/
//    admin_time.ini (the directory every admin module shares, i.e. the Pisex layout) and then
//    <configs>/as_admin_time/admin_time.ini (the plugin's own folder, PLUGIN_STANDARD §3). The
//    caller supplies the configs directory (plg::GetConfigsDir() in src/plugin.cpp); this header
//    never touches the Plugify API.
//
// 2. ParseKv1()/LoadKv1File(): a read-only Valve KeyValues (KV1) reader. The accepted grammar is
//    Valve's: quoted ("key" "value") and unquoted (key value) tokens, a key may carry a value and a
//    block, nested blocks, // and /* */ comments, \" \\ \n \r \t escapes inside quotes, and
//    #base / #include / #define lines are skipped instead of followed. Like Valve's loader, a file
//    that is a single "Config" { ... } block is returned as *that block's contents*, which is why
//    `GetInt("server_id")` reads a key inside it (the original's `KeyValues kv("Config");
//    kv.LoadFromFile(...); kv.GetInt("server_id")`).
//    A malformed file - an unterminated string, a missing '}' or a stray top-level '}' - is
//    rejected as a whole (std::nullopt), so half a config never overrides the defaults. A UTF-8 BOM
//    is ignored.
//
// 3. GetInt()/GetBool(): the semantics of pisexcompat's KeyValues, i.e. of the original's getters -
//    the value is trimmed, an empty/missing value returns the caller's default, a parse failure
//    falls back to atoi ("5 // x" -> 5, "1.5" -> 1, "abc" -> 0), and "true"/"yes"/"on" /
//    "false"/"no"/"off" are accepted for the booleans before the integer test.
//
// 4. Config: the three keys of the original with the port's defaults (server_id 1, reset on,
//    spec_time on). A missing key keeps the default, a missing or malformed file leaves the
//    caller's values untouched - exactly what the original's `if (hKv->LoadFromFile(...))` guard
//    does.

#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace as_admin_time {

// --- 1. the KV1 reader ------------------------------------------------------------------------

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	bool isBlock = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or carries no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int with the original's atoi fallback; `def` when the child is
	// missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
	// The child's boolean ("true"/"yes"/"on" -> true, "false"/"no"/"off" -> false, otherwise the
	// integer test); `def` when the child is missing or its value is empty.
	bool GetBool(std::string_view key, bool def = false) const;
};

namespace detail {

inline char LowerCase(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (LowerCase(a[i]) != LowerCase(b[i])) {
			return false;
		}
	}
	return true;
}

inline std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The original's atoi fallback for a non-numeric value ("1.5" -> 1, "abc" -> 0).
inline int Atoi(std::string_view text) {
	const std::string copy(text);
	return std::atoi(copy.c_str());
}

//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	// Children are appended as soon as they are complete, so the recursion below works on a local
	// node and never holds a reference into a vector that the recursion may reallocate.
	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			KvNode node;
			node.name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					node.value = std::move(value);
					node.hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				node.isBlock = true;
				ParseNodes(node, true);
			}
			parent.children.push_back(std::move(node));
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace detail

inline const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (detail::EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

inline std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	if (key.empty()) {
		return hasValue ? std::string_view(value) : def;
	}
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return std::string_view(found->value);
}

inline int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	int value = 0;
	const auto* begin = text.data();
	const auto* end = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if (result.ec != std::errc{}) {
		return detail::Atoi(text); // Valve falls back to atoi semantics
	}
	return value;
}

inline bool KvNode::GetBool(std::string_view key, bool def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	if (detail::EqualNoCase(text, "true") || detail::EqualNoCase(text, "yes") ||
		detail::EqualNoCase(text, "on")) {
		return true;
	}
	if (detail::EqualNoCase(text, "false") || detail::EqualNoCase(text, "no") ||
		detail::EqualNoCase(text, "off")) {
		return false;
	}
	return detail::Atoi(text) != 0;
}

// The file root: an unnamed node whose children are the top-level entries - except that, like
// Valve's loader (and pisexcompat's shim), a file that is one "Config" { ... } block is returned as
// the *contents* of that block, so `GetInt("server_id")` reads a key inside it.
inline std::optional<KvNode> ParseKv1(std::string_view text) {
	// A UTF-8 BOM would otherwise become part of the first key and hide every entry from lookups.
	constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
	if (text.starts_with(kUtf8Bom)) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	detail::Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// The single top-level block becomes the root (its name is only used for logging in Valve).
	if (root.children.size() == 1 && root.children.front().isBlock) {
		KvNode top = std::move(root.children.front());
		root.name = std::move(top.name);
		root.value = std::move(top.value);
		root.hasValue = top.hasValue;
		root.children = std::move(top.children);
	}
	return root;
}

inline std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

// --- 2. where the config is read from ----------------------------------------------------------

// The admin_system directory every Pisex admin module shares; production admin_time.ini lives
// there.
inline constexpr std::string_view kAdminSystemDir = "admin_system";
// The plugin's own folder (PLUGIN_STANDARD §3: the folder name matches the manifest name).
inline constexpr std::string_view kPluginDir = "as_admin_time";
// The file name of the original module.
inline constexpr std::string_view kConfigFile = "admin_time.ini";

inline std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

// <configs>/admin_system/admin_time.ini, then <configs>/as_admin_time/admin_time.ini. Empty when the
// host reported no configs directory (plg::GetConfigsDir is filled by the plugify module at load).
inline std::vector<std::string> ConfigCandidates(std::string_view configsDir) {
	std::vector<std::string> candidates;
	if (configsDir.empty()) {
		return candidates;
	}
	candidates.reserve(2);
	candidates.push_back(JoinPath(JoinPath(configsDir, kAdminSystemDir), kConfigFile));
	candidates.push_back(JoinPath(JoinPath(configsDir, kPluginDir), kConfigFile));
	return candidates;
}

inline bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

// --- 3. the config -----------------------------------------------------------------------------

// The keys of the original with the port's built-in defaults: the original left g_iServerID at 0
// when the key was missing, the port keeps 1 (the value both production files carry), so a config
// that loses a key never writes rows for "server 0".
struct Config {
	int serverId = 1;
	bool reset = true;
	bool specTime = true;
};

// LoadConfig() of the original over an already parsed file: missing keys keep the values already in
// `out`, exactly like the original's `GetInt(key, default)` calls on a KeyValues that has them.
inline void LoadConfigFromRoot(const KvNode& root, Config& out) {
	Config loaded = out;
	loaded.serverId = root.GetInt("server_id", loaded.serverId);
	loaded.reset = root.GetBool("reset", loaded.reset);
	loaded.specTime = root.GetBool("spec_time", loaded.specTime);
	out = loaded;
}

// Parse `text` and replace `out` with its keys. False (and `out` untouched) when the text is
// malformed - the caller then keeps its defaults or tries the next candidate file.
inline bool LoadConfigFromText(std::string_view text, Config& out) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return false;
	}
	LoadConfigFromRoot(*root, out);
	return true;
}

// Same over a file; false when the file is missing or malformed.
inline bool LoadConfigFromFile(const std::string& path, Config& out) {
	const std::optional<KvNode> root = LoadKv1File(path);
	if (!root) {
		return false;
	}
	LoadConfigFromRoot(*root, out);
	return true;
}

} // namespace as_admin_time
