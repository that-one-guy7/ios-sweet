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
static int vote_done, cmd_done;

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

uint32_t rd32(uintptr_t a)
{
	if (a == 0x0ae00000UL + 0x5000 + 0x14 && regs_sspp0_active) return 0x9d400000;
	if (a == 0x0ae00000UL + 0x5000 + 0x2c && regs_sspp0_active) return 0x10e0;
	if (a == 0x0ae00000UL + 0x5000 + 0x30 && regs_sspp0_active) return 0x000236ff;
	if (a == 0x0ae00000UL + 0x5000 + 0x00 && regs_sspp0_active) return 0x08c80438;
	if (a == 0x0ae00000UL) return 0x40000000;
	/* touch bring-up fakes: SE6 clocks off until the vote is written, then an unpowered chip NACKs */
	if (a == 0x00100000UL + 0x18868) return 0x80000000;			/* RCG root off */
	if (a == 0x00100000UL + 0x18864 || a == 0x00100000UL + 0x18004 || a == 0x00100000UL + 0x18008 ||
	    a == 0x00100000UL + 0x1800c || a == 0x00100000UL + 0x18010) return vote_done ? 0 : 0x80000000;
	if (a == 0x00a98000UL + 0x68) return 0x300;
	if (a == 0x00a98000UL + 0xe24) return 0x20202864;
	if (a == 0x00a98000UL + 0xe28) return 0x20204900;
	if (a == 0x00a98000UL + 0x610) return cmd_done ? ((1U << 10) | 1U) : 0;	/* NACK + DONE after a command */
	return 0;
}

void wr32(uintptr_t a, uint32_t v)
{
	if (a >= 0xb0120000UL && a < 0xb0160000UL) { memcpy(&ram[1][a - 0xb0120000UL], &v, 4); return; }
	if (a >= 0xb0100000UL && a < 0xb0140000UL) { memcpy(&ram[0][a - 0xb0100000UL], &v, 4); return; }
	if (a == 0x00100000UL + 0x5200c) vote_done = 1;
	if (a == 0x00a98000UL + 0x600) cmd_done = 1;
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
	bad += check_zone(ram[0], "TOUCH mi8 touch bring-up v4");
	bad += check_zone(ram[0], "CLK rc=00000000");
	bad += check_zone(ram[0], "SEINIT rc=00000000");
	bad += check_zone(ram[0], "I2C49 rc=00000001");
	/* no active pipe */
	regs_sspp0_active = 0; renders = 0; mdp_writes = 0; con_fb = 0; con_stride4 = 1080;
	mi8_diag(0);
	bad += check_zone(ram[0], "no active SSPP");
	mi8_panic_log("gic", 0, 0x96000010, 0xffff, 0x1234);
	bad += check_zone(ram[0], "PANIC gic");
	puts(bad ? "FAIL" : "PASS");
	return bad;
}
