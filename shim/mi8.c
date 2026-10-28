/*
 * Xiaomi Mi 8 (dipper, SDM845) bring-up diagnostic, v2.
 *
 * v1 result (real phone): ABL leaves PP0 in auto-refresh (PP_AUTOREFRESH =
 * 0x80000001, tear check on), so the command-mode AMOLED shows whatever is
 * written into the buffer ABL scans out (0x9d400000, 1080 px stride, red/blue
 * as expected). The manual CTL START kicks in v1 were not needed and the
 * pipeline stopped updating after them. v2 therefore does NOT write any
 * display register: it dumps the pipeline, then cycles colours for ~25 s to
 * show the panel keeps refreshing, then returns (shim.c runs the read-only
 * UFS probe next).
 *
 * Everything is also written to the ramoops console zone (DDR survives a
 * warm reset); readable with root: cat /sys/fs/pstore/console-ramoops*
 *
 * Register offsets follow the Linux SDE driver (msm-4.9 sde_hw_*.c); block
 * bases come from the live FDT. Only reads touch the display block. No
 * absolute pointers in data: ABL runs this image wherever it likes with no
 * relocation pass (see the notes on prz_write and on string tables below).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "mi8.h"

#ifdef MI8_HOST
#include "mi8_host.h"
#else
#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint64_t ticks(void) { __asm__ volatile("isb"); return rsr("cntpct_el0"); }
static inline uint64_t tfreq(void) { return rsr("cntfrq_el0"); }
#endif

/* ---- SDM845 display block map (live FDT) ---- */
#define MDP_BASE	0x0ae00000UL
#define CTL_OFF(n)	(0x2000 + (n) * 0x200)		/* qcom,sde-ctl-off */
#define PP_OFF(n)	(0x71000 + (n) * 0x800)		/* qcom,sde-pp-off */
#define INTF_1_OFF	0x6b800				/* DSI0 */
#define INTF_2_OFF	0x6c000				/* DSI1 */
#define DSI0_BASE	0x0ae94000UL

#define CTL_TOP		0x014
#define CTL_FLUSH	0x018
#define CTL_START	0x01c
#define CTL_INTF_ACTIVE	0x0f4
#define CTL_INTF_FLUSH	0x110
#define PP_TEAR_EN	0x000
#define PP_SYNC_VSYNC	0x004
#define PP_SYNC_HEIGHT	0x008
#define PP_LINE_COUNT	0x02c
#define PP_AUTOREFRESH	0x030

#define SSPP_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x2c
#define SSPP_FORMAT	0x30

/* ramoops@b0000000: 4 MB, record-size 0x40000, console-size 0x40000, ecc off.
 * The ROM kernel (4.9) puts the console zone after the dump records:
 * 4 * 0x40000 = +0x100000. +0x120000 is a hedge for kernels that lay it out
 * as size - console - ftrace - pmsg. */
#define PRZ_A		0xb0100000UL
#define PRZ_B		0xb0120000UL
#define PRZ_SIG		0x43474244U	/* 'DBGC' persistent_ram_buffer.sig */
#define LOG_MAX		7168

#define C_BLUE		0xff0000a0U
#define C_RED		0xffc00000U
#define C_GREEN		0xff006000U
#define C_PURPLE	0xff600080U

/* ---- result lines: filled at runtime, shown on screen and logged ---- */

/* Fixed-size inline text only: no pointer ever lives in data, so the compiler
 * has nothing to turn into an absolute relocation. */
struct line {
	char label[8];
	char an[40];	/* value name, or the whole text for kind 0 */
	char bn[8];
	uint32_t av, bv;
	uint8_t kind;	/* 0 = message, 1 = one or two hex values */
	uint8_t has_b;
};

#define MAXL	80
static struct line L[MAXL];
static int nl;
static char lb[LOG_MAX];
static unsigned ll;

static void cpy(char *dst, const char *src, unsigned max)
{
	unsigned i = 0;

	while (src && src[i] && i < max - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

static struct line *newline(const char *label, int idx)
{
	struct line *l;
	unsigned n;

	if (nl >= MAXL)
		return 0;
	l = &L[nl++];
	cpy(l->label, label, sizeof(l->label));
	if (idx >= 0) {
		for (n = 0; l->label[n]; n++)
			;
		if (n < sizeof(l->label) - 1) {
			l->label[n] = '0' + idx;
			l->label[n + 1] = 0;
		}
	}
	l->an[0] = l->bn[0] = 0;
	l->av = l->bv = 0;
	l->kind = l->has_b = 0;
	return l;
}

static void msg(const char *label, const char *text)
{
	struct line *l = newline(label, -1);

	if (l)
		cpy(l->an, text, sizeof(l->an));
}

/* label with optional trailing digit (idx < 0: none), one or two name=value pairs */
static void pair(const char *label, int idx, const char *an, uint32_t av, const char *bn, uint32_t bv)
{
	struct line *l = newline(label, idx);

	if (!l)
		return;
	cpy(l->an, an, sizeof(l->an));
	l->av = av;
	l->kind = 1;
	if (bn) {
		cpy(l->bn, bn, sizeof(l->bn));
		l->bv = bv;
		l->has_b = 1;
	}
}

static void lc(char c)
{
	if (ll < LOG_MAX - 1)
		lb[ll++] = c;
}

static void ls(const char *s)
{
	while (*s)
		lc(*s++);
}

static void lhex(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 28; i >= 0; i -= 4)
		lc(hex[(v >> i) & 0xf]);
}

static void build_log(void)
{
	ll = 0;
	ls("MI8DIAG v1 ");
	ls(BOARD_NAME);
	lc('\n');
	for (int i = 0; i < nl; i++) {
		ls(L[i].label);
		lc(' ');
		ls(L[i].an);
		if (L[i].kind) {
			lhex(L[i].av);
			if (L[i].has_b) {
				lc(' ');
				ls(L[i].bn);
				lhex(L[i].bv);
			}
		}
		lc('\n');
	}
}

/* struct persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; }
 * noinline/noclone + the empty asm keep `zone` a runtime value: if the compiler
 * knows it is a constant it folds "lb - zone" into a 64-bit constant-pool entry,
 * i.e. an absolute relocation, which this image cannot have. */
static void __attribute__((noinline, noclone)) prz_write(uintptr_t zone)
{
	__asm__ volatile("" : "+r"(zone));
	for (unsigned i = 0; i < ll; i++)
		wr8(zone + 12 + i, (uint8_t)lb[i]);
	wr32(zone + 4, 0);
	wr32(zone + 8, ll);
	wr32(zone + 0, PRZ_SIG);
}

static void flush_log(void)
{
	build_log();
	prz_write(PRZ_A);
	prz_write(PRZ_B);
	dsb();
}

/* ---- display ---- */

static void delay_ms(unsigned ms)
{
	uint64_t end = ticks() + tfreq() / 1000 * ms;

	while (ticks() < end)
		;
}

static void render(uint32_t bg, const char *title)
{
	con_clear(bg);
	con_str("MI8", title);
	for (int i = 0; i < nl; i++) {
		if (L[i].kind == 0)
			con_str(L[i].label, L[i].an);
		else
			con_hex2(L[i].label, L[i].an, L[i].av, L[i].has_b ? L[i].bn : 0, L[i].bv);
	}
	dsb();
}

static void scan_pipes(uint32_t *fb_addr, uint32_t *fb_stride)
{
	static const uint32_t sspp_off[8] = {
		0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
	};
	int active = 0;

	*fb_addr = 0;
	*fb_stride = 0;
	for (int i = 0; i < 8; i++) {
		uintptr_t b = MDP_BASE + sspp_off[i];
		uint32_t addr = rd32(b + SSPP_SRC0_ADDR);

		if (!addr)
			continue;
		active++;
		uint32_t ys = rd32(b + SSPP_YSTRIDE0);
		pair("SSPP", i, "addr=", addr, "ystr=", ys);
		pair(" fmt", -1, "fmt=", rd32(b + SSPP_FORMAT), "size=", rd32(b + SSPP_SIZE));
		if (!*fb_addr && addr >= 0x80000000U) {
			*fb_addr = addr;
			*fb_stride = ys & 0xffff;
		}
	}
	pair("SSPPs", -1, "active=", active, 0, 0);
}

static int tick_i = -1;

static void tick(uint32_t n)
{
	if (tick_i >= 0 && tick_i < nl) {
		L[tick_i].av = rd32(MDP_BASE + PP_OFF(0) + PP_LINE_COUNT);
		L[tick_i].bv = n;
	}
}

void mi8_diag(uintptr_t dtb)
{
	uint32_t fb_addr, fb_stride;

	nl = 0;
	msg("BOARD", BOARD_NAME);
	pair("CPU", -1, "midr=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	pair("MMFR0", -1, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf, "parng=", rsr("id_aa64mmfr0_el1") & 0xf);
	pair("EL", -1, "cur=", (uint32_t)(rsr("currentel") >> 2), "sctlr=", (uint32_t)rsr("sctlr_el1"));
	pair("DTB", -1, "at=", (uint32_t)dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	flush_log();	/* first thing: proves ramoops works even if MDSS reads hang */

	pair("MDP", -1, "rev=", rd32(MDP_BASE), 0, 0);
	flush_log();

	scan_pipes(&fb_addr, &fb_stride);
	if (fb_addr) {
		con_fb = (volatile uint32_t *)(uintptr_t)fb_addr;
		if (fb_stride && !(fb_stride & 3) && fb_stride <= 16384)
			con_stride4 = fb_stride / 4;
		if (fb_addr != FB_BASE)
			msg("FB", "ABL buffer is not cont_splash base");
	} else {
		msg("FB", "no active SSPP: splash off?");
	}
	pair("FBUSE", -1, "base=", (uint32_t)(uintptr_t)con_fb, "str4=", con_stride4);
	flush_log();

	for (int i = 0; i < 2; i++) {
		uintptr_t c = MDP_BASE + CTL_OFF(i);

		pair("CTL", i, "top=", rd32(c + CTL_TOP), "flush=", rd32(c + CTL_FLUSH));
		pair(" ", -1, "start=", rd32(c + CTL_START), "intfa=", rd32(c + CTL_INTF_ACTIVE));
	}
	flush_log();

	for (int i = 0; i < 2; i++) {
		uintptr_t p = MDP_BASE + PP_OFF(i);

		pair("PP", i, "tear=", rd32(p + PP_TEAR_EN), "vsync=", rd32(p + PP_SYNC_VSYNC));
		pair(" ", -1, "hght=", rd32(p + PP_SYNC_HEIGHT), "auto=", rd32(p + PP_AUTOREFRESH));
	}
	flush_log();

	pair("INTF", 1, "en=", rd32(MDP_BASE + INTF_1_OFF), "cfg=", rd32(MDP_BASE + INTF_1_OFF + 4));
	pair("INTF", 2, "en=", rd32(MDP_BASE + INTF_2_OFF), "cfg=", rd32(MDP_BASE + INTF_2_OFF + 4));
	pair("DSI0", -1, "ctrl=", rd32(DSI0_BASE + 4), 0, 0);
	flush_log();

	/* No display register is written from here on. A live counter line shows the
	 * pipeline is ticking. Straight-line code on purpose: a switch over colour
	 * names would become a table of string pointers (absolute relocations). */
	tick_i = nl;
	pair("PP0C", -1, "cnt=", 0, "n=", 0);
	flush_log();
	for (uint32_t round = 0; round < 1; round++) {
		tick(round * 4 + 0);
		render(C_BLUE, "cycle BLUE (no display writes)");
		delay_ms(3000);
		tick(round * 4 + 1);
		render(C_RED, "cycle RED (no display writes)");
		delay_ms(3000);
		tick(round * 4 + 2);
		render(C_GREEN, "cycle GREEN (no display writes)");
		delay_ms(3000);
		tick(round * 4 + 3);
		render(C_PURPLE, "cycle PURPLE (no display writes)");
		delay_ms(3000);
	}
	flush_log();
}

/* ---- touch bring-up probe (v6/v7): GPIO bit-banged I2C ----
 * v5 result on a real phone: SE6 = global SE14 (DT: i2c@a98000, pins gpio33/gpio34 function qup14, dmas present). Its
 * FIFO interface is disabled (the SE is GSI/DMA-only), so sweet's FIFO-mode driver cannot be used: sweet's own check
 * (SE_FIFO_IF_DISABLE_RO 0x64 bit0) rejected it (SEINIT rc=1). Same situation the LG V60 port hit; its answer was to
 * bit-bang the bus. So v6 touches no GCC or SE register at all: TLMM only.
 *   gpio33/34 = I2C pins (which one is SDA is not certain: both orders are tried), gpio31 = irq, gpio32 = reset,
 *   all in the TLMM north tile (0x3900000, cfg at pin*0x1000, io at +4: bit0 in, bit1 out value, cfg bit9 = output
 *   enable, bits[1:0] pull, bits[5:2] function, bits[8:6] drive). Open drain: drive low = latch 0 + output enable,
 *   release = output disable with pull-up. Touch rails (pm8998_l28 via RPMh) are NOT touched: an unpowered chip
 *   NACKs, a powered one ACKs and then returns its info block. */
#define TLMM_NORTH	0x03900000UL

static void memset_b(uint8_t *p, uint8_t v, unsigned n)
{
	while (n--)
		*p++ = v;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + tfreq() / 1000000 * us;

	while (ticks() < end)
		;
}

static uintptr_t pin_cfg(unsigned pin)
{
	return TLMM_NORTH + pin * 0x1000U;
}

/* sweet touch_gpio() */
static void touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, int out)
{
	uintptr_t off = pin_cfg(pin);
	uint32_t cfg = rd32(off);

	if (out)
		wr32(off + 4, rd32(off + 4) | 2U);	/* latch high before enabling the output: no reset pulse */
	cfg &= ~0x3ffU;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200U : 0U);
	wr32(off, cfg);
}

static unsigned bb_sda, bb_scl;

static void line_low(unsigned pin)
{
	uintptr_t off = pin_cfg(pin);

	wr32(off + 4, rd32(off + 4) & ~2U);
	wr32(off, rd32(off) | 0x200U);
}

static void line_rel(unsigned pin)
{
	uintptr_t off = pin_cfg(pin);

	wr32(off, rd32(off) & ~0x200U);
}

static int line_get(unsigned pin)
{
	return (int)(rd32(pin_cfg(pin) + 4) & 1U);
}

/* release SCL and wait for it to go high (clock stretching); 0 = ok, 1 = stuck low */
static int scl_high(void)
{
	line_rel(bb_scl);
	for (unsigned i = 0; i < 2000; i++) {
		if (line_get(bb_scl))
			return 0;
		delay_us(1);
	}
	return 1;
}

static void bb_setup(unsigned sda, unsigned scl)
{
	bb_sda = sda;
	bb_scl = scl;
	touch_gpio(sda, 0, 3, 1, 0);	/* GPIO function, pull-up, 4 mA, input (released) */
	touch_gpio(scl, 0, 3, 1, 0);
	delay_us(50);
}

static int bb_start(void)
{
	if (scl_high() || !line_get(bb_sda))
		return 1;
	delay_us(5);
	line_low(bb_sda);
	delay_us(5);
	line_low(bb_scl);
	delay_us(5);
	return 0;
}

static void bb_stop(void)
{
	line_low(bb_sda);
	delay_us(5);
	scl_high();
	delay_us(5);
	line_rel(bb_sda);
	delay_us(10);
}

/* write one byte, return the ACK bit (0 = ACK) or 2 on a stuck clock */
static int bb_wbyte(uint8_t v)
{
	int ack;

	for (int i = 7; i >= 0; i--) {
		if ((v >> i) & 1)
			line_rel(bb_sda);
		else
			line_low(bb_sda);
		delay_us(3);
		if (scl_high())
			return 2;
		delay_us(5);
		line_low(bb_scl);
		delay_us(3);
	}
	line_rel(bb_sda);
	delay_us(3);
	if (scl_high())
		return 2;
	delay_us(3);
	ack = line_get(bb_sda);
	delay_us(2);
	line_low(bb_scl);
	delay_us(3);
	return ack;
}

static uint8_t bb_rbyte(int ack)
{
	uint8_t v = 0;

	line_rel(bb_sda);
	for (int i = 0; i < 8; i++) {
		delay_us(3);
		scl_high();
		delay_us(3);
		v = (uint8_t)((v << 1) | (unsigned)line_get(bb_sda));
		delay_us(2);
		line_low(bb_scl);
	}
	if (ack)
		line_low(bb_sda);
	else
		line_rel(bb_sda);
	delay_us(3);
	scl_high();
	delay_us(5);
	line_low(bb_scl);
	delay_us(3);
	line_rel(bb_sda);
	return v;
}

/* 0 ok (ACK), 1 NACK at the address, 2 bus/clock stuck. Reads n bytes after writing the register pointer if reg >= 0. */
static int bb_read(uint8_t addr, int reg, uint8_t *buf, unsigned n)
{
	int a;

	if (bb_start())
		return 2;
	/* plain read: address with the R bit; register read: address with W, register, repeated start, address with R */
	a = bb_wbyte((uint8_t)((addr << 1) | (reg < 0 ? 1U : 0U)));
	if (a) {
		bb_stop();
		return a;
	}
	if (reg >= 0) {
		a = bb_wbyte((uint8_t)reg);
		if (a) {
			bb_stop();
			return a == 2 ? 2 : 1;
		}
		line_rel(bb_sda);
		delay_us(5);
		if (bb_start()) {
			bb_stop();
			return 2;
		}
		a = bb_wbyte((uint8_t)((addr << 1) | 1));
		if (a) {
			bb_stop();
			return a == 2 ? 2 : 1;
		}
	}
	for (unsigned i = 0; i < n; i++)
		buf[i] = bb_rbyte(i + 1 < n);
	bb_stop();
	return 0;
}

/* ---- RPMh client for ONE resource (port of sweet_rpmh.c, SM7150 -> SDM845) ----
 * apps_rsc DRV2 @ 0x179e0000, TCS bank @ 0x179e0d00 (DT: qcom,drv-id 2, tcs-config ACTIVE 2 / SLEEP 3 / WAKE 3 / CONTROL 1,
 * same layout as sweet). cmd-db locator 0x0c3f000c as on sweet; the db itself is at 0x85fe0000 on this phone.
 * Used for pm8998_l28 ("ldoa28", pmic4-ldo, the touch avdd rail): voltage, mode HPM, enable, ACTIVE set only.
 * Conservative like the original: refuses on any unexpected register state, never retries. */
#define RH_RSC		0x179e0000UL
#define RH_TCS		0x179e0d00UL
#define RH_DICT		0x0c3f000cUL
#define RH_DRV		2U
#define RH_TCS_SIZE	0x3000U
#define RH_ACTIVE_N	2U
#define RH_NORMAL_N	8U
#define RH_STRIDE	0x2a0U
#define RH_CMDSTR	0x14U
#define RH_IRQ_EN	0x00U
#define RH_IRQ_ST	0x04U
#define RH_IRQ_CLR	0x08U
#define RH_WAIT		0x10U
#define RH_CONTROL	0x14U
#define RH_STATUS	0x18U
#define RH_ENABLE	0x1cU
#define RH_MSGID	0x30U
#define RH_ADDR		0x34U
#define RH_DATA		0x38U
#define RH_CMDST	0x3cU
#define RH_AMC_EN	(1U << 16)
#define RH_TRIGGER	(1U << 24)
#define RH_ISSUED	(1U << 8)
#define RH_COMPLETE	(1U << 16)
#define RH_WRITE_ACK	(8U | (1U << 8) | (1U << 16))
#define RH_DB_MAGIC	0x0c0330dbU
#define RH_DB_HEADER	144U
#define RH_DB_ENTRY	24U
#define RH_VRM		4U

static uintptr_t rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;

static void rh_w(uint32_t off, uint32_t v)
{
	wr32(RH_TCS + off, v);
	dsb();
}

static int rh_sync(uint32_t off, uint32_t v)
{
	rh_w(off, v);
	for (unsigned i = 0; i < 1000; i++) {
		if (rd32(RH_TCS + off) == v)
			return 1;
		delay_us(1);
	}
	return 0;
}

static unsigned rh_db16(uint32_t off)
{
	return (unsigned)((rd32(rh_db + (off & ~3U)) >> (8U * (off & 3U))) & 0xffffU);
}

static int rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

/* 0 ok, 1 RSC geometry/revision unsupported, 2 cmd-db locator, 3 cmd-db header */
static int rh_map(void)
{
	uint32_t id = rd32(RH_RSC), solver = rd32(RH_RSC + 4), config = rd32(RH_RSC + 0x0c);
	unsigned major = (id >> 16) & 0xffU, count = (config >> (6U * RH_DRV)) & 0x3fU;
	uint32_t pa, size;

	rh_ncpt = (config >> 27) & 0x1fU;
	pair("RSC", -1, "id=", id, "cfg=", config);
	if ((major != 1 && major != 2) || config == 0xffffffffU || solver == 0xffffffffU || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_STRIDE || rh_ncpt < 3 || rh_ncpt > 16)
		return 1;
	pa = rd32(RH_DICT);
	size = rd32(RH_DICT + 4);
	pair("CMDDB", -1, "pa=", pa, "size=", size);
	if (pa < 0x80000000U || (pa & 3U) || (size & 3U) || size < RH_DB_HEADER || size > 0x100000U ||
	    (uint64_t)pa + size > 0x100000000ULL)
		return 2;
	rh_db = pa;
	rh_db_size = size;
	pair(" ", -1, "ver=", rd32(rh_db), "magic=", rd32(rh_db + 4));
	if (rd32(rh_db + 4) != RH_DB_MAGIC || rd32(rh_db) != 1U || (rd32(rh_db + 140) & 1U))
		return 3;
	return 0;
}

/* resource name "ldoa28": 0 ok (address in *address), 4 malformed db, 5 not exactly one match / bad VRM entry */
static int rh_lookup(uint32_t k0, uint32_t k1, uint32_t *address)
{
	unsigned matches = 0;

	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8U + 16U * i;
		unsigned slave = rh_db16(h);
		uint32_t entries, aux;
		unsigned count;

		if (!slave)
			break;
		entries = RH_DB_HEADER + rh_db16(h + 2);
		aux = RH_DB_HEADER + rh_db16(h + 4);
		count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3U) || !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0))
			return 4;
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j, addr;

			if (rd32(rh_db + e) != k0 || rd32(rh_db + e + 4) != k1)
				continue;
			addr = rd32(rh_db + e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15U) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20)))
				return 5;
			*address = addr;
			matches++;
		}
	}
	return matches == 1 ? 0 : 5;
}

static int rh_free(unsigned t)
{
	uint32_t b = t * RH_STRIDE;

	return rd32(RH_TCS + b + RH_STATUS) != 0 && rd32(RH_TCS + b + RH_STATUS) != 0xffffffffU &&
	       rd32(RH_TCS + b + RH_CONTROL) == 0 && rd32(RH_TCS + b + RH_ENABLE) == 0 &&
	       rd32(RH_TCS + b + RH_WAIT) == 0 && !(rd32(RH_TCS + RH_IRQ_ST) & (1U << t));
}

/* 0 ok, 6 inherited state/conflict/no free ACTIVE TCS, 7 register readback, 8 vote timeout (TCS left quarantined) */
static int rh_send(uint32_t address, unsigned mv)
{
	static const uint32_t offsets[3] = { 0, 8, 4 };	/* voltage, mode, enable (enable last) */
	uint32_t values[3];
	int chosen = -1;
	uint32_t irq_en, b, bit;
	unsigned t;

	values[0] = mv;
	values[1] = 7;	/* HPM */
	values[2] = 1;
	for (unsigned i = 0; i < RH_ACTIVE_N; i++) {
		uint32_t bb = i * RH_STRIDE, enabled = rd32(RH_TCS + bb + RH_ENABLE);

		if (enabled & ~((1U << rh_ncpt) - 1U))
			return 6;
		for (unsigned c = 0; c < rh_ncpt; c++)
			if ((enabled & (1U << c)) && (rd32(RH_TCS + bb + RH_ADDR + c * RH_CMDSTR) & ~15U) == address)
				return 6;
		if (chosen < 0 && rh_free(i))
			chosen = (int)i;
	}
	if (chosen < 0)
		return 6;
	t = (unsigned)chosen;
	b = t * RH_STRIDE;
	bit = 1U << t;
	irq_en = rd32(RH_TCS + RH_IRQ_EN);
	if (irq_en == 0xffffffffU || !rh_free(t))
		return 6;
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit))
		return 7;
	if (!rh_free(t)) {
		rh_sync(RH_IRQ_EN, (rd32(RH_TCS + RH_IRQ_EN) & ~bit) | (irq_en & bit));
		return 6;
	}
	for (unsigned c = 0; c < 3; c++) {
		uint32_t cmd = b + c * RH_CMDSTR;

		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) || !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c]))
			return 7;
	}
	if (!rh_sync(b + RH_WAIT, 7) || !rh_sync(b + RH_ENABLE, 7) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_CONTROL, RH_AMC_EN))
		return 7;
	rh_w(b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	{
		int done = 0, progress = 0;
		uint32_t irq = 0, idle = 0, st[3] = { 0, 0, 0 };

		for (unsigned us = 0; us < 100000U; us++) {
			int complete = 1;

			irq = rd32(RH_TCS + RH_IRQ_ST);
			idle = rd32(RH_TCS + b + RH_STATUS);
			for (unsigned c = 0; c < 3; c++) {
				st[c] = rd32(RH_TCS + b + RH_CMDST + c * RH_CMDSTR);
				complete &= st[c] != 0xffffffffU && (st[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
			}
			progress |= idle == 0 || !complete;
			if (idle && idle != 0xffffffffU && irq != 0xffffffffU && ((irq & bit) || progress) && complete) {
				done = 1;
				break;
			}
			delay_us(1);
		}
		pair("RHTCS", (int)t, "idle=", idle, "irq=", irq);
		pair(" ", -1, "st0=", st[0], "st1=", st[1]);
		if (!done)
			return 8;
	}
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_ENABLE, 0) ||
	    !rh_sync(b + RH_WAIT, 0))
		return 7;
	rh_w(RH_IRQ_CLR, bit);	/* write-1-to-clear: only our TCS */
	if ((rd32(RH_TCS + RH_IRQ_ST) & bit) || !rh_sync(RH_IRQ_EN, (rd32(RH_TCS + RH_IRQ_EN) & ~bit) | (irq_en & bit)))
		return 7;
	return 0;
}

/* pm8998_l28 "ldoa28" at 3.3 V; returns 0 on success, otherwise a code (see the helpers above) */
static int rh_enable_l28(void)
{
	uint32_t address = 0;
	int rc = rh_map();

	if (!rc)
		rc = rh_lookup(0x616f646cU, 0x00003832U, &address);	/* "ldoa28\0\0" */
	if (!rc) {
		pair("L28", -1, "addr=", address, 0, 0);
		rc = rh_send(address, 3300);
	}
	return rc;
}

/* does the pin follow what we drive? (proves TLMM writes and the input path work) */
static void pin_selftest(unsigned pin, int idx)
{
	uintptr_t off = pin_cfg(pin);
	uint32_t hi, lo, rel;

	touch_gpio(pin, 0, 3, 1, 0);
	rel = rd32(off + 4) & 1U;
	wr32(off + 4, rd32(off + 4) | 2U);
	wr32(off, rd32(off) | 0x200U);	/* output high */
	delay_us(50);
	hi = rd32(off + 4) & 1U;
	wr32(off + 4, rd32(off + 4) & ~2U);	/* output low */
	delay_us(50);
	lo = rd32(off + 4) & 1U;
	wr32(off, rd32(off) & ~0x200U);	/* release */
	delay_us(50);
	pair("SELF", idx, "hi=", hi, "lo=", lo);
	pair(" ", -1, "rel=", rel, "cfg=", rd32(off));
}

/* TLMM tiles on this SoC family: north 0x3900000 (holds gpio31-34), south 0x3d00000, west 0x3500000. The right tile for a
 * pin is the one where its neighbours read non-zero (an unmapped tile reads 0). */
static uintptr_t find_pin(unsigned pin)
{
	static const uint32_t tile_off[3] = { 0x03900000U, 0x03d00000U, 0x03500000U };

	for (int i = 0; i < 3; i++) {
		uintptr_t base = tile_off[i];
		uint32_t me = rd32(base + pin * 0x1000U), lo = rd32(base + (pin - 1) * 0x1000U),
			 hi = rd32(base + (pin + 1) * 0x1000U);

		pair("TILE", i, "p23=", me, "p22=", lo);
		pair(" ", -1, "p24=", hi, 0, 0);
		if (lo | hi)
			return base + pin * 0x1000U;
	}
	return TLMM_NORTH + pin * 0x1000U;
}

/* disp_vddio_vreg (touch vdd, 1.8 V) is a fixed regulator enabled by GPIO23, active high, startup delay 4 ms.
 * Drive it high if the pin answers (output high reads 1, output low reads 0), else restore it. 1 = enabled. */
static int vddio_enable(void)
{
	uintptr_t off = find_pin(23);
	uint32_t cfg = rd32(off), io = rd32(off + 4), hi, lo;

	wr32(off + 4, io | 2U);
	wr32(off, (cfg & ~0x3ffU) | 0x200U | (1U << 6));	/* gpio, no pull, 4 mA, output */
	delay_us(50);
	hi = rd32(off + 4) & 1U;
	wr32(off + 4, rd32(off + 4) & ~2U);
	delay_us(50);
	lo = rd32(off + 4) & 1U;
	pair("V23", -1, "hi=", hi, "lo=", lo);
	if (hi == 1U && lo == 0U) {
		wr32(off + 4, rd32(off + 4) | 2U);
		delay_ms(10);
		pair(" ", -1, "cfg=", rd32(off), "io=", rd32(off + 4));
		return 1;
	}
	wr32(off + 4, io);
	wr32(off, cfg);
	return 0;
}

static void idle_levels(int stage)
{
	for (unsigned order = 0; order < 2; order++) {
		unsigned sda = order ? 34 : 33, scl = order ? 33 : 34;

		bb_setup(sda, scl);
		pair("IDLE", (int)(order + stage * 2), "sda=", (uint32_t)line_get(sda), "scl=", (uint32_t)line_get(scl));
	}
}

static void bb_scan(int stage)
{
	uint8_t b[8];

	for (unsigned order = 0; order < 2; order++) {
		unsigned sda = order ? 34 : 33, scl = order ? 33 : 34;
		int rc;

		bb_setup(sda, scl);
		pair("BBPINS", (int)(order + stage * 2), "sda=", sda, "idle=", (uint32_t)(line_get(sda) | (line_get(scl) << 1)));
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x49, -1, b, 1);
		pair("I2C49", (int)(order + stage * 2), "rc=", (uint32_t)rc, "byte=", b[0]);
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x4a, -1, b, 1);
		pair("I2C4A", (int)(order + stage * 2), "rc=", (uint32_t)rc, "byte=", b[0]);
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x49, 0x80, b, 8);	/* ST FTS READ_INFO (stmfts): meaningful only if the chip answered */
		pair("INFO", (int)(order + stage * 2), "rc=", (uint32_t)rc,
		     "b0-3=", (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
		flush_log();
	}
}

void mi8_touch_probe(void)
{
	int rc;

	nl = 0;
	tick_i = -1;
	msg("TOUCH", "mi8 touch v8: vddio + avdd + gpio i2c");
	for (unsigned p = 31; p <= 34; p++)
		pair("PIN", (int)(p - 30), "cfg=", rd32(pin_cfg(p)), "io=", rd32(pin_cfg(p) + 4));
	flush_log();

	/* release the touch pins like sweet: irq = input pull-up, reset = output high */
	touch_gpio(31, 0, 3, 3, 0);
	touch_gpio(32, 0, 3, 3, 1);
	delay_ms(300);
	pin_selftest(33, 0);
	pin_selftest(34, 1);
	flush_log();

	pair("VDDIO", 1, "ok=", (uint32_t)vddio_enable(), 0, 0);	/* touch vdd: disp_vddio_vreg, gpio23 */
	idle_levels(0);
	flush_log();
	bb_scan(0);	/* vddio only */
	render(C_BLUE, "touch v8 1/2: vddio on, no avdd vote");
	delay_ms(8000);

	/* pm8998_l28 (touch avdd) through RPMh, then a reset pulse so the chip boots with power present */
	rc = rh_enable_l28();
	pair("RAIL", -1, "rc=", (uint32_t)rc, 0, 0);
	flush_log();
	if (!rc) {
		delay_ms(20);
		touch_gpio(32, 0, 3, 3, 1);
		wr32(pin_cfg(32) + 4, rd32(pin_cfg(32) + 4) & ~2U);	/* reset low */
		delay_ms(10);
		wr32(pin_cfg(32) + 4, rd32(pin_cfg(32) + 4) | 2U);	/* reset high */
		delay_ms(400);
		idle_levels(1);
		bb_scan(1);
	}
	pair("LEGEND", -1, "rc 0=ack", 0, "1=nack", 0);
	pair(" ", -1, "2=stuck", 0, "RAIL 0=ok", 0);
	flush_log();
	render(C_GREEN, "touch v8 2/2: after avdd vote");
	delay_ms(120000);
}

void mi8_panic_log(const char *stage, uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	msg("PANIC", stage);
	pair("SLOT", -1, "n=", (uint32_t)slot, "esr=", (uint32_t)esr);
	pair(" ", -1, "elr=", (uint32_t)elr, "far=", (uint32_t)far);
	flush_log();
}
