/*
 * Copyright (c) 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Checks the real time clock: the time can be set and read back, it runs at the speed of the
 * system timer, the date rolls over correctly, and the alarm fires at its time. The last line is
 * PASS (0 failures).
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>

static int failures;

#define CHECK(cond, ...)							\
	do {									\
		if (!(cond)) {							\
			failures++;						\
			printk("FAIL: " __VA_ARGS__);				\
			printk("\n");						\
		}								\
	} while (0)

static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
static volatile int alarm_hits;

static void on_alarm(const struct device *dev, uint16_t id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);
	alarm_hits++;
}

static void show(const char *what, const struct rtc_time *t)
{
	printk("%s %04d-%02d-%02d %02d:%02d:%02d (wday %d, yday %d)\n", what, t->tm_year + 1900,
	       t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, t->tm_wday, t->tm_yday);
}

static struct rtc_time make(int y, int mo, int d, int h, int mi, int s)
{
	struct rtc_time t = {
		.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
		.tm_hour = h, .tm_min = mi, .tm_sec = s,
	};

	return t;
}

static int64_t seconds_of(const struct rtc_time *t)
{
	/* only for differences inside the same few days of one month */
	return ((int64_t)t->tm_mday * 24 + t->tm_hour) * 3600 + t->tm_min * 60 + t->tm_sec;
}

int main(void)
{
	struct rtc_time t, u;
	int64_t t0, t1;
	uint16_t mask;
	int ret;

	printk("RTC test\n");
	if (!device_is_ready(rtc)) {
		printk("FAIL: the rtc is not ready\n");
		printk("FAIL (1 failures)\n");
		return 0;
	}
	ret = rtc_get_time(rtc, &t);
	printk("time at start: ret %d\n", ret);
	if (ret == 0) {
		show("  ", &t);
	}

	/* set and read back */
	t = make(2026, 10, 11, 12, 34, 56);
	ret = rtc_set_time(rtc, &t);
	CHECK(ret == 0, "set_time returned %d", ret);
	ret = rtc_get_time(rtc, &u);
	CHECK(ret == 0, "get_time returned %d", ret);
	show("read back", &u);
	CHECK(u.tm_year == t.tm_year && u.tm_mon == t.tm_mon && u.tm_mday == t.tm_mday &&
	      u.tm_hour == t.tm_hour && u.tm_min == t.tm_min && u.tm_sec >= t.tm_sec &&
	      u.tm_sec <= t.tm_sec + 2, "the time read back differs");
	CHECK(u.tm_wday == 0, "11 Oct 2026 is a Sunday, got %d", u.tm_wday);
	CHECK(u.tm_yday == 283, "day of the year %d", u.tm_yday);

	/* the speed: ten seconds of the system timer */
	rtc_get_time(rtc, &t);
	t0 = k_uptime_get();
	k_sleep(K_SECONDS(10));
	rtc_get_time(rtc, &u);
	t1 = k_uptime_get();
	printk("rtc counted %d s while the system counted %d ms\n",
	       (int)(seconds_of(&u) - seconds_of(&t)), (int)(t1 - t0));
	CHECK(seconds_of(&u) - seconds_of(&t) >= 9 && seconds_of(&u) - seconds_of(&t) <= 11,
	      "the rtc does not run at the rate of the system timer");

	/* a leap day and the end of a year */
	t = make(2028, 2, 29, 23, 59, 58);
	CHECK(rtc_set_time(rtc, &t) == 0, "set 29 Feb 2028");
	k_sleep(K_SECONDS(3));
	rtc_get_time(rtc, &u);
	show("after the leap day", &u);
	CHECK(u.tm_mon == 2 && u.tm_mday == 1, "no roll over from 29 Feb 2028 to 1 Mar");
	t = make(2026, 12, 31, 23, 59, 58);
	CHECK(rtc_set_time(rtc, &t) == 0, "set 31 Dec 2026");
	k_sleep(K_SECONDS(3));
	rtc_get_time(rtc, &u);
	show("after the year", &u);
	CHECK(u.tm_year == 2027 - 1900 && u.tm_mon == 0 && u.tm_mday == 1, "no roll over of the year");

	/* bad times are refused */
	t = make(2026, 13, 1, 0, 0, 0);
	CHECK(rtc_set_time(rtc, &t) == -EINVAL, "month 13 accepted");
	t = make(1969, 12, 31, 0, 0, 0);
	CHECK(rtc_set_time(rtc, &t) == -EINVAL, "1969 accepted");
	t = make(2100, 1, 1, 0, 0, 0);
	CHECK(rtc_set_time(rtc, &t) == -EINVAL, "2100 accepted");

	/* the alarm */
	t = make(2026, 10, 11, 12, 0, 0);
	CHECK(rtc_set_time(rtc, &t) == 0, "set the time for the alarm");
	CHECK(rtc_alarm_get_supported_fields(rtc, 0, &mask) == 0 && mask != 0, "no alarm fields");
	CHECK(rtc_alarm_set_callback(rtc, 0, on_alarm, NULL) == 0, "set the callback");
	t = make(2026, 10, 11, 12, 0, 5);
	ret = rtc_alarm_set_time(rtc, 0, mask, &t);
	CHECK(ret == 0, "alarm_set_time returned %d", ret);
	k_sleep(K_SECONDS(3));
	CHECK(alarm_hits == 0, "the alarm fired too early");
	k_sleep(K_SECONDS(5));
	CHECK(alarm_hits == 1, "the alarm fired %d times", alarm_hits);
	k_sleep(K_SECONDS(2));
	CHECK(alarm_hits == 1, "the alarm fired again");

	/* leave a plausible time for the next boot */
	t = make(2026, 10, 11, 12, 0, 0);
	rtc_set_time(rtc, &t);

	if (failures == 0) {
		printk("PASS (0 failures)\n");
	} else {
		printk("FAIL (%d failures)\n", failures);
	}

	return 0;
}
