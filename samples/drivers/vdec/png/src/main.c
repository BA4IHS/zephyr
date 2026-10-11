/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PNG decoder test. Every picture of png_cases.h is decoded and the CRC of
 * the RGBA pixels (a PNG is lossless) must equal the one computed on the host
 * from the source samples; the damaged files must be refused without harm to
 * the decoder, and repeated decoding must not run out of memory. The last
 * line is "PASS (0 failures)" when everything held.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "png_cases.h"
#include "vdec_show.h"

static int failures;

static uint32_t picture_crc(const struct vdec_frame *f)
{
	uint32_t crc = 0;

	for (int y = 0; y < f->height; y++) {
		crc = crc32_ieee_update(crc, f->plane[0] + y * f->stride[0], f->width * 4);
	}

	return crc;
}

static void check(bool ok, const char *name, const char *what)
{
	if (!ok) {
		printk("FAIL %s: %s\n", name, what);
		failures++;
	}
}

static int decode_case(const struct device *dev, const struct png_case *c, struct vdec_frame *f,
		       int64_t *ms)
{
	int64_t t = k_uptime_get();
	int ret = vdec_decode_image(dev, VDEC_CODEC_PNG, c->data, c->size, VDEC_FORMAT_RGBA8888, f);

	*ms = k_uptime_get() - t;

	return ret;
}

static void run_good(const struct device *dev)
{
	for (size_t i = 0; i < ARRAY_SIZE(png_good); i++) {
		const struct png_case *c = &png_good[i];
		struct vdec_frame f;
		int64_t ms;
		int ret = decode_case(dev, c, &f, &ms);

		if (ret != 0) {
			printk("FAIL %s: decode returned %d\n", c->name, ret);
			failures++;
			continue;
		}
		check(f.width == c->width && f.height == c->height, c->name, "size");
		uint32_t crc = picture_crc(&f);

		if (crc != c->crc) {
			printk("FAIL %s: crc %08x, expected %08x\n", c->name, crc, c->crc);
			failures++;
		} else {
			printk("ok   %-18s %4dx%-4d %3lld ms\n", c->name, f.width, f.height, ms);
		}
		vdec_frame_release(dev, &f);
	}
}

static void run_bad(const struct device *dev)
{
	for (size_t i = 0; i < ARRAY_SIZE(png_bad); i++) {
		const struct png_case *c = &png_bad[i];
		struct vdec_frame f;
		int64_t ms;
		int ret = decode_case(dev, c, &f, &ms);

		if (ret == 0) {
			printk("FAIL %s: a damaged picture was accepted\n", c->name);
			failures++;
			vdec_frame_release(dev, &f);
		} else {
			printk("ok   %-18s refused (%d)\n", c->name, ret);
		}
	}
	/* the decoder must work as before */
	{
		struct vdec_frame f;
		int64_t ms;

		if (decode_case(dev, &png_good[0], &f, &ms) != 0) {
			printk("FAIL decoder broken after the damaged pictures\n");
			failures++;
		} else {
			check(picture_crc(&f) == png_good[0].crc, "after_bad", "crc");
			vdec_frame_release(dev, &f);
		}
	}
}

static void run_stress(const struct device *dev)
{
	const struct png_case *small = &png_good[0], *big = NULL;
	int n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(png_good); i++) {
		if (strcmp(png_good[i].name, "big_rgba8") == 0) {
			big = &png_good[i];
		}
	}
	for (int i = 0; i < 200; i++) {
		struct vdec_frame f;
		int64_t ms;

		if (decode_case(dev, small, &f, &ms) != 0 || picture_crc(&f) != small->crc) {
			break;
		}
		vdec_frame_release(dev, &f);
		n++;
	}
	printk("%d of 200 repeated small decodes\n", n);
	check(n == 200, "stress_small", "repeated decoding failed");

	n = 0;
	for (int i = 0; big != NULL && i < 20; i++) {
		struct vdec_frame f;
		int64_t ms;

		if (decode_case(dev, big, &f, &ms) != 0 || picture_crc(&f) != big->crc) {
			break;
		}
		vdec_frame_release(dev, &f);
		n++;
	}
	printk("%d of 20 repeated big decodes\n", n);
	check(n == 20, "stress_big", "repeated decoding failed");
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	const struct png_case *big = NULL;

	if (!device_is_ready(dev)) {
		printk("decoder not ready\n");
		return 0;
	}

	run_good(dev);
	run_bad(dev);
	run_stress(dev);

	for (size_t i = 0; i < ARRAY_SIZE(png_good); i++) {
		if (strcmp(png_good[i].name, "big_rgba8") == 0) {
			big = &png_good[i];
		}
	}
	if (big != NULL) {
		struct vdec_frame f;
		int64_t ms;

		if (decode_case(dev, big, &f, &ms) == 0) {
			printk("big picture: %lld ms, show: %d\n", ms, vdec_show(&f, true));
		}
	}
	if (failures == 0) {
		printk("PASS (0 failures)\n");
	} else {
		printk("FAIL (%d failures)\n", failures);
	}

	k_sleep(K_FOREVER);

	return 0;
}
