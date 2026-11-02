/*
 * le_engine.c — the LE engine behind engine.h: GAP over the shared raw HCI
 * socket, ATT/GATT on L2CAP CID 4, SMP on CID 6, HOGP into IOHIDUserDevice.
 *
 * Not implemented yet (#205). It is registered now, through the same table as
 * the Classic engine, so the policy layer is engine-agnostic from the start.
 * Until then attach reports bt_status_operation_not_supported_e.
 */
#include "engine.h"

static bt_status_t
le_attach(const struct bt_controller_desc *c,
    const struct bt_engine_host *host, struct bt_engine **out)
{
	(void)c;
	(void)host;
	*out = NULL;
	return (bt_status_operation_not_supported_e);
}

const struct bt_engine_ops bt_le_engine = {
	.name		= "le",
	.transport	= BT_TRANSPORT_LE,
	.attach		= le_attach,
};
