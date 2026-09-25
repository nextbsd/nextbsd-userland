// nbui.hpp — the amber-CRT look, shared by NextBSD's text-mode tools.
//
// Lifted out of nextbsd-installer's src/theme.hpp (nextbsd-userland#304), which
// is now a three-line shim over this. It was going to have three more consumers
// -- tzsetup (#302), wlansetup (#303) and the locale tool (nextbsd/nextbsd#507)
// -- and three private copies of an amber palette drift. A NextBSD whose four
// text-mode tools are four slightly different ambers would be worse than one
// that never tried to match.
//
// The only thing that varies per tool is the masthead, so that is the one
// parameter `chrome()` gained. Everything else is byte-for-byte what the
// installer had.
//
// This is NOT a UI framework. FTXUI is the framework, vendored at ../ftxui and
// statically linked; this is a palette and three helpers, and it should stay
// that size.
//
// Truecolor amber-on-black. The whole screen is painted amber/black at the
// outer edge; bright amber, green and red are the only accents. FTXUI's default
// menu/button focus is inverted, so on this palette a selection bar renders as
// black-on-amber for free.
#pragma once
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <string>
#include <utility>
#include <vector>

namespace nbui {
using namespace ftxui;

inline const Color amber       = Color::RGB(0xff, 0x94, 0x16);
inline const Color amberBright = Color::RGB(0xff, 0xb4, 0x54);
inline const Color amberDim    = Color::RGB(0xa8, 0x5e, 0x0e);
inline const Color bg          = Color::RGB(0x00, 0x00, 0x00);
inline const Color ok          = Color::RGB(0x39, 0xd3, 0x53);
inline const Color bad         = Color::RGB(0xff, 0x4f, 0x4f);

inline Element title(const std::string& s) { return text(s) | color(amberBright) | bold; }
inline Element hint(const std::string& s)  { return text(s) | color(amberDim); }

// A keycap hint bar, e.g. keys({{"↑↓","move"},{"Enter","select"}}).
inline Element keys(std::vector<std::pair<std::string, std::string>> ks) {
  Elements e;
  for (auto& k : ks) {
    if (!k.first.empty())
      e.push_back(text(" " + k.first + " ") | color(bg) | bgcolor(amberBright) | bold);
    e.push_back(text(" " + k.second + "   ") | color(amberDim));
  }
  return hbox(std::move(e));
}

// Standard amber chrome around a screen body: masthead + heading + body + keys.
//
// `app` is the masthead text, without padding -- "NextBSD Installer",
// "Time Zone", "Wireless". It is the only thing that differs between the tools
// using this, which is why it is a parameter and nothing else is.
inline Element chrome(const std::string& app,
                      const std::string& heading,
                      const std::string& sub,
                      Element body,
                      std::vector<std::pair<std::string, std::string>> ks) {
  auto masthead = hbox({
      text(" " + app + " ") | color(bg) | bgcolor(amber) | bold,
      filler(),
      hint(" " + sub + " "),
  });
  return vbox({
             masthead,
             separator() | color(amberDim),
             title(heading),
             text(""),
             std::move(body) | flex,
             separator() | color(amberDim),
             keys(std::move(ks)),
         })
         | borderRounded | color(amber) | bgcolor(bg);
}

}  // namespace nbui
