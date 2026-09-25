// theme.hpp — the installer's view of the shared amber look.
//
// The palette and the chrome live in ../../libnbui/nbui.hpp, shared with the
// other text-mode tools (nextbsd-userland#304). This is the installer's
// masthead bound to it, so every `theme::` call in the screens is unchanged.
#pragma once
#include <nbui.hpp>
#include <string>
#include <utility>
#include <vector>

namespace nbi::theme {
using namespace nbui;

// The shared chrome takes the masthead as its first argument; ours is fixed.
inline ftxui::Element chrome(const std::string& heading,
                             const std::string& sub,
                             ftxui::Element body,
                             std::vector<std::pair<std::string, std::string>> ks) {
  return nbui::chrome("NextBSD Installer", heading, sub, std::move(body),
                      std::move(ks));
}

}  // namespace nbi::theme
