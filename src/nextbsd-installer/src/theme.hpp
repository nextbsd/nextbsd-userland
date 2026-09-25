// theme.hpp — the installer's view of the shared look.
//
// The palette and the chrome live in ../../libnbui/nbui.hpp, shared with the
// other text-mode tools (nextbsd-userland#304), and repainted from amber to
// slate (#308) because amber did not survive the sixteen-colour console.
//
// The amber* names are kept as aliases so the screens did not all have to
// change in the same commit as the palette. They are transitional: new code
// should say accent, dim and body.
#pragma once
#include <nbui.hpp>
#include <string>
#include <utility>
#include <vector>

namespace nbi::theme {
using namespace nbui;

inline const ftxui::Color& amber       = nbui::accent;
inline const ftxui::Color& amberBright = nbui::accent;
inline const ftxui::Color& amberDim    = nbui::dim;

// The shared chrome takes the masthead as its first argument; ours is fixed.
inline ftxui::Element chrome(const std::string& heading,
                             const std::string& sub,
                             ftxui::Element body,
                             std::vector<std::pair<std::string, std::string>> ks) {
  return nbui::chrome("NextBSD Installer", heading, sub, std::move(body),
                      std::move(ks));
}

}  // namespace nbi::theme
