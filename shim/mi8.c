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

/* ---- touch bring-up probe (v6): GPIO bit-banged I2C ----
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

void mi8_touch_probe(void)
{
	uint8_t b[8];
	int rc;

	nl = 0;
	tick_i = -1;
	msg("TOUCH", "mi8 touch v6: gpio bit-bang i2c");
	for (unsigned p = 31; p <= 34; p++)
		pair("PIN", (int)(p - 30), "cfg=", rd32(pin_cfg(p)), "io=", rd32(pin_cfg(p) + 4));
	flush_log();

	/* release the touch pins like sweet: irq = input pull-up, reset = output high */
	touch_gpio(31, 0, 3, 3, 0);
	touch_gpio(32, 0, 3, 3, 1);
	delay_ms(300);
	pair("PIN31", -1, "io=", rd32(pin_cfg(31) + 4), "p32io=", rd32(pin_cfg(32) + 4));

	for (unsigned order = 0; order < 2; order++) {
		unsigned sda = order ? 34 : 33, scl = order ? 33 : 34;
		int idle;

		bb_setup(sda, scl);
		idle = line_get(sda) | (line_get(scl) << 1);
		pair("BBPINS", (int)order, "sda=", sda, "idle=", (uint32_t)idle);
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x49, -1, b, 1);
		pair("I2C49", (int)order, "rc=", (uint32_t)rc, "byte=", b[0]);
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x4a, -1, b, 1);
		pair("I2C4A", (int)order, "rc=", (uint32_t)rc, "byte=", b[0]);
		memset_b(b, 0, sizeof(b));
		rc = bb_read(0x49, 0x80, b, 8);	/* ST FTS READ_INFO (stmfts): only meaningful if the chip answered */
		pair("INFO", (int)order, "rc=", (uint32_t)rc, "b0-3=", (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
		flush_log();
	}
	pair("LEGEND", -1, "rc 0=ack", 0, "1=nack", 0);
	pair(" ", -1, "2=stuck", 0, 0, 0);
	flush_log();
	render(C_GREEN, "touch v6: gpio i2c");
	delay_ms(120000);
}

void mi8_panic_log(const char *stage, uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	msg("PANIC", stage);
	pair("SLOT", -1, "n=", (uint32_t)slot, "esr=", (uint32_t)esr);
	pair(" ", -1, "elr=", (uint32_t)elr, "far=", (uint32_t)far);
	flush_log();
}
