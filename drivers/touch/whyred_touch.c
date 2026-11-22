/*
 * WHYRED touch bring-up, step 1: does the panel's touch controller answer on
 * the BLSP1 QUP1 bus at all?
 *
 * whyred has no I2C controller driver in this kernel, so this file does not
 * touch the QUP engine. It re-muxes the two bus lines as plain TLMM GPIOs
 * (SDA GPIO 2, SCL GPIO 3, both blsp_i2c1 in the whyred DTS) and bit-bangs
 * I2C at roughly 100 kHz. That is enough to answer one question per boot: is
 * the controller powered, out of reset and acknowledging its address? The
 * Novatek chip ID is read in the same run. A QUP driver and the HID report
 * path come only after an address answers.
 *
 * Register layout follows the vendor pinctrl-sdm660 tables: one 0x1000
 * window per pin, CTRL at +0x0 (mux bits 2..5, pull bits 0..1, drive bits
 * 6..8, output enable bit 9) and IO at +0x4 (bit 0 pad level, bit 1 driven
 * value). GPIO 2/3 live in the SOUTH tile (TLMM + 0x100000); the reset/IRQ
 * pair lives in NORTH (TLMM + 0x900000), both relative to the vendor
 * pinctrl@03000000 window. The internal pull-up is used
 * because whyred has no external pull resistors on this bus.
 *
 * Boot-args:
 *   sweetouchi2c=1    enable the probe
 *   sweeti2c=1        after the bit-bang scan, bring up the BLSP1 QUP1
 *                     hardware master (bsd/dev/sweet_i2c.c) and read the
 *                     DSX through it instead of bit-banging
 *   sweetouchreset=1  drive reset (GPIO 66) low for 10 ms and high for
 *                     60 ms before the scan; use it only in the boot that
 *                     asks whether the bootloader hand-off left the
 *                     controller held in reset
 *   sweetouchwait=N   hold the panel with the result for N seconds after
 *                     the scan (default 15, 0 continues at once, max 60)
 * The 0184 ring showed exactly one acknowledging address, 0x20. That is a
 * Synaptics part, not the Novatek 0x62/0x01 pair, so 0186 adds a real RMI4
 * read (two-byte register address, no page prefix), dumps the version
 * register, the function descriptor table and the F01/F12 query blocks.
 *
 *   sweetouchtouch=N  poll F12 touch data for N seconds: the top band of
 *                     the panel is blue while waiting and turns green on the
 *                     first change, and every change is written to the ring
 *   sweetouchhalt=1   diagnostic: paint the answer on the panel and stop
 *                     the boot there, repeating the result line forever.
 *                     The console stops reaching the panel somewhere around
 *                     the framebuffer hand-off, so the answer is written
 *                     straight into the scanout buffer instead of relying
 *                     on printf being visible.
 *
 * The probe is read-mostly on purpose: with no reset pulse it does not touch
 * GPIO 66/67 at all, so the log still describes the state ABL handed over.
 */
#if defined(ARM64_BOARD_CONFIG_WHYRED)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/kernel.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>

/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size,
    uint32_t flags);

/*
 * bsd/dev/sweet_i2c.c: the BLSP1 QUP1 hardware master. The bit-bang below
 * cannot make the DSX take a register address, so when sweeti2c=1 the probe
 * hands the same transactions to the QUP after the address scan.
 */
extern int sweet_i2c_init(void);
extern int sweet_i2c_xfer(uint8_t addr, const uint8_t *w, int wl, uint8_t *r,
    int rl);

/* iokit/Kernel/arm/hid/SweetHID.cpp: the digitizer event service. */
extern void sweet_hid_touch(int finger, int down, int x, int y);

#define WHYRED_TLMM_BASE	0x03000000ULL	/* vendor DTS: pinctrl@03000000 */
#define WHYRED_TLMM_SOUTH	0x00100000u	/* GPIO 0..15 */
#define WHYRED_TLMM_NORTH	0x00900000u	/* GPIO 66..67 */

#define TLMM_REG_SIZE		0x1000u
#define TLMM_CTRL		0x000u
#define TLMM_IO			0x004u
#define TLMM_OE			(1u << 9)

#define WHYRED_SDA		2
#define WHYRED_SCL		3
#define WHYRED_RST		66
#define WHYRED_IRQ		67

#define TOUCH_I2C_DELAY_US	10	/* half a bit, ~50 kHz. Ring 0198 read
					 * real bytes at 10 us; ring 0199 read
					 * all zero at 100 us, so slower is not
					 * automatically safer */

static inline uint32_t
tlmm_rd(vm_offset_t addr)
{
	uint32_t v = *(volatile uint32_t *)addr;

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
tlmm_wr(vm_offset_t addr, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)addr = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

struct touch_pin {
	vm_offset_t cfg;
	vm_offset_t io;
	uint32_t idle;		/* CTRL with mux=gpio, pull-up, OE=0 */
};

static struct touch_pin touch_sda, touch_scl;
static int touch_probe_done;

/*
 * Re-mux one pad as a plain GPIO with the internal pull-up on. Returns the
 * CTRL value so output/input transitions do not have to re-read it.
 */
static uint32_t
pin_gpio(vm_offset_t ctl)
{
	uint32_t v = tlmm_rd(ctl);

	v &= ~(0xfu << 2);	/* mux 0: plain GPIO, not blsp_i2c1 */
	v &= ~(3u << 0);
	v |= (3u << 0);		/* pull-up */
	v &= ~(7u << 6);
	v |= (3u << 6);		/* drive strength 3 */
	v &= ~TLMM_OE;		/* start as input */
	tlmm_wr(ctl, v);
	return v;
}

static void
pin_setup(struct touch_pin *p, vm_offset_t tile, unsigned pin)
{
	p->cfg = tile + TLMM_REG_SIZE * pin;
	p->io = p->cfg + (TLMM_IO - TLMM_CTRL);
	p->idle = pin_gpio(p->cfg);
}

static void
pin_out(struct touch_pin *p, int value)
{
	uint32_t io = tlmm_rd(p->io);

	/* Value first, then output enable: no glitch on the bus. */
	io = (io & ~(1u << 1)) | (value ? (1u << 1) : 0u);
	tlmm_wr(p->io, io);
	tlmm_wr(p->cfg, p->idle | TLMM_OE);
}

static void
pin_in(struct touch_pin *p)
{
	tlmm_wr(p->cfg, p->idle);
}

static int
pin_read(struct touch_pin *p)
{
	return (tlmm_rd(p->io) & 1u) != 0;
}

/*
 * Open-drain bit-bang. Only ever drive a line low; release it (input with
 * the internal pull-up) for a high. A push-pull high would fight a slave
 * that holds SCL low to stretch the clock - and a Synaptics controller does
 * stretch while it fetches a register - so every high is a release followed
 * by a wait for the line to actually rise. A timeout keeps a stuck bus from
 * hanging the boot.
 */
#define TOUCH_LINE_WAIT_US	2000

static void
bb_delay(void)
{
	delay(TOUCH_I2C_DELAY_US);
}

static void
sda_low(void)
{
	pin_out(&touch_sda, 0);
}

static void
scl_low(void)
{
	pin_out(&touch_scl, 0);
}

static int
sda_release(void)
{
	int i;

	pin_in(&touch_sda);
	for (i = 0; i < TOUCH_LINE_WAIT_US; i++) {
		if (pin_read(&touch_sda)) {
			return 1;
		}
		delay(1);
	}
	return 0;
}

static int
scl_release(void)
{
	int i;

	pin_in(&touch_scl);
	for (i = 0; i < TOUCH_LINE_WAIT_US; i++) {
		if (pin_read(&touch_scl)) {
			return 1;
		}
		delay(1);
	}
	return 0;
}

static void
i2c_start(void)
{
	sda_release();
	bb_delay();
	scl_release();
	bb_delay();
	sda_low();
	bb_delay();
	scl_low();
	bb_delay();
}

static void
i2c_stop(void)
{
	sda_low();
	bb_delay();
	scl_release();
	bb_delay();
	sda_release();
	bb_delay();
}

/* Returns 1 when the slave acknowledged the byte. */
static int
i2c_write(uint8_t byte)
{
	int bit, ack;

	for (bit = 7; bit >= 0; bit--) {
		if ((byte >> bit) & 1) {
			sda_release();
		} else {
			sda_low();
		}
		bb_delay();
		scl_release();
		bb_delay();
		scl_low();
		bb_delay();
	}
	sda_release();
	bb_delay();
	scl_release();
	bb_delay();
	ack = !pin_read(&touch_sda);
	scl_low();
	bb_delay();
	return ack;
}

static uint8_t
i2c_read(int ack)
{
	uint8_t value = 0;
	int bit;

	sda_release();
	bb_delay();
	for (bit = 0; bit < 8; bit++) {
		scl_release();
		bb_delay();
		value = (uint8_t)((value << 1) | (pin_read(&touch_sda) ? 1 : 0));
		scl_low();
		bb_delay();
	}
	if (ack) {
		sda_low();
	}
	bb_delay();
	scl_release();
	bb_delay();
	scl_low();
	bb_delay();
	sda_release();
	return value;
}

/*
 * Clock a slave out of a half-finished transfer: nine pulses with SDA
 * released. Only used when the idle bus is stuck low, which is the one case
 * where a scan would otherwise report nothing at all.
 */
static int
i2c_clear_bus(void)
{
	int i, pulsed = 0;

	sda_release();
	for (i = 0; i < 9 && !pin_read(&touch_sda); i++) {
		scl_release();
		bb_delay();
		scl_low();
		bb_delay();
		pulsed++;
	}
	if (pulsed) {
		i2c_stop();
	}
	return pulsed;
}

static int
i2c_ack(uint8_t addr)
{
	int ack;

	i2c_start();
	ack = i2c_write((uint8_t)(addr << 1));
	i2c_stop();
	return ack;
}

/*
 * Novatek register read: page select through 0xFF, then the byte offset
 * inside that page, then a repeated start and the data. The controller
 * answers at 0x62 in bootloader mode and at 0x01 once its firmware runs.
 */
static int
nvt_read(uint8_t addr, uint8_t page, uint8_t reg, uint8_t *buf, int len)
{
	int i, ok = 1;

	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	ok &= i2c_write(0xff);
	ok &= i2c_write(0x00);
	ok &= i2c_write(page);
	i2c_stop();
	if (!ok) {
		return EIO;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	ok &= i2c_write(reg);
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)((addr << 1) | 1));
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	for (i = 0; i < len; i++) {
		buf[i] = i2c_read(i + 1 < len);
	}
	i2c_stop();
	return 0;
}

/*
 * Synaptics DSX over I2C. The vendor driver
 * (drivers/input/touchscreen/synaptics_dsx/synaptics_dsx_i2c.c) selects a
 * page with a two-byte write 0xFF <page>, then writes the low address byte
 * and reads the data with a repeated start. The earlier RMI4 guess (two
 * address bytes, no 0xFF) was wrong: it wrote the second address byte into
 * register 0x00 of the chip.
 *
 * Register map used here, from synaptics_dsx_core.h: the page descriptor
 * table starts at 0x00E9 and steps down by 6 bytes per entry; each entry is
 * query base (2 bytes, big endian), command base, control base, data base and
 * a function number in the low six bits of the last byte.
 */
#define DSX_ADDR		0x20
#define DSX_PDT_START		0x00e9
#define DSX_PDT_END		0x00d0
#define DSX_PDT_ENTRY_SIZE	0x0006
#define DSX_F01_QUERY_LEN	21
#define DSX_PRODUCT_ID_OFFSET	11
#define DSX_PRODUCT_ID_SIZE	10

/*
 * Every framing we can build for "read register <reg>" leaves the same trace
 * in the 0187/0188 logs: the bytes look like a coherent page descriptor table
 * but the requested address is ignored (0x00e9 -> 27 00 09 00 21 34, the entry
 * that lives at 0x00e9, then 0x00e9 again -> the next six bytes). The chip
 * only ever streams from wherever its pointer happens to be, so the address
 * phase never lands.
 *
 * That leaves the framing itself as the suspect, and there are two variants
 * nobody has tried: a 16-bit address sent little endian, and the page select
 * and the address in one write. This table enumerates all of them; 0189 tries
 * each against the one fact we know for certain - the page descriptor table
 * starts at 0x00e9 with the F34 entry 27 00 09 00 21 34 - and keeps the first
 * framing that returns it twice in a row.
 */
enum dsx_frame {
	DFRAME_PAGE_A1,		/* 0xFF page, stop; address byte, restart */
	DFRAME_PAGE_A1_COMB,	/* 0xFF page address, one write, restart */
	DFRAME_PAGE_A2_BE,	/* page; 16-bit address, big endian */
	DFRAME_PAGE_A2_LE,	/* page; 16-bit address, little endian */
	DFRAME_A1,		/* address byte, no page select */
	DFRAME_A2_BE,		/* 16-bit address, big endian, no page */
	DFRAME_A2_LE,		/* 16-bit address, little endian, no page */
	DFRAME_PAGE_A1_DUMMY,	/* page; address byte + dummy 0x00, restart */
	DFRAME_COUNT
};

static const char *dsx_frame_name[DFRAME_COUNT] = {
	"page|a1", "page+a1", "page|a2be", "page|a2le",
	"a1", "a2be", "a2le", "page|a1dup",
};

/* The vendor driver's framing until the self test says otherwise. */
static int dsx_frame = DFRAME_PAGE_A1;

/* Nonzero once the QUP1 master owns the bus; all DSX reads then use it. */
static int dsx_qup;

/*
 * The DSX keeps its register page until it is told otherwise, exactly like
 * the vendor driver's synaptics_rmi4_i2c_set_page(): current_page starts at
 * 0xFF and 0xFF <page> goes out only when the page differs from the last one.
 * Writing the page before every read (what this probe used to do) puts the
 * part back into page 0 mid-stream and is not what the working driver does.
 * A controller reset returns it to page 0, so drop the cache there too.
 */
static int dsx_page = -1;

/*
 * The same seven framings, built as QUP transactions. Each /page/ prefix is a
 * separate write transaction with a real stop; the address phase and the read
 * are one write-without-stop followed by a read-with-stop, which is the
 * repeated start the DSX asks for and the one thing the bit-bang cannot get
 * right. A failed write returns before the read so a NACK names the phase.
 */
static int
dsx_read_q(uint8_t addr, uint16_t reg, uint8_t *buf, int len, int frame)
{
	uint8_t p[3], w[3];
	uint8_t hi = (uint8_t)((reg >> 8) & 0xff);
	uint8_t lo = (uint8_t)(reg & 0xff);
	int wl, rc;

	if ((frame == DFRAME_PAGE_A1 || frame == DFRAME_PAGE_A2_BE ||
	    frame == DFRAME_PAGE_A2_LE) && hi != dsx_page) {
		p[0] = 0xff;
		p[1] = hi;
		rc = sweet_i2c_xfer(addr, p, 2, NULL, 0);
		if (rc != 0) {
			return rc;
		}
		dsx_page = hi;
	}
	wl = 0;
	switch (frame) {
	case DFRAME_PAGE_A1:
	case DFRAME_A1:
		w[wl++] = lo;
		break;
	case DFRAME_PAGE_A1_COMB:
		w[wl++] = 0xff;
		w[wl++] = hi;
		w[wl++] = lo;
		break;
	case DFRAME_PAGE_A2_BE:
	case DFRAME_A2_BE:
		w[wl++] = hi;
		w[wl++] = lo;
		break;
	case DFRAME_PAGE_A2_LE:
	case DFRAME_A2_LE:
		w[wl++] = lo;
		w[wl++] = hi;
		break;
	default:
		return EINVAL;
	}
	rc = sweet_i2c_xfer(addr, w, wl, buf, len);
	if (rc == 0 && frame == DFRAME_PAGE_A1_COMB) {
		/* the combined write carries the page select */
		dsx_page = hi;
	}
	return rc;
}

static int
dsx_read_f(uint8_t addr, uint16_t reg, uint8_t *buf, int len, int frame)
{
	uint8_t hi = (uint8_t)((reg >> 8) & 0xff);
	uint8_t lo = (uint8_t)(reg & 0xff);
	int i, ok = 1;

	if (dsx_qup) {
		return dsx_read_q(addr, reg, buf, len, frame);
	}

	if ((frame == DFRAME_PAGE_A1 || frame == DFRAME_PAGE_A1_DUMMY ||
	    frame == DFRAME_PAGE_A2_BE ||
	    frame == DFRAME_PAGE_A2_LE) && hi != dsx_page) {
		/* page select: 0xFF <page>, its own transaction */
		i2c_start();
		ok &= i2c_write((uint8_t)(addr << 1));
		ok &= i2c_write(0xff);
		ok &= i2c_write(hi);
		i2c_stop();
		if (!ok) {
			return EIO;
		}
		dsx_page = hi;
	}

	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	switch (frame) {
	case DFRAME_PAGE_A1:
	case DFRAME_A1:
		ok &= i2c_write(lo);
		break;
	case DFRAME_PAGE_A1_DUMMY:
		/* The a2le accident: the extra byte is swallowed as data, but
		 * it changes what the next repeated start reads. */
		ok &= i2c_write(lo);
		ok &= i2c_write(0x00);
		break;
	case DFRAME_PAGE_A1_COMB:
		/* the vendor write, with the address appended to it */
		ok &= i2c_write(0xff);
		ok &= i2c_write(hi);
		ok &= i2c_write(lo);
		break;
	case DFRAME_PAGE_A2_BE:
	case DFRAME_A2_BE:
		ok &= i2c_write(hi);
		ok &= i2c_write(lo);
		break;
	case DFRAME_PAGE_A2_LE:
	case DFRAME_A2_LE:
		ok &= i2c_write(lo);
		ok &= i2c_write(hi);
		break;
	default:
		ok = 0;
		break;
	}
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	if (frame == DFRAME_PAGE_A1_COMB) {
		/* the combined write carries the page select */
		dsx_page = hi;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)((addr << 1) | 1));
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	for (i = 0; i < len; i++) {
		buf[i] = i2c_read(i + 1 < len);
	}
	i2c_stop();
	return 0;
}

static int
dsx_read(uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	return dsx_read_f(addr, reg, buf, len, dsx_frame);
}

/*
 * One addressed write, in the vendor's framing: START, address+W, register,
 * data, STOP. The page is taken from the cache and only sent when it really
 * changes, because ring 0192/0193/0194 showed that a stray FF <page> parks
 * the controller in an all-zero state.
 */
static int
dsx_write_f(uint8_t addr, uint16_t reg, const uint8_t *buf, int len)
{
	uint8_t hi = (uint8_t)((reg >> 8) & 0xff);
	uint8_t lo = (uint8_t)(reg & 0xff);
	int i, ok = 1;

	if (dsx_qup) {
		/* the QUP port only implements reads so far */
		return EIO;
	}
	if (hi != dsx_page) {
		i2c_start();
		ok &= i2c_write((uint8_t)(addr << 1));
		ok &= i2c_write(0xff);
		ok &= i2c_write(hi);
		i2c_stop();
		if (!ok) {
			return EIO;
		}
		dsx_page = hi;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	ok &= i2c_write(lo);
	for (i = 0; i < len; i++) {
		ok &= i2c_write(buf[i]);
	}
	i2c_stop();
	return ok ? 0 : EIO;
}

/*
 * A framing is only believed when the same register reads back the same six
 * bytes twice and the entry one slot above it reads back something else. A
 * streaming pointer fails the first test, and an off-by-one framing fails the
 * second.
 */
static int
dsx_frame_works(int frame, uint8_t *out)
{
	uint8_t a[DSX_PDT_ENTRY_SIZE], b[DSX_PDT_ENTRY_SIZE];
	uint8_t c[DSX_PDT_ENTRY_SIZE];
	int i, j, differ = 0;

	if (dsx_read_f(DSX_ADDR, DSX_PDT_START, a, sizeof(a), frame) != 0 ||
	    dsx_read_f(DSX_ADDR, DSX_PDT_START, b, sizeof(b), frame) != 0 ||
	    dsx_read_f(DSX_ADDR, DSX_PDT_START - DSX_PDT_ENTRY_SIZE, c,
	    sizeof(c), frame) != 0) {
		return 0;
	}
	for (i = 0; i < (int)sizeof(a); i++) {
		if (a[i] != b[i]) {
			/*
			 * Print both reads: the interesting failure is not
			 * "unstable" in the abstract but which byte moved,
			 * because a streaming pointer shifts the whole block
			 * while a lost address bit usually changes one.
			 */
			printf("WHYRED TOUCH: %s unstable",
			    dsx_frame_name[frame]);
			for (j = 0; j < (int)sizeof(a); j++)
				printf(" %02x", a[j]);
			printf(" ->");
			for (j = 0; j < (int)sizeof(b); j++)
				printf(" %02x", b[j]);
			printf("\n");
			return 0;
		}
		if (a[i] != c[i]) {
			differ = 1;
		}
	}
	if (!differ || (!a[0] && !a[5])) {
		return 0;
	}
	for (i = 0; i < (int)sizeof(a); i++) {
		out[i] = a[i];
	}
	return 1;
}

/* Hex plus printable text: the product and build strings are ASCII. */
static void
dsx_dump(const char *what, uint16_t reg, const uint8_t *b, int len)
{
	char text[32];
	int i;

	printf("WHYRED TOUCH: dsx %s [0x%04x]", what, reg);
	for (i = 0; i < len; i++) {
		printf(" %02x", b[i]);
	}
	printf(" |");
	for (i = 0; i < len && i < (int)sizeof(text) - 1; i++) {
		text[i] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
	}
	text[i] = 0;
	printf("%s|\n", text);
}

struct dsx_funcs {
	uint16_t f01_query;
	uint16_t f12_query;
	uint16_t f12_ctrl;
	uint16_t f12_data;
	int found_f01;
	int found_f12;
	int entries;
};

/*
 * Read-only identification. No control register is written, so a wrong
 * address cannot wedge the controller: the page table, the F01 query block
 * (manufacturer, product string, build id) and the F12 query block are all
 * plain reads.
 */
static void
dsx_identify(struct dsx_funcs *f)
{
	uint8_t b[DSX_F01_QUERY_LEN];
	uint16_t addr;

	f->f01_query = 0;
	f->f12_query = 0;
	f->f12_ctrl = 0;
	f->f12_data = 0;
	f->found_f01 = 0;
	f->found_f12 = 0;
	f->entries = 0;

	for (addr = DSX_PDT_START; addr > DSX_PDT_END;
	    addr = (uint16_t)(addr - DSX_PDT_ENTRY_SIZE)) {
		unsigned fn;

		if (dsx_read(DSX_ADDR, addr, b, DSX_PDT_ENTRY_SIZE) != 0) {
			printf("WHYRED TOUCH: dsx pdt read failed at 0x%04x\n",
			    addr);
			return;
		}
		if (!b[0] && !b[1] && !b[2] && !b[3] && !b[4] && !b[5]) {
			continue;	/* unused slot */
		}
		fn = b[5] & 0x3f;
		f->entries++;
		printf("WHYRED TOUCH: dsx fn %02x query %02x cmd %02x "
		    "ctrl %02x data %02x intr %02x\n", fn, b[0], b[1], b[2],
		    b[3], b[4]);
		if (fn == 0x01 && !f->found_f01) {
			f->f01_query = b[0];
			f->found_f01 = 1;
		} else if (fn == 0x12 && !f->found_f12) {
			f->f12_query = b[0];
			f->f12_ctrl = b[2];
			f->f12_data = b[3];
			f->found_f12 = 1;
		}
	}

	if (!f->found_f01) {
		printf("WHYRED TOUCH: dsx no F01 in the page table\n");
		return;
	}
	if (dsx_read(DSX_ADDR, f->f01_query, b, DSX_F01_QUERY_LEN) != 0) {
		printf("WHYRED TOUCH: dsx F01 query read failed\n");
		return;
	}
	dsx_dump("f01", f->f01_query, b, DSX_F01_QUERY_LEN);
	printf("WHYRED TOUCH: dsx manufacturer %u product \"%.*s\" build "
	    "%02x%02x%02x\n", b[0], DSX_PRODUCT_ID_SIZE,
	    &b[DSX_PRODUCT_ID_OFFSET], b[18], b[19], b[20]);

	if (f->found_f12) {
		if (dsx_read(DSX_ADDR, f->f12_query, b, 12) == 0) {
			dsx_dump("f12q", f->f12_query, b, 12);
		}
		printf("WHYRED TOUCH: dsx f12 ctrl 0x%04x data 0x%04x\n",
		    f->f12_ctrl, f->f12_data);
	}
}

/* Painted band across the top of the panel; defined with panel_report below. */
static void panel_band(uint32_t colour);

/*
 * Address-framing matrix. Every framing is tried against the one register
 * whose contents we know for certain, 0x00e9, and the first one that gives
 * back the same six bytes twice - and something else one slot above - is
 * kept for the rest of the probe. One boot therefore answers both "does the
 * address phase work at all" and, if it does, which framing the controller
 * wanted.
 */
static void
dsx_diag(void)
{
	uint8_t a[DSX_PDT_ENTRY_SIZE], b[DSX_PDT_ENTRY_SIZE];
	int frame, best = -1;

	printf("WHYRED TOUCH: framing matrix over %s\n",
	    dsx_qup ? "QUP1 (hardware I2C)" : "bit-bang");
	for (frame = 0; frame < DFRAME_COUNT; frame++) {
		if (dsx_frame_works(frame, a)) {
			dsx_dump(dsx_frame_name[frame], DSX_PDT_START, a,
			    sizeof(a));
			printf("WHYRED TOUCH: frame %s addressed\n",
			    dsx_frame_name[frame]);
			if (best < 0) {
				best = frame;
			}
		} else if (dsx_read_f(DSX_ADDR, DSX_PDT_START, b,
		    sizeof(b), frame) == 0) {
			dsx_dump(dsx_frame_name[frame], DSX_PDT_START, b,
			    sizeof(b));
			printf("WHYRED TOUCH: frame %s streams\n",
			    dsx_frame_name[frame]);
		} else {
			printf("WHYRED TOUCH: frame %s failed\n",
			    dsx_frame_name[frame]);
		}
	}
	if (best >= 0) {
		dsx_frame = best;
		printf("WHYRED TOUCH: using frame %s\n",
		    dsx_frame_name[best]);
	} else {
		printf("WHYRED TOUCH: no framing addressed the chip, keeping "
		    "%s\n", dsx_frame_name[dsx_frame]);
	}
}

/*
 * One-transaction page read.
 *
 * Ring 0201 settles what the matrix could not: the first read after the page
 * select and address write lands exactly on 0x00E9 (27 00 09 00 21 34, the
 * F34 record), and the very next read of the same register returns
 * 2f 26 0d 04 01 01, the F01 record six bytes on at 0x00E3. So the address
 * byte arrives intact, but the part keeps one read pointer and advances it by
 * the number of bytes returned; the next address write never lands.
 *
 * That makes addressing useless and a long read useful: one transfer returns
 * reg[n] .. reg[n+len-1] in order. Read a whole window and place it with the
 * F12 page-descriptor record, whose bytes are known, instead of guessing where
 * the window starts. F12's data register (0x06) then sits at a computed
 * offset, and the 2D header there is what a touch poll has to read.
 */
#define DSX_PAGE_LEN	128
static void
dsx_page_probe(void)
{
	static const uint8_t f12_rec[6] = {
		0x4c, 0x00, 0x13, 0x06, 0x01, 0x12
	};
	uint8_t page[DSX_PAGE_LEN];
	int base = -1, f12off, i, off, poll;

	if (dsx_read_f(DSX_ADDR, 0x0000, page, sizeof(page), dsx_frame) != 0) {
		printf("WHYRED TOUCH: page read failed\n");
		return;
	}
	printf("WHYRED TOUCH: page %d via %s\n", DSX_PAGE_LEN,
	    dsx_frame_name[dsx_frame]);
	for (i = 0; i < DSX_PAGE_LEN; i += 16) {
		int n = DSX_PAGE_LEN - i > 16 ? 16 : DSX_PAGE_LEN - i;

		dsx_dump("page", (uint16_t)i, page + i, n);
	}
	for (off = 0; off + 5 < DSX_PAGE_LEN; off++) {
		for (i = 0; i < 6; i++) {
			if (page[off + i] != f12_rec[i]) {
				break;
			}
		}
		if (i == 6) {
			base = DSX_PDT_START - off;
			break;
		}
	}
	if (base < 0) {
		printf("WHYRED TOUCH: no F12 record in the window, cannot "
		    "place F12\n");
		return;
	}
	printf("WHYRED TOUCH: F12 record at %d, window base 0x%04x\n",
	    off, base);
	f12off = 0x06 - base;
	if (f12off < 0 || f12off >= DSX_PAGE_LEN) {
		printf("WHYRED TOUCH: F12 data 0x0006 is outside the window\n");
		return;
	}
	printf("WHYRED TOUCH: F12 data at %d, first 0x%02x\n",
	    f12off, page[f12off]);
	for (poll = 0; poll < 10; poll++) {
		if (dsx_read_f(DSX_ADDR, 0x0000, page, sizeof(page),
		    dsx_frame) != 0) {
			printf("WHYRED TOUCH: poll %d failed\n", poll);
			break;
		}
		printf("WHYRED TOUCH: poll %d F12 0x%02x%s\n", poll,
		    page[f12off], (page[f12off] & 0x80) ? " valid" : "");
		delay(300000);
	}
}

/*
 * Read F12's 2D data block and report every change. The layout is not
 * assumed: the first sample is the idle snapshot, and a difference in any of
 * the eleven bytes means the controller is reporting something. The top band
 * is blue while nothing is seen and turns green on the first change, so the
 * panel itself answers "does the digitizer see my finger?" without a console.
 */
static struct touch_pin touch_irq;
static int touch_irq_ready;

/*
 * Ring 0194 settled the framing question and opened a different one. The
 * PDT read still lands exactly once, but the chip also answers every tap:
 * IRQ GPIO 67 moved on every touch while F12 data stayed zero. That is not a
 * dead controller, it is the vendor driver's wakeup-gesture (LPWG) state:
 * the part reports a gesture to wake the SoC and keeps the normal 2D data
 * block inactive until the host writes continuous reporting mode again.
 *
 * The recovery driver proves the exit path: synaptics_rmi4_resume() puts the
 * device back in normal operation (F01 control, bits [2:0] = 0) and then
 * synaptics_rmi4_f12_wg(false) writes the three F12 reporting-control bytes
 * with F12_CONTINUOUS_MODE. Ring 0194 also showed that the page-select write
 * FF <page> is what zeroes every later read, so 0195 stops sending it, spends
 * the one landing read on F01 control, and uses the wake write as the address
 * key before each F12 sample.
 *
 * The addresses are the ones the 0193/0194 PDT dumps and the recovery ring
 * agree on: F01 control 0x0D, F12 query 0x4C, F12 control 0x13, F12 data
 * 0x06, all on page 0.
 */
#define DSX_PDT_WINDOW	24
#define DSX_F12_FUNC	0x12
#define DSX_F01_CTRL	0x0d
#define DSX_F12_QUERY	0x4c
#define DSX_F12_CTRL	0x13
#define DSX_F12_DATA	0x06

/*
 * Every vendor read starts with the page select and then a repeated start.
 * 0195 showed that F01 control is writable but that reads after the write
 * return zeros, and the recovery driver shows the part is only sane in the
 * ~3 s between its probe and its first suspend into wakeup-gesture mode.
 * The way back to that state is the F01 command register: the vendor's
 * synaptics_rmi4_sw_reset() writes 0x01 there, waits
 * reset_delay_ms and rescans the PDT. 0196 does exactly that once, with the
 * bit-bang that now has a working register write.
 */
#define DSX_F01_CMD	0x06
#define DSX_F01_DATA	0x11
#define DSX_F01_QUERY	0x00

static int dsx_f12_found;
static int dsx_f12_query;
static int dsx_f12_ctrl;
static int dsx_f12_data;
static int dsx_f12_page;
static int dsx_f01_ctrl;
static int dsx_f01_cmd;
static int dsx_f01_data;
static int dsx_f01_query;
static uint8_t dsx_f01_normal = 0x80;	/* configured + normal operation */

/*
 * 0197: the window. The vendor driver probes the DSX at 0.92 s and registers
 * input2, then suspends into wakeup-gesture mode at 3.26 s; after that every
 * addressed read returns the same byte stream. XNU probed at the end of
 * santoni_sdhci_init(), i.e. after eMMC enumeration, and always missed it.
 * whyred_touch_early() runs at the top of that init, waits until
 * sweetouchearly=MS from kernel start and dumps the PDT there. The addresses
 * it finds are cached for the later touch test, which then does not reset the
 * part a second time.
 */
static int dsx_early_ran;
/*
 * 0200: band 6 (blue) showed the early probe stops inside dsx_reset_probe().
 * The late probe runs the same helper happily, so the marks are only painted
 * when the early probe sets this flag: one band per reset step and per PDT
 * record read tells which single bit-bang transaction never returns.
 */
static int dsx_early_marks_on;

/* Painted before any eMMC I/O exists; defined with the early probe below. */
static void touch_early_mark(int step, uint32_t color);
static int dsx_early_found;
static int dsx_f12_touch_reg;	/* data base + data0 offset, for polling */
static int dsx_f12_report_ctrl;	/* control base + ctrl28 offset */
static int dsx_f12_programmed;
static int dsx_f12_rec_size = 8;	/* status + x/y + z + wx/wy */
static int dsx_f12_fingers = 10;	/* ctrl23 max_reported_objects */
static int dsx_hid_started;
static int dsx_addressable;	/* F34 reads back twice after the reset */
static uint64_t dsx_early_start_ms;
static uint64_t dsx_early_end_ms;

static uint64_t
touch_now_ms(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}

static void
touch_wait_until_ms(uint32_t target_ms)
{
	while (touch_now_ms() < (uint64_t)target_ms) {
		delay(1000);
	}
}

/*
 * Force the two-byte page select before the read. The vendor caches the page
 * and 0191 copied that, but after a reset the first addressed read is the one
 * that matters, so every diagnostic read asks for page 0 explicitly.
 */
static int
dsx_read_page(uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	dsx_page = -1;
	return dsx_read_f(addr, reg, buf, len, dsx_frame);
}

/*
 * The vendor's rmi4_i2c_read() only sends FF <page> when the page changes.
 * Once the address phase is trusted, every F12 register read must use this
 * cached form; re-sending the page select before a read is what the 0191
 * ring showed parks the pointer.
 */
static int
dsx_read_cached(uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	return dsx_read_f(addr, reg, buf, len, dsx_frame);
}

static void
dsx_block(const char *label, uint16_t reg, int len)
{
	uint8_t b[32];

	if (len > (int)sizeof(b)) {
		len = (int)sizeof(b);
	}
	if (dsx_read_page(DSX_ADDR, reg, b, len) == 0) {
		dsx_dump(label, reg, b, len);
	} else {
		printf("WHYRED TOUCH: %s read at 0x%04x failed\n", label, reg);
	}
}

static int
dsx_wake_normal(void)
{
	int rc = dsx_write_f(DSX_ADDR, (uint16_t)dsx_f01_ctrl,
	    &dsx_f01_normal, 1);

	printf("WHYRED TOUCH: f01 ctrl 0x%04x <- 0x%02x %s\n",
	    dsx_f01_ctrl, dsx_f01_normal, rc == 0 ? "ack" : "nack");
	return rc;
}

static int
dsx_sw_reset_at(uint16_t base)
{
	uint8_t cmd = 0x01;
	int rc;

	dsx_page = -1;
	rc = dsx_write_f(DSX_ADDR, base, &cmd, 1);
	printf("WHYRED TOUCH: f01 cmd 0x%04x <- 0x01 %s, waiting 300 ms\n",
	    base, rc == 0 ? "ack" : "nack");
	if (rc == 0) {
		delay(300000);
	}
	return rc;
}

/*
 * The F01 command base is not 0x06 on this unit: the PDT record in the 0205
 * stream is 2f 26 0d 04 01 01, so the vendor's sw_reset writes 0x01 to 0x26.
 * 0196 and 0205 wrote it to 0x06, which is F12's *data* register, and the
 * controller never left its suspended/streaming state. The default is only a
 * fallback for the case where no stream was found at all.
 */
static int
dsx_sw_reset(void)
{
	uint16_t base = (uint16_t)(dsx_f01_cmd ? dsx_f01_cmd : DSX_F01_CMD);

	return dsx_sw_reset_at(base);
}

/*
 * The vendor scan: downwards from 0x00E9 in six-byte records, each one an
 * addressed read. The record is query, command, control, data, interrupt
 * count/version, function number. Stop at the 0x00 end marker instead of
 * reading a fixed window, so nothing past the table can be mistaken for a
 * function.
 */
static int
dsx_pdt_walk(const char *tag)
{
	uint8_t e[DSX_PDT_ENTRY_SIZE];
	uint16_t addr;
	int i, found = 0;

	dsx_f12_found = 0;
	dsx_f12_query = 0;
	dsx_f12_ctrl = 0;
	dsx_f12_data = 0;
	dsx_f12_page = 0;
	dsx_f01_ctrl = 0;
	dsx_f01_cmd = 0;
	dsx_f01_data = 0;
	dsx_f01_query = 0;

	addr = DSX_PDT_START;
	for (i = 0; i < 42; i++) {
		if (dsx_read_page(DSX_ADDR, addr, e, sizeof(e)) != 0) {
			printf("WHYRED TOUCH: %s pdt[0x%04x] read failed\n",
			    tag, addr);
			break;
		}
		printf("WHYRED TOUCH: %s pdt[0x%04x] %02x %02x %02x %02x "
		    "%02x %02x fn %02x\n", tag, addr, e[0], e[1], e[2], e[3],
		    e[4], e[5], e[5] & 0x3f);
		if (dsx_early_marks_on && i < 6) {
			static const uint32_t early_pdt_colour[6] = {
				0xffff8080,	/* 10 salmon */
				0xffffff40,	/* 11 light yellow */
				0xffff40ff,	/* 12 pink */
				0xff40ffff,	/* 13 light cyan */
				0xff4040ff,	/* 14 violet */
				0xffc0c0c0,	/* 15 light grey */
			};

			touch_early_mark(10 + i, early_pdt_colour[i]);
		}
		found++;
		if ((e[5] & 0x3f) == 0x00 || e[5] == 0xff) {
			printf("WHYRED TOUCH: %s end of PDT at 0x%04x\n", tag,
			    addr);
			break;
		}
		if ((e[5] & 0x3f) == 0x01) {
			dsx_f01_query = e[0];
			dsx_f01_cmd = e[1];
			dsx_f01_ctrl = e[2];
			dsx_f01_data = e[3];
		}
		if ((e[5] & 0x3f) == DSX_F12_FUNC) {
			dsx_f12_found = 1;
			dsx_f12_query = e[0];
			dsx_f12_ctrl = e[2];
			dsx_f12_data = e[3];
			dsx_f12_page = (addr >> 8) & 0xff;
		}
		if (addr < DSX_PDT_ENTRY_SIZE) {
			break;
		}
		addr -= DSX_PDT_ENTRY_SIZE;
	}
	printf("WHYRED TOUCH: %s: %d records; F01 %s q %02x c %02x d %02x; "
	    "F12 %s q %02x c %02x d %02x\n", tag, found,
	    dsx_f01_query || dsx_f01_cmd || dsx_f01_ctrl || dsx_f01_data ?
	    "found" : "missing", dsx_f01_query, dsx_f01_ctrl, dsx_f01_data,
	    dsx_f12_found ? "found" : "missing", dsx_f12_query,
	    dsx_f12_ctrl, dsx_f12_data);
	return found;
}

/*
 * 0206: the 0205 ring settles the layout. The part ignores the address phase
 * after the first read and streams its register map; in that stream the
 * function records sit back to back in walk order:
 *
 *   27 00 09 00 21 34   F34  query 0x27 cmd 0x00 ctrl 0x09 data 0x00
 *   2f 26 0d 04 01 01   F01  query 0x2f cmd 0x26 ctrl 0x0d data 0x04
 *   4c 00 13 06 01 12   F12  query 0x4c cmd 0x00 ctrl 0x13 data 0x06
 *   00 00 00 00 00 00   end of PDT
 *
 * The F01 command base on this unit is therefore 0x26, not the RMI4 default
 * 0x06. 0196 and 0205 wrote the sw_reset command 0x01 to 0x06, which is
 * F12's data register, so the controller was never reset out of wakeup
 * gesture mode and every later read kept streaming. Read the whole register
 * space once, find the F12 record by value and take the nearest F01 record
 * (function number 0x01, control base 0x0d) as the authority; the record also
 * appears at its real address when the part is addressable, so the search is
 * two-sided.
 */
#define DSX_STREAM_LEN		0x0400
#define DSX_STREAM_READ		0x0200
#define DSX_STREAM_ROW		64

static uint8_t dsx_pdt_map[DSX_STREAM_LEN];

static const uint8_t dsx_f12_record[6] = {
	0x4c, 0x00, 0x13, 0x06, 0x01, 0x12
};

static int
dsx_pdt_stream(void)
{
	int f12 = -1, f01 = -1, i, off;

	/*
	 * The pointer position is unknown when the part streams, so read two
	 * 0x200 windows back to back: the 0x218-byte cycle is longer than one
	 * window, and two windows guarantee a complete record somewhere,
	 * including one that would straddle the first window's end.
	 */
	if (dsx_read_page(DSX_ADDR, 0x0000, dsx_pdt_map,
	    DSX_STREAM_READ) != 0 ||
	    dsx_read_page(DSX_ADDR, 0x0000, dsx_pdt_map + DSX_STREAM_READ,
	    DSX_STREAM_READ) != 0) {
		printf("WHYRED TOUCH: pdt stream read failed\n");
		return -1;
	}
	for (off = 0; off < DSX_STREAM_LEN; off += DSX_STREAM_ROW) {
		dsx_dump("f01scan", (uint16_t)off, dsx_pdt_map + off,
		    DSX_STREAM_ROW);
	}
	for (off = 0; off + 6 <= DSX_STREAM_LEN; off++) {
		if (memcmp(dsx_pdt_map + off, dsx_f12_record, 6) == 0) {
			f12 = off;
			break;
		}
	}
	if (f12 < 0) {
		printf("WHYRED TOUCH: no F12 record in the pdt stream\n");
		return -1;
	}
	/* F01 sits six bytes from F12 in stream order, either side. */
	for (i = f12 - 18; i <= f12 + 18; i++) {
		if (i < 0 || i + 6 > DSX_STREAM_LEN) {
			continue;
		}
		if ((dsx_pdt_map[i + 5] & 0x3f) == 0x01 &&
		    dsx_pdt_map[i + 2] == 0x0d) {
			f01 = i;
			break;
		}
	}
	if (f01 < 0) {
		printf("WHYRED TOUCH: no F01 record next to F12 in the pdt "
		    "stream\n");
		return -1;
	}
	dsx_f01_query = dsx_pdt_map[f01];
	dsx_f01_cmd = dsx_pdt_map[f01 + 1];
	dsx_f01_ctrl = dsx_pdt_map[f01 + 2];
	dsx_f01_data = dsx_pdt_map[f01 + 3];
	dsx_f12_found = 1;
	dsx_f12_query = dsx_pdt_map[f12];
	dsx_f12_ctrl = dsx_pdt_map[f12 + 2];
	dsx_f12_data = dsx_pdt_map[f12 + 3];
	dsx_f12_page = 0;
	printf("WHYRED TOUCH: pdt stream F01 at 0x%04x q %02x cmd %02x "
	    "ctrl %02x data %02x; F12 at 0x%04x q %02x ctrl %02x data %02x\n",
	    f01, dsx_f01_query, dsx_f01_cmd, dsx_f01_ctrl, dsx_f01_data,
	    f12, dsx_f12_query, dsx_f12_ctrl, dsx_f12_data);
	return 0;
}

/*
 * Prove the address phase by reading the same two registers twice. An
 * addressable part returns the same bytes both times; a streaming part
 * returns the next bytes of its map. The first entry at 0xe9 is not assumed
 * to have any particular function number: this unit's F01 is vendor-specific
 * (2f 26 0d 04), so only the repetition counts.
 */
static int
dsx_addr_works(void)
{
	uint8_t a[DSX_PDT_ENTRY_SIZE], b[DSX_PDT_ENTRY_SIZE];
	uint8_t c[DSX_PDT_ENTRY_SIZE], d[DSX_PDT_ENTRY_SIZE];
	int i, ok;

	if (dsx_read_page(DSX_ADDR, DSX_PDT_START, a, sizeof(a)) != 0 ||
	    dsx_read_page(DSX_ADDR, DSX_PDT_START, b, sizeof(b)) != 0 ||
	    dsx_read_page(DSX_ADDR,
	    (uint16_t)(DSX_PDT_START - DSX_PDT_ENTRY_SIZE), c,
	    sizeof(c)) != 0 ||
	    dsx_read_page(DSX_ADDR,
	    (uint16_t)(DSX_PDT_START - DSX_PDT_ENTRY_SIZE), d,
	    sizeof(d)) != 0) {
		return 0;
	}
	printf("WHYRED TOUCH: addr test 0x%04x %02x %02x %02x %02x %02x %02x "
	    "-> %02x %02x %02x %02x %02x %02x\n", DSX_PDT_START, a[0], a[1],
	    a[2], a[3], a[4], a[5], b[0], b[1], b[2], b[3], b[4], b[5]);
	printf("WHYRED TOUCH: addr test 0x%04x %02x %02x %02x %02x %02x %02x "
	    "-> %02x %02x %02x %02x %02x %02x\n",
	    DSX_PDT_START - DSX_PDT_ENTRY_SIZE, c[0], c[1], c[2], c[3], c[4],
	    c[5], d[0], d[1], d[2], d[3], d[4], d[5]);
	ok = memcmp(a, b, sizeof(a)) == 0 && memcmp(c, d, sizeof(c)) == 0;
	/* A constant 00/ff region repeats even while streaming. */
	if (ok) {
		for (i = 0; i < (int)sizeof(a); i++) {
			if (a[i] != 0x00 && a[i] != 0xff) {
				break;
			}
		}
		if (i == (int)sizeof(a)) {
			ok = 0;
		}
	}
	return ok;
}

/*
 * 0206 order: map first, reset second. The stream is the only place the real
 * F01 command base can be read on this unit, and the reset must go to that
 * base. If the part is already addressable (cold boot, pointer at 0) no reset
 * is needed; if it streams, try the record's base first and the RMI4 default
 * 0x06 second, so one boot distinguishes the two candidates.
 */
static void
dsx_reset_probe(void)
{
	uint8_t v = 0;
	int addressable;

	if (dsx_early_marks_on) {
		touch_early_mark(7, 0xff800000);	/* maroon: before reset */
	}
	if (dsx_pdt_stream() != 0) {
		printf("WHYRED TOUCH: no pdt stream; using the walk only\n");
	}
	addressable = dsx_addr_works();
	if (!addressable) {
		(void)dsx_sw_reset();
		addressable = dsx_addr_works();
	}
	if (!addressable && dsx_f01_cmd != DSX_F01_CMD) {
		printf("WHYRED TOUCH: still streaming after reset at 0x%02x, "
		    "trying the RMI4 default 0x%02x\n", dsx_f01_cmd,
		    DSX_F01_CMD);
		(void)dsx_sw_reset_at(DSX_F01_CMD);
		addressable = dsx_addr_works();
	}
	dsx_addressable = addressable;
	printf("WHYRED TOUCH: address phase %s\n",
	    addressable ? "works" : "still ignored (streaming)");
	if (dsx_early_marks_on) {
		touch_early_mark(8, 0xff800080);	/* purple: reset command done */
	}
	if (dsx_read_page(DSX_ADDR, (uint16_t)(dsx_f01_data ?
	    dsx_f01_data : DSX_F01_DATA), &v, 1) == 0) {
		printf("WHYRED TOUCH: f01 status 0x%02x\n", v);
	} else {
		printf("WHYRED TOUCH: f01 status read failed\n");
	}
	if (dsx_early_marks_on) {
		touch_early_mark(9, 0xff808080);	/* grey: F01 status read */
	}
	/*
	 * The walk is authoritative only when the address phase works. While
	 * the part streams, keep the bases parsed out of the map.
	 */
	if (addressable) {
		dsx_pdt_walk("after reset");
	}
	if (dsx_early_marks_on) {
		touch_early_mark(16, 0xff606060);	/* dark grey: walk done */
	}
	if (!dsx_f01_ctrl) {
		printf("WHYRED TOUCH: no F01 control base, skipping configure\n");
	} else if (dsx_read_page(DSX_ADDR, (uint16_t)dsx_f01_ctrl, &v, 1) ==
	    0) {
		printf("WHYRED TOUCH: f01 ctrl read 0x%02x\n", v);
		if (!(v & 0x80)) {
			dsx_f01_normal = (uint8_t)(v | 0x80);
			dsx_wake_normal();
		}
	} else {
		printf("WHYRED TOUCH: f01 ctrl read failed\n");
	}
	if (dsx_f12_found) {
		dsx_block("f12 query", (uint16_t)dsx_f12_query, 32);
		dsx_block("f12 ctrl", (uint16_t)dsx_f12_ctrl, 32);
		dsx_block("f12 data", (uint16_t)dsx_f12_data, 1);
	}
	if (dsx_early_marks_on) {
		touch_early_mark(17, 0xff008080);	/* teal: F12 blocks read */
	}
}

/*
 * The vendor's F12 setup, ported from synaptics_rmi4_f12_init() in the
 * whyred kernel (the reference driver for this exact part):
 *
 *   query_5 lives at query_base+5; its first byte is size_of_query6 and the
 *   rest are ctrl0.. presence bits. Because every control register is one
 *   byte, the offset of control register N is the number of present controls
 *   below N, so ctrl_28 = control_base + (ctrl0..ctrl27 present).
 *
 *   Continuous reporting is control 28; the vendor writes RPT_DEFAULT
 *   (0x1F = type + X/Y LSB/MSB) to it, not 0x01.
 *
 *   query_8 lives at query_base+8; byte 0 is size_of_query9 and the rest are
 *   data0.. presence bits. The 2D report starts at
 *   data_base + (data0 present), because data0 carries the wakeup gesture and
 *   sits before the fingers (the vendor's data1_offset).
 *
 * 0206: the F01 record on this unit is 2f 26 0d 04 01 01, not the RMI4
 * default 00 06 0d 11, so the old "not real" gate rejected the only valid
 * PDT this part ever returns. Accept the record from the map when it has a
 * command and control base; the F12 record is matched by its full six bytes.
 */
#define DSX_F12_Q5_SIZE	0x04
#define DSX_F12_Q5_DATA	0x05
#define DSX_F12_Q8_SIZE	0x07
#define DSX_F12_Q8_DATA	0x08
#define DSX_F12_RPT_DEFAULT	0x1f
#define DSX_HID_MAX_FINGERS	10

static int
dsx_bits_present(const uint8_t *bytes, int nbytes, int limit)
{
	int i, seen = 0;

	for (i = 0; i < nbytes * 8 && seen < limit; i++) {
		if (bytes[i >> 3] & (1 << (i & 7))) {
			seen++;
		}
	}
	return seen;
}

static int
dsx_f12_program(void)
{
	uint8_t q5[10], q8[10], v, rpt, c23[2] = { 0, 0 };
	int q5n, q8n, ctrl23, ctrl28, data0, rc, i5, i8;

	dsx_f12_programmed = 0;
	dsx_f12_touch_reg = 0;
	dsx_f12_report_ctrl = 0;
	if (!dsx_f12_found) {
		printf("WHYRED TOUCH: no F12, skipping reporting setup\n");
		return -1;
	}
	/*
	 * 0206: the stream is the authority while the part is suspended, so
	 * the only requirement is a structurally valid F01/F12 pair. The MTP
	 * style literal 00 06 0d 11 does not exist on this unit.
	 */
	if (!dsx_f01_cmd || !dsx_f01_ctrl) {
		printf("WHYRED TOUCH: no usable F01 record (q %02x cmd %02x "
		    "ctrl %02x data %02x); not programming F12\n",
		    dsx_f01_query, dsx_f01_cmd, dsx_f01_ctrl, dsx_f01_data);
		return -1;
	}
	q5n = 0;
	if (dsx_read_cached(DSX_ADDR, (uint16_t)(dsx_f12_query +
	    DSX_F12_Q5_SIZE), &v, 1) == 0) {
		q5n = v;
	}
	/*
	 * size 0 is legal: it means the block has no presence bytes, so every
	 * control offset below is 0 (the vendor does not reject it either).
	 */
	if (q5n > (int)sizeof(q5)) {
		q5n = (int)sizeof(q5);
	}
	for (i5 = 0; i5 < (int)sizeof(q5); i5++) {
		q5[i5] = 0;
	}
	if (q5n > 0 && dsx_read_cached(DSX_ADDR, (uint16_t)(dsx_f12_query +
	    DSX_F12_Q5_DATA), q5, q5n) != 0) {
		printf("WHYRED TOUCH: query_5 read failed\n");
		return -1;
	}
	q8n = 0;
	if (dsx_read_cached(DSX_ADDR, (uint16_t)(dsx_f12_query +
	    DSX_F12_Q8_SIZE), &v, 1) == 0) {
		q8n = v;
	}
	/* The vendor clamps query_8 data to 5 bytes; keep the same window. */
	if (q8n > 5) {
		q8n = 5;
	}
	for (i8 = 0; i8 < (int)sizeof(q8); i8++) {
		q8[i8] = 0;
	}
	if (q8n > 0 && dsx_read_cached(DSX_ADDR, (uint16_t)(dsx_f12_query +
	    DSX_F12_Q8_DATA), q8, q8n) != 0) {
		printf("WHYRED TOUCH: query_8 read failed\n");
		return -1;
	}
	printf("WHYRED TOUCH: f12 query_5 size %d %02x %02x %02x %02x; "
	    "query_8 size %d %02x %02x %02x %02x\n", q5n, q5[0], q5[1],
	    q5[2], q5[3], q8n, q8[0], q8[1], q8[2], q8[3]);
	ctrl28 = q5n > 1 ? dsx_bits_present(q5 + 1, q5n - 1, 28) : 0;
	ctrl23 = q5n > 1 ? dsx_bits_present(q5 + 1, q5n - 1, 23) : 0;
	data0 = (q8n > 1 && (q8[1] & 0x01)) ? 1 : 0;
	dsx_f12_report_ctrl = dsx_f12_ctrl + ctrl28;
	dsx_f12_touch_reg = dsx_f12_data + data0;
	/*
	 * ctrl23 byte 1 is max_reported_objects; the record itself is 8 bytes
	 * (status, x, y, z, wx, wy), the same layout the vendor driver reads.
	 */
	dsx_f12_fingers = DSX_HID_MAX_FINGERS;
	if (dsx_read_cached(DSX_ADDR, (uint16_t)(dsx_f12_ctrl + ctrl23),
	    c23, sizeof(c23)) == 0 && c23[1] >= 1 &&
	    c23[1] <= DSX_HID_MAX_FINGERS) {
		dsx_f12_fingers = c23[1];
	}
	dsx_f12_rec_size = 8;
	printf("WHYRED TOUCH: f12 ctrl28 0x%04x (offset %d), touch data 0x%04x "
	    "(data0 %d), ctrl23 %02x %02x -> %d fingers x %d bytes\n",
	    dsx_f12_report_ctrl, ctrl28, dsx_f12_touch_reg, data0,
	    c23[0], c23[1], dsx_f12_fingers, dsx_f12_rec_size);
	rpt = DSX_F12_RPT_DEFAULT;
	dsx_page = -1;
	rc = dsx_write_f(DSX_ADDR, (uint16_t)dsx_f12_report_ctrl, &rpt, 1);
	printf("WHYRED TOUCH: f12 ctrl28 <- 0x%02x %s\n", rpt,
	    rc == 0 ? "ack" : "nack");
	if (rc == 0 && dsx_read_cached(DSX_ADDR,
	    (uint16_t)dsx_f12_report_ctrl, &v, 1) == 0) {
		printf("WHYRED TOUCH: f12 ctrl28 readback 0x%02x\n", v);
	}
	dsx_f12_programmed = (rc == 0);
	return dsx_f12_programmed ? 0 : -1;
}

/*
 * Poll F12's 2D data block. The base is whatever the map/walk found; each
 * sample uses the cached page, like synaptics_rmi4_i2c_read() does.
 */
static void
dsx_f12_poll(void)
{
	uint8_t b[8];
	uint16_t reg;
	int i;

	if (!dsx_f12_found) {
		printf("WHYRED TOUCH: no F12 in the PDT, cannot poll touch\n");
		return;
	}
	reg = dsx_f12_touch_reg ? (uint16_t)dsx_f12_touch_reg :
	    (uint16_t)((dsx_f12_page << 8) | dsx_f12_data);
	printf("WHYRED TOUCH: poll F12 0x%04x, 12 samples\n", reg);
	for (i = 0; i < 12; i++) {
		if (dsx_read_cached(DSX_ADDR, reg, b, sizeof(b)) != 0) {
			printf("WHYRED TOUCH: poll %d read failed\n", i);
			break;
		}
		printf("WHYRED TOUCH: poll %d F12[%02x] %02x %02x %02x %02x "
		    "%02x %02x %02x %02x%s\n", i, reg, b[0], b[1], b[2],
		    b[3], b[4], b[5], b[6], b[7],
		    (b[0] & 0x80) ? " valid" : "");
		delay(300000);
	}
}

/*
 * 0218: the 2D block is live once ctrl28 has been written. Poll it in a
 * kernel thread and hand every change to SweetHIDTouch; continuous reporting
 * is what the vendor driver uses, and the thread keeps the controller in that
 * mode while the rest of the boot runs.
 */
static void
dsx_hid_thread(void *arg, __unused wait_result_t wr)
{
	uint8_t buf[DSX_HID_MAX_FINGERS * 8];
	uint8_t was[DSX_HID_MAX_FINGERS];
	int px[DSX_HID_MAX_FINGERS], py[DSX_HID_MAX_FINGERS];
	int errors = 0, i;

	(void)arg;
	memset(was, 0, sizeof(was));
	memset(px, 0, sizeof(px));
	memset(py, 0, sizeof(py));
	printf("WHYRED TOUCH: HID poll %d fingers x %d bytes at 0x%04x\n",
	    dsx_f12_fingers, dsx_f12_rec_size, dsx_f12_touch_reg);
	for (;;) {
		int fingers = dsx_f12_fingers;
		int rec = dsx_f12_rec_size;
		int n;

		if (fingers < 1) {
			fingers = 1;
		}
		if (fingers > DSX_HID_MAX_FINGERS) {
			fingers = DSX_HID_MAX_FINGERS;
		}
		if (rec < 5) {
			rec = 5;
		}
		if (rec > 8) {
			rec = 8;
		}
		n = fingers * rec;
		if (dsx_read_cached(DSX_ADDR, (uint16_t)dsx_f12_touch_reg,
		    buf, n) != 0) {
			if (errors++ == 0 || (errors % 100) == 0) {
				printf("WHYRED TOUCH: HID poll read failed "
				    "(%d)\n", errors);
			}
			delay(20000);
			continue;
		}
		errors = 0;
		for (i = 0; i < fingers; i++) {
			uint8_t st = buf[i * rec];
			int down = ((st & 0x03) == 0x01) ||
			    ((st & 0x0f) == 0x06);
			int x = ((int)buf[i * rec + 2] << 8) |
			    buf[i * rec + 1];
			int y = ((int)buf[i * rec + 4] << 8) |
			    buf[i * rec + 3];

			if (down != was[i] ||
			    (down && (x != px[i] || y != py[i]))) {
				sweet_hid_touch(i, down, x, y);
				was[i] = (uint8_t)down;
				px[i] = x;
				py[i] = y;
			}
		}
		delay(8000);
	}
}

static void
dsx_hid_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	if (dsx_hid_started || !dsx_f12_found || !dsx_f12_touch_reg) {
		return;
	}
	if (PE_parse_boot_argn("sweetouchhid", &on, sizeof(on)) && !on) {
		printf("WHYRED TOUCH: HID poll disabled\n");
		return;
	}
	dsx_hid_started = 1;
	if (kernel_thread_start(dsx_hid_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		dsx_hid_started = 0;
		printf("WHYRED TOUCH: HID poll thread start failed\n");
	}
}

static void
touch_irq_setup(vm_offset_t north)
{
	if (!touch_irq_ready) {
		pin_setup(&touch_irq, north, WHYRED_IRQ);
		touch_irq_ready = 1;
	}
}

static void
dsx_touch_test(const struct dsx_funcs *f, uint32_t seconds)
{
	uint8_t idle[11], b[11];
	int i, touched = 0, irq_level, irq_edges = 0;

	if (!f->found_f12 || !f->f12_data) {
		printf("WHYRED TOUCH: no F12 data base, cannot poll touch\n");
		return;
	}
	if (seconds > 60) {
		seconds = 60;	/* keep the boot bounded */
	}
	if (dsx_read_page(DSX_ADDR, f->f12_data, idle, sizeof(idle)) != 0) {
		printf("WHYRED TOUCH: F12 data read failed\n");
		return;
	}
	printf("WHYRED TOUCH: polling F12 0x%04x for %u s; idle %02x %02x %02x "
	    "%02x %02x\n", f->f12_data, seconds, idle[0], idle[1], idle[2],
	    idle[3], idle[4]);
	irq_level = touch_irq_ready ? pin_read(&touch_irq) : 1;
	printf("WHYRED TOUCH: irq %d at start\n", irq_level);
	panel_band(0xff2040e0);		/* blue: waiting for a finger */

	for (i = 0; i < (int)(seconds * 50); i++) {
		int changed = 0;

		if (touch_irq_ready) {
			int level = pin_read(&touch_irq);

			if (level != irq_level) {
				irq_edges++;
				printf("WHYRED TOUCH: irq %d -> %d (edge %d)\n",
				    irq_level, level, irq_edges);
				irq_level = level;
				if (!touched) {
					touched = 1;
					panel_band(0xff20e020);
				}
			}
		}
		if (dsx_read_page(DSX_ADDR, f->f12_data, b, sizeof(b)) != 0) {
			printf("WHYRED TOUCH: F12 data read failed\n");
			break;
		}
		for (int k = 0; k < (int)sizeof(b); k++) {
			if (b[k] != idle[k]) {
				changed = 1;
			}
		}
		if (changed) {
			printf("WHYRED TOUCH: f12 %02x %02x %02x %02x %02x %02x "
			    "%02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3],
			    b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
			for (int k = 0; k < (int)sizeof(b); k++) {
				idle[k] = b[k];
			}
			if (!touched) {
				touched = 1;
				panel_band(0xff20e020);	/* green: seen */
			}
		} else if (i % 50 == 0) {
			/* Once a second: prove the poll is alive and show
			 * what the chip reports while nobody touches it. */
			printf("WHYRED TOUCH: poll %u s data %02x %02x %02x %02x "
			    "%02x irq %d\n", i / 50, b[0], b[1], b[2], b[3],
			    b[4], irq_level);
		}
		delay(20000);
	}
	printf("WHYRED TOUCH: touch test done (%s)\n",
	    touched ? "data changed" : "nothing seen");
}

/*
 * whyred's panel scanout buffer, same constants santoni_fb_test() uses
 * (bsd/dev/santoni_sdhci.c). Painting here is independent of the console:
 * the four-bar test proved writes to this address reach the panel.
 *
 * Colour code, top quarter of the screen:
 *   white    first ack 0x01  Novatek running firmware
 *   yellow   first ack 0x62  Novatek still in bootloader
 *   cyan     first ack 0x20  Synaptics panel
 *   magenta  some other address answered
 *   red      nothing acknowledged
 * Bottom three quarters: green if at least one address answered, else red.
 */
#define WHYRED_FB_PHYS		0x9d400000ULL
#define WHYRED_FB_WIDTH		1080u
#define WHYRED_FB_HEIGHT	2160u
#define WHYRED_FB_STRIDE	4320u	/* bytes, 32 bpp */

static uint32_t
panel_code(int acks, uint8_t first)
{
	if (acks <= 0) {
		return 0xffe02020;	/* red */
	}
	switch (first) {
	case 0x01:
		return 0xffffffff;	/* white */
	case 0x62:
		return 0xffffff20;	/* yellow */
	case 0x20:
		return 0xff20e0e0;	/* cyan */
	default:
		return 0xffff20ff;	/* magenta */
	}
}

static volatile uint32_t *panel_fb;

static volatile uint32_t *
panel_map(void)
{
	if (panel_fb == NULL) {
		panel_fb = (volatile uint32_t *)ml_io_map_unmappable(
		    WHYRED_FB_PHYS,
		    (vm_size_t)WHYRED_FB_STRIDE * WHYRED_FB_HEIGHT,
		    VM_WIMG_IO);
	}
	return panel_fb;
}

static int
panel_fill_rows(uint32_t top, uint32_t bottom)
{
	volatile uint32_t *fb = panel_map();

	if (fb == NULL) {
		return ENOMEM;
	}
	for (uint32_t y = 0; y < WHYRED_FB_HEIGHT; y++) {
		volatile uint32_t *line = fb + (uint64_t)y *
		    (WHYRED_FB_STRIDE / 4);
		uint32_t px = (y < WHYRED_FB_HEIGHT / 4) ? top : bottom;

		for (uint32_t x = 0; x < WHYRED_FB_WIDTH; x++) {
			line[x] = px;
		}
	}
	return 0;
}

/* Cheap visible state for the touch poll: a band across the top. */
static void
panel_band(uint32_t colour)
{
	volatile uint32_t *fb = panel_map();

	if (fb == NULL) {
		return;
	}
	for (uint32_t y = 0; y < 120; y++) {
		volatile uint32_t *line = fb + (uint64_t)y *
		    (WHYRED_FB_STRIDE / 4);

		for (uint32_t x = 0; x < WHYRED_FB_WIDTH; x++) {
			line[x] = colour;
		}
	}
}

static int
panel_report(int acks, uint8_t first)
{
	return panel_fill_rows(panel_code(acks, first),
	    acks > 0 ? 0xff20e020 : 0xffe02020);
}

/*
 * whyred's DT: synaptics,reset-active-ms = 20, reset-delay-ms = 200. The
 * controller boots its firmware during that delay, so wait before any read.
 * This is also the escape hatch from flash-programming mode: a reset after
 * ENABLE_FLASH_PROG brings the part back to application firmware.
 */
static void
dsx_reset(vm_offset_t north)
{
	struct touch_pin rst;

	pin_setup(&rst, north, WHYRED_RST);
	pin_out(&rst, 0);
	delay(20000);
	pin_out(&rst, 1);
	delay(250000);
	/* the controller comes back in page 0; forget the cached page */
	dsx_page = -1;
	printf("WHYRED TOUCH: reset pulse low 20 ms, high 200 ms + 50 ms "
	    "settle\n");
}

/*
 * Diagnostics only: hand the phone back to the bootloader without a key
 * press. The vendor msm-poweroff.c does exactly this for
 * "reboot bootloader": write RESTART_REASON_BOOTLOADER into the IMEM slot
 * and drop PS_HOLD. sdm660.dtsi names both:
 *
 *   qcom,msm-imem@146bf000 / restart_reason@65c -> 0x146bf65c
 *   restart@10ac000    ("qcom,pshold")          -> 0x010ac000
 *
 * ABL reads the reason on the next boot and enters fastboot. IMEM is in the
 * always-on domain and survives the warm reset PS_HOLD causes, which is why
 * the reason has to be written first.
 */
#define IMEM_RESTART_REASON_PA	0x146bf65cu
#define PSHOLD_PA		0x010ac000u
#define RESTART_REASON_BOOTLOADER 0x77665500u

static void
whyred_reboot_bootloader(void)
{
	volatile uint32_t *reason, *pshold;

	reason = (volatile uint32_t *)ml_io_map_unmappable(
	    IMEM_RESTART_REASON_PA, 4, VM_WIMG_IO);
	pshold = (volatile uint32_t *)ml_io_map_unmappable(PSHOLD_PA, 4,
	    VM_WIMG_IO);
	if (!reason || !pshold) {
		printf("WHYRED TOUCH: cannot map IMEM/PS_HOLD, staying up\n");
		return;
	}
	*reason = RESTART_REASON_BOOTLOADER;
	__asm__ volatile("dsb sy" ::: "memory");
	printf("WHYRED TOUCH: restart reason 0x%08x set, resetting to fastboot\n",
	    RESTART_REASON_BOOTLOADER);
	/* two flusher ticks, so the ring lands before the reset */
	delay(3000000);
	*pshold = 0;
	for (;;) {
		delay(1000000);
	}
}

/*
 * Once XNU owns the USB controller the diag image has no way back except the
 * reset above, and a stall in the GCC or QUP register window happens before
 * that call is reached - the phone then sits on the colour bars with no USB at
 * all until someone holds Power. Arm this before the first clock write so a
 * hung probe still lands in fastboot: the timer runs on another core, and both
 * the IMEM reason and PS_HOLD are outside the BLSP register window that
 * stalls. sweetouchwatchdog=<seconds> overrides the delay, 0 disables it.
 */
static void
whyred_reboot_watchdog(void *arg, __unused wait_result_t wr)
{
	int seconds = (int)(uintptr_t)arg;

	printf("WHYRED TOUCH: reboot watchdog armed, %d s\n", seconds);
	while (seconds > 0) {
		int chunk = seconds > 10 ? 10 : seconds;

		(void)tsleep(&whyred_reboot_watchdog, PZERO, "sweetwd",
		    chunk * hz);
		seconds -= chunk;
	}
	printf("WHYRED TOUCH: watchdog fired, probe did not finish\n");
	whyred_reboot_bootloader();
}

static void
whyred_arm_reboot_watchdog(void)
{
	uint32_t seconds = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sweetouchwatchdog", &seconds,
	    sizeof(seconds))) {
		seconds = 300;
	}
	if (seconds == 0) {
		return;
	}
	if (kernel_thread_start(whyred_reboot_watchdog,
	    (void *)(uintptr_t)seconds, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("WHYRED TOUCH: cannot arm the reboot watchdog\n");
	}
}

/*
 * 0199: breadcrumbs that survive a hang before the ring flusher starts.
 * The ring lives on SYSTEM and is only written after eMMC init, so a probe
 * that runs before eMMC and hangs leaves no new ring at all - exactly what
 * the 0198 boot did. These bands are written straight into the boot
 * framebuffer ABL left on screen, one band per step, so the panel shows how
 * far the early probe got even when nothing else is reachable.
 *
 * PE_state.video.v_baseAddr still holds the physical ABL address at this
 * point and is not a usable pointer, so the band window is mapped here. Only
 * the rows the bands occupy are mapped: the whole 1080x2160 frame is 9 MiB
 * and this runs before eMMC init.
 */
#define TOUCH_FB_PHYS	0x9d400000ULL
#define TOUCH_FB_STRIDE	4320u	/* bytes, 32 bpp */
#define TOUCH_FB_ROW0	40u
/*
 * 0202: map the whole frame once. 0201 mapped a second window for the eMMC
 * phase bands, and that boot still ended at the white touch band, so either
 * the probe never returned or the second mapping never happened. A single
 * window removes the second variable; it costs the rest of the 9 MiB
 * scanout buffer, and this already runs before eMMC.
 */
#define TOUCH_FB_ROWS	2160u	/* the whole 1080x2160 panel */

static volatile uint32_t *
touch_fb(void)
{
	static volatile uint32_t *fb;

	if (!fb) {
		fb = (volatile uint32_t *)ml_io_map_unmappable(TOUCH_FB_PHYS,
		    (vm_size_t)TOUCH_FB_ROWS * TOUCH_FB_STRIDE, VM_WIMG_IO);
	}
	return fb;
}

static void
touch_early_mark(int step, uint32_t color)
{
	volatile uint32_t *fb = touch_fb();
	uint32_t y0 = TOUCH_FB_ROW0 + (uint32_t)step * 70u;
	uint32_t x, y;

	if (fb == NULL) {
		return;
	}
	for (y = y0; y < y0 + 50u; y++) {
		volatile uint32_t *line = fb +
		    (uint64_t)y * (TOUCH_FB_STRIDE / 4u);

		for (x = 80; x < 980; x++) {
			line[x] = color;
		}
	}
	printf("WHYRED TOUCH: early mark %d\n", step);
}

/*
 * 0201: whyred_touch_early() returns, so the touch side is done, but the
 * ring still only starts after eMMC init. A boot that hangs between the
 * early probe and santoni_log_start() is therefore just as silent as the
 * 0198 one, and the white band alone cannot tell the eMMC phases apart.
 * Paint one mark per phase of santoni_sdhci_init()/sdhci_take_over() below
 * the touch bands. Same mapping as the touch bands (0202).
 */
#define TOUCH_PHASE_ROW0	1450u
#define TOUCH_PHASE_STEP	54u
#define TOUCH_PHASE_TALL	40u

static void
touch_phase_mark(int step, uint32_t color)
{
	volatile uint32_t *fb = touch_fb();
	uint32_t y0 = TOUCH_PHASE_ROW0 + (uint32_t)step * TOUCH_PHASE_STEP;
	uint32_t x, y;

	if (fb == NULL) {
		return;
	}
	for (y = y0; y < y0 + TOUCH_PHASE_TALL; y++) {
		volatile uint32_t *line = fb +
		    (uint64_t)y * (TOUCH_FB_STRIDE / 4u);

		for (x = 80; x < 980; x++) {
			line[x] = color;
		}
	}
	printf("WHYRED TOUCH: phase mark %d\n", step);
}

/*
 * 0202: the white band is painted before the last printf, the timestamps and
 * the return, so a boot that stops at white cannot say whether the probe
 * ever returned. Paint in the gap between the last touch band and the first
 * eMMC phase, after those statements, so it only shows if the function
 * really fell through to its return.
 */
static void
touch_end_mark(void)
{
	volatile uint32_t *fb = touch_fb();
	uint32_t x, y;

	if (fb == NULL) {
		return;
	}
	for (y = 1425; y < 1445; y++) {
		volatile uint32_t *line = fb +
		    (uint64_t)y * (TOUCH_FB_STRIDE / 4u);

		for (x = 80; x < 980; x++) {
			line[x] = 0xff00ff40;	/* lime: early probe returned */
		}
	}
	printf("WHYRED TOUCH: end mark\n");
}

/*
 * Called by the eMMC driver after the early probe. No-op when the probe
 * never ran (other boot args, other boards), so a normal boot keeps the
 * framebuffer exactly as SweetFramebuffer left it.
 */
void
whyred_touch_mark(int step, uint32_t color)
{
	if (!dsx_early_ran) {
		return;
	}
	touch_phase_mark(step, color);
}

/*
 * The early probe: same bit-bang setup the late probe does, but placed inside
 * the 0.9-3.2 s window instead of after eMMC init. It only reads: reset, read
 * F01 status, walk the PDT, dump F12 query/control/data. The touch poll and
 * the panel band stay in the late probe, where the framebuffer exists.
 */
int
whyred_touch_early(void)
{
	vm_offset_t south, north;
	uint32_t on = 0, at = 0;

	if (dsx_early_ran) {
		return 0;
	}
	if (!PE_parse_boot_argn("sweetouchi2c", &on, sizeof(on)) || !on) {
		return 0;
	}
	if (!PE_parse_boot_argn("sweetouchearly", &at, sizeof(at)) || !at) {
		return 0;
	}
	if (at > 4000) {
		at = 4000;
	}
	dsx_early_ran = 1;
	touch_early_mark(1, 0xffff0000);	/* red: entered */
	whyred_arm_reboot_watchdog();
	touch_early_mark(2, 0xffff8000);	/* orange: watchdog armed */
	touch_wait_until_ms(at);
	dsx_early_start_ms = touch_now_ms();
	printf("WHYRED TOUCH: early probe requested %u ms, starts at %llu ms "
	    "kernel time\n", at, (unsigned long long)dsx_early_start_ms);
	touch_early_mark(3, 0xffffff00);	/* yellow: past the wait */

	south = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_SOUTH,
	    0x10000, VM_WIMG_IO);
	north = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_NORTH,
	    0x50000, VM_WIMG_IO);
	if (south == 0 || north == 0) {
		printf("WHYRED TOUCH: early TLMM map failed (south %p north %p)\n",
		    (void *)south, (void *)north);
		return ENOMEM;
	}
	(void)north;
	touch_early_mark(4, 0xff00ff00);	/* green: TLMM mapped */
	pin_setup(&touch_sda, south, WHYRED_SDA);
	pin_setup(&touch_scl, south, WHYRED_SCL);
	printf("WHYRED TOUCH: early sda%d scl%d idle sda %d scl %d\n",
	    WHYRED_SDA, WHYRED_SCL, pin_read(&touch_sda),
	    pin_read(&touch_scl));
	touch_early_mark(5, 0xff00ffff);	/* cyan: pins set */
	(void)i2c_clear_bus();
	touch_early_mark(6, 0xff0000ff);	/* blue: bus cleared */
	dsx_page = -1;
	dsx_early_marks_on = 1;
	dsx_reset_probe();
	dsx_early_marks_on = 0;
	touch_early_mark(18, 0xff404040);	/* charcoal: probe returned */
	dsx_early_found = dsx_f12_found;
	/*
	 * 0218: the address-phase test stays informational. This unit streams
	 * its map even after the vendor reset, but register writes still ack,
	 * and ctrl28 is what turns continuous 2D reporting on. Gating the write
	 * on dsx_addressable left F12 silent for every boot so far.
	 */
	if (dsx_f12_found) {
		(void)dsx_f12_program();
	}
	touch_early_mark(19, 0xffffffff);	/* white: done */
	dsx_early_end_ms = touch_now_ms();
	/*
	 * 0204: this format had two %s but only one string argument. The
	 * second %s read the next register (1) and vprintf_internal took a
	 * data abort on address 0x1, painting white and then panicking - the
	 * "hang" at the last band. The suffix is printed by the separate
	 * printf() calls below.
	 */
	printf("WHYRED TOUCH: early done at %llu ms, F12 %s",
	    (unsigned long long)dsx_early_end_ms, dsx_f12_found ?
	    "found" : "missing");
	if (dsx_f12_found) {
		printf(" at page %d query 0x%02x ctrl 0x%02x data 0x%02x",
		    dsx_f12_page, dsx_f12_query, dsx_f12_ctrl, dsx_f12_data);
	}
	if (dsx_f12_programmed) {
		printf(", reporting on 0x%04x, touch 0x%04x",
		    dsx_f12_report_ctrl, dsx_f12_touch_reg);
	}
	printf("\n");
	touch_end_mark();
	return 0;
}

int
whyred_touch_probe(void)
{
	vm_offset_t south, north;
	uint32_t on = 0, pulse = 0, wait, halt, qup = 0, reboot = 0;
	uint8_t id[3] = { 0, 0, 0 };
	uint8_t first_ack = 0;
	struct dsx_funcs dsx;
	int addr, acks = 0, pulsed, saw_rmi = 0;
	uint32_t touch_s;
	uint32_t rst_cfg, rst_io, irq_cfg, irq_io;

	if (touch_probe_done) {
		return 0;
	}
	if (!PE_parse_boot_argn("sweetouchi2c", &on, sizeof(on)) || !on) {
		return 0;
	}
	touch_probe_done = 1;

	/*
	 * Arm the way out before the first clock or QUP register write. A stall
	 * in either window leaves the phone with no USB at all, and the reset
	 * at the end of the probe is never reached, so the timer has to start
	 * here.
	 */
	if (PE_parse_boot_argn("sweetouchreboot", &reboot, sizeof(reboot)) &&
	    reboot && !dsx_early_ran) {
		whyred_arm_reboot_watchdog();
	}

	south = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_SOUTH,
	    0x10000, VM_WIMG_IO);
	north = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_NORTH,
	    0x50000, VM_WIMG_IO);
	if (south == 0 || north == 0) {
		printf("WHYRED TOUCH: TLMM map failed (south %p north %p)\n",
		    (void *)south, (void *)north);
		return ENOMEM;
	}

	/* Raw reset/IRQ state before anything is touched. */
	rst_cfg = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_RST + TLMM_CTRL);
	rst_io = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_RST + TLMM_IO);
	irq_cfg = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_IRQ + TLMM_CTRL);
	irq_io = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_IRQ + TLMM_IO);
	printf("WHYRED TOUCH: tlmm south %p north %p; rst cfg %08x io %08x; "
	    "irq cfg %08x io %08x\n", (void *)south, (void *)north, rst_cfg,
	    rst_io, irq_cfg, irq_io);

	pin_setup(&touch_sda, south, WHYRED_SDA);
	pin_setup(&touch_scl, south, WHYRED_SCL);
	printf("WHYRED TOUCH: sda%d ctl %08x scl%d ctl %08x; idle sda %d scl "
	    "%d\n", WHYRED_SDA, touch_sda.idle, WHYRED_SCL, touch_scl.idle,
	    pin_read(&touch_sda), pin_read(&touch_scl));

	pulsed = i2c_clear_bus();
	if (pulsed) {
		printf("WHYRED TOUCH: bus was stuck low, %d clocks\n", pulsed);
	}

	if (PE_parse_boot_argn("sweetouchreset", &pulse, sizeof(pulse)) && pulse) {
		dsx_reset(north);
	}

	for (addr = 0x08; addr <= 0x77; addr++) {
		if (!i2c_ack((uint8_t)addr)) {
			continue;
		}
		acks++;
		if (acks == 1) {
			first_ack = (uint8_t)addr;
		}
		printf("WHYRED TOUCH: ack 0x%02x\n", addr);
		if (addr == DSX_ADDR) {
			/* Synaptics DSX: the Novatek page protocol would only
			 * return noise, the DSX page-table dump below is the
			 * real identification. */
			saw_rmi = 1;
			continue;
		}
		if (nvt_read((uint8_t)addr, 0x00, 0x00, id, sizeof(id)) == 0) {
			printf("WHYRED TOUCH: id 0x%02x %02x %02x %02x\n", addr,
			    id[0], id[1], id[2]);
		} else {
			printf("WHYRED TOUCH: id 0x%02x read failed\n", addr);
		}
	}

	printf("WHYRED TOUCH: scan done, %d address(es) acknowledged; sda %d "
	    "scl %d\n", acks, pin_read(&touch_sda), pin_read(&touch_scl));

	if (saw_rmi) {
		/*
		 * The address is known and the bus is healthy, so the QUP can
		 * take over the same wires. Init is after the scan on purpose:
		 * it re-muxes GPIO2/3 from plain GPIO to blsp_i2c1, which would
		 * break the bit-bang search if it came first. If the QUP does
		 * not come up, every read below stays on the old path.
		 */
		if (PE_parse_boot_argn("sweeti2c", &qup, sizeof(qup)) && qup) {
			if (sweet_i2c_init() == 0) {
				dsx_qup = 1;
				dsx_page = -1;
				printf("WHYRED TOUCH: DSX reads over QUP1 "
				    "(hardware I2C, repeated start)\n");
			} else {
				printf("WHYRED TOUCH: QUP1 init failed, "
				    "staying on bit-bang\n");
			}
		}
		touch_irq_setup(north);
		/*
		 * Ring 0195: F01 control is writable, but after that write every
		 * read returned zero, while the recovery driver shows the part
		 * sane right after its own probe and asleep in wakeup-gesture mode
		 * a few seconds later. 0196 therefore issues the vendor software
		 * reset (F01 command 0x01) through the bit-bang and scans the PDT
		 * downwards with a real page select before each addressed read,
		 * exactly like synaptics_rmi4_query_device().
		 */
		if (dsx_early_ran) {
			printf("WHYRED TOUCH: early probe ran %llu..%llu ms, F12 "
			    "%s\n", (unsigned long long)dsx_early_start_ms,
			    (unsigned long long)dsx_early_end_ms,
			    dsx_early_found ? "found" : "missing");
		}
		if (dsx_early_ran && dsx_early_found) {
			printf("WHYRED TOUCH: late probe reuses the early PDT "
			    "result, no second reset\n");
		} else {
			dsx_reset_probe();
		}
		if (dsx_f12_found) {
			(void)dsx_f12_program();
		}
		dsx_f12_poll();
		dsx.found_f12 = dsx_f12_found;
		if (dsx_f12_found) {
			dsx.f12_query = (uint16_t)dsx_f12_query;
			dsx.f12_ctrl = (uint16_t)dsx_f12_ctrl;
			dsx.f12_data = dsx_f12_touch_reg ?
			    (uint16_t)dsx_f12_touch_reg :
			    (uint16_t)((dsx_f12_page << 8) | dsx_f12_data);
		}
		touch_s = 0;
		if (PE_parse_boot_argn("sweetouchtouch", &touch_s,
		    sizeof(touch_s)) && touch_s) {
			dsx_touch_test(&dsx, touch_s);
		}
	}

	/*
	 * Hold the panel so the answer is readable without a serial cable:
	 * the first iOS frame clears the console, and these lines print
	 * minutes before it. sweetouchwait=SECONDS overrides the 15 s hold,
	 * 0 continues immediately.
	 */
	/*
	 * sweetouchhalt=1: repeat the result forever instead of booting on.
	 * The panel is the only output on this phone, so a boot that keeps
	 * scrolling is easy to miss; a halted boot can be read at leisure and
	 * then powered off. Diagnostic images only.
	 */
	halt = 0;
	if (PE_parse_boot_argn("sweetouchhalt", &halt, sizeof(halt)) && halt) {
		if (panel_report(acks, first_ack) != 0) {
			printf("WHYRED TOUCH: panel paint failed\n");
		}
		for (;;) {
			printf("WHYRED TOUCH: RESULT acks=%d first=0x%02x sda=%d "
			    "scl=%d id=%02x%02x%02x\n", acks, first_ack,
			    pin_read(&touch_sda), pin_read(&touch_scl), id[0],
			    id[1], id[2]);
			delay(2000000);
		}
	}

	wait = 15;
	(void)PE_parse_boot_argn("sweetouchwait", &wait, sizeof(wait));
	if (wait) {
		if (wait > 60) {
			wait = 60;
		}
		printf("WHYRED TOUCH: holding the panel %u s\n", wait);
		delay((int)(wait * 1000000u));
	}

	/*
	 * The diag image cannot be recovered by adb once XNU owns the phone,
	 * so it walks itself back to fastboot. Only the diagnostic build
	 * passes this boot argument.
	 */
	if (PE_parse_boot_argn("sweetouchreboot", &reboot, sizeof(reboot)) &&
	    reboot) {
		whyred_reboot_bootloader();
	}
	/*
	 * Diagnostics are done, so the poll thread can own the bus. It starts
	 * here rather than earlier because the bit-bang has no lock and
	 * dsx_f12_poll()/dsx_touch_test() above must not race it.
	 */
	dsx_hid_start();
	return acks ? 0 : ENXIO;
}

#else /* !ARM64_BOARD_CONFIG_WHYRED */

int
whyred_touch_probe(void)
{
	return ENXIO;
}

#endif /* ARM64_BOARD_CONFIG_WHYRED */
