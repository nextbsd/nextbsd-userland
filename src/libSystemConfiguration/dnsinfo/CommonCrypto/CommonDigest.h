/*
 * CommonDigest.h -- the CC_SHA1_* calls dnsinfo uses, over libmd.
 *
 * NextBSD has no CommonCrypto. dnsinfo_create.c hashes the configuration
 * blob to notice changes (_dns_configuration_signature) and nothing here
 * relies on it being SHA-1 beyond the digest length. Link with -lmd.
 */
#ifndef _COMMONCRYPTO_COMMONDIGEST_H_SHIM_
#define	_COMMONCRYPTO_COMMONDIGEST_H_SHIM_

#include <sys/types.h>
#include <sha.h>

#define	CC_SHA1_DIGEST_LENGTH	20

typedef SHA_CTX CC_SHA1_CTX;

static inline int
CC_SHA1_Init(CC_SHA1_CTX *ctx)
{
	SHA1_Init(ctx);
	return (1);
}

static inline int
CC_SHA1_Update(CC_SHA1_CTX *ctx, const void *data, size_t len)
{
	SHA1_Update(ctx, data, len);
	return (1);
}

static inline int
CC_SHA1_Final(unsigned char *md, CC_SHA1_CTX *ctx)
{
	SHA1_Final(md, ctx);
	return (1);
}

#endif /* _COMMONCRYPTO_COMMONDIGEST_H_SHIM_ */
