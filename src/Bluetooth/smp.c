/*
 * smp.c — the Security Manager on L2CAP CID 6 (Core v5.4 Vol 3 Part H), as
 * the initiator: bluetoothd is the central and starts every pairing.
 *
 * What we offer, in the Pairing Request:
 *
 *   IO capability     KeyboardDisplay: we can show a passkey (a keyboard
 *                     types it) and take one (a device that shows it)
 *   AuthReq           bonding, MITM, Secure Connections
 *   key distribution  we hand out nothing; the peer gives us its LTK
 *                     (legacy only), IRK + identity address, and CSRK
 *
 * The method follows from the peer's IO capability (§2.3.5.1, table 2.8):
 *
 *   peer NoInputNoOutput (mice)          Just Works, accepted at once: the
 *                                        user asked for this pairing
 *   peer KeyboardOnly (keyboards)        Passkey Entry: we show the number,
 *                                        the user types it on the keyboard
 *   peer DisplayOnly                     Passkey Entry: the user types the
 *                                        number the device shows
 *   peer DisplayYesNo / KeyboardDisplay  Numeric Comparison with Secure
 *                                        Connections; Passkey Entry without
 *
 * Prompts are the same pull-model prompts the Classic security manager
 * raises (engine.h, BT_EV_PAIR_PROMPT), answered through smp_reply.
 *
 * Legacy pairing: TK, Mconfirm/Sconfirm (c1), Mrand/Srand, STK (s1), then
 * LE_Start_Encryption with the STK and the peer's keys over the encrypted
 * link. Secure Connections: P-256 public keys, the confirm/random exchange
 * (f4; twenty rounds for Passkey Entry), Numeric Comparison (g2), MacKey and
 * LTK (f5), the DHKey checks (f6), then LE_Start_Encryption with the LTK.
 *
 * Clean-room: written from the specification. Key bytes are never logged.
 */
#include "le.h"
#include "hci.h"
#include "hci_compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* PDUs. */
#define	SMP_PAIRING_REQ		0x01
#define	SMP_PAIRING_RSP		0x02
#define	SMP_PAIRING_CONFIRM	0x03
#define	SMP_PAIRING_RANDOM	0x04
#define	SMP_PAIRING_FAILED	0x05
#define	SMP_ENC_INFO		0x06
#define	SMP_CENTRAL_IDENT	0x07
#define	SMP_IDENT_INFO		0x08
#define	SMP_IDENT_ADDR_INFO	0x09
#define	SMP_SIGNING_INFO	0x0a
#define	SMP_SECURITY_REQ	0x0b
#define	SMP_PUBLIC_KEY		0x0c
#define	SMP_DHKEY_CHECK		0x0d
#define	SMP_KEYPRESS		0x0e

/* IO capabilities. */
#define	SMP_IO_DISPLAY_ONLY	0x00
#define	SMP_IO_DISPLAY_YESNO	0x01
#define	SMP_IO_KEYBOARD_ONLY	0x02
#define	SMP_IO_NONE		0x03
#define	SMP_IO_KEYBOARD_DISPLAY	0x04

/* AuthReq bits. */
#define	SMP_AUTH_BONDING	0x01
#define	SMP_AUTH_MITM		0x04
#define	SMP_AUTH_SC		0x08

/* Pairing Failed reasons. */
#define	SMP_ERR_PASSKEY		0x01
#define	SMP_ERR_AUTH_REQ	0x03
#define	SMP_ERR_CONFIRM		0x04
#define	SMP_ERR_NOT_SUPPORTED	0x05
#define	SMP_ERR_KEY_SIZE	0x06
#define	SMP_ERR_UNSPECIFIED	0x08
#define	SMP_ERR_INVALID		0x0a
#define	SMP_ERR_DHKEY_CHECK	0x0b
#define	SMP_ERR_NUMERIC		0x0c

#define	SMP_MIN_KEY_SIZE	7

static const char *
method_str(bt_pair_method_t m)
{
	switch (m) {
	case bt_pair_numeric_comparison_e:	return ("numeric comparison");
	case bt_pair_passkey_entry_e:		return ("passkey entry");
	case bt_pair_passkey_display_e:		return ("passkey display");
	default:				return ("just works");
	}
}

static int
smp_send(struct le_conn *c, const uint8_t *pdu, size_t len)
{
	struct smp_state *s = &c->smp;

	if (c->smp_fd < 0)
		return (-1);
	for (;;) {
		if (send(c->smp_fd, pdu, len, 0) >= 0)
			break;
		if (errno != EINTR) {
			le_log(c->e, "SMP send failed: %s", strerror(errno));
			return (-1);
		}
	}
	s->deadline = bt_now_ms() + LE_SMP_TIMEOUT_MS;
	return (0);
}

static int
smp_send16(struct le_conn *c, uint8_t code, const uint8_t v[16])
{
	uint8_t pdu[17];

	pdu[0] = code;
	(void)memcpy(pdu + 1, v, 16);
	return (smp_send(c, pdu, sizeof(pdu)));
}

/* Forget everything secret. The stage goes back to idle. */
static void
smp_wipe(struct le_conn *c)
{
	struct smp_state *s = &c->smp;

	if (s->ecdh != NULL)
		le_ecdh_free(s->ecdh);
	explicit_bzero(s, sizeof(*s));
	s->stage = SMP_IDLE;
	s->method = bt_pair_none_e;
	s->waiting = bt_pair_none_e;
}

void
smp_init(struct le_conn *c)
{
	smp_wipe(c);
}

/* End the pairing and tell the engine; the engine wipes the state. */
static void
finish(struct le_conn *c, bt_status_t st)
{
	struct smp_state *s = &c->smp;

	if (s->stage == SMP_IDLE || s->stage == SMP_DONE)
		return;
	s->stage = st == bt_status_success_e ? SMP_DONE : SMP_IDLE;
	s->waiting = bt_pair_none_e;
	if (s->ecdh != NULL) {
		le_ecdh_free(s->ecdh);
		s->ecdh = NULL;
	}
	le_pair_done(c, st);
}

static void
fail(struct le_conn *c, uint8_t reason, bt_status_t st)
{
	uint8_t pdu[2];

	pdu[0] = SMP_PAIRING_FAILED;
	pdu[1] = reason;
	(void)smp_send(c, pdu, sizeof(pdu));
	le_log(c->e, "pairing failed here (reason 0x%02x)", reason);
	finish(c, st);
}

/* Park a prompt with the policy layer. */
static void
prompt(struct le_conn *c, bt_pair_method_t method, uint32_t passkey)
{
	struct smp_state *s = &c->smp;
	struct bt_event ev;
	struct bt_pair_prompt *p = &ev.u.prompt;

	(void)memset(&ev, 0, sizeof(ev));
	ev.kind = BT_EV_PAIR_PROMPT;
	ev.peer = c->addr;
	ev.transport = BT_TRANSPORT_LE;
	p->id = le_prompt_id();
	p->peer = c->addr;
	p->transport = BT_TRANSPORT_LE;
	p->method = method;
	p->passkey = passkey;
	p->initiated = true;
	p->expires = time(NULL) + LE_USER_TIMEOUT_MS / 1000;

	s->waiting = method;
	s->prompt_id = p->id;
	s->user_deadline = bt_now_ms() + LE_USER_TIMEOUT_MS;
	le_raise(c->e, &ev);
}

/* A, B for f5/f6: the 48-bit address with its type octet on top. */
static void
addr7(const struct bt_addr *a, uint8_t out[7])
{
	(void)memcpy(out, a->b, 6);
	out[6] = a->type;
}

static void
passkey16(uint32_t passkey, uint8_t out[16])
{
	(void)memset(out, 0, 16);
	out[0] = (uint8_t)passkey;
	out[1] = (uint8_t)(passkey >> 8);
	out[2] = (uint8_t)(passkey >> 16);
	out[3] = (uint8_t)(passkey >> 24);
}

/* Zero the key above the negotiated size (§2.3.4). */
static void
mask_key(uint8_t key[16], uint8_t size)
{
	if (size < 16)
		(void)memset(key + size, 0, 16 - size);
}

/* ---- legacy -------------------------------------------------------------------- */

static void
legacy_confirm(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t conf[16];

	le_random(s->lrand, sizeof(s->lrand));
	if (le_c1(s->tk, s->lrand, s->preq, s->pres, c->e->local.type,
	    c->e->local.b, c->addr.type, c->addr.b, conf) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	s->stage = SMP_W4_CONFIRM;
	if (smp_send16(c, SMP_PAIRING_CONFIRM, conf) != 0)
		finish(c, bt_status_pairing_failed_e);
}

static void
legacy_random(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t conf[16], stk[16];

	if (le_c1(s->tk, s->rrand, s->preq, s->pres, c->e->local.type,
	    c->e->local.b, c->addr.type, c->addr.b, conf) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	if (memcmp(conf, s->rconfirm, 16) != 0) {
		fail(c, SMP_ERR_CONFIRM, s->method == bt_pair_just_works_e ?
		    bt_status_pairing_failed_e : bt_status_pairing_rejected_e);
		return;
	}
	if (le_s1(s->tk, s->rrand, s->lrand, stk) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	mask_key(stk, s->key_size);
	s->stage = SMP_W4_ENCRYPTION;
	s->deadline = bt_now_ms() + LE_SMP_TIMEOUT_MS;
	if (le_start_encryption(c, stk, 0, 0) != 0)
		finish(c, bt_status_internal_error_e);
	explicit_bzero(stk, sizeof(stk));
}

/* ---- Secure Connections --------------------------------------------------------- */

/* Passkey Entry, one of twenty rounds: send Cai (§2.3.5.6.3). */
static void
sc_passkey_round(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t conf[16], z;

	z = (uint8_t)(0x80 | ((s->passkey >> s->round) & 1));
	le_random(s->lrand, sizeof(s->lrand));
	if (le_f4(s->lpk, s->rpk, s->lrand, z, conf) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	s->have_rconfirm = false;
	s->stage = SMP_W4_CONFIRM;
	if (smp_send16(c, SMP_PAIRING_CONFIRM, conf) != 0)
		finish(c, bt_status_pairing_failed_e);
}

static void	sc_check_peer(struct le_conn *c);

/* MacKey and LTK, then our DHKey check Ea (§2.3.5.6.5). */
static void
sc_dhkey_check(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t a[7], b[7], r[16], ea[16];

	addr7(&c->e->local, a);
	addr7(&c->addr, b);
	if (s->method == bt_pair_passkey_entry_e ||
	    s->method == bt_pair_passkey_display_e)
		passkey16(s->passkey, r);
	else
		(void)memset(r, 0, sizeof(r));
	if (le_f5(s->dhkey, s->lrand, s->rrand, a, b, s->mackey,
	    s->ltk) != 0 ||
	    le_f6(s->mackey, s->lrand, s->rrand, r, s->preq + 1, a, b,
	    ea) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	s->stage = SMP_W4_DHKEY_CHECK;
	if (smp_send16(c, SMP_DHKEY_CHECK, ea) != 0) {
		finish(c, bt_status_pairing_failed_e);
		return;
	}
	if (s->have_rcheck)
		sc_check_peer(c);
}

/* The peer's Eb (§2.3.5.6.5), then encrypt with the LTK. */
static void
sc_check_peer(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t a[7], b[7], r[16], eb[16];

	addr7(&c->e->local, a);
	addr7(&c->addr, b);
	if (s->method == bt_pair_passkey_entry_e ||
	    s->method == bt_pair_passkey_display_e)
		passkey16(s->passkey, r);
	else
		(void)memset(r, 0, sizeof(r));
	if (le_f6(s->mackey, s->rrand, s->lrand, r, s->pres + 1, b, a,
	    eb) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	if (memcmp(eb, s->rcheck, 16) != 0) {
		fail(c, SMP_ERR_DHKEY_CHECK, bt_status_pairing_failed_e);
		return;
	}
	mask_key(s->ltk, s->key_size);
	s->stage = SMP_W4_ENCRYPTION;
	s->deadline = bt_now_ms() + LE_SMP_TIMEOUT_MS;
	if (le_start_encryption(c, s->ltk, 0, 0) != 0)
		finish(c, bt_status_internal_error_e);
}

/* The peer's random arrived: check its confirm. */
static void
sc_random(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint8_t conf[16];
	uint32_t v;

	if (s->method == bt_pair_passkey_entry_e ||
	    s->method == bt_pair_passkey_display_e) {
		uint8_t z = (uint8_t)(0x80 | ((s->passkey >> s->round) & 1));

		if (le_f4(s->rpk, s->lpk, s->rrand, z, conf) != 0) {
			fail(c, SMP_ERR_UNSPECIFIED,
			    bt_status_internal_error_e);
			return;
		}
		if (memcmp(conf, s->rconfirm, 16) != 0) {
			fail(c, SMP_ERR_CONFIRM, bt_status_pairing_rejected_e);
			return;
		}
		if (++s->round < 20) {
			sc_passkey_round(c);
			return;
		}
		sc_dhkey_check(c);
		return;
	}

	/* Just Works and Numeric Comparison: Cb = f4(PKb, PKa, Nb, 0). */
	if (le_f4(s->rpk, s->lpk, s->rrand, 0, conf) != 0) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	if (memcmp(conf, s->rconfirm, 16) != 0) {
		fail(c, SMP_ERR_CONFIRM, bt_status_pairing_failed_e);
		return;
	}
	if (s->method == bt_pair_numeric_comparison_e) {
		if (le_g2(s->lpk, s->rpk, s->lrand, s->rrand, &v) != 0) {
			fail(c, SMP_ERR_UNSPECIFIED,
			    bt_status_internal_error_e);
			return;
		}
		s->stage = SMP_W4_USER;
		s->deadline = bt_now_ms() + LE_USER_TIMEOUT_MS;
		prompt(c, bt_pair_numeric_comparison_e, v);
		return;
	}
	sc_dhkey_check(c);
}

/* ---- the method ------------------------------------------------------------------ */

static bt_pair_method_t
pick_method(const struct smp_state *s)
{
	uint8_t peer_io = s->pres[1];
	bool mitm = ((s->preq[3] | s->pres[3]) & SMP_AUTH_MITM) != 0;

	if (!mitm)
		return (bt_pair_just_works_e);
	switch (peer_io) {
	case SMP_IO_DISPLAY_ONLY:
		return (bt_pair_passkey_entry_e);
	case SMP_IO_KEYBOARD_ONLY:
		return (bt_pair_passkey_display_e);
	case SMP_IO_DISPLAY_YESNO:
		return (s->sc ? bt_pair_numeric_comparison_e :
		    bt_pair_passkey_entry_e);
	case SMP_IO_KEYBOARD_DISPLAY:
		return (s->sc ? bt_pair_numeric_comparison_e :
		    bt_pair_passkey_display_e);
	default:
		return (bt_pair_just_works_e);
	}
}

/* With the passkey known, start the exchange that uses it. */
static void
passkey_go(struct le_conn *c)
{
	struct smp_state *s = &c->smp;

	if (s->sc) {
		s->round = 0;
		sc_passkey_round(c);
	} else {
		passkey16(s->passkey, s->tk);
		legacy_confirm(c);
	}
}

/*
 * Authentication stage 1 begins: after the Pairing Response (legacy) or the
 * public key exchange (Secure Connections).
 */
static void
auth_begin(struct le_conn *c)
{
	struct smp_state *s = &c->smp;

	switch (s->method) {
	case bt_pair_passkey_display_e:
		s->passkey = arc4random_uniform(1000000);
		s->have_passkey = true;
		/* Show it; the protocol goes on while the user types. */
		prompt(c, bt_pair_passkey_display_e, s->passkey);
		passkey_go(c);
		return;
	case bt_pair_passkey_entry_e:
		s->stage = SMP_W4_USER;
		s->deadline = bt_now_ms() + LE_USER_TIMEOUT_MS;
		prompt(c, bt_pair_passkey_entry_e, 0);
		return;
	case bt_pair_numeric_comparison_e:
		/* The peer sends Cb first; the number comes after Nb. */
		s->stage = SMP_W4_CONFIRM;
		return;
	default:
		if (s->sc) {
			s->stage = SMP_W4_CONFIRM;
			return;
		}
		(void)memset(s->tk, 0, sizeof(s->tk));
		legacy_confirm(c);
		return;
	}
}

/* ---- the PDUs --------------------------------------------------------------------- */

static void
on_pairing_rsp(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;
	uint8_t pdu[65];

	if (s->stage != SMP_W4_RESPONSE || len != 7)
		return;
	(void)memcpy(s->pres, p, 7);
	s->key_size = s->pres[4] < 16 ? s->pres[4] : 16;
	if (s->key_size < SMP_MIN_KEY_SIZE) {
		fail(c, SMP_ERR_KEY_SIZE, bt_status_pairing_failed_e);
		return;
	}
	s->sc = (s->preq[3] & s->pres[3] & SMP_AUTH_SC) != 0;
	/* The keys the peer will send: what we asked for and it agreed. */
	s->expect = (uint8_t)(s->preq[6] & s->pres[6] & 0x07);
	if (s->sc)
		s->expect &= (uint8_t)~SMP_DIST_ENC;	/* LTK is derived */
	s->method = pick_method(s);
	le_log(c->e, "%s pairing, %s, peer IO capability %u",
	    s->sc ? "Secure Connections" : "legacy", method_str(s->method),
	    s->pres[1]);

	if (!s->sc) {
		auth_begin(c);
		return;
	}
	s->ecdh = le_ecdh_new(s->lpk);
	if (s->ecdh == NULL) {
		fail(c, SMP_ERR_UNSPECIFIED, bt_status_internal_error_e);
		return;
	}
	pdu[0] = SMP_PUBLIC_KEY;
	(void)memcpy(pdu + 1, s->lpk, 64);
	s->stage = SMP_W4_PUBKEY;
	if (smp_send(c, pdu, sizeof(pdu)) != 0)
		finish(c, bt_status_pairing_failed_e);
}

static void
on_public_key(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	if (s->stage != SMP_W4_PUBKEY || len != 65)
		return;
	(void)memcpy(s->rpk, p + 1, 64);
	/* A peer that echoes our own key is attacking the exchange. */
	if (memcmp(s->rpk, s->lpk, 64) == 0 ||
	    le_ecdh_shared(s->ecdh, s->rpk, s->dhkey) != 0) {
		fail(c, SMP_ERR_INVALID, bt_status_pairing_failed_e);
		return;
	}
	auth_begin(c);
}

static void
on_confirm(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	/*
	 * Legacy and SC Passkey Entry: the answer to our confirm. SC Just
	 * Works and Numeric Comparison: Cb, which the responder sends first,
	 * right after the public keys.
	 */
	if (len != 17 || s->stage != SMP_W4_CONFIRM)
		return;
	(void)memcpy(s->rconfirm, p + 1, 16);
	s->have_rconfirm = true;
	if (s->sc && (s->method == bt_pair_just_works_e ||
	    s->method == bt_pair_numeric_comparison_e))
		le_random(s->lrand, sizeof(s->lrand));	/* Na */
	s->stage = SMP_W4_RANDOM;
	if (smp_send16(c, SMP_PAIRING_RANDOM, s->lrand) != 0)
		finish(c, bt_status_pairing_failed_e);
}

static void
on_random(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	if (s->stage != SMP_W4_RANDOM || len != 17)
		return;
	(void)memcpy(s->rrand, p + 1, 16);
	if (s->sc)
		sc_random(c);
	else
		legacy_random(c);
}

static void
on_dhkey_check(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	if (len != 17 || !s->sc)
		return;
	(void)memcpy(s->rcheck, p + 1, 16);
	s->have_rcheck = true;
	if (s->stage == SMP_W4_DHKEY_CHECK)
		sc_check_peer(c);
}

/* All the keys the peer promised have arrived? */
static void
keys_check(struct le_conn *c)
{
	if (c->smp.stage == SMP_W4_KEYS && c->smp.expect == 0)
		finish(c, bt_status_success_e);
}

static void
on_key(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	if (s->stage != SMP_W4_KEYS)
		return;
	switch (p[0]) {
	case SMP_ENC_INFO:
		if (len != 17)
			return;
		(void)memcpy(s->peer_ltk.key, p + 1, 16);
		break;
	case SMP_CENTRAL_IDENT: {
		int i;

		if (len != 11)
			return;
		s->peer_ltk.ediv = (uint16_t)(p[1] | p[2] << 8);
		s->peer_ltk.rand = 0;
		for (i = 7; i >= 0; i--)
			s->peer_ltk.rand = (s->peer_ltk.rand << 8) | p[3 + i];
		s->peer_ltk.key_size = s->key_size;
		s->peer_ltk.authenticated = s->method != bt_pair_just_works_e;
		s->peer_ltk.secure_conn = false;
		s->have_ltk = true;
		s->expect &= (uint8_t)~SMP_DIST_ENC;
		break;
	}
	case SMP_IDENT_INFO:
		if (len != 17)
			return;
		(void)memcpy(s->irk, p + 1, 16);
		s->have_irk = true;
		break;
	case SMP_IDENT_ADDR_INFO:
		if (len != 8)
			return;
		s->identity.type = p[1] != 0 ? BT_ADDR_RANDOM : BT_ADDR_PUBLIC;
		(void)memcpy(s->identity.b, p + 2, 6);
		s->have_identity = true;
		s->expect &= (uint8_t)~SMP_DIST_ID;
		break;
	case SMP_SIGNING_INFO:
		if (len != 17)
			return;
		(void)memcpy(s->csrk, p + 1, 16);
		s->have_csrk = true;
		s->expect &= (uint8_t)~SMP_DIST_SIGN;
		break;
	default:
		return;
	}
	s->deadline = bt_now_ms() + LE_SMP_TIMEOUT_MS;
	keys_check(c);
}

static void
smp_pdu(struct le_conn *c, const uint8_t *p, size_t len)
{
	struct smp_state *s = &c->smp;

	switch (p[0]) {
	case SMP_PAIRING_RSP:
		on_pairing_rsp(c, p, len);
		break;
	case SMP_PUBLIC_KEY:
		on_public_key(c, p, len);
		break;
	case SMP_PAIRING_CONFIRM:
		on_confirm(c, p, len);
		break;
	case SMP_PAIRING_RANDOM:
		on_random(c, p, len);
		break;
	case SMP_DHKEY_CHECK:
		on_dhkey_check(c, p, len);
		break;
	case SMP_ENC_INFO:
	case SMP_CENTRAL_IDENT:
	case SMP_IDENT_INFO:
	case SMP_IDENT_ADDR_INFO:
	case SMP_SIGNING_INFO:
		on_key(c, p, len);
		break;
	case SMP_PAIRING_FAILED:
		if (s->stage == SMP_IDLE || s->stage == SMP_DONE)
			break;
		le_log(c->e, "the device ended the pairing (reason 0x%02x)",
		    len >= 2 ? p[1] : 0);
		finish(c, (len >= 2 && (p[1] == SMP_ERR_PASSKEY ||
		    p[1] == SMP_ERR_NUMERIC || p[1] == SMP_ERR_CONFIRM)) ?
		    bt_status_pairing_rejected_e : bt_status_pairing_failed_e);
		break;
	case SMP_SECURITY_REQ:
		if (len >= 2 && (s->stage == SMP_IDLE || s->stage == SMP_DONE))
			le_security_request(c, p[1]);
		break;
	case SMP_KEYPRESS:
		break;
	case SMP_PAIRING_REQ:
	default: {
		/* We are the central; a peripheral never sends a request. */
		uint8_t pdu[2] = { SMP_PAIRING_FAILED, 0x07 /* unsupported */ };

		if (p[0] == SMP_PAIRING_REQ)
			pdu[1] = SMP_ERR_NOT_SUPPORTED;
		(void)smp_send(c, pdu, sizeof(pdu));
		break;
	}
	}
}

void
smp_input(struct le_conn *c)
{
	uint8_t buf[LE_SMP_PDU_MAX + 16];
	ssize_t n;
	int guard;

	for (guard = 0; guard < 16 && c->smp_fd >= 0 && c->state != LC_FREE;
	    guard++) {
		n = recv(c->smp_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			n = 0;
		}
		if (n == 0) {
			/* The SMP channel goes with the link; ATT notices. */
			le_unwatch(c->e, c->smp_fd);
			(void)close(c->smp_fd);
			c->smp_fd = -1;
			c->smp_connected = false;
			if (c->smp.stage != SMP_IDLE &&
			    c->smp.stage != SMP_DONE)
				finish(c, bt_status_pairing_failed_e);
			break;
		}
		smp_pdu(c, buf, (size_t)n);
	}
	explicit_bzero(buf, sizeof(buf));
}

/* ---- the engine's side -------------------------------------------------------------- */

bt_status_t
smp_start(struct le_conn *c)
{
	struct smp_state *s = &c->smp;

	if (c->smp_fd < 0)
		return (bt_status_not_connected_e);
	smp_wipe(c);
	s->preq[0] = SMP_PAIRING_REQ;
	s->preq[1] = SMP_IO_KEYBOARD_DISPLAY;
	s->preq[2] = 0;			/* no OOB data */
	s->preq[3] = SMP_AUTH_BONDING | SMP_AUTH_MITM | SMP_AUTH_SC;
	s->preq[4] = 16;		/* maximum key size */
	s->preq[5] = 0;			/* we distribute nothing */
	s->preq[6] = SMP_DIST_ENC | SMP_DIST_ID | SMP_DIST_SIGN;
	s->stage = SMP_W4_RESPONSE;
	if (smp_send(c, s->preq, sizeof(s->preq)) != 0) {
		smp_wipe(c);
		return (bt_status_internal_error_e);
	}
	return (bt_status_success_e);
}

bt_status_t
smp_reply(struct le_conn *c, const struct bt_pair_answer *a)
{
	struct smp_state *s = &c->smp;
	bt_pair_method_t m = s->waiting;

	if (m == bt_pair_none_e || a->id != s->prompt_id)
		return (bt_status_no_prompt_e);

	if (!a->accept) {
		s->waiting = bt_pair_none_e;
		fail(c, m == bt_pair_numeric_comparison_e ? SMP_ERR_NUMERIC :
		    m == bt_pair_just_works_e ? SMP_ERR_UNSPECIFIED :
		    SMP_ERR_PASSKEY, bt_status_pairing_rejected_e);
		return (bt_status_success_e);
	}

	switch (m) {
	case bt_pair_passkey_entry_e:
		if (a->passkey > BT_PASSKEY_MAX)
			return (bt_status_invalid_parameter_e);
		s->waiting = bt_pair_none_e;
		s->passkey = a->passkey;
		s->have_passkey = true;
		passkey_go(c);
		break;
	case bt_pair_numeric_comparison_e:
		s->waiting = bt_pair_none_e;
		if (s->stage == SMP_W4_USER)
			sc_dhkey_check(c);
		break;
	case bt_pair_passkey_display_e:
	default:
		/* Acknowledged; the device's keyboard finishes it. */
		s->waiting = bt_pair_none_e;
		break;
	}
	return (bt_status_success_e);
}

void
smp_encrypted(struct le_conn *c, uint8_t status)
{
	struct smp_state *s = &c->smp;

	if (s->stage != SMP_W4_ENCRYPTION)
		return;
	if (status != 0) {
		le_log(c->e, "encrypting with the new key failed: 0x%02x",
		    status);
		finish(c, bt_status_pairing_failed_e);
		return;
	}
	if (s->sc) {
		/* The LTK both sides derived is the bond's key. */
		(void)memcpy(s->peer_ltk.key, s->ltk, 16);
		s->peer_ltk.ediv = 0;
		s->peer_ltk.rand = 0;
		s->peer_ltk.key_size = s->key_size;
		s->peer_ltk.authenticated = s->method != bt_pair_just_works_e;
		s->peer_ltk.secure_conn = true;
		s->have_ltk = true;
	}
	s->stage = SMP_W4_KEYS;
	s->deadline = bt_now_ms() + LE_SMP_TIMEOUT_MS;
	keys_check(c);
}

void
smp_abort(struct le_conn *c, bt_status_t st)
{
	if (c->smp.stage == SMP_IDLE || c->smp.stage == SMP_DONE)
		return;
	fail(c, SMP_ERR_UNSPECIFIED, st);
}

uint64_t
smp_next_timer(struct le_conn *c)
{
	struct smp_state *s = &c->smp;
	uint64_t t;

	if (s->stage == SMP_IDLE || s->stage == SMP_DONE)
		return (0);
	t = s->deadline;
	if (s->waiting != bt_pair_none_e && s->user_deadline > t)
		t = s->user_deadline;	/* a prompt extends the wait */
	return (t);
}

void
smp_timer(struct le_conn *c, uint64_t now)
{
	uint64_t t = smp_next_timer(c);

	if (t == 0 || now < t)
		return;
	le_log(c->e, "pairing timed out");
	/* §3.4: after the SMP timeout no further SMP PDUs are sent. */
	finish(c, bt_status_pairing_timeout_e);
}

void
smp_bond(struct le_conn *c, struct bt_bond *b)
{
	struct smp_state *s = &c->smp;

	(void)memset(b, 0, sizeof(*b));
	b->addr = s->have_identity ? s->identity : c->addr;
	b->transports = BT_TRANSPORT_LE;
	if (s->have_ltk) {
		b->ltk = s->peer_ltk;
		b->flags |= BT_BOND_LTK;
	}
	if (s->have_irk) {
		(void)memcpy(b->irk, s->irk, 16);
		b->flags |= BT_BOND_IRK;
	}
	if (s->have_csrk) {
		(void)memcpy(b->csrk, s->csrk, 16);
		b->flags |= BT_BOND_CSRK;
	}
}
