/*
 * discovery.c — each controller's discovery cache. See discovery.h.
 */
#include "discovery.h"

#include <string.h>

/* A name in either cache, remembered across scans. */
static void
remember_name(struct bt_adapter *a, const struct bt_addr *addr,
    const char *name)
{
	int i;

	for (i = 0; i < BT_SCAN_MAX; i++) {
		if (a->names[i].name[0] != '\0' &&
		    bt_addr_equal(&a->names[i].addr, addr)) {
			(void)strlcpy(a->names[i].name, name,
			    sizeof(a->names[i].name));
			return;
		}
	}
	/* Round-robin: the oldest remembered name goes. */
	i = a->nnames_next;
	a->nnames_next = (a->nnames_next + 1) % BT_SCAN_MAX;
	a->names[i].addr = *addr;
	(void)strlcpy(a->names[i].name, name, sizeof(a->names[i].name));
}

const struct bt_found *
disc_find(struct bt_adapter *a, const struct bt_addr *addr)
{
	int i;

	for (i = 0; i < a->nfound; i++) {
		if (bt_addr_equal(&a->found[i].addr, addr))
			return (&a->found[i]);
	}
	return (NULL);
}

void
disc_found(struct bt_adapter *a, const struct bt_found *f)
{
	struct bt_found *e;
	int i, oldest;

	e = (struct bt_found *)disc_find(a, &f->addr);
	if (e == NULL) {
		if (a->nfound < BT_SCAN_MAX) {
			e = &a->found[a->nfound++];
		} else {
			/* Full: replace the one seen longest ago. */
			oldest = 0;
			for (i = 1; i < a->nfound; i++) {
				if (a->found[i].last_seen <
				    a->found[oldest].last_seen)
					oldest = i;
			}
			e = &a->found[oldest];
		}
		(void)memset(e, 0, sizeof(*e));
		e->addr = f->addr;
		e->rssi = 127;
		(void)disc_name(a, &f->addr, e->name, sizeof(e->name));
	}

	/* Merge: a later sighting fills in, it never blanks. */
	e->transports |= f->transports;
	if (f->name[0] != '\0') {
		(void)strlcpy(e->name, f->name, sizeof(e->name));
		remember_name(a, &f->addr, f->name);
	}
	if (f->rssi != 127)
		e->rssi = f->rssi;
	if (f->class_of_device != 0)
		e->class_of_device = f->class_of_device;
	if (f->appearance != 0)
		e->appearance = f->appearance;
	if (f->eir_len > 0) {
		(void)memcpy(e->eir, f->eir, f->eir_len);
		e->eir_len = f->eir_len;
	}
	if (f->last_seen > e->last_seen)
		e->last_seen = f->last_seen;
	a->scan_dirty = true;
}

void
disc_prune(struct bt_adapter *a, time_t now)
{
	int i = 0;

	while (i < a->nfound) {
		if (now - a->found[i].last_seen > BT_SCAN_TTL) {
			a->found[i] = a->found[--a->nfound];
			a->scan_dirty = true;
		} else
			i++;
	}
}

int
disc_count(struct bt_adapter *a)
{
	return (a->nfound);
}

const struct bt_found *
disc_at(struct bt_adapter *a, int idx)
{
	if (idx < 0 || idx >= a->nfound)
		return (NULL);
	return (&a->found[idx]);
}

bool
disc_name(struct bt_adapter *a, const struct bt_addr *addr, char *out,
    size_t len)
{
	const struct bt_found *f;
	int i;

	f = disc_find(a, addr);
	if (f != NULL && f->name[0] != '\0') {
		(void)strlcpy(out, f->name, len);
		return (true);
	}
	for (i = 0; i < BT_SCAN_MAX; i++) {
		if (a->names[i].name[0] != '\0' &&
		    bt_addr_equal(&a->names[i].addr, addr)) {
			(void)strlcpy(out, a->names[i].name, len);
			return (true);
		}
	}
	return (false);
}
