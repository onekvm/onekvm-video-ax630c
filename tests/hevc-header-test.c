// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Golden SPS/PPS syntax for the fixed-QP hardware slices. HEVC vectors
 * originate from vendor-diff-hevc-20260905/H3/fixqp32.h265; their VUI is
 * intentionally corrected to limited-range BT.709. H.264 signals the same
 * colour fields at HD and SD sizes, without changing its reorder bounds. */
#include <stdio.h>
#include <string.h>
#include "vcenc_hevc_header.h"

static int hex2bin(const char *h, uint8_t *b) { int n = 0; for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); b[n++] = (uint8_t)v; } return n; }
static int cmp(const char *what, const uint8_t *got, int gn, const char *hex)
{
	uint8_t want[256]; int wn = hex2bin(hex, want);
	int ok = (gn == wn + 4) && !memcmp(got + 4, want, wn);
	printf("%-16s %s (%d bytes)\n", what, ok ? "MATCH" : "DIFF", gn);
	if (!ok) { printf("  got :"); for (int i = 4; i < gn; i++) printf("%02x", got[i]); printf("\n  want:%s\n", hex); }
	return !ok;
}
int main(void)
{
	uint8_t b[256]; int fails = 0, n;
	n = vcenc_write_vps(b, 153);
	fails += cmp("VPS 1080p", b, n, "40010c01ffff014000000300800000030000030099ac09");
	n = vcenc_write_hevc_sps(b, 1920, 1080, 153);
	fails += cmp("SPS 1080p", b, n, "420101014000000300800000030000030099a003c08010e58dae4b2b66b9713501010104000003000400000300f0fe2c4a");
	n = vcenc_write_hevc_pps(b, 32);
	fails += cmp("PPS qp32 fixqp", b, n, "4401c0e306066480");
	n = vcenc_write_hevc_sps(b, 1366, 768, 153);
	fails += cmp("SPS 1366x768", b, n, "420101014000000300800000030000030099a002ac80301d78dae4b2b66b97135010101040000003004000000f0fe2c4a0");
	n = vcenc_write_sps(b, 1920, 1080);
	fails += cmp("AVC SPS 1080p", b, n, "674d002a8d8d603c0113f2cd40404041e1108dc0");
	n = vcenc_write_sps(b, 640, 480);
	fails += cmp("AVC SPS 480p", b, n, "674d001f8d8d60501ed35010101078442370");
	n = vcenc_write_pps(b, 32);
	fails += cmp("AVC PPS qp32", b, n, "68ee0672");
	uint8_t all[512]; n = vcenc_write_hevc_headers(all, 1920, 1080, 32, 153);
	printf("headers 1080p qp32: %d bytes (vendor 92)\n", n);
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
