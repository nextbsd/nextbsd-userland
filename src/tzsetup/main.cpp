// tzsetup — pick a time zone, in the installer's look, and offer to keep the
// clock synchronised in the same breath (nextbsd-userland#302, E18 U18).
//
// Replaces FreeBSD's bsddialog tzsetup at the same path, so the muscle memory
// and the greeter's task list both stay right; nextbsd-freebsd-compat strips
// the base copy the way it does for passwd and adduser.
//
// WHY THE NTP CHECKBOX IS HERE
//
// Setting a time zone and keeping the clock right are one thought. The NTP job
// ships Disabled, so without this it is off on every fresh install and nothing
// says so -- and the alternative is telling someone to go and run
// `launchctl load -w org.nextbsd.ntpd`, which is a worse answer than a
// checkbox. Darwin agrees, incidentally: its systemsetup carries -settimezone
// and -setusingnetworktime side by side.
//
// Three screens: region, zone, confirm. The apply half is in apply.cpp so the
// non-interactive path runs exactly the code the dialog does.

#include <nbui.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apply.hpp"
#include "zones.hpp"

using namespace ftxui;

namespace {

const char kApp[] = "Time Zone";

struct State {
  std::vector<tz::Zone> zones;
  std::map<std::string, std::string> countries;
  std::vector<std::string> regions;
  int region = 0;
  int zone = 0;
  bool ntp = true;
  std::vector<tz::Zone> in_region;   // the current region's zones
  std::string current;               // the zone already set, if any
};

void refill(State& st) {
  st.in_region.clear();
  if (st.regions.empty())
    return;
  const std::string& r = st.regions[(size_t)st.region];
  for (const auto& z : st.zones)
    if (z.region == r)
      st.in_region.push_back(z);
  if (st.zone >= (int)st.in_region.size())
    st.zone = 0;
}

std::string usage() {
  return "usage: tzsetup [-n] [zone]\n"
         "       tzsetup -l\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  tz::Paths paths;
  bool dry = false, list = false;
  int ch;

  while ((ch = getopt(argc, argv, "lna:")) != -1) {
    switch (ch) {
      case 'l': list = true; break;
      case 'n': dry = true; break;
      case 'a': paths.zoneinfo = optarg; break;   // for the tests
      default: fputs(usage().c_str(), stderr); return 2;
    }
  }
  argc -= optind;
  argv += optind;

  State st;
  st.zones = tz::load_zones(paths.zoneinfo);
  if (st.zones.empty()) {
    fprintf(stderr, "tzsetup: no zone table under %s\n", paths.zoneinfo.c_str());
    return 1;
  }
  st.countries = tz::load_countries(paths.zoneinfo);
  st.regions = tz::regions_of(st.zones);
  st.current = tz::current_zone(paths);

  if (list) {
    for (const auto& z : st.zones)
      printf("%s\n", z.name.c_str());
    return 0;
  }

  // Non-interactive: a zone on the command line sets it and says nothing. This
  // is what scripts and the installer use, and it needs no terminal.
  if (argc == 1) {
    if (dry) {
      printf("would set %s\n", argv[0]);
      return 0;
    }
    std::string e = tz::set_zone(paths, argv[0]);
    if (!e.empty()) {
      fprintf(stderr, "tzsetup: %s\n", e.c_str());
      return 1;
    }
    return 0;
  }
  if (argc > 1) {
    fputs(usage().c_str(), stderr);
    return 2;
  }
  if (!isatty(STDIN_FILENO)) {
    fprintf(stderr, "tzsetup: not a terminal; give a zone name instead\n");
    return 1;
  }

  // Start on the region the machine is already in, so confirming does nothing
  // surprising.
  if (!st.current.empty()) {
    for (size_t i = 0; i < st.regions.size(); i++) {
      std::string::size_type slash = st.current.find('/');
      if (slash != std::string::npos && st.regions[i] == st.current.substr(0, slash)) {
        st.region = (int)i;
        break;
      }
    }
  }
  refill(st);
  if (!st.current.empty())
    for (size_t i = 0; i < st.in_region.size(); i++)
      if (st.in_region[i].name == st.current)
        st.zone = (int)i;

  st.ntp = tz::ntp_loaded(paths);

  auto screen = ScreenInteractive::Fullscreen();
  int step = 0;                 // 0 region, 1 zone, 2 confirm
  std::string error;

  auto region_menu = Menu(&st.regions, &st.region);
  std::vector<std::string> zone_labels;
  auto zone_menu = Menu(&zone_labels, &st.zone);
  auto ntp_box = Checkbox("Keep the clock synchronised over the network", &st.ntp);

  auto relabel = [&] {
    refill(st);
    zone_labels.clear();
    for (const auto& z : st.in_region)
      zone_labels.push_back(tz::label_of(z, st.countries));
  };
  relabel();

  auto container = Container::Tab({region_menu, zone_menu, ntp_box}, &step);

  auto app = CatchEvent(container, [&](Event e) {
    if (e == Event::Escape || e == Event::Character('q')) {
      if (step == 0) { screen.Exit(); return true; }
      step--;
      return true;
    }
    if (e == Event::Return) {
      if (step == 0) { relabel(); step = 1; return true; }
      if (step == 1) { step = 2; return true; }
      // Confirm.
      const tz::Zone& z = st.in_region[(size_t)st.zone];
      error = tz::set_zone(paths, z.name);
      if (error.empty() && st.ntp != tz::ntp_loaded(paths))
        error = tz::set_ntp(paths, st.ntp);
      screen.Exit();
      return true;
    }
    return false;
  });

  auto renderer = Renderer(app, [&] {
    if (step == 0)
      return nbui::chrome(kApp, "Region",
                          st.current.empty() ? "no time zone set yet" : st.current,
                          region_menu->Render() | vscroll_indicator | frame,
                          {{"↑↓", "move"}, {"Enter", "choose"}, {"q", "quit"}});
    if (step == 1)
      return nbui::chrome(kApp, st.regions[(size_t)st.region],
                          "step 2 of 3",
                          zone_menu->Render() | vscroll_indicator | frame,
                          {{"↑↓", "move"}, {"Enter", "choose"}, {"Esc", "back"}});
    const tz::Zone& z = st.in_region[(size_t)st.zone];
    return nbui::chrome(kApp, "Confirm", "step 3 of 3",
                        vbox({
                            hbox({nbui::hint("time zone   "),
                                  text(z.name) | color(nbui::accent) | bold}),
                            text(""),
                            ntp_box->Render(),
                            text(""),
                            nbui::hint("The clock is set from the network by"),
                            nbui::hint("org.nextbsd.ntpd, which ships turned off."),
                        }),
                        {{"Space", "toggle"}, {"Enter", "apply"}, {"Esc", "back"}});
  });

  screen.Loop(renderer);

  if (!error.empty()) {
    fprintf(stderr, "tzsetup: %s\n", error.c_str());
    return 1;
  }
  return 0;
}
