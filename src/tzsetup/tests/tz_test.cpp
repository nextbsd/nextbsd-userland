// A harness for the halves of tzsetup that do not need a terminal: the table
// parser and the apply layer. The dialog is FTXUI and is not exercised here;
// what is exercised is every line the dialog calls once someone presses Enter,
// which is where the damage would be.
#include <cstdio>
#include <cstring>
#include <string>

#include "apply.hpp"
#include "zones.hpp"

static int fails = 0, checks = 0;

static void ck(const char* what, bool cond, const std::string& got = "") {
  checks++;
  if (!cond) {
    fails++;
    printf("FAIL %-52s %s\n", what, got.c_str());
  }
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    fprintf(stderr, "usage: tz_test <zoneinfo-dir> <staged-root>\n");
    return 2;
  }
  const std::string zi = argv[1], root = argv[2];

  tz::Paths p;
  p.zoneinfo  = zi;
  p.localtime = root + "/localtime";
  p.db        = root + "/zoneinfo";
  p.launchctl = root + "/launchctl";
  p.ntp_plist = root + "/org.nextbsd.ntpd.plist";

  // ---- the tables
  auto zones = tz::load_zones(zi);
  ck("the zone table parses", !zones.empty(),
     "got " + std::to_string(zones.size()) + " zones");

  bool found_london = false, region_ok = false;
  for (const auto& z : zones)
    if (z.name == "Europe/London") {
      found_london = true;
      region_ok = z.region == "Europe";
    }
  ck("Europe/London is in it", found_london);
  ck("and its region is split off", region_ok);

  auto regions = tz::regions_of(zones);
  ck("regions are found", regions.size() > 3,
     std::to_string(regions.size()) + " regions");
  bool dup = false;
  for (size_t i = 0; i < regions.size(); i++)
    for (size_t j = i + 1; j < regions.size(); j++)
      if (regions[i] == regions[j]) dup = true;
  ck("with no duplicates", !dup);

  auto cc = tz::load_countries(zi);
  ck("the country table parses", !cc.empty(),
     std::to_string(cc.size()) + " entries");

  // A label should read as a place, not a path.
  for (const auto& z : zones)
    if (z.name == "Europe/London") {
      std::string l = tz::label_of(z, cc);
      ck("a label drops the region", l.find("Europe/") == std::string::npos, l);
      // Assert the SHAPE, not a country name: tzdata spells the United
      // Kingdom "Britain (UK)" on some hosts and "United Kingdom" on others,
      // and a test that pins one of those tests the host's tzdata, not this
      // code. What matters is that a place got appended after the separator.
      std::string::size_type dash = l.find("\u2014");
      ck("and names somewhere after a dash",
         dash != std::string::npos && l.size() > dash + 4, l);
    }
  for (const auto& z : zones)
    if (z.name == "America/New_York") {
      std::string l = tz::label_of(z, cc);
      ck("underscores become spaces", l.find("New York") != std::string::npos, l);
    }

  // ---- setting a zone
  ck("nothing is set to begin with", tz::current_zone(p).empty());

  std::string e = tz::set_zone(p, "Europe/London");
  ck("setting a zone succeeds", e.empty(), e);
  ck("and it is recorded", tz::current_zone(p) == "Europe/London",
     tz::current_zone(p));

  // localtime must be a COPY, not a link: a symlink into /usr/share breaks a
  // single-user boot where only / is mounted.
  FILE* f = fopen(p.localtime.c_str(), "rb");
  ck("localtime exists", f != nullptr);
  if (f) {
    char magic[4] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    ck("and holds a tzfile", n == 4 && memcmp(magic, "TZif", 4) == 0,
       std::string(magic, n));
  }

  // ---- the refusals that matter, because a zone name indexes the filesystem
  //
  // Point zoneinfo at a staged directory with a real file NEXT to it, so a
  // climbing name resolves to something that exists. Against the host's
  // /usr/share/zoneinfo a name like "../../etc/passwd" does not resolve, so
  // the stat below would reject it and the test would pass whether or not the
  // guard existed -- which it did, until this was written.
  {
    tz::Paths t2 = p;
    t2.zoneinfo = root + "/zi";
    FILE* s = fopen((root + "/secret").c_str(), "w");
    if (s) { fputs("NOT-A-TZFILE", s); fclose(s); }
    e = tz::set_zone(t2, "../secret");
    ck("a climbing name is refused even when it resolves", !e.empty(), e);

    // And prove it by content: localtime must still be the tzfile.
    FILE* lt = fopen(t2.localtime.c_str(), "rb");
    char buf[16] = {0};
    size_t n = lt ? fread(buf, 1, 12, lt) : 0;
    if (lt) fclose(lt);
    ck("and localtime was not overwritten",
       n >= 4 && memcmp(buf, "TZif", 4) == 0, std::string(buf, n));
  }

  e = tz::set_zone(p, "../../etc/passwd");
  ck("a climbing path is refused", !e.empty(), e);
  e = tz::set_zone(p, "/etc/passwd");
  ck("an absolute path is refused", !e.empty(), e);
  e = tz::set_zone(p, "");
  ck("an empty name is refused", !e.empty(), e);
  e = tz::set_zone(p, "Nowhere/Nothing");
  ck("an unknown zone is refused", !e.empty(), e);
  ck("and none of those changed it", tz::current_zone(p) == "Europe/London",
     tz::current_zone(p));

  // ---- the NTP job
  e = tz::set_ntp(p, true);
  ck("no plist means no NTP change", !e.empty(), e);

  FILE* pl = fopen(p.ntp_plist.c_str(), "w");
  if (pl) fclose(pl);
  e = tz::set_ntp(p, true);
  ck("with a plist and a launchctl it loads", e.empty(), e);

  printf("\n%d checks, %d failures\n", checks, fails);
  if (fails == 0) {
    printf("TZ-OK: the tables, setting a zone, the refusals and the NTP job\n");
    return 0;
  }
  printf("TZ-FAIL\n");
  return 1;
}
