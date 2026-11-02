/*
 * classic_engine.c — the BR/EDR engine behind engine.h.
 *
 * Not implemented yet. The controller bring-up (#207) attaches it; inquiry
 * (#208), the security manager (#209) and connections (#213) fill it in. Until
 * then attach reports bt_status_operation_not_supported_e, and every other
 * entry is NULL, which the policy layer reads the same way.
 */
#include "engine.h"

static bt_status_t
classic_attach(const struct bt_controller_desc *c,
    const struct bt_engine_host *host, struct bt_engine **out)
{
	(void)c;
	(void)host;
	*out = NULL;
	return (bt_status_operation_not_supported_e);
}

const struct bt_engine_ops bt_classic_engine = {
	.name		= "classic",
	.transport	= BT_TRANSPORT_BREDR,
	.attach		= classic_attach,
};
