/*
 * test_hostname_synth.c — golden cases for the shared SMBIOS hostname
 * synthesis (src/launchd/freebsd-shims/nextbsd_hostname_synth.h) used by
 * both launchd early-init and hostnamed (nextbsd/nextbsd#325).
 *
 * Host-runnable; the pure nbhs_synthesize_from() needs no kenv:
 *   cc -I src/launchd/freebsd-shims -o /tmp/t tests/test_hostname_synth.c && /tmp/t
 *
 * The (version, product) pairs are what these machines' firmware reports.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <nextbsd_hostname_synth.h>

static void
check(const char *version, const char *product, const char *want)
{
	char got[NBHS_NAME_MAX + 1];

	nbhs_synthesize_from(version, product, got, sizeof(got));
	if (strcmp(got, want) != 0) {
		printf("HOSTNAME-SYNTH-FAIL: (%s, %s) -> '%s', want '%s'\n",
		    version ? version : "NULL", product ? product : "NULL",
		    got, want);
		assert(0);
	}
}

int
main(void)
{
	/* Stop at the model number; the board revision is not part of it. */
	check(NULL, "Raspberry Pi 500 Rev 1.0", "Raspberry-Pi-500");
	check(NULL, "Raspberry Pi 5 Model B Rev 1.0", "Raspberry-Pi-5");
	/* A screen size before the model number is skipped. */
	check("", "XPS 13 9320", "XPS-9320");
	check("Not Specified", "Latitude 7420", "Latitude-7420");
	check(NULL, "HP EliteBook 840 G8 Notebook PC", "HP-EliteBook-840");
	check("1.0", "MacBookPro15,1", "MacBookPro15");
	check(NULL, "Laptop 13 (AMD Ryzen 7040Series)", "Laptop-13");
	/* A product that is only a part number: the version names it. */
	check("ThinkPad T460s", "20F9CTO1WW", "ThinkPad-T460s");
	check("ThinkPad X1 Carbon Gen 9", "20XWCTO1WW", "ThinkPad-X1");
	/* Virtual machines: the product, not the machine type in version. */
	check("virt-10.0", "QEMU Virtual Machine", "QEMU-Virtual-Machine");
	check("pc-q35-10.0", "Standard PC (Q35 + ICH9, 2009)",
	    "Standard-PC-Q35");
	check("1.2", "VirtualBox", "VirtualBox");
	/* No number: at most three words. */
	check(NULL, "A Very Long Product Name", "A-Very-Long");
	/* Nothing usable. */
	check("1.0", "1.0", "NextBSD");
	check("", "  ", "NextBSD");
	check(NULL, NULL, "NextBSD");
	/* Capped at NBHS_SLUG_MAX. */
	check(NULL, "Supercalifragilisticexpialidociousmachineworks 1",
	    "NextBSD");
	/* Tiny output buffer is truncated, never overrun. */
	{
		char small[6];

		nbhs_synthesize_from("1.2", "VirtualBox", small,
		    sizeof(small));
		assert(strcmp(small, "Virtu") == 0);
	}
	puts("HOSTNAME-SYNTH-OK: early-init and hostnamed share one synthesis");
	return (0);
}
