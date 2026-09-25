// apply.hpp — what setting a time zone actually does, and the NTP job.
//
// Separated from the UI so the non-interactive path (`tzsetup Europe/London`)
// and the tests exercise exactly the code the dialog runs. Every path is a
// parameter rather than a constant for the same reason: the tests point the
// whole lot at a staged directory and assert the files, without touching /etc
// and without privilege.
#pragma once
#include <string>

namespace tz {

struct Paths {
  std::string zoneinfo   = "/usr/share/zoneinfo";
  std::string localtime  = "/etc/localtime";
  std::string db         = "/var/db/zoneinfo";
  std::string launchctl  = "/bin/launchctl";
  std::string ntp_plist  = "/System/Library/LaunchDaemons/org.nextbsd.ntpd.plist";
};

// The zone recorded in /var/db/zoneinfo, or "" when none is.
std::string current_zone(const Paths& p);

// Copy zoneinfo/<zone> over localtime and record the name in db.
// Returns "" on success, else a message naming what failed.
std::string set_zone(const Paths& p, const std::string& zone);

// Is org.nextbsd.ntpd loaded right now?
bool ntp_loaded(const Paths& p);

// launchctl load -w / unload -w on the NTP job. "" on success.
std::string set_ntp(const Paths& p, bool on);

}  // namespace tz
