/*
 * test_hostname_synth.c — golden cases for the shared SMBIOS hostname
 * synthesis (src/launchd/freebsd-shims/nextbsd_hostname_synth.h) used by
 * both launchd early-init and hostnamed (nextbsd/nextbsd#325).
 *
 * Host-runnable; the pure nbhs_synthesize_from() needs no kenv:
 *   cc -I src/launchd/freebsd-shims -o /tmp/t tests/test_hostname_synth.c && /tmp/t
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <nextbsd_hostname_synth.h>

static void
check(const char *version, const char *product, const char *uuid,
    const char *want)
{
	char got[NBHS_NAME_MAX + 1];

	nbhs_synthesize_from(version, product, uuid, got, sizeof(got));
	if (strcmp(got, want) != 0) {
		printf("HOSTNAME-SYNTH-FAIL: (%s, %s, %s) -> '%s', want '%s'\n",
		    version ? version : "NULL", product ? product : "NULL",
		    uuid ? uuid : "NULL", got, want);
		assert(0);
	}
}

int
main(void)
{
	/* The issue's VirtualBox guest: non-identifying version "1.2" must not
	 * become "1-2"; generic product gets the UUID suffix. */
	check("1.2", "VirtualBox", "68F9A871-8E8B-4C3A-9D2E-000000000001",
	    "VirtualBox-68f9a871");
	/* Identifying model name is used bare, no suffix. */
	check("ThinkPad T460s", "20F9CTO1WW", "12345678-aaaa", "ThinkPad-T460s");
	/* All-zero or missing UUID: no suffix. */
	check("1.2", "VirtualBox", "00000000-0000-0000", "VirtualBox");
	check(NULL, "VirtualBox", NULL, "VirtualBox");
	/* No usable SMBIOS at all: last resort, still suffixed when possible. */
	check(NULL, NULL, "DEADBEEF-1", "nextbsd-deadbeef");
	check("", "  ", NULL, "nextbsd");
	/* Numeric-only product is not identifying either. */
	check("1.0", "1.0", "abcdef01-2", "nextbsd-abcdef01");
	/* Slug capped at NBHS_SLUG_MAX; suffixed result stays a valid label. */
	check(NULL, "A Very Long Product Name That Keeps Going And Going",
	    "cafef00d-1", "A-Very-Long-Product-Name-That-Keeps-Goin-cafef00d");
	/* Tiny output buffer is truncated, never overrun. */
	{
		char small[6];

		nbhs_synthesize_from("1.2", "VirtualBox", "68f9a871", small,
		    sizeof(small));
		assert(strcmp(small, "Virtu") == 0);
	}
	puts("HOSTNAME-SYNTH-OK: early-init and hostnamed share one synthesis");
	return (0);
}
