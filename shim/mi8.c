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

/* ---- touch bring-up probe (v4) ----
 * Mi 8 touch: STM FTS at i2c 0x49 on i2c@a98000 (QUPV3_1 SE6), irq GPIO31, reset GPIO32 (TLMM north tile),
 * avdd = pm8998_l28 3.3 V (RPMh, NOT touched here), vdd = fixed disp_vddio_vreg.
 * v3 result on a real phone: SE6 has I2C firmware (fw=0x300) and 32-bit FIFOs, but its clock is off (vote bit 28
 * clear, RCG root off), so the I2C read timed out; pins 31/32 sit in the suspend state (output, io=0).
 * v4 does what sweet's driver does, with the SDM845 GCC offsets (wrapper1 S6: RCG 0x18868, branch 0x18864, vote
 * bit 28; wrapper1 core/ahb: bits 18-21): vote the clocks, init the GENI SE exactly like sweet_touch.c, release
 * the touch pins, then one I2C read at 0x49 and 0x4a. Expected: rc=1 (NACK) = master works, chip unpowered
 * (needs the RPMh L28 vote); rc=0 = chip answered. GCC writes happen only if the registers look as expected. */
#define GCC_BASE	0x00100000UL
#define GCC_QUP_VOTE	0x5200c
#define GCC_RCG_W1S6	0x18868
#define GCC_BR_S6	0x18864
#define TLMM_NORTH	0x03900000UL
#define SE6_BASE	0x00a98000UL
#define SE_STATUS	0x040
#define SE_FW		0x068
#define SE_DMA		0x258
#define SE_RX_LEN	0x270
#define SE_SCL		0x278
#define SE_CMD		0x600
#define SE_CTRL		0x604
#define SE_IRQ		0x610
#define SE_IRQ_EN	0x614
#define SE_IRQ_CLR	0x618
#define SE_RX		0x780
#define SE_TX_COUNT	0x800
#define SE_RX_COUNT	0x804
#define SE_HW_TX	0xe24
#define SE_HW_RX	0xe28
#define SE_DONE		(1U << 0)
#define SE_NACK		(1U << 10)
#define SE_ERRORS	(0x3eU | (1U << 9) | (1U << 10) | (1U << 12) | (1U << 13) | (3U << 24) | (3U << 28))
#define SE_EVENTS	(SE_DONE | SE_ERRORS | (1U << 26) | (1U << 27) | (1U << 30))

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + tfreq() / 1000000 * us;

	while (ticks() < end)
		;
}

static int wait_clear(uintptr_t addr, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; i++) {
		if (!(rd32(addr) & mask))
			return 1;
		delay_us(1);
	}
	return 0;
}

/* 0 ok; 1 sanity check failed (nothing written); 2 DFS level 0 is not XO; 3 RCG update timeout; 4+n branch n stuck */
static int se6_clocks(void)
{
	uintptr_t rcg = GCC_BASE + GCC_RCG_W1S6;
	static const uint32_t br[5] = { 0x18004, 0x18008, 0x1800c, 0x18010, GCC_BR_S6 };
	uint32_t cfg, vote;

	if (!(rd32(rcg) & 0x80000000U) || !(rd32(GCC_BASE + GCC_BR_S6) & 0x80000000U))
		return 1;
	vote = rd32(GCC_BASE + GCC_QUP_VOTE);
	wr32(GCC_BASE + GCC_QUP_VOTE, vote | (0x0fU << 18) | (1U << 28));
	if (rd32(rcg + 0x14) & 1U) {
		if ((rd32(rcg + 0x1c) & 0x371fU) != 1U)
			return 2;
	} else {
		cfg = rd32(rcg + 4);
		cfg = (cfg & ~(0x371fU | (1U << 20))) | 1U;
		wr32(rcg + 4, cfg);
		wr32(rcg, rd32(rcg) | 1U);
		if (!wait_clear(rcg, 1U))
			return 3;
	}
	for (unsigned i = 0; i < 5; i++)
		if (!wait_clear(GCC_BASE + br[i], 0x80000000U))
			return 4 + (int)i;
	return 0;
}

/* sweet_touch.c touch_se_init(), verbatim register programming (GENI SE layout is the same on every SoC). */
static int se6_init(void)
{
	uint32_t fw = rd32(SE6_BASE + SE_FW), tx = rd32(SE6_BASE + SE_HW_TX), rx = rd32(SE6_BASE + SE_HW_RX);

	if (((fw >> 8) & 0xffU) != 3U || (rd32(SE6_BASE + 0x64) & 1U) || ((tx >> 24) & 0x3fU) != 32U ||
	    ((rx >> 24) & 0x3fU) != 32U || ((tx >> 16) & 0x3fU) < 2U || ((rx >> 16) & 0x3fU) < 2U)
		return 1;
	if (rd32(SE6_BASE + SE_STATUS) & 0x1001U)
		return 2;
	wr32(SE6_BASE + 0xe18, 0);
	wr32(SE6_BASE + 0xe1c, 0);
	wr32(SE6_BASE + 0x644, 0);
	wr32(SE6_BASE + SE_IRQ_CLR, 0xffffffffU);
	wr32(SE6_BASE + 0x648, 0xffffffffU);
	wr32(SE6_BASE + SE_DMA, 0);
	wr32(SE6_BASE + 0x28, rd32(SE6_BASE + 0x28) | 0x7fU);
	wr32(SE6_BASE + 0xe30, rd32(SE6_BASE + 0xe30) | 0x0fU);
	wr32(SE6_BASE + 0x24, 0x7f);
	wr32(SE6_BASE + 0x20, 1);
	wr32(SE6_BASE + 0x7c, 0);
	wr32(SE6_BASE + 0x48, (7U << 4) | 1U);
	wr32(SE6_BASE + SE_SCL, (10U << 20) | (11U << 10) | 26U);	/* 100 kHz */
	wr32(SE6_BASE + 0x254, 0);
	wr32(SE6_BASE + 0x260, 0x0004380eU);
	wr32(SE6_BASE + 0x264, 0x000c3e0eU);
	wr32(SE6_BASE + 0x284, 0x0004380eU);
	wr32(SE6_BASE + 0x288, 0x000c3e0eU);
	wr32(SE6_BASE + 0x80c, 1);
	wr32(SE6_BASE + 0x810, 0);
	wr32(SE6_BASE + 0x814, ((rx >> 16) & 0x3fU) - 1U);
	wr32(SE6_BASE + SE_IRQ_EN, SE_EVENTS);
	if ((rd32(SE6_BASE + SE_TX_COUNT) & 0xfffffffU) || (rd32(SE6_BASE + SE_RX_COUNT) & 0x1ffffffU))
		return 3;
	return 0;
}

/* sweet touch_gpio(): TLMM cfg at pin*0x1000, io at +4 (bit1 = output value) */
static void touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, int out)
{
	uintptr_t off = TLMM_NORTH + pin * 0x1000U;
	uint32_t cfg = rd32(off);

	if (out)
		wr32(off + 4, rd32(off + 4) | 2U);	/* latch high before enabling the output: no reset pulse */
	cfg &= ~0x3ffU;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200U : 0U);
	wr32(off, cfg);
}

/* 0 ok, 1 NACK, 2 bus error, 3 timeout. *last = last IRQ status seen. */
static int se6_read(uint8_t addr, uint8_t *buf, unsigned nr, uint32_t *last)
{
	unsigned got = 0;

	if (!wait_clear(SE6_BASE + SE_STATUS, 1U))
		return 3;
	wr32(SE6_BASE + SE_IRQ_CLR, 0xffffffffU);
	wr32(SE6_BASE + SE_RX_LEN, nr);
	wr32(SE6_BASE + SE_CMD, (2U << 27) | ((uint32_t)addr << 9));	/* I2C READ */
	for (unsigned i = 0; i < 20000; i++) {
		uint32_t irq = rd32(SE6_BASE + SE_IRQ);
		uint32_t count = rd32(SE6_BASE + SE_RX_COUNT) & 0x1ffffffU;

		*last = irq;
		if (irq & SE_ERRORS) {
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				delay_us(1);
				irq = rd32(SE6_BASE + SE_IRQ);
			}
			*last = irq;
			wr32(SE6_BASE + SE_IRQ_CLR, irq);
			return (irq & SE_NACK) ? 1 : 2;
		}
		if (count > 64)
			return 2;
		while (count--) {
			uint32_t w = rd32(SE6_BASE + SE_RX);

			for (unsigned j = 0; j < 4 && got < nr; j++)
				buf[got++] = (uint8_t)(w >> (8U * j));
		}
		if (irq & SE_DONE) {
			wr32(SE6_BASE + SE_IRQ_CLR, irq);
			return got == nr ? 0 : 2;
		}
		delay_us(1);
	}
	wr32(SE6_BASE + SE_CTRL, 2);	/* abort, release the bus */
	delay_us(2000);
	wr32(SE6_BASE + SE_IRQ_CLR, 0xffffffffU);
	return 3;
}

void mi8_touch_probe(void)
{
	uint8_t b[4] = { 0, 0, 0, 0 };
	uint32_t last = 0;
	int ck, ini, rc;

	nl = 0;
	tick_i = -1;
	msg("TOUCH", "mi8 touch bring-up v4");
	pair("GCC", -1, "qupvt=", rd32(GCC_BASE + GCC_QUP_VOTE), "br6=", rd32(GCC_BASE + GCC_BR_S6));
	pair("RCG", -1, "cmd=", rd32(GCC_BASE + GCC_RCG_W1S6), "cfg=", rd32(GCC_BASE + GCC_RCG_W1S6 + 4));
	pair(" ", -1, "dfs=", rd32(GCC_BASE + GCC_RCG_W1S6 + 0x14), "l0=", rd32(GCC_BASE + GCC_RCG_W1S6 + 0x1c));
	pair("PINS", -1, "p31=", rd32(TLMM_NORTH + 31 * 0x1000U), "p32=", rd32(TLMM_NORTH + 32 * 0x1000U));
	flush_log();

	ck = se6_clocks();
	pair("CLK", -1, "rc=", (uint32_t)ck, "vote=", rd32(GCC_BASE + GCC_QUP_VOTE));
	pair(" ", -1, "br6=", rd32(GCC_BASE + GCC_BR_S6), "cmd=", rd32(GCC_BASE + GCC_RCG_W1S6));
	flush_log();
	render(C_BLUE, "touch v4 1/2: clocks");
	delay_ms(6000);

	ini = ck ? 9 : se6_init();
	pair("SEINIT", -1, "rc=", (uint32_t)ini, "st=", rd32(SE6_BASE + SE_STATUS));
	flush_log();
	if (!ini) {
		touch_gpio(31, 0, 3, 3, 0);	/* interrupt: input, pull-up, 8 mA */
		touch_gpio(32, 0, 3, 3, 1);	/* reset released (output high) */
		delay_ms(300);
		pair("PINS2", -1, "p31io=", rd32(TLMM_NORTH + 31 * 0x1000U + 4), "p32io=",
		     rd32(TLMM_NORTH + 32 * 0x1000U + 4));
		flush_log();
		rc = se6_read(0x49, b, 1, &last);
		pair("I2C49", -1, "rc=", (uint32_t)rc, "irq=", last);
		pair(" ", -1, "byte=", b[0], 0, 0);
		flush_log();
		rc = se6_read(0x4a, b, 1, &last);
		pair("I2C4A", -1, "rc=", (uint32_t)rc, "irq=", last);
	}
	pair("LEGEND", -1, "rc 0=ack", 0, "1=nack", 0);
	flush_log();
	render(C_GREEN, "touch v4 2/2: se6 init + i2c");
	delay_ms(120000);
}

void mi8_panic_log(const char *stage, uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	msg("PANIC", stage);
	pair("SLOT", -1, "n=", (uint32_t)slot, "esr=", (uint32_t)esr);
	pair(" ", -1, "elr=", (uint32_t)elr, "far=", (uint32_t)far);
	flush_log();
}
