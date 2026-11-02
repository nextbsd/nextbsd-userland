/*
 * keystore.h — bluetoothd's secrets: the one module that touches key bytes.
 *
 *   /Local/Library/Preferences/SystemConfiguration/
 *       org.nextbsd.Bluetooth.keys.plist      mode 0600, root
 *
 *   LinkKeys               { <addr> = { Key = <data 16>, Type = <int> } }
 *                          Classic; Type is the HCI link key type (0x00-0x08)
 *   LongTermKeys           { <addr> = { LTK = <data 16>, EDIV = <int>,
 *                                       Rand = <data 8>, KeySize = <int>,
 *                                       Authenticated = <bool>,
 *                                       SecureConnections = <bool> } }
 *   IdentityResolvingKeys  { <addr> = <data 16> }      LE, the peer's IRK
 *   SignatureKeys          { <addr> = <data 16> }      LE, the peer's CSRK
 *
 * <addr> is BT_ADDR_FMT_KEY, the same key as PairedDevices. Key bytes are in
 * HCI/SMP wire order (least significant octet first), as the controller takes
 * them; Rand is the 8 wire octets. Note that macOS up to 11 stored link keys
 * byte-reversed, which is the usual trap when importing from a dual boot.
 *
 * PLAINTEXT. There is no keychain on NextBSD; this file is the accepted debt
 * (as WLAN's passphrases are), kept in its own 0600 file so that when a
 * keychain lands, this module is the only thing that changes. Nothing here
 * ever formats key bytes into a log line.
 */
#ifndef _BLUETOOTHD_KEYSTORE_H_
#define _BLUETOOTHD_KEYSTORE_H_

#include <stdbool.h>

#include "engine.h"

/*
 * Merge the stored secrets for `key` (BT_ADDR_FMT_KEY) into `b`: fills the key
 * fields and sets BT_BOND_LINK_KEY / _LTK / _IRK / _CSRK for what was found.
 * Returns true if anything was found.
 */
bool	keystore_load(const char *key, struct bt_bond *b);

/*
 * Store the secrets `b->flags` says it has, under `key`, replacing that
 * device's previous keys (a family `b` lacks is removed). 0 on success.
 */
int	keystore_save(const char *key, const struct bt_bond *b);

/* Remove every key for `key`. 0 on success (also when there was none). */
int	keystore_forget(const char *key);

#endif /* _BLUETOOTHD_KEYSTORE_H_ */
