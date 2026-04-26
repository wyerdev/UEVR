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

std::string strip_quotes(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return std::string{s.substr(1, s.size() - 2)};
    }
    return std::string{s};
}

// Quote if value contains whitespace, '#', ';', or '=' so it round-trips
// through parse() unambiguously. Otherwise emit raw for diff-friendliness.
std::string quote_if_needed(const std::string& v) {
    bool needs = v.empty();
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '#' || c == ';' || c == '=' || c == '"') {
            needs = true;
            break;
        }
    }
    if (!needs) return v;
    // Escape embedded quotes by doubling them; parse() does NOT currently
    // unescape, but values containing literal quotes are not expected in
    // shader settings so this is defense-in-depth, not a feature.
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
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
        std::string value = strip_quotes(trim(sv.substr(eq + 1)));
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
        out << k << " = " << quote_if_needed(v) << "\n";
    }

    for (const auto& s : sections) {
        out << "\n[" << s.name << "]\n";

        // Track which keys we've already emitted so the trailing map-order
        // sweep doesn't duplicate them.
        std::map<std::string, bool> emitted;
        for (const auto& k : s.key_order) {
            auto it = s.data.find(k);
            if (it == s.data.end()) continue;
            out << k << " = " << quote_if_needed(it->second) << "\n";
            emitted[k] = true;
        }
        for (const auto& [k, v] : s.data) {
            if (emitted.count(k)) continue;
            out << k << " = " << quote_if_needed(v) << "\n";
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
