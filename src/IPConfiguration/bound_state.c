/*
 * bound_state.c — per-interface lease table, mutex-guarded.
 *
 * See bound_state.h for why this stopped being a single global (it was
 * the shared root of #37 and #38) and how the primary election works
 * (#41).
 *
 * The table is a flat BOUND_MAX_IF array scanned linearly. With at most
 * a handful of interfaces that is faster than anything cleverer, and it
 * keeps every operation a single critical section.
 */
#include "bound_state.h"

#include <pthread.h>
#include <string.h>

struct bound_entry {
	bool			bound;
	bool			linklocal;	/* a 169.254 binding (#426) */
	char			ifname[IF_NAMESIZE];
	struct dhcp_lease	lease;
	struct saved_pkt	packet;		/* pkt_size 0: none */
};

/*
 * Designated initializer: only the mutex needs a value. Spelling a
 * struct dhcp_lease out positionally means this warns (and eventually breaks)
 * every time that struct gains a field — which is exactly what
 * -Wmissing-field-initializers caught. Static storage zero-fills the rest,
 * which is the same "nothing is bound" state bound_state_init() establishes.
 */
static struct {
	pthread_mutex_t		lock;
	struct bound_entry	e[BOUND_MAX_IF];
} g_bound = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Caller holds the lock. Returns the slot for `ifname`, or NULL. */
static struct bound_entry *
find_locked(const char *ifname)
{
	int i;

	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_bound.e[i].bound &&
		    strcmp(g_bound.e[i].ifname, ifname) == 0)
			return (&g_bound.e[i]);
	}
	return (NULL);
}

/* Caller holds the lock. The highest-ranked bound slot of one kind, or NULL. */
static struct bound_entry *
best_locked(bool linklocal)
{
	struct bound_entry *best = NULL;
	int best_rank = -1, i;

	for (i = 0; i < BOUND_MAX_IF; i++) {
		int r;

		if (!g_bound.e[i].bound || g_bound.e[i].linklocal != linklocal)
			continue;
		r = iface_rank(g_bound.e[i].ifname);
		if (r > best_rank) {
			best_rank = r;
			best = &g_bound.e[i];
		}
	}
	return (best);
}

/*
 * Caller holds the lock. A link-local binding has no router, so it is primary
 * only when nothing else is bound (#426).
 */
static struct bound_entry *
primary_locked(void)
{
	struct bound_entry *best = best_locked(false);

	return (best != NULL ? best : best_locked(true));
}

void
bound_state_init(void)
{
	int i;

	(void)pthread_mutex_lock(&g_bound.lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		g_bound.e[i].bound = false;
		g_bound.e[i].linklocal = false;
		g_bound.e[i].ifname[0] = '\0';
		(void)memset(&g_bound.e[i].lease, 0,
		    sizeof(g_bound.e[i].lease));
		g_bound.e[i].packet.pkt_size = 0;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
}

int
iface_rank(const char *ifname)
{
	if (ifname == NULL || ifname[0] == '\0')
		return (IFRANK_OTHER);
	if (strncmp(ifname, "lo", 2) == 0)
		return (IFRANK_NEVER);
	if (strncmp(ifname, "wlan", 4) == 0)
		return (IFRANK_WIRELESS);
	/*
	 * Everything else that got far enough to hold a DHCP lease came
	 * through dhcp_pick_interface()'s IFT_ETHER filter, so treating it
	 * as wired is right. Note this deliberately catches wlan0 *only* by
	 * name: net80211 VAPs report IFT_ETHER too (they go through
	 * ether_ifattach), so there is no media type to test against.
	 */
	return (IFRANK_WIRED);
}

static void
set_entry(const char *ifname, const struct dhcp_lease *lease, bool linklocal)
{
	struct bound_entry *e;
	int i;

	if (ifname == NULL || lease == NULL)
		return;

	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e == NULL) {
		for (i = 0; i < BOUND_MAX_IF; i++) {
			if (!g_bound.e[i].bound) {
				e = &g_bound.e[i];
				break;
			}
		}
	}
	if (e != NULL) {
		e->bound = true;
		e->linklocal = linklocal;
		(void)strlcpy(e->ifname, ifname, sizeof(e->ifname));
		e->lease = *lease;
	}
	/* Table full: drop the binding rather than evict someone else's.
	 * BOUND_MAX_IF is far above any real machine, so this is a
	 * can't-happen guard, not a policy. */
	(void)pthread_mutex_unlock(&g_bound.lock);
}

void
bound_state_set(const char *ifname, const struct dhcp_lease *lease)
{
	set_entry(ifname, lease, false);
}

void
bound_state_set_linklocal(const char *ifname, const struct dhcp_lease *lease)
{
	set_entry(ifname, lease, true);
}

void
bound_state_clear(const char *ifname)
{
	struct bound_entry *e;

	if (ifname == NULL)
		return;
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e != NULL) {
		e->bound = false;
		e->linklocal = false;
		e->ifname[0] = '\0';
		(void)memset(&e->lease, 0, sizeof(e->lease));
		e->packet.pkt_size = 0;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
}

bool
bound_state_is_linklocal(const char *ifname)
{
	struct bound_entry *e;
	bool hit;

	if (ifname == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	hit = (e != NULL && e->linklocal);
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}

bool
bound_state_is_bound(const char *ifname)
{
	bool hit;

	if (ifname == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	hit = (find_locked(ifname) != NULL);
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}

int
bound_state_count(void)
{
	int n = 0, i;

	(void)pthread_mutex_lock(&g_bound.lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_bound.e[i].bound)
			n++;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (n);
}

int
bound_state_list(char names[][IF_NAMESIZE], int max)
{
	int i, n = 0;

	(void)pthread_mutex_lock(&g_bound.lock);
	for (i = 0; i < BOUND_MAX_IF && n < max; i++) {
		if (g_bound.e[i].bound)
			(void)strlcpy(names[n++], g_bound.e[i].ifname,
			    IF_NAMESIZE);
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (n);
}

bool
bound_state_primary(char *name_out, size_t name_out_sz)
{
	struct bound_entry *e;
	bool any;

	(void)pthread_mutex_lock(&g_bound.lock);
	e = primary_locked();
	any = (e != NULL);
	if (any && name_out != NULL && name_out_sz > 0)
		(void)strlcpy(name_out, e->ifname, name_out_sz);
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (any);
}

bool
bound_state_get_addr(const char *ifname, uint32_t *addr_out)
{
	struct bound_entry *e;
	bool hit = false;

	if (ifname == NULL || addr_out == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e != NULL) {
		*addr_out = e->lease.addr.s_addr;
		hit = true;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}

bool
bound_state_get_lease(const char *ifname, struct dhcp_lease *out)
{
	struct bound_entry *e;
	bool hit = false;

	if (ifname == NULL || out == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e != NULL) {
		*out = e->lease;
		hit = true;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}

void
bound_state_set_packet(const char *ifname, const struct saved_pkt *pkt)
{
	struct bound_entry *e;

	if (ifname == NULL || pkt == NULL)
		return;
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e != NULL)
		e->packet = *pkt;
	(void)pthread_mutex_unlock(&g_bound.lock);
}

bool
bound_state_get_packet(const char *ifname, struct saved_pkt *out)
{
	struct bound_entry *e;
	bool hit = false;

	if (ifname == NULL || out == NULL)
		return (false);
	(void)pthread_mutex_lock(&g_bound.lock);
	e = find_locked(ifname);
	if (e != NULL && e->packet.pkt_size != 0) {
		*out = e->packet;
		hit = true;
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (hit);
}

int
bound_state_names(char names[][IF_NAMESIZE], int max)
{
	struct bound_entry *primary;
	int i, n = 0;

	(void)pthread_mutex_lock(&g_bound.lock);
	primary = primary_locked();
	if (primary != NULL && n < max)
		(void)strlcpy(names[n++], primary->ifname, IF_NAMESIZE);
	for (i = 0; i < BOUND_MAX_IF && n < max; i++) {
		if (!g_bound.e[i].bound || &g_bound.e[i] == primary)
			continue;
		(void)strlcpy(names[n++], g_bound.e[i].ifname, IF_NAMESIZE);
	}
	(void)pthread_mutex_unlock(&g_bound.lock);
	return (n);
}
