// zones.hpp — the zoneinfo tables, parsed.
//
// Two files ship with tzdata and this reads both:
//
//   zone1970.tab   codes \t coordinates \t TZ \t comments    (one line per zone)
//   iso3166.tab    code  \t country name
//
// zone1970.tab's first field can hold SEVERAL comma-separated country codes for
// a zone shared across borders, which is why a zone carries a list rather than
// one code. Older tzdata ships zone.tab instead, where that field is a single
// code; both parse the same way here.
#pragma once
#include <string>
#include <vector>
#include <map>

namespace tz {

struct Zone {
  std::string name;                 // "Europe/London"
  std::string region;               // "Europe"
  std::vector<std::string> codes;   // {"GB"}
  std::string comment;              // "" or e.g. "Chatham Islands"
};

// Every zone in the table, in file order. Empty when no table could be read.
std::vector<Zone> load_zones(const std::string& zoneinfo_dir);

// ISO 3166 code -> country name. Empty when the table is missing; callers fall
// back to showing the bare code.
std::map<std::string, std::string> load_countries(const std::string& zoneinfo_dir);

// The regions present, in the order they first appear, e.g. Africa, America...
std::vector<std::string> regions_of(const std::vector<Zone>& zones);

// A human label for a zone: the part after the region, plus the country and
// any comment the table carries. "London  — United Kingdom".
std::string label_of(const Zone& z, const std::map<std::string, std::string>& cc);

}  // namespace tz
