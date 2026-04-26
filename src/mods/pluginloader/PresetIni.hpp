// Minimal INI reader/writer for the .uevrpreset file format.
//
// Format (all lines optional, missing keys keep defaults):
//
//   # comment
//   format = 1
//   name   = "Cinematic VR"
//
//   [SectionName]
//   key1 = value
//   key2 = "value with spaces"
//
// - Sections are case-sensitive.
// - Keys are case-sensitive.
// - Values are trimmed; surrounding double quotes are stripped if present.
// - Lines starting with # or ; are comments. Trailing comments are NOT supported
//   (a # inside a value is part of the value). Keep it simple and unambiguous.
// - Unknown sections / unknown keys are kept by the parser; consumers decide
//   what to do with them. Forward-compat: a plugin not present in the build
//   still has its section preserved on round-trip if we choose to (currently
//   we don't — the writer only emits registered sections).
//
// Designed to be co-located with the host-side preset I/O in PluginLoader.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uevr::preset_ini {

// Header pairs (e.g. format=1, name="...", author="..."). Order preserved.
struct Header {
    std::vector<std::pair<std::string, std::string>> entries;
};

// One parsed section's key/value pairs. Map for stable lookup; the writer
// emits in caller-supplied order, not in map order.
using Section = std::map<std::string, std::string>;

struct Document {
    Header header;
    // section name -> key/value map
    std::map<std::string, Section> sections;
};

// Parse from a string. Returns an empty Document on error (callers can detect
// via sections.empty() && header.entries.empty()). Bad lines are silently
// skipped — preset files are best-effort by design (forward-compat goal).
Document parse(const std::string& text);

// Convenience: read the file then parse. Returns empty Document if file missing.
Document parse_file(const std::filesystem::path& path);

// Serialize a Document to a string suitable for round-tripping through parse().
// `header_entries` are written first (in given order). For each (section_name,
// section_data, key_order) tuple in `sections`, the section is written with
// keys in the given order. Keys not in key_order are appended at the end in
// map order so nothing is dropped. Use empty key_order to emit in map order.
struct SectionEmit {
    std::string name;
    Section data;
    std::vector<std::string> key_order; // optional; empty = map order
};

std::string emit(const std::vector<std::pair<std::string, std::string>>& header_entries,
                 const std::vector<SectionEmit>& sections);

// Write atomically: emit -> tmp file -> rename. Returns true on success.
bool write_file(const std::filesystem::path& path,
                const std::vector<std::pair<std::string, std::string>>& header_entries,
                const std::vector<SectionEmit>& sections);

} // namespace uevr::preset_ini
