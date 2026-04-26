#include "PresetIni.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace uevr::preset_ini {

namespace {

std::string_view trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// Sanitize a value for emission: strip the small set of characters that
// would actually break round-trip through parse() (control chars terminate
// the line; `"` would re-trigger quote-stripping on read; leading/trailing
// whitespace is eaten by trim()). Everything else — spaces, `=`, `#`, `;`
// mid-value — passes through as-is. No quoting, no escaping.
//
// User-writable strings (preset display name) are already filtered by the
// UI's `sanitize_name`; this is the last-line boundary guard for everything
// the plugins emit.
std::string sanitize_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        if (c < 0x20) continue;     // \n \r \t and other controls
        if (c == '"') continue;     // never accept quotes in values
        out.push_back(static_cast<char>(c));
    }
    // Drop leading/trailing whitespace — trim() on the parse side eats them
    // anyway, so emitting them would silently change the value on round-trip.
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    size_t start = 0;
    while (start < out.size() && (out[start] == ' ' || out[start] == '\t')) ++start;
    return start == 0 ? out : out.substr(start);
}

} // namespace

Document parse(const std::string& text) {
    Document doc;
    std::string current_section; // empty = header
    bool header_done = false;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        auto sv = trim(line);
        if (sv.empty()) continue;
        if (sv.front() == '#' || sv.front() == ';') continue;

        if (sv.front() == '[' && sv.back() == ']') {
            current_section = std::string{trim(sv.substr(1, sv.size() - 2))};
            header_done = true;
            // Touch the section so it exists even if empty.
            doc.sections.try_emplace(current_section);
            continue;
        }

        auto eq = sv.find('=');
        if (eq == std::string_view::npos) continue;

        std::string key{trim(sv.substr(0, eq))};
        std::string value{trim(sv.substr(eq + 1))};
        if (key.empty()) continue;

        if (!header_done && current_section.empty()) {
            doc.header.entries.emplace_back(std::move(key), std::move(value));
        } else {
            doc.sections[current_section][std::move(key)] = std::move(value);
        }
    }
    return doc;
}

Document parse_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

std::string emit(const std::vector<std::pair<std::string, std::string>>& header_entries,
                 const std::vector<SectionEmit>& sections) {
    std::ostringstream out;
    out << "# UEVR Shader Preset\n";
    for (const auto& [k, v] : header_entries) {
        out << k << " = " << sanitize_value(v) << "\n";
    }

    for (const auto& s : sections) {
        out << "\n[" << s.name << "]\n";

        // Track which keys we've already emitted so the trailing map-order
        // sweep doesn't duplicate them.
        std::map<std::string, bool> emitted;
        for (const auto& k : s.key_order) {
            auto it = s.data.find(k);
            if (it == s.data.end()) continue;
            out << k << " = " << sanitize_value(it->second) << "\n";
            emitted[k] = true;
        }
        for (const auto& [k, v] : s.data) {
            if (emitted.count(k)) continue;
            out << k << " = " << sanitize_value(v) << "\n";
        }
    }
    return out.str();
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::pair<std::string, std::string>>& header_entries,
                const std::vector<SectionEmit>& sections) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << emit(header_entries, sections);
            if (!f.good()) return false;
        }
        std::error_code ec;
        // rename over an existing target on Windows requires remove first;
        // use filesystem::rename which performs replace on POSIX and
        // ReplaceFile semantics via overload on MSVC's filesystem.
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            // Fallback: copy + remove
            std::filesystem::copy_file(tmp, path,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
            if (ec) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace uevr::preset_ini
