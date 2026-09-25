#include "zones.hpp"

#include <fstream>
#include <sstream>

namespace tz {
namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream is(s);
  while (std::getline(is, cur, sep))
    out.push_back(cur);
  return out;
}

// The first of the candidates that opens. Newer tzdata ships zone1970.tab and
// keeps zone.tab for compatibility; older ships only zone.tab.
std::ifstream open_first(const std::string& dir,
                         const std::vector<std::string>& names) {
  for (const auto& n : names) {
    std::ifstream f(dir + "/" + n);
    if (f)
      return f;
  }
  return std::ifstream();
}

}  // namespace

std::vector<Zone> load_zones(const std::string& dir) {
  std::vector<Zone> out;
  std::ifstream f = open_first(dir, {"zone1970.tab", "zone.tab"});
  std::string line;

  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::vector<std::string> col = split(line, '\t');
    if (col.size() < 3 || col[2].empty())
      continue;
    Zone z;
    z.codes = split(col[0], ',');
    z.name = col[2];
    z.comment = col.size() > 3 ? col[3] : "";
    std::string::size_type slash = z.name.find('/');
    z.region = slash == std::string::npos ? z.name : z.name.substr(0, slash);
    out.push_back(std::move(z));
  }
  return out;
}

std::map<std::string, std::string> load_countries(const std::string& dir) {
  std::map<std::string, std::string> out;
  std::ifstream f = open_first(dir, {"iso3166.tab"});
  std::string line;

  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::string::size_type tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    out[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return out;
}

std::vector<std::string> regions_of(const std::vector<Zone>& zones) {
  std::vector<std::string> out;
  for (const auto& z : zones) {
    bool seen = false;
    for (const auto& r : out)
      if (r == z.region) { seen = true; break; }
    if (!seen)
      out.push_back(z.region);
  }
  return out;
}

std::string label_of(const Zone& z, const std::map<std::string, std::string>& cc) {
  std::string::size_type slash = z.name.find('/');
  std::string label = slash == std::string::npos ? z.name : z.name.substr(slash + 1);
  for (auto& c : label)
    if (c == '_')
      c = ' ';

  std::string where;
  if (!z.codes.empty()) {
    auto it = cc.find(z.codes[0]);
    where = it != cc.end() ? it->second : z.codes[0];
  }
  if (!z.comment.empty())
    where = where.empty() ? z.comment : where + ", " + z.comment;
  return where.empty() ? label : label + "  — " + where;
}

}  // namespace tz
