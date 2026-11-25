// app.hpp — installer state shared across the wizard screens.
#pragma once
#include <string>
#include <vector>

namespace nbi {

enum class Screen {
  Mode, Disk, Install, Finish,            // wizard steps
  Quit, Reboot, Shutdown, Shell,          // terminal actions
};

struct Volume {
  std::string label, size, fstype, note;
};

struct Disk {
  std::string dev;        // ada0 / nvd0 / vtbd0 — the unambiguous handle
  std::string size;       // human readable
  std::string model;
  bool is_media = false;  // the live install medium — shown but not selectable
  std::vector<Volume> volumes;
};

struct ExistingRoot {
  bool found = false;
  std::string dev;        // e.g. ada0p2
  std::string version;    // e.g. "NextBSD 2026-05-20"
};

struct AppState {
  bool demo = false;      // synthetic probe data, simulated install (no hardware)
  bool dry_run = false;   // engine prints actions instead of executing them

  std::string build_id = "continuous";

  std::vector<Disk> disks;
  ExistingRoot existing;

  // user choices
  int  disk_index = 0;   // selected install target (index into disks)

  // result
  bool success = false;
  std::string fail_stage;
  int elapsed_s = -1;     // how long the install ran, shown when it ends (nextbsd#426)
};

} // namespace nbi
