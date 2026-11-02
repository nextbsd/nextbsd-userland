/*
 * discovery.h — each controller's discovery cache (#208).
 *
 * Scan results cannot cross MIG as a plist (out-of-line data is broken) nor
 * sit in the dynamic store (CONFIG_DATA_MAX is 8 KiB), so bluetoothd keeps
 * them here and bt_scan_entry hands them out one per RPC, as wland does.
 *
 * An entry is engine.h's struct bt_found, which carries both transports: a
 * Classic inquiry fills class of device and EIR, LE scanning (#205) fills
 * appearance and AD data, and a dual-mode device seen on both is one entry.
 * At most BT_SCAN_MAX entries per controller; one not seen for BT_SCAN_TTL
 * seconds is dropped. Names are also kept in a separate cache that outlives
 * the entries, so a device seen again is not asked for its name again.
 *
 * All calls expect bluetoothd_lock() held.
 */
#ifndef _BLUETOOTHD_DISCOVERY_H_
#define _BLUETOOTHD_DISCOVERY_H_

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "bluetoothd.h"

/* Merge a sighting into the cache; marks the adapter's scan_dirty. */
void			disc_found(struct bt_adapter *a,
			    const struct bt_found *f);

/* Drop entries not seen for BT_SCAN_TTL seconds. */
void			disc_prune(struct bt_adapter *a, time_t now);

int			disc_count(struct bt_adapter *a);
const struct bt_found	*disc_at(struct bt_adapter *a, int idx);
const struct bt_found	*disc_find(struct bt_adapter *a,
			    const struct bt_addr *addr);

/* A cached name for `addr`, from either cache. */
bool			disc_name(struct bt_adapter *a,
			    const struct bt_addr *addr, char *out, size_t len);

#endif /* _BLUETOOTHD_DISCOVERY_H_ */
