/* Host test for shim/mi8.c: fake MMIO + fake ramoops, checks the pstore layout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "console.h"
#include "mi8.h"

volatile uint32_t *con_fb;
uint32_t con_stride4 = 1080;
static int renders, mdp_writes;
static char screen_titles[8][64];

void con_clear(uint32_t bg) { (void)bg; }
void con_skip(void) {}
void con_hex(const char *l, uint64_t v) { (void)l; (void)v; }
void con_str(const char *l, const char *s) { if (!strcmp(l, "MI8") && renders < 8) strcpy(screen_titles[renders++], s); }
void con_hex2(const char *l, const char *a, uint32_t va, const char *b, uint32_t vb) { (void)l; (void)a; (void)va; (void)b; (void)vb; }

static uint8_t ram[2][0x40000];
static uint32_t regs_sspp0_active = 1;
static uint64_t tick;

uint64_t ticks(void) { return tick += 1000; }
uint64_t tfreq(void) { return 19200000; }
uint64_t host_rsr(const char *n)
{
	if (!strcmp(n, "midr_el1")) return 0x51df8030;
	if (!strcmp(n, "mpidr_el1")) return 0x81000000;
	if (!strcmp(n, "id_aa64mmfr0_el1")) return 0x00100122;
	if (!strcmp(n, "currentel")) return 4;
	if (!strcmp(n, "sctlr_el1")) return 0x30d00800;
	return 0;
}

#define TLMM_N 0x03900000UL
static uint32_t pcfg[160], pout[160];

/* --- I2C slave model on SDA = gpio33, SCL = gpio34: FTS-like device at 0x49, reg 0x80 returns 36 01 02 03 ... --- */
enum { S_IDLE, S_ADDR, S_ACKA, S_WR, S_ACKW, S_RD, S_MACK, S_IGN };
static int st = S_IDLE, bits, rw, slave_low, regptr = -1, rdi, expect_reg, mack;
static unsigned shift;
static int last_sda = 1, last_scl = 1;

static int master_low(unsigned pin) { return (pcfg[pin] & 0x200) && !(pout[pin] & 1); }
static int line_sda(void) { return !master_low(33) && !slave_low; }
static int line_scl(void) { return !master_low(34); }
static unsigned char rdbyte(void) { return regptr == 0x80 ? (unsigned char[]){0x36, 1, 2, 3, 4, 5, 6, 7}[rdi & 7] : 0xA5; }

static void slave_step(void)
{
	int sda = line_sda(), scl = line_scl();

	if (scl && last_scl && last_sda && !sda) { st = S_ADDR; bits = 0; shift = 0; expect_reg = 1; }	/* START */
	else if (scl && last_scl && !last_sda && sda) { st = S_IDLE; slave_low = 0; }				/* STOP */
	else if (scl && !last_scl) {										/* rising */
		if (st == S_ADDR || st == S_WR) { shift = (shift << 1) | (unsigned)sda; bits++; }
		else if (st == S_MACK) mack = !sda;
	} else if (!scl && last_scl) {										/* falling */
		if (st == S_ADDR && bits == 8) {
			rw = shift & 1;
			if ((shift >> 1) == 0x49) { st = S_ACKA; slave_low = 1; } else st = S_IGN;
		} else if (st == S_ACKA) {
			slave_low = 0;
			if (rw) { st = S_RD; bits = 0; rdi = 0; slave_low = !(rdbyte() & 0x80); } else { st = S_WR; bits = 0; shift = 0; }
		} else if (st == S_WR && bits == 8) {
			if (expect_reg) { regptr = (int)shift; expect_reg = 0; }
			st = S_ACKW; slave_low = 1;
		} else if (st == S_ACKW) { slave_low = 0; st = S_WR; bits = 0; shift = 0; }
		else if (st == S_RD) {
			bits++;
			if (bits == 8) { slave_low = 0; st = S_MACK; }
			else if (bits > 0) slave_low = !((rdbyte() >> (7 - bits)) & 1);
		} else if (st == S_MACK) {
			if (mack) { rdi++; st = S_RD; bits = 0; slave_low = !(rdbyte() & 0x80); } else { st = S_IGN; slave_low = 0; }
		}
	}
	last_sda = line_sda(); last_scl = line_scl();
}

uint32_t rd32(uintptr_t a)
{
	if (a == 0x0ae00000UL + 0x5000 + 0x14 && regs_sspp0_active) return 0x9d400000;
	if (a == 0x0ae00000UL + 0x5000 + 0x2c && regs_sspp0_active) return 0x10e0;
	if (a == 0x0ae00000UL + 0x5000 + 0x30 && regs_sspp0_active) return 0x000236ff;
	if (a == 0x0ae00000UL + 0x5000 + 0x00 && regs_sspp0_active) return 0x08c80438;
	if (a == 0x0ae00000UL) return 0x40000000;
	if (a >= TLMM_N && a < TLMM_N + 160 * 0x1000UL) {	/* TLMM north tile: cfg at pin*0x1000, io at +4 */
		unsigned pin = (unsigned)((a - TLMM_N) / 0x1000), off = (unsigned)((a - TLMM_N) % 0x1000);
		if (off == 0) return pcfg[pin];
		if (off == 4) {
			unsigned in = (pcfg[pin] & 0x200) ? (pout[pin] & 1) : 1;
			if (pin == 33) in = (unsigned)line_sda();
			if (pin == 34) in = (unsigned)line_scl();
			return in | (pout[pin] << 1);
		}
	}
	return 0;
}

void wr32(uintptr_t a, uint32_t v)
{
	if (a >= 0xb0120000UL && a < 0xb0160000UL) { memcpy(&ram[1][a - 0xb0120000UL], &v, 4); return; }
	if (a >= 0xb0100000UL && a < 0xb0140000UL) { memcpy(&ram[0][a - 0xb0100000UL], &v, 4); return; }
	if (a >= TLMM_N && a < TLMM_N + 160 * 0x1000UL) {
		unsigned pin = (unsigned)((a - TLMM_N) / 0x1000), off = (unsigned)((a - TLMM_N) % 0x1000);
		if (off == 0) pcfg[pin] = v;
		if (off == 4) pout[pin] = (v >> 1) & 1;
		if (pin == 33 || pin == 34) slave_step();
		return;
	}
	if (a >= 0x0ae00000UL && a < 0x0af00000UL) mdp_writes++;	/* v2 must never write the display block */
}
void wr8(uintptr_t a, uint8_t v)
{
	if (a >= 0xb0120000UL && a < 0xb0160000UL) ram[1][a - 0xb0120000UL] = v;
	else if (a >= 0xb0100000UL && a < 0xb0140000UL) ram[0][a - 0xb0100000UL] = v;
	else { fprintf(stderr, "stray wr8 %lx\n", (unsigned long)a); exit(2); }
}

static int check_zone(const uint8_t *z, const char *want)
{
	uint32_t sig, start, size;
	memcpy(&sig, z, 4); memcpy(&start, z + 4, 4); memcpy(&size, z + 8, 4);
	if (sig != 0x43474244 || start != 0 || size == 0 || size > 0x40000 - 12) { printf("bad header sig=%x start=%u size=%u\n", sig, start, size); return 1; }
	char *txt = malloc(size + 1); memcpy(txt, z + 12, size); txt[size] = 0;
	int ok = strstr(txt, want) != NULL;
	if (!ok) printf("missing '%s'\n", want);
	free(txt);
	return !ok;
}

int main(void)
{
	int bad = 0;
	/* Zone A and B overlap (B starts 0x20000 into A's console zone, as on real hardware); both must be valid
	 * from their own start, so check the combined effect through separate buffers: ram[1] mirrors the B copy. */
	mi8_diag(0x9c000000UL ? 0x85a00000UL : 0);
	bad += check_zone(ram[0], "SSPP0 addr=9d400000");
	bad += check_zone(ram[1], "SSPP0 addr=9d400000");
	bad += check_zone(ram[0], "ystr=000010e0");
	bad += check_zone(ram[0], "FBUSE base=9d400000 str4=00000438");
	bad += check_zone(ram[0], "PP0C cnt=");
	if (renders != 4 || mdp_writes != 0) { printf("renders=%d mdp_writes=%d\n", renders, mdp_writes); bad++; }
	if ((uintptr_t)con_fb != 0x9d400000 || con_stride4 != 1080) { printf("fb=%p stride4=%u\n", (void *)con_fb, con_stride4); bad++; }
	printf("titles: %s | %s | %s | %s\n", screen_titles[0], screen_titles[1], screen_titles[2], screen_titles[3]);

	mi8_touch_probe();
	if (getenv("DEBUG_DUMP")) { uint32_t sz; memcpy(&sz, ram[0] + 8, 4); fwrite(ram[0] + 12, 1, sz, stdout); }
	bad += check_zone(ram[0], "TOUCH mi8 touch v6: gpio bit-bang i2c");
	bad += check_zone(ram[0], "BBPINS0 sda=00000021 idle=00000003");	/* both lines idle high */
	bad += check_zone(ram[0], "BBPINS1 sda=00000022 idle=00000003");
	bad += check_zone(ram[0], "I2C490 rc=00000000 byte=000000a5");		/* FTS-like slave at 0x49 ACKs */
	bad += check_zone(ram[0], "I2C4A0 rc=00000001");			/* nothing at 0x4a: NACK */
	bad += check_zone(ram[0], "INFO0 rc=00000000 b0-3=03020136");	/* register 0x80 read with repeated start */
	/* no active pipe */
	regs_sspp0_active = 0; renders = 0; mdp_writes = 0; con_fb = 0; con_stride4 = 1080;
	mi8_diag(0);
	bad += check_zone(ram[0], "no active SSPP");
	mi8_panic_log("gic", 0, 0x96000010, 0xffff, 0x1234);
	bad += check_zone(ram[0], "PANIC gic");
	puts(bad ? "FAIL" : "PASS");
	return bad;
}
