// nbui.hpp — the look shared by NextBSD's text-mode tools.
//
// Lifted out of nextbsd-installer's theme.hpp (nextbsd-userland#304), which is
// now a shim over this, and repainted from amber to slate (#308).
//
// WHY SLATE, AND WHY EVERY VALUE IS AN ANSI COLOUR
//
// The amber theme rendered exactly as designed over ssh and in Terminal.app,
// and fell apart on the vt console, which has sixteen colours. A truecolor
// value there is mapped to the nearest ANSI colour, and nothing checked what
// that mapping did to the design:
//
//     amber       #ff9416  ->  bright red
//     amberBright #ffb454  ->  bright yellow
//     amberDim    #a85e0e  ->  yellow
//     bad         #ff4f4f  ->  bright red      <- the same as the body colour
//
// So on real hardware it was not an amber theme but a red-and-yellow one, and
// an error was distinguishable from ordinary text only by its wording.
//
// The fix is not a nicer hex. It is the rule that EVERY VALUE HERE IS EXACTLY
// ONE OF THE SIXTEEN, so the mapping is the identity and a terminal and a
// console draw the same picture. That rule is enforced below by a
// static_assert rather than left as a comment, because a comment would not
// have stopped the amber palette either.
//
// Contrast against black, all well past the 4.5:1 that body text needs:
//
//     body    br-white  21.0:1   menu items, values
//     dim     white      9.0:1   hints, rules, the sub-line
//     accent  br-cyan   17.1:1   headings, masthead, selection
//     ok      br-green  15.8:1   success only
//     bad     br-red     6.7:1   errors only
//
// Apple is no guide here: it never shipped text-mode dialogs, so there is no
// Darwin precedent to match, and a website palette is not a console palette.
// This is designed in the sixteen first, with truecolor as the refinement.
//
// This is NOT a UI framework. FTXUI is the framework, vendored at ../ftxui and
// statically linked; this is a palette and three helpers, and it should stay
// that size.
#pragma once
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nbui {
using namespace ftxui;

namespace detail {

struct Rgb {
  std::uint8_t r, g, b;
};

// The sixteen, as vt and xterm render them. A value equal to one of these
// survives the console unchanged; anything else is mapped by nearest-RGB, and
// that mapping is what ruined the amber theme.
constexpr Rgb kAnsi[16] = {
    {0x00, 0x00, 0x00}, {0xaa, 0x00, 0x00}, {0x00, 0xaa, 0x00}, {0xaa, 0x55, 0x00},
    {0x00, 0x00, 0xaa}, {0xaa, 0x00, 0xaa}, {0x00, 0xaa, 0xaa}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0xff, 0x55, 0x55}, {0x55, 0xff, 0x55}, {0xff, 0xff, 0x55},
    {0x55, 0x55, 0xff}, {0xff, 0x55, 0xff}, {0x55, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

constexpr bool is_ansi(Rgb c) {
  for (const Rgb& a : kAnsi)
    if (a.r == c.r && a.g == c.g && a.b == c.b)
      return true;
  return false;
}

// Every colour this header exposes, as a triple, so the rule can be checked.
constexpr Rgb kBg     = {0x00, 0x00, 0x00};  // black
constexpr Rgb kBody   = {0xff, 0xff, 0xff};  // bright white
constexpr Rgb kDim    = {0xaa, 0xaa, 0xaa};  // white
constexpr Rgb kAccent = {0x55, 0xff, 0xff};  // bright cyan
constexpr Rgb kOk     = {0x55, 0xff, 0x55};  // bright green
constexpr Rgb kBad    = {0xff, 0x55, 0x55};  // bright red

// The whole point, enforced. Reaching for a nicer hex fails the build here
// rather than silently degrading on the one display that matters.
static_assert(is_ansi(kBg),     "nbui: background must be one of the 16 ANSI colours");
static_assert(is_ansi(kBody),   "nbui: body must be one of the 16 ANSI colours");
static_assert(is_ansi(kDim),    "nbui: dim must be one of the 16 ANSI colours");
static_assert(is_ansi(kAccent), "nbui: accent must be one of the 16 ANSI colours");
static_assert(is_ansi(kOk),     "nbui: ok must be one of the 16 ANSI colours");
static_assert(is_ansi(kBad),    "nbui: bad must be one of the 16 ANSI colours");

inline Color of(Rgb c) { return Color::RGB(c.r, c.g, c.b); }

}  // namespace detail

inline const Color bg     = detail::of(detail::kBg);
inline const Color body   = detail::of(detail::kBody);
inline const Color dim    = detail::of(detail::kDim);
inline const Color accent = detail::of(detail::kAccent);
inline const Color ok     = detail::of(detail::kOk);
inline const Color bad    = detail::of(detail::kBad);

// Headings and the selection share the accent; there is no separate "bright".
inline const Color bright = accent;

inline Element title(const std::string& s) { return text(s) | color(accent) | bold; }
inline Element hint(const std::string& s)  { return text(s) | color(dim); }

// A keycap hint bar, e.g. keys({{"↑↓","move"},{"Enter","select"}}).
inline Element keys(std::vector<std::pair<std::string, std::string>> ks) {
  Elements e;
  for (auto& k : ks) {
    if (!k.first.empty())
      e.push_back(text(" " + k.first + " ") | color(bg) | bgcolor(accent) | bold);
    e.push_back(text(" " + k.second + "   ") | color(dim));
  }
  return hbox(std::move(e));
}

// Standard chrome around a screen body: masthead + heading + body + keys.
//
// `app` is the masthead text, without padding -- "NextBSD Installer",
// "Time Zone", "Wireless". It is the only thing that differs between the tools
// using this, which is why it is a parameter and nothing else is.
inline Element chrome(const std::string& app,
                      const std::string& heading,
                      const std::string& sub,
                      Element body_,
                      std::vector<std::pair<std::string, std::string>> ks) {
  auto masthead = hbox({
      text(" " + app + " ") | color(bg) | bgcolor(accent) | bold,
      filler(),
      hint(" " + sub + " "),
  });
  return vbox({
             masthead,
             separator() | color(dim),
             title(heading),
             text(""),
             std::move(body_) | flex,
             separator() | color(dim),
             keys(std::move(ks)),
         })
         | borderRounded | color(accent) | bgcolor(bg);
}

}  // namespace nbui
