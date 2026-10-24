/*
 * whyred (SDM636 / SDM660) eMMC 5.1 Host Controller Probe.
 *
 * Internal eMMC (sdhc_1 / mmc@c0c4000):
 *   Standard SDHCI Host Controller at 0x0c0c4000.
 *   CQHCI at 0x0c0c5000.
 *
 * Runs with MMU off (Device memory). Reads host controller version,
 * capabilities, and status left behind by ABL bootloader.
 */
#include <stdint.h>
#include "console.h"
#include "sdhci.h"

#define SDHCI1_BASE		0x0c0c4000UL	/* sdhc_1 in sdm630.dtsi */

#define SDHCI_BLOCK_SIZE	0x04
#define SDHCI_BLOCK_COUNT	0x06
#define SDHCI_ARGUMENT		0x08
#define SDHCI_COMMAND		0x0e
#define SDHCI_RESP0		0x10
#define SDHCI_PRESENT_STATE	0x24
#define SDHCI_POWER_CONTROL	0x29
#define SDHCI_CLOCK_CONTROL	0x2c
#define SDHCI_SOFTWARE_RESET	0x2f
#define SDHCI_INT_STATUS	0x30
#define SDHCI_CAPABILITIES	0x40
#define SDHCI_HOST_VERSION	0xfe

static inline uint32_t sdhci_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(SDHCI1_BASE + off);
}

static inline uint16_t sdhci_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(SDHCI1_BASE + off);
}

static inline uint8_t sdhci_rd8(uint32_t off)
{
	return *(volatile uint8_t *)(SDHCI1_BASE + off);
}

void test_sdhci(void)
{
	con_str("SDHCI", "probing whyred eMMC 5.1 (sdhc_1)");

	uint16_t ver = sdhci_rd16(SDHCI_HOST_VERSION);
	uint32_t pstate = sdhci_rd32(SDHCI_PRESENT_STATE);
	uint32_t cap = sdhci_rd32(SDHCI_CAPABILITIES);
	uint32_t clk = sdhci_rd16(SDHCI_CLOCK_CONTROL);
	uint8_t pwr = sdhci_rd8(SDHCI_POWER_CONTROL);

	con_hex2("SDHC1", "ver=", ver, "pstate=", pstate);
	con_hex2("CAP", "cap0=", cap, "clk=", clk);
	con_hex2("PWR", "pwr=", pwr, "base=", SDHCI1_BASE);

	/* Check card present bit (bit 16 in PRESENT_STATE) */
	int card_present = (pstate >> 16) & 1;
	int card_stable = (pstate >> 17) & 1;
	con_str("CARD", card_present ? (card_stable ? "eMMC present (stable)" : "eMMC present") : "no card detected");
}
