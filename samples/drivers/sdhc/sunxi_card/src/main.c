/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Starts the card in the TF slot again and again and reads it: the first sector (the boot sector
 * must end with 0x55 0xaa), then a block of sectors twice, which must be the same both times. It
 * prints what it found for every round and ends with PASS or FAIL, so a card or slot that answers
 * with 0xff, with garbage or not at all shows up without any application around it.
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#define DISK		"SD"
#define CHUNK		128	/* sectors per read */

static int failures;

static void check(bool ok, const char *what, int round)
{
	if (!ok) {
		failures++;
	}
	printk("card: round %2d %-46s %s\n", round, what, ok ? "ok" : "FAILED");
}

static uint32_t read_run(uint8_t *buf, uint32_t first, uint32_t sectors, int *err, int64_t *ms)
{
	uint32_t crc = 0;
	int64_t t0 = k_uptime_get();

	*err = 0;
	for (uint32_t s = 0; s < sectors; s += CHUNK) {
		uint32_t n = MIN(CHUNK, sectors - s);

		*err = disk_access_read(DISK, buf, first + s, n);
		if (*err != 0) {
			break;
		}
		crc = crc32_ieee_update(crc, buf, n * 512U);
	}
	*ms = k_uptime_get() - t0;

	return crc;
}

int main(void)
{
	uint8_t *buf = aligned_alloc(64, CHUNK * 512U);
	uint32_t count = 0, size = 0;
	int ok_rounds = 0;

	printk("card: %d rounds, %d sectors each\n", CONFIG_SAMPLE_CARD_ROUNDS, CONFIG_SAMPLE_CARD_SECTORS);
	if (buf == NULL) {
		printk("card: FAIL (no memory)\n");
		return 0;
	}
	for (int r = 1; r <= CONFIG_SAMPLE_CARD_ROUNDS; r++) {
		uint32_t a, b;
		int e1, e2;
		int64_t t1, t2;
		bool good;

		/* start the card from the beginning, as the first mount does */
		disk_access_ioctl(DISK, DISK_IOCTL_CTRL_DEINIT, NULL);
		check(disk_access_init(DISK) == 0, "init", r);
		if (disk_access_ioctl(DISK, DISK_IOCTL_GET_SECTOR_COUNT, &count) != 0 ||
		    disk_access_ioctl(DISK, DISK_IOCTL_GET_SECTOR_SIZE, &size) != 0) {
			check(false, "size of the card", r);
			continue;
		}
		if (r == 1) {
			printk("card: %u sectors of %u bytes (%u MiB)\n", count, size, count / 2048);
		}
		check(disk_access_read(DISK, buf, 0, 1) == 0, "read of sector 0", r);
		good = buf[510] == 0x55 && buf[511] == 0xaa;
		printk("card: round %2d sector 0: %02x %02x %02x ... %02x %02x\n", r, buf[0], buf[1], buf[2],
		       buf[510], buf[511]);
		check(good, "sector 0 ends with 55 aa", r);

		a = read_run(buf, 0, CONFIG_SAMPLE_CARD_SECTORS, &e1, &t1);
		b = read_run(buf, 0, CONFIG_SAMPLE_CARD_SECTORS, &e2, &t2);
		check(e1 == 0 && e2 == 0, "read of the sectors", r);
		check(a == b, "the two reads are the same", r);
		printk("card: round %2d crc %08x %08x, %lld and %lld ms\n", r, a, b, (long long)t1,
		       (long long)t2);
		if (good && e1 == 0 && e2 == 0 && a == b) {
			ok_rounds++;
		}
		k_msleep(200);
	}
	printk("card: %d of %d rounds good\n", ok_rounds, CONFIG_SAMPLE_CARD_ROUNDS);
	printk("card: %s\n", failures == 0 ? "PASS (0 failures)" : "FAIL");

	return 0;
}
