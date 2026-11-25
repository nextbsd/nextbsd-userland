#include "engine.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace nbi::engine {

namespace {

// Split a line into tab-separated fields (keeps empty fields).
std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return out;
}

// $NEXTBSD_INSTALLER_LIBEXEC, else the compiled-in default.
std::string libexec_dir() {
  const char* e = std::getenv("NEXTBSD_INSTALLER_LIBEXEC");
  return (e && *e) ? std::string(e) : std::string(NBI_LIBEXEC_DEFAULT);
}

// Single-quote for /bin/sh so a device/user/hostname string can't break out.
std::string shq(const std::string& s) {
  std::string o = "'";
  for (char c : s) o += (c == '\'') ? std::string("'\\''") : std::string(1, c);
  o += "'";
  return o;
}

// The engine scripts call base tools in /sbin + /usr/sbin (sysctl, diskinfo,
// gpart, newfs, cpdup, pw). Guarantee they resolve regardless of the PATH the
// installer was launched with (a getty/launchd console context can be sparse).
const char* kBasePath = "PATH=/sbin:/usr/sbin:/bin:/usr/bin ";

} // namespace

void probe(AppState& st) {
  if (st.demo) {
    st.build_id = "continuous · 2026-06-08 00:19 UTC · 0d5f191";
    st.disks = {
        {"ada0", "238 GB", "Samsung SSD 860 EVO", false,
         {{"\"BACKUP\"", "212 GB", "ufs", ""},
          {"\"EFI\"", "260 MB", "fat32", "(ESP)"}}},
        {"ada1", "931 GB", "WDC WD10EZEX-08WN4A0", false,
         {{"\"vault\"", "900 GB", "zfs", "(zpool: tank)"},
          {"\"swap\"", "16 GB", "freebsd-swap", ""}}},
        {"da0", "15 GB", "SanDisk Ultra USB", true,
         {{"\"NEXTBSD_INST\"", "15 GB", "iso9660", ""}}},
    };
    st.existing = {false, "", ""};
    return;
  }

  // Real probe: run probe-disks.sh and parse its DISK/VOL/MEDIA/DMI records
  // (format documented in engine/probe-disks.sh).
  std::string cmd = std::string(kBasePath) +
                    shq(libexec_dir() + "/probe-disks.sh") + " 2>/dev/null";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return;
  std::string acc;
  char buf[8192];
  while (fgets(buf, sizeof buf, f)) acc += buf;
  pclose(f);

  std::istringstream in(acc);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto fld = split_tabs(line);
    if (fld.empty()) continue;
    if (fld[0] == "DISK" && fld.size() >= 5) {
      Disk d;
      d.dev = fld[1];
      d.size = fld[2];
      d.model = fld[3];
      d.is_media = (fld[4] == "1");
      st.disks.push_back(std::move(d));
    } else if (fld[0] == "VOL" && fld.size() >= 5 && !st.disks.empty()) {
      // VOL<TAB>dev<TAB>label<TAB>size<TAB>fstype<TAB>note
      Volume v;
      v.label = fld[2];
      v.size = fld[3];
      v.fstype = fld[4];
      if (fld.size() >= 6) v.note = fld[5];
      Disk* tgt = &st.disks.back();
      for (auto& d : st.disks)
        if (d.dev == fld[1]) { tgt = &d; break; }
      tgt->volumes.push_back(std::move(v));
    }
    // MEDIA/DMI records from probe-disks.sh are ignored: the booted medium is
    // flagged per-disk via is_media, and the installer no longer suggests a
    // hostname, so neither is needed here. Nor are SYSTEM and NEED, or
    // DISK's sixth field (bytes): Installer.app's.
  }
}

bool run_install(AppState& st,
                 const std::function<void(int, const std::string&)>& progress) {
  if (st.demo) {
    struct Step { int to; const char* msg; };
    const std::array<Step, 5> steps = {{
        {8,   "Partitioning target disk (GPT: ESP + freebsd-ufs)…"},
        {15,  "Creating UFS filesystem (label ROOTFS)…"},
        {88,  "Cloning base with cpdup…"},
        {95,  "Installing bootcode + populating ESP…"},
        {100, "Finalizing…"},
    }};
    int cur = 0;
    for (const auto& s : steps) {
      while (cur < s.to) {
        ++cur;
        progress(cur, s.msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(22));
      }
    }
    st.success = true;
    return true;
  }

  // Real (and --dry-run) path: exec do-install.sh and stream its PROGRESS/STATUS
  // lines to the UI. --dry-run runs the SAME script with NEXTBSD_DRYRUN=1, so the
  // engine prints every destructive command instead of executing it — the safe
  // way to exercise the on-target flow without touching a disk.
  if (st.disk_index < 0 || st.disk_index >= static_cast<int>(st.disks.size())) {
    st.fail_stage = "no target disk selected";
    return false;
  }

  // The installer no longer creates a user or sets a hostname — the installed
  // system boots with a passwordless root (from the base) and hostnamed's
  // synthesized default; the operator sets a root password / adds users /
  // changes the hostname after first boot. So the engine only needs the disk.
  std::string cmd = std::string(kBasePath);
  if (st.dry_run) cmd += "NEXTBSD_DRYRUN=1 ";
  cmd += shq(libexec_dir() + "/do-install.sh");
  cmd += " -d " + shq(st.disks[st.disk_index].dev);
  cmd += " 2>&1";

  const auto started = std::chrono::steady_clock::now();
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) {
    st.fail_stage = "failed to launch install engine";
    return false;
  }

  int pct = 0;
  std::string status;
  std::string last_raw;  // most recent non-record line — usually the real error
  char buf[8192];
  while (fgets(buf, sizeof buf, f)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    auto fld = split_tabs(line);
    if (fld.size() >= 2 && fld[0] == "PROGRESS") {
      pct = std::atoi(fld[1].c_str());
      progress(pct, status);
    } else if (fld.size() >= 2 && fld[0] == "STATUS") {
      status = fld[1];
      progress(pct, status);
    } else if (fld.size() >= 2 && fld[0] == "PHASE") {
      // prepare/copy/setup/finish, for Installer.app; STATUS says more.
    } else if (!line.empty() && line != "DONE") {
      // Anything else (2>&1 stderr from gpart/newfs/mount/cpdup/pw) — keep the
      // latest so a failure reports WHY, not just which stage it died in.
      last_raw = line;
    }
  }
  int rc = pclose(f);
  st.elapsed_s = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());

  int code = (rc == -1 || !WIFEXITED(rc)) ? -1 : WEXITSTATUS(rc);
  if (code == 0) {
    st.success = true;
    return true;
  }
  st.fail_stage = status.empty()
                      ? ("install engine exited with code " + std::to_string(code))
                      : ("failed: " + status);
  if (!last_raw.empty()) st.fail_stage += "  (" + last_raw + ")";
  return false;
}

} // namespace nbi::engine
