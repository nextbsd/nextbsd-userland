#include "apply.hpp"

#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <vector>

namespace tz {
namespace {

std::string err(const std::string& what) {
  return what + ": " + std::strerror(errno);
}

// Run a program and report whether it exited 0. No shell: the zone name comes
// from a table but the launchctl path and label are ours, and a fork/exec keeps
// it that way regardless.
bool run(const std::string& prog, const std::vector<std::string>& args) {
  pid_t pid = fork();
  if (pid == -1)
    return false;
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(prog.c_str()));
    for (const auto& a : args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(prog.c_str(), argv.data());
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) == -1)
    return false;
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

}  // namespace

std::string current_zone(const Paths& p) {
  std::ifstream f(p.db);
  std::string z;
  if (f && std::getline(f, z)) {
    while (!z.empty() && (z.back() == '\n' || z.back() == '\r' || z.back() == ' '))
      z.pop_back();
    return z;
  }
  return "";
}

std::string set_zone(const Paths& p, const std::string& zone) {
  // A zone name indexes the filesystem, so refuse anything that could climb
  // out of the zoneinfo directory before it is ever concatenated.
  if (zone.empty() || zone.front() == '/' ||
      zone.find("..") != std::string::npos)
    return "not a usable zone name: " + zone;

  const std::string src = p.zoneinfo + "/" + zone;
  struct stat st;
  if (stat(src.c_str(), &st) == -1 || !S_ISREG(st.st_mode))
    return "no such time zone: " + zone;

  // Copy rather than symlink. FreeBSD's tzsetup copies, and a symlink into
  // /usr/share breaks a single-user boot where only / is mounted.
  std::ifstream in(src, std::ios::binary);
  if (!in)
    return err("reading " + src);
  const std::string tmp = p.localtime + ".tz.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return err("writing " + tmp);
    out << in.rdbuf();
    if (!out)
      return err("writing " + tmp);
  }
  if (rename(tmp.c_str(), p.localtime.c_str()) == -1) {
    unlink(tmp.c_str());
    return err("replacing " + p.localtime);
  }

  // The name, so anything asking "which zone is this" gets an answer without
  // comparing file contents.
  std::ofstream db(p.db, std::ios::trunc);
  if (!db)
    return err("writing " + p.db);
  db << zone << "\n";
  if (!db)
    return err("writing " + p.db);
  return "";
}

bool ntp_loaded(const Paths& p) {
  return run(p.launchctl, {"list", "org.nextbsd.ntpd"});
}

std::string set_ntp(const Paths& p, bool on) {
  struct stat st;
  if (stat(p.ntp_plist.c_str(), &st) == -1)
    return "org.nextbsd.ntpd is not installed";
  if (!run(p.launchctl, {on ? "load" : "unload", "-w", p.ntp_plist}))
    return std::string("launchctl ") + (on ? "load" : "unload") +
           " -w did not succeed";
  return "";
}

}  // namespace tz
