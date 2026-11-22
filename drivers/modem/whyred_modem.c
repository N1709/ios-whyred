/*
 * whyred: modem (MSS/QDSP6) bring-up, F1. Boot-arg gated, read-mostly.
 *
 *   whyredmodem=1       deassert the MSS restart, enable the MSS clocks and
 *                      read QDSP6/RMB (no firmware)
 *   whyredmodem=2       also load MBA.MBN from the modem FAT16 (eMMC), hand
 *                      the MBA region to MSS with SCM, reset the Q6 and wait
 *                      for the MBA to answer
 *   whyredmodem=3       also authenticate MODEM.MDT, load every segment blob
 *                      into the modem carve-out and wait for auth complete
 *   whyredmodemdelay=S  seconds to wait after boot before it (default 25)
 *   whyredmodemclk=N    clock mask (default 0x3f): bit0 mss_cfg_ahb,
 *                      bit1 bimc_mss_q6_axi, bit2 boot_rom_ahb,
 *                      bit3 gpll0_out_msscc, bit4 mss_snoc_axi,
 *                      bit5 mss_mnoc_bimc_axi. Use it to bisect one boot
 *                      per value if a clock write wedges the thread.
 *
 * F0b (`whyred_wlan.c`) showed the ABL left mss_cfg_ahb (0x8a000) and
 * mss_snoc_axi (0x8a03c) off, while mss_q6_bimc_axi (0x8a040) was on. The
 * The first whyredmodem boot enabled the full active-clock set and read a
 * clean parked state:
 * Q6 reset 00000007 (held), PWR_CTL 00700000 (clamped, BHS/LDO off), RMB all
 * zero. This patch keeps that step and adds the MBA load.
 *
 * Order matters (pil-msa.c): power up, deassert the MSS restart (0x179000=0),
 * enable the clocks, program RMB, run the QDSP6 reset, wait for PBL/MBA.
 * The MBA image is copied as-is into pil_mba_region (0x94800000, 2 MiB) and
 * the first 1 MiB is assigned to MSS_MSA with SCM MEM_PROT_ASSIGN (the DTB's
 * qcom,mem-protect-id is 0xf). Step 3 authenticates MODEM.MDT, loads the 26
 * segment blobs into the modem carve-out and waits for AUTH_COMPLETE.
 *
 * Register meanings: Linux pil-msa.c, pil-q6v5.c, gcc-sdm660.c,
 * secure_buffer.c and the SDM660 DTB, read as behaviour references.
 */
#if defined(ARM64_BOARD_CONFIG_WHYRED)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include <vm/memory_types.h>
#include <libkern/libkern.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int santoni_emmc_read(uint64_t lba, uint32_t count, void *buf);
extern uint64_t santoni_emmc_modem_lba(void);

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x94000u
#define QDSP6_PA        0x04080000u
#define QDSP6_SIZE      0x00001000u
#define RMB_PA          0x04180000u
#define RMB_SIZE        0x00001000u
#define MSS_RESTART_PA  0x00179000u
#define MSS_RESTART_SIZE 0x00001000u

#define MBA_PA          0x94800000u
#define MBA_SIZE        0x00200000u
#define MBA_ASSIGN_SIZE 0x00100000u

/* modem_fw_region@8ac00000 (126 MiB) from the SDM660 DTB */
#define MODEM_FW_PA     0x8ac00000u
#define MODEM_FW_SIZE   0x07e00000u

/* GCC byte offsets from GCC_PA (gcc-sdm660.c) */
#define GCC_MSS_CFG_AHB         0x8a000u
#define GCC_MSS_MNOC_BIMC_AXI   0x8a004u
#define GCC_MSS_SNOC_AXI        0x8a03cu
#define GCC_MSS_Q6_BIMC_AXI     0x8a040u
#define GCC_BIMC_MSS_Q6_AXI     0x4401cu
#define GCC_BOOT_ROM_AHB_HALT   0x38004u
#define GCC_BOOT_ROM_AHB_VOTE   0x52004u
#define GCC_BOOT_ROM_AHB_BIT    (1u << 10)
#define GCC_GPLL0_OUT_MSSCC     0x5200cu
#define GCC_GPLL0_OUT_MSSCC_BIT (1u << 2)
#define CBCR_EN                 (1u << 0)
#define CBCR_OFF                (1u << 31)

/* QDSP6SS (pil-q6v5.c) */
#define Q6_RESET                0x014u
#define Q6_GFMUX_CTL            0x020u
#define Q6_PWR_CTL              0x030u
#define Q6V6_MEM_PWR_CTL        0x034u
#define Q6_BHS_STATUS           0x078u
#define Q6V62_BHS_STATUS        0x0c4u
#define Q6_XO_CBCR              0x038u
#define Q6_STRAP_ACC            0x110u

/* QDSP6SS_RESET / PWR_CTL / GFMUX bits */
#define Q6SS_STOP_CORE          (1u << 0)
#define Q6SS_CORE_ARES          (1u << 1)
#define Q6SS_BUS_ARES_ENA       (1u << 2)
#define Q6SS_CLK_ENA            (1u << 1)
#define Q6SS_L2DATA_STBY_N      (1u << 18)
#define Q6SS_SLP_RET_N          (1u << 19)
#define Q6SS_CLAMP_IO           (1u << 20)
#define QDSP6v55_CLAMP_WL       (1u << 21)
#define QDSP6v55_CLAMP_QMC_MEM  (1u << 22)
#define QDSP6v55_BHS_ON         (1u << 24)
#define QDSP6v55_LDO_BYP        (1u << 25)
#define QDSP6v55_BHS_EN_REST_ACK (1u << 0)

/* RMB (pil-msa.c) */
#define RMB_MBA_IMAGE           0x00u
#define RMB_PBL_STATUS          0x04u
#define RMB_MBA_COMMAND         0x08u
#define RMB_MBA_STATUS          0x0cu
#define RMB_PMI_META_DATA       0x10u
#define RMB_PMI_CODE_START      0x14u
#define RMB_PMI_CODE_LENGTH     0x18u
#define RMB_PROTOCOL_VERSION    0x1cu
#define RMB_DEBUG               0x20u
#define CMD_META_DATA_READY     0x1u
#define CMD_LOAD_READY          0x2u
#define STATUS_PBL_SUCCESS      0x1u
#define STATUS_XPU_UNLOCKED     0x1u
#define STATUS_XPU_UNLOCKED_SCRIBBLED 0x2u
#define STATUS_META_DATA_AUTH_SUCCESS 0x3u
#define STATUS_AUTH_COMPLETE    0x4u

/* SCM memory protection (secure_buffer.c, secure_buffer.h) */
#define SCM_FN(svc, cmd)        (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_MP              0x0cu
#define MEM_PROT_ASSIGN_ID      0x16u
#define VMID_HLOS               0x3u
#define VMID_MSS_MSA            0x0fu
#define PERM_READ               0x4u
#define PERM_WRITE              0x2u
#define PERM_EXEC               0x1u
#define SCM_ARGINFO_7           0x1117u  /* SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL) */

/* modem FAT16 (BPB: 512/32/1/2/512/49). The base LBA comes from the GPT
 * "modem" entry; it is not the same on both SDM660 boards. */
static uint64_t modem_lba;
#define SECTOR_SIZE             512u
#define FAT_DIR_MAX             (192u << 10)
#define FAT_BOUNCE_SIZE         (1u << 20)
#define SCM_BUF_SIZE            0x1000u
#define FAT_MAX_CLUSTER         0xfff8u

static vm_offset_t gcc, rst, q6, rmb, mba;
static void *fat_buf, *bounce, *scm_buf, *mdt_buf;
static uint64_t scm_pa;
static int modem_chan;
static uint32_t fat_cluster_bytes, fat_spc, fat_data_start, fat_fat_off;
static uint32_t fat_root_start, fat_root_entries, fat_fat_size;
static uint8_t *fat_image;              /* IMAGE/ directory, cached */
static uint32_t fat_image_len;

static inline uint32_t
mrd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/*
 * ml_io_map() returns Device-nGnRnE memory. XNU's memset/bzero store
 * engine emits STNP (non-temporal) pairs, which are not safe on Device
 * memory: an earlier boot wedged the modem thread inside the 1 MiB MBA zeroing
 * with no error line. Zero/copy with explicit volatile 64-bit stores.
 */
static void
io_zero(vm_offset_t dst, uint32_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	uint32_t i;

	for (i = 0; i + 8 <= n; i += 8) {
		*d++ = 0;
	}
	for (; i < n; i++) {
		*(volatile uint8_t *)(dst + i) = 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
io_copy(vm_offset_t dst, const void *src, uint32_t n)
{
	const uint8_t *s = src;
	volatile uint64_t *d = (volatile uint64_t *)dst;
	uint32_t i;

	for (i = 0; i + 8 <= n; i += 8) {
		uint64_t v;

		memcpy(&v, s + i, 8);
		*d++ = v;
	}
	for (; i < n; i++) {
		*(volatile uint8_t *)(dst + i) = s[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
modem_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&modem_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
modem_announce(const char *what)
{
	printf("WHYRED MODEM: about to %s\n", what);
	modem_wait(2000);
}

/*
 * The eMMC is shared with the root filesystem and the console flusher. The
 * SDHCI wrapper serializes on its lock, but a transfer can still time out
 * while the card is busy; retry with a thread delay instead of hammering the
 * controller (an earlier run died on the first modem sector read at 45 s).
 */
static int
emmc_read(uint64_t lba, uint32_t count, void *buf)
{
	int err = EIO;

	for (int attempt = 0; attempt < 5; attempt++) {
		err = santoni_emmc_read(lba, count, buf);
		if (err == 0) {
			return 0;
		}
		printf("WHYRED MODEM: eMMC read 0x%llx+%u failed %d "
		    "(attempt %d/5)\n", (unsigned long long)lba, count, err,
		    attempt + 1);
		modem_wait(100);
	}
	return err;
}

static uint32_t
modem_poll_off(uint32_t off, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++) {
		v = mrd(gcc, off);
		if (!(v & CBCR_OFF)) {
			break;
		}
		modem_wait(10);
	}
	return v;
}

static void
gcc_branch(const char *name, uint32_t off)
{
	uint32_t v = mrd(gcc, off);

	printf("WHYRED MODEM: gcc %s before %08x\n", name, v);
	if (!(v & CBCR_EN)) {
		mwr(gcc, off, v | CBCR_EN);
	}
	v = modem_poll_off(off, 100);
	printf("WHYRED MODEM: gcc %s after %08x (%s)\n", name, v,
	    (v & CBCR_OFF) ? "STILL OFF" : "on");
}

static void
modem_restart_deassert(void)
{
	uint32_t v;

	modem_announce("deassert the MSS restart (write 0 to 0x179000)");
	v = mrd(rst, 0);
	printf("WHYRED MODEM: mss restart before %08x\n", v);
	if (v != 0) {
		mwr(rst, 0, 0);
	}
	delay(100);
	v = mrd(rst, 0);
	printf("WHYRED MODEM: mss restart after %08x (%s)\n", v,
	    v == 0 ? "deasserted" : "STILL ASSERTED");
}

static void
modem_clocks(uint32_t mask)
{
	uint32_t v;

	printf("WHYRED MODEM: clock mask %02x\n", mask);
	if (mask & 0x01) {
		gcc_branch("mss_cfg_ahb", GCC_MSS_CFG_AHB);
	}
	if (mask & 0x02) {
		gcc_branch("bimc_mss_q6_axi", GCC_BIMC_MSS_Q6_AXI);
	}
	if (mask & 0x04) {
		v = mrd(gcc, GCC_BOOT_ROM_AHB_VOTE);
		printf("WHYRED MODEM: gcc boot_rom vote before %08x (halt %08x)\n",
		    v, mrd(gcc, GCC_BOOT_ROM_AHB_HALT));
		mwr(gcc, GCC_BOOT_ROM_AHB_VOTE, v | GCC_BOOT_ROM_AHB_BIT);
		v = modem_poll_off(GCC_BOOT_ROM_AHB_HALT, 100);
		printf("WHYRED MODEM: gcc boot_rom vote after %08x (halt %08x, %s)\n",
		    mrd(gcc, GCC_BOOT_ROM_AHB_VOTE), v,
		    (v & CBCR_OFF) ? "STILL OFF" : "on");
	}
	if (mask & 0x08) {
		v = mrd(gcc, GCC_GPLL0_OUT_MSSCC);
		printf("WHYRED MODEM: gcc gpll0_out_msscc before %08x\n", v);
		mwr(gcc, GCC_GPLL0_OUT_MSSCC, v | GCC_GPLL0_OUT_MSSCC_BIT);
		printf("WHYRED MODEM: gcc gpll0_out_msscc after %08x\n",
		    mrd(gcc, GCC_GPLL0_OUT_MSSCC));
	}
	if (mask & 0x10) {
		gcc_branch("mss_snoc_axi", GCC_MSS_SNOC_AXI);
	}
	if (mask & 0x20) {
		gcc_branch("mss_mnoc_bimc_axi", GCC_MSS_MNOC_BIMC_AXI);
	}
	printf("WHYRED MODEM: gcc mss_q6_bimc_axi (reference) %08x\n",
	    mrd(gcc, GCC_MSS_Q6_BIMC_AXI));
}

static void
modem_dump_q6(void)
{
	modem_announce("read the QDSP6 and RMB blocks (clocks are on)");
	printf("WHYRED MODEM: Q6 reset %08x gfmux %08x pwr %08x mem_pwr %08x\n",
	    mrd(q6, Q6_RESET), mrd(q6, Q6_GFMUX_CTL), mrd(q6, Q6_PWR_CTL),
	    mrd(q6, Q6V6_MEM_PWR_CTL));
	printf("WHYRED MODEM: Q6 bhs %08x bhs62 %08x xo_cbcr %08x strap_acc "
	    "%08x\n", mrd(q6, Q6_BHS_STATUS), mrd(q6, Q6V62_BHS_STATUS),
	    mrd(q6, Q6_XO_CBCR), mrd(q6, Q6_STRAP_ACC));
	printf("WHYRED MODEM: RMB image %08x pbl %08x cmd %08x status %08x\n",
	    mrd(rmb, RMB_MBA_IMAGE), mrd(rmb, RMB_PBL_STATUS),
	    mrd(rmb, RMB_MBA_COMMAND), mrd(rmb, RMB_MBA_STATUS));
}

/* --- SCM memory protection ------------------------------------------------ */

static uint64_t modem_scm7(uint32_t fn, uint32_t arginfo, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t extra_pa, uint64_t *res);

/*
 * One page holds the mem_prot_info entry, the source VMID list and the
 * dest_vm_and_perm_info list. The physical address goes in x2..x4; the
 * remaining four arguments go in the extended-argument page pointed to by
 * x5 (scm.c allocate_extra_arg_buffer, N_REGISTER_ARGS).
 */
static int
scm_mem_assign(uint32_t pa, uint32_t size, const uint32_t *src_vmids,
    int nsrc, const uint32_t *dst_vmids, const uint32_t *dst_perms, int ndst)
{
	struct mem_prot_info {
		uint64_t addr;
		uint64_t size;
	};
	struct dest_info {
		uint32_t vm;
		uint32_t perm;
		uint64_t ctx;
		uint32_t ctx_size;
		uint32_t pad;
	};
	uint8_t *buf = scm_buf;
	struct mem_prot_info *mp = (struct mem_prot_info *)(void *)buf;
	uint32_t *src = (uint32_t *)(void *)(buf + 0x40);
	struct dest_info *dst = (struct dest_info *)(void *)(buf + 0x80);
	uint64_t *extra = (uint64_t *)(void *)(buf + 0x200);
	uint64_t ret = 0;
	uint32_t fn = SCM_FN(SCM_SVC_MP, MEM_PROT_ASSIGN_ID);
	int err;

	if (nsrc < 1 || nsrc > 2 || ndst < 1 || ndst > 2) {
		return EINVAL;
	}
	memset(buf, 0, SCM_BUF_SIZE);
	mp->addr = pa;
	mp->size = size;
	for (int i = 0; i < nsrc; i++) {
		src[i] = src_vmids[i];
	}
	for (int i = 0; i < ndst; i++) {
		dst[i].vm = dst_vmids[i];
		dst[i].perm = dst_perms[i];
	}

	extra[0] = (uint64_t)nsrc * sizeof(uint32_t);   /* source list bytes */
	extra[1] = scm_pa + 0x80;                       /* dest list pa */
	extra[2] = (uint64_t)ndst * sizeof(*dst);       /* dest list bytes */
	extra[3] = 0;

	flush_dcache64((addr64_t)scm_pa, SCM_BUF_SIZE, TRUE);
	__asm__ volatile("dsb sy" ::: "memory");

	err = (int)modem_scm7(fn, SCM_ARGINFO_7, scm_pa, sizeof(*mp), scm_pa + 0x40,
	    scm_pa + 0x200, &ret);
	printf("WHYRED MODEM: SCM assign %08x+%x src %u dst %u ret %d res %llx\n",
	    pa, size, nsrc, ndst, err, (unsigned long long)ret);
	return err ? err : (ret ? EIO : 0);
}

static int
scm_assign(uint32_t pa, uint32_t size, uint32_t src_vmid, uint32_t dst_vmid,
    uint32_t dst_perm)
{
	return scm_mem_assign(pa, size, &src_vmid, 1, &dst_vmid, &dst_perm, 1);
}

/* --- modem FAT16 ---------------------------------------------------------- */

static inline uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
fat_cluster_sector(uint32_t cluster)
{
	return fat_data_start + (cluster - 2) * fat_spc;
}

static uint32_t
fat_next(uint32_t cluster)
{
	/*
	 * fat_buf already starts at the FAT's first sector. Adding
	 * fat_fat_off (the partition-relative offset, 512) made every chain
	 * read the wrong entry and stop after two clusters.
	 */
	return le16((const uint8_t *)fat_buf + cluster * 2);
}

/* Build the 8.3 name of a directory entry, without the trailing dot. */
static void
fat_name(const uint8_t *e, char *out, size_t outlen)
{
	size_t n = 0;

	for (int i = 0; i < 8 && e[i] != ' '; i++) {
		if (n + 1 < outlen) {
			out[n++] = (char)e[i];
		}
	}
	if (e[8] != ' ') {
		if (n + 1 < outlen) {
			out[n++] = '.';
		}
		for (int i = 8; i < 11 && e[i] != ' '; i++) {
			if (n + 1 < outlen) {
				out[n++] = (char)e[i];
			}
		}
	}
	out[n] = '\0';
}

/*
 * Read the BPB, FAT, root directory and IMAGE/ directory once. The partition
 * is the phone's own modem image (FAT16, 512 B sectors, 16 KiB clusters);
 * anything else is refused, never written.
 */
static int
fat_open(void)
{
	uint8_t *fat, *root;
	uint32_t fat_bytes, root_bytes, cluster, n, off;
	int err;

	modem_lba = santoni_emmc_modem_lba();
	if (modem_lba == 0) {
		printf("WHYRED MODEM: no \"modem\" GPT partition; eMMC read skipped\n");
		return ENOENT;
	}
	err = emmc_read(modem_lba, 1, bounce);
	if (err) {
		printf("WHYRED MODEM: modem sector 0 read failed %d\n", err);
		return err;
	}
	{
		const uint8_t *b = bounce;

		if (le16(b + 0x0b) != 512 || b[0x0d] != 32 ||
		    le16(b + 0x0e) != 1 || b[0x10] != 2 ||
		    le16(b + 0x11) != 512 || le16(b + 0x16) != 49) {
			printf("WHYRED MODEM: unexpected modem BPB (%u/%u/%u/%u/"
			    "%u/%u)\n", le16(b + 0x0b), b[0x0d], le16(b + 0x0e),
			    b[0x10], le16(b + 0x11), le16(b + 0x16));
			return EINVAL;
		}
		fat_root_entries = le16(b + 0x11);
		fat_fat_size = le16(b + 0x16);
		fat_spc = b[0x0d];
		fat_root_start = le16(b + 0x0e) + b[0x10] * fat_fat_size;
	}
	fat_cluster_bytes = fat_spc * SECTOR_SIZE;
	fat_fat_off = le16((const uint8_t *)bounce + 0x0e) * SECTOR_SIZE;
	fat_data_start = fat_root_start + (fat_root_entries * 32 + SECTOR_SIZE - 1) /
	    SECTOR_SIZE;

	/* FAT: 49 sectors (25,088 bytes). */
	fat_bytes = fat_fat_size * SECTOR_SIZE;
	fat = (uint8_t *)fat_buf;
	err = emmc_read(modem_lba + fat_fat_off / SECTOR_SIZE,
	    fat_fat_size, fat);
	if (err) {
		printf("WHYRED MODEM: FAT read failed %d\n", err);
		return err;
	}

	/* Root directory: 512 entries. */
	root_bytes = fat_root_entries * 32;
	root = fat + fat_bytes;
	err = emmc_read(modem_lba + fat_root_start, root_bytes / SECTOR_SIZE,
	    root);
	if (err) {
		printf("WHYRED MODEM: root directory read failed %d\n", err);
		return err;
	}

	cluster = 0;
	for (uint32_t i = 0; i < fat_root_entries; i++) {
		const uint8_t *e = root + i * 32;
		char name[13];

		if (e[0] == 0x00) {
			break;
		}
		if (e[0] == 0xe5 || e[0x0b] == 0x0f) {
			continue;
		}
		fat_name(e, name, sizeof(name));
		if (strcasecmp(name, "IMAGE") == 0 && (e[0x0b] & 0x10)) {
			cluster = le16(e + 0x1a) | (le16(e + 0x14) << 16);
			break;
		}
	}
	if (cluster < 2) {
		printf("WHYRED MODEM: IMAGE/ not found in the modem FAT\n");
		return ENOENT;
	}

	/* IMAGE/: follow the chain into the remaining cached space. */
	off = fat_bytes + root_bytes;
	n = 0;
	while (cluster >= 2 && cluster < FAT_MAX_CLUSTER) {
		if (off + fat_cluster_bytes > FAT_DIR_MAX) {
			printf("WHYRED MODEM: IMAGE/ exceeds the cache\n");
			return ENOMEM;
		}
		err = emmc_read(modem_lba + fat_cluster_sector(cluster),
		    fat_spc, (uint8_t *)fat_buf + off);
		if (err) {
			printf("WHYRED MODEM: IMAGE/ read failed %d\n", err);
			return err;
		}
		off += fat_cluster_bytes;
		n++;
		cluster = fat_next(cluster);
	}
	fat_image = fat + fat_bytes + root_bytes;
	fat_image_len = off - (fat_bytes + root_bytes);
	printf("WHYRED MODEM: modem FAT: %u image clusters, %u bytes cached\n", n,
	    fat_image_len);
	return 0;
}

static int
fat_lookup(const char *name, uint32_t *cluster, uint32_t *size)
{
	for (uint32_t i = 0; i < fat_image_len / 32; i++) {
		const uint8_t *e = fat_image + i * 32;
		char entry[13];

		if (e[0] == 0x00) {
			break;
		}
		if (e[0] == 0xe5 || e[0x0b] == 0x0f || (e[0x0b] & 0x10)) {
			continue;
		}
		fat_name(e, entry, sizeof(entry));
		if (strcasecmp(entry, name) == 0) {
			*cluster = le16(e + 0x1a) | (le16(e + 0x14) << 16);
			*size = le32(e + 0x1c);
			return 0;
		}
	}
	return ENOENT;
}

/* Stream one FAT16 file into an uncached mapping (the modem region). */
static int
fat_read_to_va(const char *name, vm_offset_t dst, uint32_t max)
{
	uint32_t cluster, size, off = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("WHYRED MODEM: %s not found\n", name);
		return err;
	}
	if (size > max) {
		printf("WHYRED MODEM: %s is %u bytes, max %u\n", name, size, max);
		return EINVAL;
	}
	printf("WHYRED MODEM: %s %u bytes at va %08x\n", name, size,
	    (uint32_t)dst);
	while (off < size) {
		uint32_t n = MIN(size - off, fat_cluster_bytes);

		err = emmc_read(modem_lba + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("WHYRED MODEM: %s read failed %d at %u\n", name,
			    err, off);
			return err;
		}
		io_copy(dst + off, bounce, n);
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("WHYRED MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				return EIO;
			}
		}
	}
	return 0;
}

/* Read one FAT16 file into a cached buffer (MDT and other small files). */
static int
fat_read_to_buf(const char *name, void *dst, uint32_t max, uint32_t *out)
{
	uint32_t cluster, size, off = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("WHYRED MODEM: %s not found\n", name);
		return err;
	}
	if (size > max) {
		printf("WHYRED MODEM: %s is %u bytes, max %u\n", name, size, max);
		return EINVAL;
	}
	while (off < size) {
		uint32_t n = MIN(size - off, fat_cluster_bytes);

		err = emmc_read(modem_lba + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("WHYRED MODEM: %s read failed %d at %u\n", name,
			    err, off);
			return err;
		}
		memcpy((uint8_t *)dst + off, bounce, n);
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("WHYRED MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				return EIO;
			}
		}
	}
	*out = size;
	return 0;
}

/* Stream one segment blob; its directory size must match p_filesz. */
static int
fat_read_blob(const char *name, vm_offset_t dst, uint32_t want)
{
	uint32_t cluster, size, off = 0;
	int err;

	err = fat_lookup(name, &cluster, &size);
	if (err) {
		printf("WHYRED MODEM: %s not found\n", name);
		return err;
	}
	if (size != want) {
		printf("WHYRED MODEM: %s is %u bytes, expected %u\n", name, size,
		    want);
		return EINVAL;
	}
	while (off < size) {
		uint32_t n = MIN(size - off, fat_cluster_bytes);

		err = emmc_read(modem_lba + fat_cluster_sector(cluster),
		    fat_spc, bounce);
		if (err) {
			printf("WHYRED MODEM: %s read failed %d at %u\n", name,
			    err, off);
			return err;
		}
		io_copy(dst + off, bounce, n);
		off += n;
		if (off < size) {
			cluster = fat_next(cluster);
			if (cluster < 2 || cluster >= FAT_MAX_CLUSTER) {
				printf("WHYRED MODEM: %s chain ends at %u/%u\n",
				    name, off, size);
				return EIO;
			}
		}
	}
	return 0;
}

/* --- MBA ------------------------------------------------------------------ */

static uint64_t
modem_scm7(uint32_t fn, uint32_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t extra_pa, uint64_t *res)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo,
	    x2 __asm__("x2") = a0, x3 __asm__("x3") = a1,
	    x4 __asm__("x4") = a2, x5 __asm__("x5") = extra_pa,
	    x6 __asm__("x6") = 0;

	for (int n = 0; n < 1000; n++) {
		__asm__ volatile ("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
		      "+r"(x5), "+r"(x6)
		    :
		    : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
		      "x15", "x16", "x17", "memory");
		if (x0 != 1) {
			break;
		}
		x0 = 1;
		x1 = arginfo;
		x2 = a0;
		x3 = a1;
		x4 = a2;
		x5 = extra_pa;
		x6 = 0;
	}
	*res = x1;
	return x0;
}

static int
modem_q6_reset(void)
{
	uint32_t v, mem;
	int i;

	modem_announce("run the QDSP6 power-up and reset sequence");
	v = mrd(q6, Q6_RESET);
	mwr(q6, Q6_RESET, v | Q6SS_STOP_CORE | Q6SS_CORE_ARES | Q6SS_BUS_ARES_ENA);

	/* BHS requires the XO branch clock. */
	v = mrd(q6, Q6_XO_CBCR);
	mwr(q6, Q6_XO_CBCR, v | 1u);
	for (i = 0; i < 200; i++) {
		if (!(mrd(q6, Q6_XO_CBCR) & CBCR_OFF)) {
			break;
		}
		delay(1);
	}
	if (i == 200) {
		printf("WHYRED MODEM: xo_cbcr did not come on (%08x)\n",
		    mrd(q6, Q6_XO_CBCR));
		return ETIMEDOUT;
	}
	printf("WHYRED MODEM: q6 xo_cbcr %08x\n", mrd(q6, Q6_XO_CBCR));

	v = mrd(q6, Q6_PWR_CTL);
	mwr(q6, Q6_PWR_CTL, v | QDSP6v55_BHS_ON);
	__asm__ volatile("dsb sy" ::: "memory");
	delay(1);
	for (i = 0; i < 200; i++) {
		if (mrd(q6, Q6V62_BHS_STATUS) & QDSP6v55_BHS_EN_REST_ACK) {
			break;
		}
		delay(1);
	}
	if (i == 200) {
		printf("WHYRED MODEM: BHS_EN_REST_ACK not set (%08x)\n",
		    mrd(q6, Q6V62_BHS_STATUS));
		return ETIMEDOUT;
	}
	printf("WHYRED MODEM: q6 bhs62 %08x\n", mrd(q6, Q6V62_BHS_STATUS));

	v = mrd(q6, Q6_PWR_CTL);
	v |= QDSP6v55_LDO_BYP;
	mwr(q6, Q6_PWR_CTL, v);
	v &= ~QDSP6v55_CLAMP_QMC_MEM;
	mwr(q6, Q6_PWR_CTL, v);
	v |= Q6SS_L2DATA_STBY_N | Q6SS_SLP_RET_N;
	mwr(q6, Q6_PWR_CTL, v);

	/* Turn on the memories one bit at a time (q6v62-1-5: bits 29..0). */
	mem = mrd(q6, Q6V6_MEM_PWR_CTL);
	for (i = 29; i >= 0; i--) {
		mem |= (1u << i);
		mwr(q6, Q6V6_MEM_PWR_CTL, mem);
		(void)mrd(q6, Q6V6_MEM_PWR_CTL);
		delay(1);
	}

	v = mrd(q6, Q6_PWR_CTL);
	v &= ~QDSP6v55_CLAMP_WL;
	mwr(q6, Q6_PWR_CTL, v);
	v &= ~Q6SS_CLAMP_IO;
	mwr(q6, Q6_PWR_CTL, v);

	v = mrd(q6, Q6_RESET);
	v &= ~(Q6SS_CORE_ARES | Q6SS_STOP_CORE);
	mwr(q6, Q6_RESET, v);

	v = mrd(q6, Q6_GFMUX_CTL);
	v |= Q6SS_CLK_ENA;
	mwr(q6, Q6_GFMUX_CTL, v);
	printf("WHYRED MODEM: q6 reset %08x gfmux %08x pwr %08x mem_pwr %08x\n",
	    mrd(q6, Q6_RESET), mrd(q6, Q6_GFMUX_CTL), mrd(q6, Q6_PWR_CTL),
	    mrd(q6, Q6V6_MEM_PWR_CTL));
	return 0;
}

static int
modem_boot_mba(void)
{
	uint32_t status = 0;
	int err, i;

	modem_announce("read MBA.MBN from the modem partition");
	io_zero(mba, MBA_ASSIGN_SIZE);
	err = fat_read_to_va("MBA.MBN", mba, MBA_ASSIGN_SIZE);
	if (err) {
		return err;
	}
	if (mrd(mba, 0) != 0x464c457fu) {
		printf("WHYRED MODEM: MBA.MBN has no ELF magic (%08x)\n",
		    mrd(mba, 0));
		return EINVAL;
	}

	modem_announce("assign the MBA region to MSS");
	err = scm_assign(MBA_PA, MBA_ASSIGN_SIZE, VMID_HLOS, VMID_MSS_MSA,
	    PERM_READ | PERM_WRITE);
	if (err) {
		return EIO;
	}

	modem_announce("program RMB and reset the Q6");
	mwr(rmb, RMB_MBA_IMAGE, MBA_PA);
	mwr(rmb, RMB_PMI_CODE_START, 0);
	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	printf("WHYRED MODEM: RMB image %08x code_start %08x code_len %08x\n",
	    mrd(rmb, RMB_MBA_IMAGE), mrd(rmb, RMB_PMI_CODE_START),
	    mrd(rmb, RMB_PMI_CODE_LENGTH));
	err = modem_q6_reset();
	if (err) {
		return err;
	}

	for (i = 0; i < 500; i++) {
		status = mrd(rmb, RMB_PBL_STATUS);
		if (status != 0) {
			break;
		}
		modem_wait(10);
	}
	printf("WHYRED MODEM: pbl status %08x after %d ms\n", status, i * 10);
	if (status != STATUS_PBL_SUCCESS) {
		printf("WHYRED MODEM: PBL did not report success\n");
		return EIO;
	}

	status = 0;
	for (i = 0; i < 500; i++) {
		status = mrd(rmb, RMB_MBA_STATUS);
		if (status != 0) {
			break;
		}
		modem_wait(10);
	}
	printf("WHYRED MODEM: mba status %08x after %d ms\n", status, i * 10);
	if (status != STATUS_XPU_UNLOCKED &&
	    status != STATUS_XPU_UNLOCKED_SCRIBBLED) {
		printf("WHYRED MODEM: MBA did not unlock\n");
		return EIO;
	}
	printf("WHYRED MODEM: MBA alive (pbl %08x, mba %08x)\n",
	    mrd(rmb, RMB_PBL_STATUS), mrd(rmb, RMB_MBA_STATUS));
	return 0;
}

/* --- MDT, segments and final authentication (F1 step 3) ------------------- */

#define MDT_BUF_SIZE 16384u

static int
modem_load_mdt(void)
{
	const uint8_t *mdt = mdt_buf;
	uint32_t len = 0;
	int32_t status = 0;
	int err, i;

	modem_announce("read MODEM.MDT and authenticate the header");
	err = fat_read_to_buf("MODEM.MDT", mdt_buf, MDT_BUF_SIZE, &len);
	if (err) {
		return err;
	}
	if (len < 0x34 || memcmp(mdt, "\177ELF", 4) != 0) {
		printf("WHYRED MODEM: MODEM.MDT is not the expected ELF\n");
		return EINVAL;
	}
	printf("WHYRED MODEM: MODEM.MDT %u bytes, entry %08x phnum %u\n", len,
	    le32(mdt + 24), le16(mdt + 44));

	io_zero(mba + MBA_ASSIGN_SIZE, 0x3000);
	io_copy(mba + MBA_ASSIGN_SIZE, mdt_buf, len);
	err = scm_assign(MBA_PA + MBA_ASSIGN_SIZE, 0x3000, VMID_HLOS,
	    VMID_MSS_MSA, PERM_READ | PERM_WRITE);
	if (err) {
		return EIO;
	}

	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	mwr(rmb, RMB_PMI_META_DATA, MBA_PA + MBA_ASSIGN_SIZE);
	mwr(rmb, RMB_MBA_COMMAND, CMD_META_DATA_READY);
	for (i = 0; i < 1000; i++) {
		status = (int32_t)mrd(rmb, RMB_MBA_STATUS);
		if (status == (int32_t)STATUS_META_DATA_AUTH_SUCCESS ||
		    status < 0) {
			break;
		}
		modem_wait(10);
	}
	printf("WHYRED MODEM: mdt auth status %d after %d ms\n", status, i * 10);
	if (status != (int32_t)STATUS_META_DATA_AUTH_SUCCESS) {
		printf("WHYRED MODEM: MBA rejected the MDT header\n");
		return EIO;
	}
	(void)scm_assign(MBA_PA + MBA_ASSIGN_SIZE, 0x3000, VMID_MSS_MSA,
	    VMID_HLOS, PERM_READ | PERM_WRITE | PERM_EXEC);
	return 0;
}

static int
modem_load_segments(void)
{
	const uint8_t *mdt = mdt_buf;
	const uint8_t *ph;
	uint32_t phoff, phnum, region_start = 0xffffffffu, region_end = 0;
	uint32_t i, len, paddr, filesz, memsz, status;
	vm_offset_t fw;
	int err;

	phoff = le32(mdt + 28);
	phnum = le16(mdt + 44);
	if (phoff < 0x34 || phnum == 0 || phoff + phnum * 32 > MDT_BUF_SIZE) {
		printf("WHYRED MODEM: MDT program headers are invalid\n");
		return EINVAL;
	}
	for (i = 0; i < phnum; i++) {
		ph = mdt + phoff + i * 32;
		if (le32(ph) != 1 ||
		    (le32(ph + 28) & (7u << 24)) == (2u << 24) ||
		    le32(ph + 20) == 0) {
			continue;
		}
		paddr = le32(ph + 12);
		memsz = le32(ph + 20);
		if (paddr < region_start) {
			region_start = paddr;
		}
		if (paddr + memsz > region_end) {
			region_end = paddr + memsz;
		}
	}
	region_start &= ~0xfffu;
	region_end = (region_end + 0xfffu) & ~0xfffu;
	if (region_start < MODEM_FW_PA ||
	    region_end > MODEM_FW_PA + MODEM_FW_SIZE ||
	    region_end <= region_start) {
		printf("WHYRED MODEM: image limits %08x..%08x outside the "
		    "carve-out\n", region_start, region_end);
		return EINVAL;
	}
	printf("WHYRED MODEM: modem image %08x..%08x (%u bytes)\n", region_start,
	    region_end, region_end - region_start);
	fw = ml_io_map(region_start, region_end - region_start);
	if (fw == 0) {
		printf("WHYRED MODEM: modem region map failed\n");
		return ENOMEM;
	}

	{
		uint32_t src[1] = { VMID_HLOS };
		uint32_t dst[2] = { VMID_HLOS, VMID_MSS_MSA };
		uint32_t perm[2] = { PERM_READ | PERM_WRITE,
		    PERM_READ | PERM_WRITE };

		err = scm_mem_assign(region_start, region_end - region_start,
		    src, 1, dst, perm, 2);
	}
	if (err) {
		return EIO;
	}

	len = 0;
	mwr(rmb, RMB_PMI_CODE_LENGTH, 0);
	for (i = 0; i < phnum; i++) {
		char name[16];
		vm_offset_t dst;

		ph = mdt + phoff + i * 32;
		if (le32(ph) != 1 ||
		    (le32(ph + 28) & (7u << 24)) == (2u << 24) ||
		    le32(ph + 20) == 0) {
			continue;
		}
		paddr = le32(ph + 12);
		filesz = le32(ph + 16);
		memsz = le32(ph + 20);
		if (paddr < region_start || paddr + memsz > region_end) {
			printf("WHYRED MODEM: blob %u is outside the image\n", i);
			return EINVAL;
		}
		dst = fw + (paddr - region_start);
		if (filesz != 0) {
			snprintf(name, sizeof(name), "MODEM.B%02u", i);
			err = fat_read_blob(name, dst, filesz);
			if (err) {
				return err;
			}
		}
		if (memsz > filesz) {
			io_zero(dst + filesz, memsz - filesz);
		}
		if (len == 0) {
			mwr(rmb, RMB_PMI_CODE_START, paddr);
			mwr(rmb, RMB_MBA_COMMAND, CMD_LOAD_READY);
		}
		len += memsz;
		mwr(rmb, RMB_PMI_CODE_LENGTH, len);
		status = mrd(rmb, RMB_MBA_STATUS);
		printf("WHYRED MODEM: blob %02u %08x filesz %u memsz %u len %08x "
		    "status %08x\n", i, paddr, filesz, memsz, len, status);
		if ((int32_t)status < 0) {
			printf("WHYRED MODEM: MBA rejected blob %u\n", i);
			return EIO;
		}
	}

	{
		uint32_t src[2] = { VMID_HLOS, VMID_MSS_MSA };
		uint32_t dst[1] = { VMID_MSS_MSA };
		uint32_t perm[1] = { PERM_READ | PERM_WRITE };

		err = scm_mem_assign(region_start, region_end - region_start,
		    src, 2, dst, perm, 1);
	}
	if (err) {
		return EIO;
	}
	return 0;
}

static int
modem_auth(void)
{
	int32_t status = 0;
	int i;

	modem_announce("wait for the MBA image authentication");
	for (i = 0; i < 1000; i++) {
		status = (int32_t)mrd(rmb, RMB_MBA_STATUS);
		if (status == (int32_t)STATUS_AUTH_COMPLETE || status < 0) {
			break;
		}
		modem_wait(10);
	}
	printf("WHYRED MODEM: auth status %d after %d ms\n", status, i * 10);
	if (status != (int32_t)STATUS_AUTH_COMPLETE) {
		printf("WHYRED MODEM: modem image authentication failed\n");
		return EIO;
	}
	(void)scm_assign(MBA_PA, MBA_ASSIGN_SIZE, VMID_MSS_MSA, VMID_HLOS,
	    PERM_READ | PERM_WRITE | PERM_EXEC);
	printf("WHYRED MODEM: auth complete, modem is running\n");
	return 0;
}

/* --- thread --------------------------------------------------------------- */

static void
whyred_modem_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t step = 0, delay_s = 25, clk_mask = 0x3f;

	(void)PE_parse_boot_argn("whyredmodem", &step, sizeof(step));
	(void)PE_parse_boot_argn("whyredmodemdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("whyredmodemclk", &clk_mask, sizeof(clk_mask));
	if (delay_s > 300) {
		delay_s = 300;
	}
	printf("WHYRED MODEM: F1 step %u in %u s, clock mask %02x\n", step,
	    delay_s, clk_mask);
	modem_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	rst = ml_io_map(MSS_RESTART_PA, MSS_RESTART_SIZE);
	if (gcc == 0 || rst == 0) {
		printf("WHYRED MODEM: GCC/restart map failed (gcc %p rst %p)\n",
		    (void *)gcc, (void *)rst);
		return;
	}
	modem_restart_deassert();
	modem_clocks(clk_mask);

	if (step < 1) {
		printf("WHYRED MODEM: step %u does nothing\n", step);
		return;
	}
	q6 = ml_io_map(QDSP6_PA, QDSP6_SIZE);
	rmb = ml_io_map(RMB_PA, RMB_SIZE);
	if (q6 == 0 || rmb == 0) {
		printf("WHYRED MODEM: QDSP6/RMB map failed (q6 %p rmb %p)\n",
		    (void *)q6, (void *)rmb);
		return;
	}
	modem_dump_q6();

	if (step < 2) {
		printf("WHYRED MODEM: step 1 done (no firmware loaded)\n");
		return;
	}

	mba = ml_io_map(MBA_PA, MBA_SIZE);
	if (mba == 0) {
		printf("WHYRED MODEM: MBA map failed\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, (vm_offset_t *)&fat_buf, FAT_DIR_MAX,
	    PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("WHYRED MODEM: no contiguous memory for the FAT cache\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, (vm_offset_t *)&bounce,
	    FAT_BOUNCE_SIZE, PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, (vm_offset_t *)&scm_buf,
	    SCM_BUF_SIZE, PAGE_MASK, 0xff000, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("WHYRED MODEM: no contiguous bounce/SCM buffer\n");
		return;
	}
	{
		ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)scm_buf);

		if (pn == 0) {
			printf("WHYRED MODEM: SCM buffer has no physical page\n");
			return;
		}
		scm_pa = ptoa_64(pn);
	}

	if (fat_open() == 0) {
		int err = modem_boot_mba();

		if (err) {
			printf("WHYRED MODEM: step 2 failed %d\n", err);
		} else if (step >= 3) {
			if (kmem_alloc(kernel_map, (vm_offset_t *)&mdt_buf,
			    MDT_BUF_SIZE, KMA_DATA | KMA_ZERO,
			    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				printf("WHYRED MODEM: no memory for the MDT\n");
			} else if (modem_load_mdt() == 0 &&
			    modem_load_segments() == 0) {
				(void)modem_auth();
			}
		}
	}
	printf("WHYRED MODEM: step %u finished\n", step);
}

int whyred_modem_probe(void);

int
whyred_modem_probe(void)
{
	static int started;
	uint32_t step = 0;
	thread_t thread;

	if (started ||
	    !PE_parse_boot_argn("whyredmodem", &step, sizeof(step)) ||
	    step == 0) {
		return 0;
	}
	started = 1;
	if (kernel_thread_start(whyred_modem_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_WHYRED */

int whyred_modem_probe(void);
int
whyred_modem_probe(void)
{
	return 0;
}

#endif
