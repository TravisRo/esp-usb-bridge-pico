/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

#define IS_RGBW true

#ifdef PICO_DEFAULT_WS2812_PIN
#define WS2812_PIN PICO_DEFAULT_WS2812_PIN
#else
// default to pin 2 if the board doesn't have a default WS2812 pin defined
#define WS2812_PIN 16
#endif
static PIO _pio_ws2812 = NULL;
static int _sm_ws2812 = 0;

static inline void put_pixel(uint32_t pixel_grb) {
	pio_sm_put_blocking(_pio_ws2812, _sm_ws2812, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
	return
	        ((uint32_t) (r) << 8) |
	        ((uint32_t) (g) << 16) |
	        (uint32_t) (b);
}

void pattern_snakes(uint len, uint t) {
	for (uint i = 0; i < len; ++i)
	{
		uint x = (i + (t >> 1)) % 64;
		if (x < 10)
			put_pixel(urgb_u32(0xff, 0, 0));
		else if (x >= 15 && x < 25)
			put_pixel(urgb_u32(0, 0xff, 0));
		else if (x >= 30 && x < 40)
			put_pixel(urgb_u32(0, 0, 0xff));
		else
			put_pixel(0);
	}
}

void pattern_random(uint len, uint t) {
	if (t % 8)
		return;
	for (int i = 0; i < len; ++i)
		put_pixel(rand());
}

void pattern_sparkle(uint len, uint t) {
	if (t % 8)
		return;
	for (int i = 0; i < len; ++i)
		put_pixel(rand() % 16 ? 0 : 0xffffffff);
}

void pattern_greys(uint len, uint t) {
	int max = 100; // let's not draw too much current!
	t %= max;
	for (int i = 0; i < len; ++i)
	{
		put_pixel(t * 0x10101);
		if (++t >= max) t = 0;
	}
}

typedef void (*pattern)(uint len, uint t);
const struct
{
	pattern pat;
	const char *name;
} pattern_table[] = {
	{pattern_snakes,  "Snakes!"},
	{pattern_random,  "Random data"},
	{pattern_sparkle, "Sparkles"},
	{pattern_greys,   "Greys"},
};

void ws2812_pio_init(PIO pio)
{
	_pio_ws2812 = pio;
	_sm_ws2812 = pio_claim_unused_sm(pio, true);
	uint offset = pio_add_program(pio, &ws2812_program);

	ws2812_program_init(_pio_ws2812, _sm_ws2812, offset, WS2812_PIN, 800000, IS_RGBW);

}

void ws2812_put_pixel(uint8_t r, uint8_t g, uint8_t b)
{
	put_pixel(urgb_u32(r, g, b));
}