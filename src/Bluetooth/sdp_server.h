/*
 * sdp_server.h — bluetoothd's SDP server: L2CAP PSM 1 and the local record
 * database.
 *
 * bluetoothd answers SDP itself; there is no sdpd and no /var/run/sdp local
 * protocol. Every record this host offers is registered in-process, by the
 * part of bluetoothd that implements the service:
 *
 *   SDP server             handle 0x00000000, always (the spec requires it)
 *   PnP Information        always: this host's Device ID
 *   A2DP Sink              off until the A2DP sink role (#219) turns it on
 *   A2DP Source            off until the A2DP source role (#218) turns it on
 *   AVRCP Target           off until AVRCP (#220) turns it on
 *   AVRCP Controller       off until AVRCP (#220) turns it on
 *   HFP Audio Gateway      off until the hands-free gateway turns it on
 *
 * The server is one listening L2CAP socket on PSM 1 bound to every controller
 * (BDADDR_ANY), serviced by a thread of its own: a peer's SDP query must not
 * wait for the main loop, and the main loop must not wait for a peer. The
 * record database has its own lock, so the toggles below may be called from
 * any thread.
 *
 * Darwin shape: on macOS bluetoothd owns the SDP server and IOBluetooth
 * publishes local services into it (IOBluetoothSDPServiceRecord
 * publishedServiceRecordWithDictionary:); this is the in-process equivalent.
 */
#ifndef _BLUETOOTHD_SDP_SERVER_H_
#define _BLUETOOTHD_SDP_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Start the server: open the PSM 1 listener and its thread. The controller
 * bring-up calls this once it has loaded NgBtsocket (the L2CAP sockets);
 * calling it again while the server runs is a no-op that returns 0, so it may
 * be called on every controller arrival. Returns -1 with errno set when the
 * L2CAP socket cannot be opened (EPROTONOSUPPORT: the stack is not loaded
 * yet); the database is kept and a later call retries.
 */
int	sdp_server_start(void);

/* Stop the thread and close every socket. The database is kept. */
void	sdp_server_stop(void);

bool	sdp_server_running(void);

/* The optional local services. Values index a table: append only. */
typedef enum {
	SDP_LOCAL_A2DP_SINK		= 0,	/* AudioSink 0x110B */
	SDP_LOCAL_A2DP_SOURCE		= 1,	/* AudioSource 0x110A */
	SDP_LOCAL_AVRCP_TARGET		= 2,	/* A/V RC Target 0x110C */
	SDP_LOCAL_AVRCP_CONTROLLER	= 3,	/* A/V RC 0x110E, 0x110F */
	SDP_LOCAL_HFP_AG		= 4,	/* Handsfree AG 0x111F */
	SDP_LOCAL_COUNT
} sdp_local_service_t;

/*
 * Publish or withdraw one optional record. `features` is the record's
 * SupportedFeatures (0x0311) value; 0 keeps the default:
 *
 *   A2DP Sink         0x0001  headphone
 *   A2DP Source       0x0001  player
 *   AVRCP Target      0x0001  category 1 (player)
 *   AVRCP Controller  0x0002  category 2 (monitor: absolute volume)
 *   HFP AG            0x0000  (no optional features)
 *
 * The HFP AG record also needs its RFCOMM channel: sdp_server_set_hfp_channel
 * first. Returns 0, or -1 for a bad service or no memory.
 */
int	sdp_server_set_service(sdp_local_service_t svc, bool enabled,
	    uint16_t features);

bool	sdp_server_service_enabled(sdp_local_service_t svc);

/* RFCOMM server channel of the HFP AG record (1..30; default 13). */
int	sdp_server_set_hfp_channel(uint8_t channel);

/*
 * This host's Device ID, for the PnP Information record. vendor_id_source is
 * 1 (Bluetooth SIG) or 2 (USB-IF). The default is source 2, vendor 0xFFFF
 * ("no assigned vendor ID" in the Device ID profile), product 0, version
 * 0x0100: NextBSD has no assigned vendor ID.
 */
int	sdp_server_set_pnp(uint8_t vendor_id_source, uint16_t vendor_id,
	    uint16_t product_id, uint16_t version);

#endif /* _BLUETOOTHD_SDP_SERVER_H_ */
