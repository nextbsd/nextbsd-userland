// Phase 5 — Finish. Success summary + the two terminal actions (Reboot /
// Shutdown) plus a Shell escape hatch. On failure, shows the failing stage.
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <string>

#include "screens.hpp"
#include "theme.hpp"

using namespace ftxui;

namespace nbi {

Screen run_finish(ScreenInteractive& screen, AppState& st) {
  Screen next = Screen::Reboot;

  auto reboot = Button("Reboot", [&] { next = Screen::Reboot; screen.Exit(); });
  auto shut = Button("Shut Down", [&] { next = Screen::Shutdown; screen.Exit(); });
  auto layout = Container::Horizontal({reboot, shut});

  const std::string disk = st.disks.empty() ? "?" : st.disks[st.disk_index].dev;
  // How long the install took (nextbsd#426): "52 s", "4 min 12 s".
  std::string took;
  if (st.elapsed_s >= 0) {
    const int m = st.elapsed_s / 60, s = st.elapsed_s % 60;
    took = m > 0 ? std::to_string(m) + " min " + std::to_string(s) + " s"
                 : std::to_string(s) + " s";
  }

  auto renderer = Renderer(layout, [&] {
    Element head =
        st.success
            ? (text("✓  NextBSD installed successfully.") | color(theme::ok) | bold)
            : (text("✗  Installation failed: " + st.fail_stage) | color(theme::bad) | bold);
    auto row = [](const std::string& k, const std::string& v) {
      return hbox({text("  " + k) | color(theme::amberDim) | size(WIDTH, EQUAL, 14),
                   text(v) | color(theme::amberBright)});
    };
    auto body = vbox({
        head,
        text(""),
        row("Build", st.build_id),
        row("Disk", disk),
        row("Time", took.empty() ? "—" : took),
        text(""),
        theme::hint("Remove the install medium before rebooting."),
        theme::hint("Log in as root (no password), then set a password, add"),
        theme::hint("users, or change the hostname."),
        filler(),
        hbox({reboot->Render(), text("  "), shut->Render()}),
    });
    return theme::chrome("Installation Complete", st.build_id, body,
                         {{"Tab", "move"}, {"Enter", "select"}});
  });

  screen.Loop(renderer);
  return next;
}

} // namespace nbi
