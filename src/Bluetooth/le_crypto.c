/*
 * le_crypto.c — the Security Manager's crypto toolbox (Core v5.4 Vol 3 Part H
 * §2.2) and P-256 ECDH, over libcrypto's EVP interface.
 *
 * The specification writes every value most significant octet first; the PDUs
 * and HCI carry them least significant first, and so does everything the LE
 * engine keeps (le.h). The functions here take and return wire order and
 * reverse at the AES / CMAC / EC boundary, so callers never think about it.
 * The message layouts below are the specification's concatenations written
 * out in wire order: "A || B" with B least significant is B's octets first.
 *
 * Nothing here logs; key material never leaves these buffers except through
 * the out parameters.
 */
#include "le.h"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <stdlib.h>
#include <string.h>

static void
reverse(uint8_t *dst, const uint8_t *src, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[n - 1 - i];
}

/* AES-128 of one block, most significant octet first throughout. */
static int
aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	EVP_CIPHER_CTX *ctx;
	int n = 0, ok;

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL)
		return (-1);
	ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) == 1 &&
	    EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
	    EVP_EncryptUpdate(ctx, out, &n, in, 16) == 1 && n == 16;
	EVP_CIPHER_CTX_free(ctx);
	return (ok ? 0 : -1);
}

/* AES-CMAC (RFC 4493), most significant octet first throughout. */
static int
cmac(const uint8_t key[16], const uint8_t *m, size_t len, uint8_t out[16])
{
	EVP_MAC *mac;
	EVP_MAC_CTX *ctx = NULL;
	OSSL_PARAM params[2];
	size_t n = 0;
	int ok = 0;

	mac = EVP_MAC_fetch(NULL, "CMAC", NULL);
	if (mac == NULL)
		return (-1);
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER,
	    (char *)"AES-128-CBC", 0);
	params[1] = OSSL_PARAM_construct_end();
	ctx = EVP_MAC_CTX_new(mac);
	if (ctx != NULL)
		ok = EVP_MAC_init(ctx, key, 16, params) == 1 &&
		    EVP_MAC_update(ctx, m, len) == 1 &&
		    EVP_MAC_final(ctx, out, &n, 16) == 1 && n == 16;
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(mac);
	return (ok ? 0 : -1);
}

/* AES-CMAC in wire order: key and message in, MAC out. */
static int
cmac_le(const uint8_t key[16], const uint8_t *m, size_t len, uint8_t out[16])
{
	uint8_t k[16], mb[80], o[16];
	int rc;

	if (len > sizeof(mb))
		return (-1);
	reverse(k, key, 16);
	reverse(mb, m, len);
	rc = cmac(k, mb, len, o);
	if (rc == 0)
		reverse(out, o, 16);
	explicit_bzero(k, sizeof(k));
	explicit_bzero(mb, sizeof(mb));
	explicit_bzero(o, sizeof(o));
	return (rc);
}

/* e(key, plaintext), the security function of §2.2.1. */
int
le_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t k[16], i[16], o[16];
	int rc;

	reverse(k, key, 16);
	reverse(i, in, 16);
	rc = aes128(k, i, o);
	if (rc == 0)
		reverse(out, o, 16);
	explicit_bzero(k, sizeof(k));
	explicit_bzero(i, sizeof(i));
	explicit_bzero(o, sizeof(o));
	return (rc);
}

/*
 * c1, the legacy confirm value (§2.2.3):
 *   p1 = pres || preq || rat' || iat'
 *   p2 = padding || ia || ra
 *   c1 = e(k, e(k, r XOR p1) XOR p2)
 */
int
le_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7],
    const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat,
    const uint8_t ra[6], uint8_t out[16])
{
	uint8_t p1[16], p2[16], t[16];
	int i;

	p1[0] = iat;
	p1[1] = rat;
	(void)memcpy(p1 + 2, preq, 7);
	(void)memcpy(p1 + 9, pres, 7);
	(void)memcpy(p2, ra, 6);
	(void)memcpy(p2 + 6, ia, 6);
	(void)memset(p2 + 12, 0, 4);

	for (i = 0; i < 16; i++)
		t[i] = r[i] ^ p1[i];
	if (le_e(k, t, t) != 0)
		return (-1);
	for (i = 0; i < 16; i++)
		t[i] ^= p2[i];
	if (le_e(k, t, out) != 0)
		return (-1);
	explicit_bzero(t, sizeof(t));
	return (0);
}

/* s1, the legacy STK (§2.2.4): e(k, r1' || r2'), r1' and r2' 64 bits. */
int
le_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16],
    uint8_t out[16])
{
	uint8_t r[16];
	int rc;

	(void)memcpy(r, r2, 8);
	(void)memcpy(r + 8, r1, 8);
	rc = le_e(k, r, out);
	explicit_bzero(r, sizeof(r));
	return (rc);
}

/* ah, the random address hash (§2.2.2): e(k, padding || r) mod 2^24. */
int
le_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3])
{
	uint8_t t[16];

	(void)memset(t, 0, sizeof(t));
	(void)memcpy(t, r, 3);
	if (le_e(irk, t, t) != 0)
		return (-1);
	(void)memcpy(out, t, 3);
	return (0);
}

/* f4, the LE Secure Connections confirm value (§2.2.6). */
int
le_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
    uint8_t z, uint8_t out[16])
{
	uint8_t m[65];

	/* U || V || Z */
	m[0] = z;
	(void)memcpy(m + 1, v, 32);
	(void)memcpy(m + 33, u, 32);
	return (cmac_le(x, m, sizeof(m), out));
}

/* f5, the MacKey and LTK (§2.2.7). */
int
le_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16],
    const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16],
    uint8_t ltk[16])
{
	static const uint8_t salt[16] = {
		0xbe, 0x83, 0x60, 0x5a, 0xdb, 0x0b, 0x37, 0x60,
		0x38, 0xa5, 0xf5, 0xaa, 0x91, 0x83, 0x88, 0x6c
	};
	static const uint8_t keyid[4] = { 0x65, 0x6c, 0x74, 0x62 };  /* "btle" */
	uint8_t t[16], m[53];
	int rc;

	if (cmac_le(salt, w, 32, t) != 0)
		return (-1);
	/* Counter || keyID || N1 || N2 || A1 || A2 || Length(256) */
	m[0] = 0x00;
	m[1] = 0x01;
	(void)memcpy(m + 2, a2, 7);
	(void)memcpy(m + 9, a1, 7);
	(void)memcpy(m + 16, n2, 16);
	(void)memcpy(m + 32, n1, 16);
	(void)memcpy(m + 48, keyid, 4);
	m[52] = 0;
	rc = cmac_le(t, m, sizeof(m), mackey);
	m[52] = 1;
	if (rc == 0)
		rc = cmac_le(t, m, sizeof(m), ltk);
	explicit_bzero(t, sizeof(t));
	return (rc);
}

/* f6, the DHKey check value (§2.2.8). */
int
le_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16],
    const uint8_t r[16], const uint8_t iocap[3], const uint8_t a1[7],
    const uint8_t a2[7], uint8_t out[16])
{
	uint8_t m[65];

	/* N1 || N2 || R || IOcap || A1 || A2 */
	(void)memcpy(m, a2, 7);
	(void)memcpy(m + 7, a1, 7);
	(void)memcpy(m + 14, iocap, 3);
	(void)memcpy(m + 17, r, 16);
	(void)memcpy(m + 33, n2, 16);
	(void)memcpy(m + 49, n1, 16);
	return (cmac_le(w, m, sizeof(m), out));
}

/* g2, the numeric comparison value (§2.2.9), already reduced mod 10^6. */
int
le_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
    const uint8_t y[16], uint32_t *out)
{
	uint8_t m[80], t[16];

	/* U || V || Y */
	(void)memcpy(m, y, 16);
	(void)memcpy(m + 16, v, 32);
	(void)memcpy(m + 48, u, 32);
	if (cmac_le(x, m, sizeof(m), t) != 0)
		return (-1);
	*out = ((uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 |
	    (uint32_t)t[3] << 24) % 1000000;
	return (0);
}

/* ---- P-256 ------------------------------------------------------------------- */

void *
le_ecdh_new(uint8_t pk[64])
{
	EVP_PKEY *k;
	uint8_t pub[65];
	size_t n = 0;

	k = EVP_EC_gen("P-256");
	if (k == NULL)
		return (NULL);
	if (EVP_PKEY_get_octet_string_param(k, OSSL_PKEY_PARAM_PUB_KEY, pub,
	    sizeof(pub), &n) != 1 || n != sizeof(pub) || pub[0] != 0x04) {
		EVP_PKEY_free(k);
		return (NULL);
	}
	/* Uncompressed point 04 || X || Y, each most significant first. */
	reverse(pk, pub + 1, 32);
	reverse(pk + 32, pub + 33, 32);
	return (k);
}

int
le_ecdh_shared(void *key, const uint8_t peer[64], uint8_t dhkey[32])
{
	EVP_PKEY_CTX *pctx = NULL, *dctx = NULL;
	EVP_PKEY *pk = NULL;
	OSSL_PARAM params[3];
	uint8_t pub[65], secret[32];
	size_t n = sizeof(secret);
	int rc = -1;

	pub[0] = 0x04;
	reverse(pub + 1, peer, 32);
	reverse(pub + 33, peer + 32, 32);
	params[0] = OSSL_PARAM_construct_utf8_string(
	    OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0);
	params[1] = OSSL_PARAM_construct_octet_string(
	    OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub));
	params[2] = OSSL_PARAM_construct_end();

	pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	if (pctx == NULL || EVP_PKEY_fromdata_init(pctx) != 1 ||
	    EVP_PKEY_fromdata(pctx, &pk, EVP_PKEY_PUBLIC_KEY, params) != 1)
		goto out;
	/*
	 * An invalid-curve point is the attack against SC pairing; the peer's
	 * key must be on P-256. EVP_PKEY_derive_set_peer checks it too.
	 */
	EVP_PKEY_CTX_free(pctx);
	pctx = EVP_PKEY_CTX_new(pk, NULL);
	if (pctx == NULL || EVP_PKEY_public_check(pctx) != 1)
		goto out;
	dctx = EVP_PKEY_CTX_new((EVP_PKEY *)key, NULL);
	if (dctx == NULL || EVP_PKEY_derive_init(dctx) != 1 ||
	    EVP_PKEY_derive_set_peer(dctx, pk) != 1 ||
	    EVP_PKEY_derive(dctx, secret, &n) != 1 || n != sizeof(secret))
		goto out;
	reverse(dhkey, secret, 32);
	rc = 0;
out:
	explicit_bzero(secret, sizeof(secret));
	EVP_PKEY_CTX_free(dctx);
	EVP_PKEY_CTX_free(pctx);
	EVP_PKEY_free(pk);
	return (rc);
}

void
le_ecdh_free(void *key)
{
	EVP_PKEY_free((EVP_PKEY *)key);
}

void
le_random(void *buf, size_t len)
{
	arc4random_buf(buf, len);
}
