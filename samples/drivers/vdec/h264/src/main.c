/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * H.264 decoder test. Every stream of h264_cases.h is decoded and each frame
 * is compared with the CRC32 of the host decode (H.264 decoding is exactly
 * specified, so the frames must be identical). Streams the decoder does not
 * support must be refused, damaged ones must not hang or crash it, and after
 * each of them a good stream is decoded again. The last line is
 * "PASS (0 failures)" when everything held.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "h264_cases.h"

#define FEED_CHUNK	(48 * 1024)
#define CASE_TIMEOUT_MS	30000
#define MAX_IDLE_LOOPS	2000

static int failures;

static uint32_t frame_crc(const struct vdec_frame *f)
{
	uint32_t crc = 0;

	for (int y = 0; y < f->height; y++) {
		crc = crc32_ieee_update(crc, f->plane[0] + y * f->stride[0], f->width);
	}
	for (int y = 0; y < f->height / 2; y++) {
		crc = crc32_ieee_update(crc, f->plane[1] + y * f->stride[1], f->width);
	}

	return crc;
}

/* Length of the next piece to feed: whole NAL units only, at most FEED_CHUNK bytes if possible */
static size_t nal_chunk(const uint8_t *p, size_t len)
{
	size_t cut = 0;

	if (len <= FEED_CHUNK) {
		return len;
	}
	for (size_t i = 3; i + 3 < FEED_CHUNK; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			cut = (p[i - 1] == 0) ? i - 1 : i;
		}
	}
	if (cut == 0) {
		/* a single NAL unit larger than the chunk: up to the next start code */
		for (size_t i = FEED_CHUNK; i + 3 < len; i++) {
			if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
				return (p[i - 1] == 0) ? i - 1 : i;
			}
		}
		return len;
	}

	return cut;
}

struct result {
	int open;		/* vdec_stream_open() result */
	int frames;
	int bad;		/* frames whose CRC differs */
	int error;		/* first error other than EAGAIN/ENODATA, 0 if none */
	int64_t ms;
};

static void run_stream(const struct device *dev, const struct h264_case *c, struct result *r)
{
	const struct vdec_stream_config cfg = {
		.codec = VDEC_CODEC_H264,
		.format = VDEC_FORMAT_NV12,
	};
	struct vdec_stream *stream;
	struct vdec_frame frame;
	size_t off = 0, used;
	int64_t start = k_uptime_get();
	int idle = 0;

	memset(r, 0, sizeof(*r));
	r->open = vdec_stream_open(dev, &cfg, &stream);
	if (r->open != 0) {
		return;
	}
	for (;;) {
		int ret = vdec_stream_get_frame(dev, stream, &frame);

		if (k_uptime_get() - start > CASE_TIMEOUT_MS || idle > MAX_IDLE_LOOPS) {
			r->error = -ETIMEDOUT;
			break;
		}
		if (ret == -EAGAIN && off < c->size) {
			size_t n = nal_chunk(c->data + off, c->size - off);

			ret = vdec_stream_feed(dev, stream, c->data + off, n, r->frames * 33, &used);
			if (ret == 0) {
				off += used;
				idle = 0;
			} else if (ret == -EINVAL) {
				/* no NAL unit in this piece: skip it */
				off += n;
			} else if (ret != -EAGAIN) {
				r->error = ret;
				break;
			} else {
				idle++;
			}
			if (off >= c->size) {
				vdec_stream_flush(dev, stream);
			}
			continue;
		}
		if (ret == -EAGAIN) {
			/* all data is in: flush so that the decoder delivers what it holds */
			vdec_stream_flush(dev, stream);
			idle++;
			continue;
		}
		if (ret == -ENODATA) {
			break;
		}
		if (ret != 0) {
			r->error = ret;
			break;
		}
		idle = 0;
		if (c->mode == H264_EXACT || c->mode == H264_PREFIX) {
			uint32_t crc = frame_crc(&frame);

			if (r->frames >= c->frames) {
				/* beyond the frames the host delivered: not compared */
			} else if (crc != c->crc[r->frames]) {
				if (r->bad < 4) {
					printk("  %s frame %d (%dx%d): crc %08x, expected %08x\n", c->name,
					       r->frames, frame.width, frame.height, crc,
					       r->frames < c->frames ? c->crc[r->frames] : 0);
				}
				r->bad++;
			}
		}
		r->frames++;
		vdec_frame_release(dev, &frame);
	}
	vdec_stream_close(dev, stream);
	r->ms = k_uptime_get() - start;
}

static void check(bool ok, const char *name, const char *what)
{
	if (!ok) {
		printk("FAIL %s: %s\n", name, what);
		failures++;
	}
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	const struct h264_case *good = NULL;

	if (!device_is_ready(dev)) {
		printk("decoder not ready\n");
		return 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(h264_cases); i++) {
		if (strcmp(h264_cases[i].name, "base") == 0) {
			good = &h264_cases[i];
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(h264_cases); i++) {
		const struct h264_case *c = &h264_cases[i];
		struct result r, again;

		if (sizeof(CONFIG_SAMPLE_H264_ONLY) > 1) {
			char pat[48];

			snprintk(pat, sizeof(pat), " %s ", c->name);
			if (strstr(" " CONFIG_SAMPLE_H264_ONLY " ", pat) == NULL) {
				continue;
			}
		}
		run_stream(dev, c, &r);
		switch (c->mode) {
		case H264_EXACT:
			if (r.open != 0 || r.error != 0 || r.frames != c->frames || r.bad != 0) {
				printk("FAIL %s: open %d, error %d, %d of %d frames, %d bad crc\n", c->name,
				       r.open, r.error, r.frames, c->frames, r.bad);
				failures++;
			} else {
				printk("ok   %-18s %3d frames %6lld ms\n", c->name, r.frames, r.ms);
			}
			break;
		case H264_PREFIX:
			if (r.open != 0 || r.frames < c->frames || r.bad != 0 || r.error == -ETIMEDOUT) {
				printk("FAIL %s: open %d, error %d, %d of %d frames, %d bad crc\n", c->name,
				       r.open, r.error, r.frames, c->frames, r.bad);
				failures++;
			} else {
				printk("ok   %-18s %3d frames, then error %d\n", c->name, r.frames, r.error);
			}
			break;
		case H264_REFUSED:
			if (r.open == 0 && r.error == 0 && r.frames > 0) {
				printk("FAIL %s: an unsupported stream gave %d frames\n", c->name,
				       r.frames);
				failures++;
			} else {
				printk("ok   %-18s refused (open %d, error %d, %d frames)\n", c->name,
				       r.open, r.error, r.frames);
			}
			check(r.error != -ETIMEDOUT, c->name, "hang");
			break;
		default:
			printk("ok   %-18s survived (open %d, error %d, %d frames, %lld ms)\n", c->name,
			       r.open, r.error, r.frames, r.ms);
			check(r.error != -ETIMEDOUT, c->name, "hang");
			break;
		}

		if (c->mode != H264_EXACT && good != NULL) {
			/* the decoder must work as before */
			run_stream(dev, good, &again);
			if (again.open != 0 || again.error != 0 || again.frames != good->frames ||
			    again.bad != 0) {
				printk("FAIL decoder broken after %s: open %d, error %d, %d frames, %d bad\n",
				       c->name, again.open, again.error, again.frames, again.bad);
				failures++;
			}
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
