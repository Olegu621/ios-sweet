#pragma once
#include <stdint.h>

/* peridot (POCO F6): ABL splash region, live FDT cont_splash_region
 * (0xe3940000 + 0x2b00000), 1220x2712 DSC video panel. */
#define FB_BASE		0xe3940000UL
#define FB_W		1220
#define FB_H		2712
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
