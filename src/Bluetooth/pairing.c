/*
 * pairing.c — running pairings, parked prompts and their publication. See
 * pairing.h.
 */
#include "pairing.h"
#include "bluetoothd.h"
#include "btstate.h"
#include "btstore.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define	PAIRING_MAX	8
#define	DEVQ_MAX	16

struct pairing {
	bool			used;
	char			node[32];
	struct bt_addr		peer;
	bt_pair_state_t		state;
	bt_status_t		result;
	bool			created;	/* the bond is this pairing's */
	struct bt_pair_prompt	prompt;		/* valid while waiting */
	time_t			finished;
	bool			dirty;		/* publication out of date */
	bool			published;	/* the Pairing/<addr> key is set */
};

/* Devices whose State:/Bluetooth/Device/<addr> needs updating. */
struct devq {
	bool		used;
	struct bt_addr	peer;
	bool		paired;
};

static struct pairing	g_pairings[PAIRING_MAX];
static struct devq	g_devq[DEVQ_MAX];

/* Classic addresses are public; match on the six octets. */
static bool
same(const struct bt_addr *a, const struct bt_addr *b)
{
	return (memcmp(a->b, b->b, BT_ADDR_LEN) == 0 &&
	    (a->type == b->type || a->type == BT_ADDR_PUBLIC ||
	    b->type == BT_ADDR_PUBLIC));
}

static struct pairing *
find(const struct bt_addr *peer)
{
	int i;

	for (i = 0; i < PAIRING_MAX; i++) {
		if (g_pairings[i].used && same(&g_pairings[i].peer, peer))
			return (&g_pairings[i]);
	}
	return (NULL);
}

void
pairing_begin_locked(const char *node, const struct bt_addr *peer)
{
	struct pairing *p;
	time_t now = time(NULL);
	int i;

	p = find(peer);
	for (i = 0; p == NULL && i < PAIRING_MAX; i++) {
		if (!g_pairings[i].used)
			p = &g_pairings[i];
	}
	/* Full: reuse a finished record that is not on display. */
	for (i = 0; p == NULL && i < PAIRING_MAX; i++) {
		if (g_pairings[i].state != bt_pair_state_pending_e &&
		    g_pairings[i].state != bt_pair_state_waiting_e &&
		    !g_pairings[i].published)
			p = &g_pairings[i];
	}
	if (p == NULL)
		return;		/* eight pairings at once: report none */

	(void)memset(p, 0, sizeof(*p));
	p->used = true;
	(void)strlcpy(p->node, node, sizeof(p->node));
	p->peer = *peer;
	p->state = bt_pair_state_pending_e;
	p->result = bt_status_success_e;
	p->finished = now;
}

bool
pairing_active_locked(const struct bt_addr *peer)
{
	struct pairing *p = find(peer);

	return (p != NULL && (p->state == bt_pair_state_pending_e ||
	    p->state == bt_pair_state_waiting_e));
}

void
pairing_mark_created_locked(const struct bt_addr *peer)
{
	struct pairing *p = find(peer);

	if (p != NULL)
		p->created = true;
}

void
pairing_prompt_locked(const struct bt_pair_prompt *pr)
{
	struct pairing *p = find(&pr->peer);

	if (p == NULL)
		return;
	p->prompt = *pr;
	p->state = bt_pair_state_waiting_e;
	p->dirty = true;
	bluetoothd_wake();
}

void
pairing_answered_locked(const struct bt_addr *peer)
{
	struct pairing *p = find(peer);

	if (p == NULL || p->state != bt_pair_state_waiting_e)
		return;
	p->state = bt_pair_state_pending_e;
	p->dirty = true;
	bluetoothd_wake();
}

bool
pairing_finish_locked(const struct bt_addr *peer, bt_status_t st)
{
	struct pairing *p = find(peer);

	if (p == NULL)
		return (false);
	p->state = (st == bt_status_success_e) ? bt_pair_state_succeeded_e :
	    bt_pair_state_failed_e;
	p->result = st;
	p->finished = time(NULL);
	p->dirty = true;
	bluetoothd_wake();
	return (p->created);
}

void
pairing_drop_node_locked(const char *node)
{
	int i;

	for (i = 0; i < PAIRING_MAX; i++) {
		struct pairing *p = &g_pairings[i];

		if (p->used && strcmp(p->node, node) == 0 &&
		    (p->state == bt_pair_state_pending_e ||
		    p->state == bt_pair_state_waiting_e))
			(void)pairing_finish_locked(&p->peer,
			    bt_status_no_adapter_e);
	}
}

bool
pairing_status_locked(const struct bt_addr *peer, bt_pair_state_t *state,
    bt_status_t *result, struct bt_pair_prompt *prompt, char *node,
    size_t nodelen)
{
	struct pairing *p = find(peer);

	*state = bt_pair_state_none_e;
	*result = bt_status_success_e;
	(void)memset(prompt, 0, sizeof(*prompt));
	if (node != NULL && nodelen > 0)
		node[0] = '\0';
	if (p == NULL)
		return (false);
	*state = p->state;
	*result = p->result;
	if (p->state == bt_pair_state_waiting_e)
		*prompt = p->prompt;
	if (node != NULL)
		(void)strlcpy(node, p->node, nodelen);
	return (true);
}

void
pairing_device_changed_locked(const struct bt_addr *peer, bool paired)
{
	int i, slot = -1;

	for (i = 0; i < DEVQ_MAX; i++) {
		if (g_devq[i].used && same(&g_devq[i].peer, peer)) {
			slot = i;
			break;
		}
		if (!g_devq[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return;		/* sixteen at once; the next change catches up */
	g_devq[slot].used = true;
	g_devq[slot].peer = *peer;
	g_devq[slot].paired = paired;
	bluetoothd_wake();
}

static const char *
method_name(bt_pair_method_t m)
{
	switch (m) {
	case bt_pair_just_works_e:		return ("Confirm");
	case bt_pair_numeric_comparison_e:	return ("NumericComparison");
	case bt_pair_passkey_entry_e:		return ("PasskeyEntry");
	case bt_pair_passkey_display_e:		return ("PasskeyDisplay");
	case bt_pair_pin_e:			return ("PIN");
	default:				return ("None");
	}
}

void
pairing_publish(void)
{
	struct work {
		struct bt_addr		peer;
		bool			show;
		struct bt_pair_prompt	prompt;
	} work[PAIRING_MAX];
	struct devq devs[DEVQ_MAX];
	char key[BT_ADDR_STRLEN], path[128];
	time_t now = time(NULL);
	int i, n = 0, nd = 0;
	bool notify = false;

	bluetoothd_lock();
	for (i = 0; i < PAIRING_MAX; i++) {
		struct pairing *p = &g_pairings[i];

		if (!p->used)
			continue;
		if (p->dirty) {
			work[n].peer = p->peer;
			work[n].show = (p->state == bt_pair_state_waiting_e);
			work[n].prompt = p->prompt;
			if (work[n].show || p->published) {
				n++;
				notify = true;
			}
			p->published = (p->state == bt_pair_state_waiting_e);
			p->dirty = false;
		}
		/* Forget finished pairings once nobody can be polling them. */
		if (p->state != bt_pair_state_pending_e &&
		    p->state != bt_pair_state_waiting_e && !p->published &&
		    now - p->finished > PAIRING_KEEP_S)
			p->used = false;
	}
	for (i = 0; i < DEVQ_MAX; i++) {
		if (g_devq[i].used) {
			devs[nd++] = g_devq[i];
			g_devq[i].used = false;
		}
	}
	bluetoothd_unlock();

	for (i = 0; i < n; i++) {
		(void)bt_addr_ntoa(&work[i].peer, BT_ADDR_FMT_KEY, key,
		    sizeof(key));
		(void)snprintf(path, sizeof(path), "%s%s",
		    BT_STATE_PAIRING_PREFIX, key);
		if (work[i].show)
			(void)bt_state_publish_pairing(path, &work[i].prompt,
			    method_name(work[i].prompt.method));
		else
			(void)bt_state_remove(path);
	}
	if (notify)
		bt_state_notify(BT_NOTIFY_PAIRING);

	for (i = 0; i < nd; i++) {
		struct bt_bond b;

		(void)bt_addr_ntoa(&devs[i].peer, BT_ADDR_FMT_KEY, key,
		    sizeof(key));
		(void)snprintf(path, sizeof(path), "%s%s",
		    BT_STATE_DEVICE_PREFIX, key);
		if (devs[i].paired &&
		    bt_store_bond_find(&devs[i].peer, false, &b))
			(void)bt_state_publish_device(path, &b);
		else
			(void)bt_state_remove(path);
	}
	if (nd > 0)
		bt_state_notify(BT_NOTIFY_DEVICE);
}
