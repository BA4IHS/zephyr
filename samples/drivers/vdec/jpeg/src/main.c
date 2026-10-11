/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * JPEG decoder test. Every picture of jpeg_cases.h is decoded to NV12 and to
 * NV21 and compared with the signature of the host decode (the mean of every
 * 16x16 luma block and 8x8 chroma block, a decoder is not bit exact); damaged
 * files must not hang or crash the decoder, structurally wrong ones must be
 * refused, and repeated decoding must not run out of memory. The last line is
 * "PASS (0 failures)" when everything held.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "jpeg_cases.h"
#include "vdec_show.h"

#define LUMA_TOLERANCE		2
#define CHROMA_TOLERANCE	3
#define SLOW_MS			2500

static int failures;

/* damaged files the decoder may or may not notice, but must survive */
static const char *const lenient[] = {
	"bad_trunc_scan", "bad_trunc_end", "bad_entropy_flip", "bad_early_eoi",
};

static bool may_decode(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(lenient); i++) {
		if (strcmp(name, lenient[i]) == 0) {
			return true;
		}
	}

	return false;
}

static int decode_case(const struct device *dev, const struct jpeg_case *c,
		       enum vdec_format fmt, struct vdec_frame *f, int64_t *ms)
{
	int64_t t = k_uptime_get();
	int ret = vdec_decode_image(dev, VDEC_CODEC_JPEG, c->data, c->size, fmt, f);

	*ms = k_uptime_get() - t;

	return ret;
}

/* Largest difference between the picture and the signature */
static void compare(const struct vdec_frame *f, const struct jpeg_case *c, bool nv21, int *ymax,
		    int *cmax)
{
	int w = f->width, h = f->height;
	int cw = (w + 1) / 2, ch = (h + 1) / 2;
	int n = 0;

	*ymax = *cmax = 0;
	for (int by = 0; by < h; by += 16) {
		for (int bx = 0; bx < w; bx += 16) {
			uint32_t s = 0, cnt = 0;

			for (int y = by; y < MIN(by + 16, h); y++) {
				const uint8_t *row = f->plane[0] + y * f->stride[0];

				for (int x = bx; x < MIN(bx + 16, w); x++) {
					s += row[x];
					cnt++;
				}
			}
			*ymax = MAX(*ymax, abs((int)(s / cnt) - c->ys[n]));
			n++;
		}
	}
	n = 0;
	for (int by = 0; by < ch; by += 8) {
		for (int bx = 0; bx < cw; bx += 8) {
			uint32_t s0 = 0, s1 = 0, cnt = 0;

			for (int y = by; y < MIN(by + 8, ch); y++) {
				const uint8_t *row = f->plane[1] + y * f->stride[1];

				for (int x = bx; x < MIN(bx + 8, cw); x++) {
					s0 += row[2 * x];
					s1 += row[2 * x + 1];
					cnt++;
				}
			}
			if (nv21) {
				uint32_t t = s0;

				s0 = s1;
				s1 = t;
			}
			*cmax = MAX(*cmax, MAX(abs((int)(s0 / cnt) - c->us[n]),
					       abs((int)(s1 / cnt) - c->vs[n])));
			n++;
		}
	}
}

static void run_good(const struct device *dev)
{
	for (size_t i = 0; i < ARRAY_SIZE(jpeg_good); i++) {
		const struct jpeg_case *c = &jpeg_good[i];

		for (int k = 0; k < 2; k++) {
			enum vdec_format fmt = k ? VDEC_FORMAT_NV21 : VDEC_FORMAT_NV12;
			struct vdec_frame f;
			int64_t ms;
			int ymax, cmax;
			int ret = decode_case(dev, c, fmt, &f, &ms);

			if (ret != 0) {
				printk("FAIL %s %s: decode returned %d\n", c->name, k ? "nv21" : "nv12",
				       ret);
				failures++;
				continue;
			}
			if (f.width != c->width || f.height != c->height) {
				printk("FAIL %s: size %dx%d, expected %dx%d\n", c->name, f.width,
				       f.height, c->width, c->height);
				failures++;
			} else {
				compare(&f, c, k, &ymax, &cmax);
				if (ymax > LUMA_TOLERANCE || cmax > CHROMA_TOLERANCE) {
					printk("FAIL %s %s: luma off by %d, chroma by %d\n", c->name,
					       k ? "nv21" : "nv12", ymax, cmax);
					failures++;
				} else {
					printk("ok   %-14s %s %4dx%-4d %4lld ms  y%d c%d\n", c->name,
					       k ? "nv21" : "nv12", f.width, f.height, ms, ymax, cmax);
				}
			}
			vdec_frame_release(dev, &f);
		}
	}
}

static void run_bad(const struct device *dev)
{
	for (size_t i = 0; i < ARRAY_SIZE(jpeg_bad); i++) {
		const struct jpeg_case *c = &jpeg_bad[i];
		struct vdec_frame f;
		int64_t ms;
		int ret = decode_case(dev, c, VDEC_FORMAT_NV12, &f, &ms);

		if (ret == 0) {
			if (!may_decode(c->name)) {
				printk("FAIL %s: a damaged picture was accepted\n", c->name);
				failures++;
			} else {
				printk("ok   %-18s decoded something (%lld ms)\n", c->name, ms);
			}
			vdec_frame_release(dev, &f);
		} else {
			printk("ok   %-18s refused (%d, %lld ms)\n", c->name, ret, ms);
		}
		if (ms > SLOW_MS) {
			printk("FAIL %s: took %lld ms\n", c->name, ms);
			failures++;
		}
		/* the decoder must work as before */
		{
			struct vdec_frame g;
			int ymax, cmax;

			if (decode_case(dev, &jpeg_good[7], VDEC_FORMAT_NV12, &g, &ms) != 0) {
				printk("FAIL decoder broken after %s\n", c->name);
				failures++;
				continue;
			}
			compare(&g, &jpeg_good[7], false, &ymax, &cmax);
			if (ymax > LUMA_TOLERANCE || cmax > CHROMA_TOLERANCE) {
				printk("FAIL decoder gives wrong pictures after %s\n", c->name);
				failures++;
			}
			vdec_frame_release(dev, &g);
		}
	}
}

static void run_stress(const struct device *dev)
{
	const struct jpeg_case *small = &jpeg_good[7], *big = NULL;
	int n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(jpeg_good); i++) {
		if (strcmp(jpeg_good[i].name, "s1920x1080") == 0) {
			big = &jpeg_good[i];
		}
	}
	for (int i = 0; i < 100; i++) {
		struct vdec_frame f;
		int64_t ms;
		int ymax, cmax;

		if (decode_case(dev, small, VDEC_FORMAT_NV12, &f, &ms) != 0) {
			break;
		}
		compare(&f, small, false, &ymax, &cmax);
		vdec_frame_release(dev, &f);
		if (ymax > LUMA_TOLERANCE || cmax > CHROMA_TOLERANCE) {
			break;
		}
		n++;
	}
	printk("%d of 100 repeated small decodes\n", n);
	if (n != 100) {
		printk("FAIL stress_small\n");
		failures++;
	}

	n = 0;
	for (int i = 0; big != NULL && i < 10; i++) {
		struct vdec_frame f;
		int64_t ms;
		int ymax, cmax;

		if (decode_case(dev, big, VDEC_FORMAT_NV12, &f, &ms) != 0) {
			break;
		}
		compare(&f, big, false, &ymax, &cmax);
		vdec_frame_release(dev, &f);
		if (ymax > LUMA_TOLERANCE || cmax > CHROMA_TOLERANCE) {
			break;
		}
		n++;
	}
	printk("%d of 10 repeated 1080p decodes\n", n);
	if (n != 10) {
		printk("FAIL stress_big\n");
		failures++;
	}
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	struct vdec_frame f;
	int64_t ms;

	if (!device_is_ready(dev)) {
		printk("decoder not ready\n");
		return 0;
	}

	run_good(dev);
	run_bad(dev);
	run_stress(dev);

	if (decode_case(dev, &jpeg_good[9], VDEC_FORMAT_NV12, &f, &ms) == 0) {
		/* the display reads the picture while it is shown: keep the frame */
		printk("show: %d\n", vdec_show(&f, false));
	}
	if (failures == 0) {
		printk("PASS (0 failures)\n");
	} else {
		printk("FAIL (%d failures)\n", failures);
	}
	k_sleep(K_FOREVER);

	return 0;
}
