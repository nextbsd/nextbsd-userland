/*
 * iokit_watch.h — follow 802.11 radios through the IORegistry.
 *
 * net80211 (Wlan.kext) publishes each radio as an IO80211Controller entry
 * when it attaches and retires it when it detaches (nextbsd-userland#502).
 * wland matches that class with IOServiceAddMatchingNotification, first-match
 * and terminated, the way Darwin's wifid follows IO80211Controller, instead of
 * polling net.wlan.devices.
 */
#ifndef _WLAND_IOKIT_WATCH_H_
#define _WLAND_IOKIT_WATCH_H_

#include <stdbool.h>

/*
 * Register both notifications and drain their initial iterators, so a radio
 * that already exists counts as a first match. Callbacks run on a serial
 * dispatch queue; each records what happened and writes a byte to `wake_fd`
 * (the write end of the main loop's self-pipe).
 *
 * Returns 0 when armed. Returns -1 when /dev/ioregistry is absent (a kernel
 * without the in-kernel registry) or a registration failed; the caller then
 * falls back to rescanning net.wlan.devices.
 */
int	iokit_watch_start(int wake_fd);

/*
 * Take and clear the events recorded since the last call: `*matched` if an
 * IO80211Controller appeared, `*terminated` if one went away. Called from the
 * main loop after the self-pipe wakes it.
 */
void	iokit_watch_take(bool *matched, bool *terminated);

#endif /* _WLAND_IOKIT_WATCH_H_ */
