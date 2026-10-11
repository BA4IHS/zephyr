/*
 * Copyright (c) 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The real time clock of the sunxi SoCs. The date is a day number (1 is the 1st of January 1970,
 * the counter holds 127 years) and the time of day is hours, minutes and seconds in one word. Both
 * registers are written through a slow path: a bit of the control register stays set while a write
 * is on its way to the 32 kHz domain, and a second write in that time is lost. The alarm fires when
 * the day number and the time of day equal the alarm registers.
 */

#define DT_DRV_COMPAT allwinner_sunxi_rtc

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(rtc_sunxi, CONFIG_RTC_LOG_LEVEL);

#define RTC_BASE	DT_INST_REG_ADDR(0)

#define REG_LOSC_CTRL	0x00
#define REG_AUTO_STA	0x04
#define REG_YMD		0x10
#define REG_HMS		0x14
#define REG_ALRM_DAY	0x20
#define REG_ALRM_HMS	0x24
#define REG_ALRM_EN	0x28
#define REG_ALRM_IRQ_EN	0x2c
#define REG_ALRM_IRQ_STA 0x30
#define REG_ALRM_CONFIG	0x50
#define REG_HOSC_32K	0x60

/* control register: the key in the upper half guards every write */
#define LOSC_KEY		0x16aa0000U
#define LOSC_SRC_EXTERNAL	BIT(0)
#define LOSC_SRC_HOSC		BIT(1)
#define LOSC_EXT_EN		BIT(4)
#define LOSC_YMD_BUSY		BIT(7)
#define LOSC_HMS_BUSY		BIT(8)
#define LOSC_AUTO_SWITCH_EN	BIT(14)
#define LOSC_AUTO_SWITCH_BYPASS	BIT(15)

#define AUTO_STA_EXT_OK		BIT(2)
#define HOSC_32K_EN		BIT(16)

#define ALRM_BIT		BIT(0)
#define ALRM_WAKEUP_OUT		BIT(0)

#define YEAR_MIN	1970
#define YEAR_MAX	2097

#define WRITE_TIMEOUT_MS	1000

struct rtc_sunxi_data {
	struct k_spinlock lock;
#ifdef CONFIG_RTC_ALARM
	rtc_alarm_callback alarm_cb;
	void *alarm_user_data;
	bool alarm_pending;
#endif
};

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(RTC_BASE + off);
}

static inline void wr(uint32_t off, uint32_t val)
{
	sys_write32(val, RTC_BASE + off);
}

/* ---- the calendar ------------------------------------------------------------------------- */

static bool is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const uint8_t month_len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int days_in_month(int year, int mon)
{
	return month_len[mon] + (mon == 1 && is_leap(year) ? 1 : 0);
}

/* day number of a date, 1 for the 1st of January 1970; mon is 0..11 */
static uint32_t day_number(int year, int mon, int mday)
{
	uint32_t days = mday;

	for (int m = 0; m < mon; m++) {
		days += days_in_month(year, m);
	}
	for (int y = YEAR_MIN; y < year; y++) {
		days += is_leap(y) ? 366 : 365;
	}

	return days;
}

static void date_of_day(uint32_t day, struct rtc_time *tm)
{
	int year = YEAR_MIN, mon = 0;
	uint32_t d = day;

	while (d > (uint32_t)(is_leap(year) ? 366 : 365)) {
		d -= is_leap(year) ? 366 : 365;
		year++;
	}
	tm->tm_yday = d - 1;
	while (d > (uint32_t)days_in_month(year, mon)) {
		d -= days_in_month(year, mon);
		mon++;
	}
	tm->tm_year = year - 1900;
	tm->tm_mon = mon;
	tm->tm_mday = d;
	/* the 1st of January 1970 was a Thursday */
	tm->tm_wday = (day - 1 + 4) % 7;
}

static uint32_t hms_word(const struct rtc_time *tm)
{
	return (uint32_t)tm->tm_sec | (uint32_t)tm->tm_min << 8 | (uint32_t)tm->tm_hour << 16;
}

/* ---- time ---------------------------------------------------------------------------------- */

static int busy_wait_clear(uint32_t mask)
{
	for (int i = 0; i < WRITE_TIMEOUT_MS / 5; i++) {
		if ((rd(REG_LOSC_CTRL) & mask) == 0U) {
			return 0;
		}
		k_msleep(5);
	}

	return -ETIMEDOUT;
}

static int sunxi_rtc_set_time(const struct device *dev, const struct rtc_time *tm)
{
	int year = tm->tm_year + 1900;
	int ret;

	ARG_UNUSED(dev);
	if (year < YEAR_MIN || year > YEAR_MAX || tm->tm_mon < 0 || tm->tm_mon > 11 ||
	    tm->tm_mday < 1 || tm->tm_mday > days_in_month(year, tm->tm_mon) || tm->tm_hour < 0 ||
	    tm->tm_hour > 23 || tm->tm_min < 0 || tm->tm_min > 59 || tm->tm_sec < 0 ||
	    tm->tm_sec > 59) {
		return -EINVAL;
	}
	ret = busy_wait_clear(LOSC_YMD_BUSY | LOSC_HMS_BUSY);
	if (ret != 0) {
		return ret;
	}
	wr(REG_HMS, hms_word(tm));
	ret = busy_wait_clear(LOSC_HMS_BUSY);
	if (ret != 0) {
		return ret;
	}
	wr(REG_YMD, day_number(year, tm->tm_mon, tm->tm_mday));

	return busy_wait_clear(LOSC_YMD_BUSY | LOSC_HMS_BUSY);
}

static int sunxi_rtc_get_time(const struct device *dev, struct rtc_time *tm)
{
	uint32_t day, hms;

	ARG_UNUSED(dev);
	/* the two registers belong to different domains: read until both are stable */
	do {
		day = rd(REG_YMD);
		hms = rd(REG_HMS);
	} while (day != rd(REG_YMD) || hms != rd(REG_HMS));
	if (day == 0U) {
		return -ENODATA;
	}
	date_of_day(day, tm);
	tm->tm_sec = hms & 0x3f;
	tm->tm_min = (hms >> 8) & 0x3f;
	tm->tm_hour = (hms >> 16) & 0x1f;
	tm->tm_isdst = -1;
	tm->tm_nsec = 0;

	return 0;
}

/* ---- alarm --------------------------------------------------------------------------------- */

#ifdef CONFIG_RTC_ALARM

#define ALARM_FIELDS (RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | \
		      RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MONTHDAY | \
		      RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR)

static int sunxi_rtc_alarm_get_supported_fields(const struct device *dev, uint16_t id,
						uint16_t *mask)
{
	ARG_UNUSED(dev);
	if (id != 0U) {
		return -EINVAL;
	}
	*mask = ALARM_FIELDS;

	return 0;
}

static int sunxi_rtc_alarm_set_time(const struct device *dev, uint16_t id, uint16_t mask,
				    const struct rtc_time *tm)
{
	int year;

	ARG_UNUSED(dev);
	if (id != 0U || (mask != 0U && mask != ALARM_FIELDS)) {
		return -EINVAL;
	}
	/* the mask 0 turns the alarm off */
	wr(REG_ALRM_EN, 0);
	wr(REG_ALRM_IRQ_EN, 0);
	wr(REG_ALRM_IRQ_STA, ALRM_BIT);
	if (mask == 0U) {
		return 0;
	}
	year = tm->tm_year + 1900;
	if (year < YEAR_MIN || year > YEAR_MAX || tm->tm_mon < 0 || tm->tm_mon > 11 ||
	    tm->tm_mday < 1 || tm->tm_mday > days_in_month(year, tm->tm_mon)) {
		return -EINVAL;
	}
	wr(REG_ALRM_DAY, day_number(year, tm->tm_mon, tm->tm_mday));
	wr(REG_ALRM_HMS, hms_word(tm));
	wr(REG_ALRM_CONFIG, ALRM_WAKEUP_OUT);
	wr(REG_ALRM_IRQ_EN, ALRM_BIT);
	wr(REG_ALRM_EN, ALRM_BIT);

	return 0;
}

static int sunxi_rtc_alarm_get_time(const struct device *dev, uint16_t id, uint16_t *mask,
				    struct rtc_time *tm)
{
	ARG_UNUSED(dev);
	if (id != 0U) {
		return -EINVAL;
	}
	if ((rd(REG_ALRM_EN) & ALRM_BIT) == 0U) {
		*mask = 0;
		return 0;
	}
	memset(tm, 0, sizeof(*tm));
	date_of_day(rd(REG_ALRM_DAY), tm);
	tm->tm_sec = rd(REG_ALRM_HMS) & 0x3f;
	tm->tm_min = (rd(REG_ALRM_HMS) >> 8) & 0x3f;
	tm->tm_hour = (rd(REG_ALRM_HMS) >> 16) & 0x1f;
	*mask = ALARM_FIELDS;

	return 0;
}

static int sunxi_rtc_alarm_is_pending(const struct device *dev, uint16_t id)
{
	struct rtc_sunxi_data *data = dev->data;
	k_spinlock_key_t key;
	int pending;

	if (id != 0U) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	pending = data->alarm_pending ? 1 : 0;
	data->alarm_pending = false;
	k_spin_unlock(&data->lock, key);

	return pending;
}

static int sunxi_rtc_alarm_set_callback(const struct device *dev, uint16_t id,
					rtc_alarm_callback cb, void *user_data)
{
	struct rtc_sunxi_data *data = dev->data;
	k_spinlock_key_t key;

	if (id != 0U) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	data->alarm_cb = cb;
	data->alarm_user_data = user_data;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void sunxi_rtc_isr(const struct device *dev)
{
	struct rtc_sunxi_data *data = dev->data;
	rtc_alarm_callback cb;
	void *user_data;

	if ((rd(REG_ALRM_IRQ_STA) & ALRM_BIT) == 0U) {
		return;
	}
	wr(REG_ALRM_IRQ_STA, ALRM_BIT);
	/* one shot: the alarm stays off until it is set again */
	wr(REG_ALRM_EN, 0);
	wr(REG_ALRM_IRQ_EN, 0);
	cb = data->alarm_cb;
	user_data = data->alarm_user_data;
	if (cb != NULL) {
		cb(dev, 0, user_data);
	} else {
		data->alarm_pending = true;
	}
}

#endif /* CONFIG_RTC_ALARM */

/* ---- setup --------------------------------------------------------------------------------- */

static int sunxi_rtc_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	wr(REG_ALRM_EN, 0);
	wr(REG_ALRM_IRQ_EN, 0);
	wr(REG_ALRM_IRQ_STA, ALRM_BIT);

	{
		uint32_t v;

		/*
		 * Bit 0 of the control register selects the crystal or the on-chip oscillator, bit 1
		 * the 24 MHz divider instead of both. The writes need the key in the upper half, the
		 * automatic switch back to the on-chip oscillator is off for a fixed choice.
		 */
		if (DT_INST_ENUM_IDX(0, clock_source) != 0) {
			v = rd(REG_LOSC_CTRL) & 0xffffU;
			v = (v | LOSC_AUTO_SWITCH_BYPASS) & ~LOSC_AUTO_SWITCH_EN;
			wr(REG_LOSC_CTRL, v | LOSC_KEY);
		}
		switch (DT_INST_ENUM_IDX(0, clock_source)) {
		case 1: /* internal */
			v = rd(REG_LOSC_CTRL) & 0xffffU & ~(LOSC_SRC_EXTERNAL | LOSC_SRC_HOSC);
			wr(REG_LOSC_CTRL, v | LOSC_KEY);
			break;
		case 2: /* external */
			v = rd(REG_LOSC_CTRL) & 0xffffU;
			wr(REG_LOSC_CTRL, v | LOSC_EXT_EN | LOSC_KEY);
			k_msleep(10);
			v = rd(REG_LOSC_CTRL) & 0xffffU;
			wr(REG_LOSC_CTRL, ((v | LOSC_SRC_EXTERNAL) & ~LOSC_SRC_HOSC) | LOSC_KEY);
			break;
		case 3: /* hosc */
			/*
			 * The sequence: the automatic switch off with the crystal path selected, the
			 * crystal driver off, the divider of the 24 MHz crystal on, and the counter
			 * switched to it.
			 */
			v = (rd(REG_LOSC_CTRL) & 0xffffU & ~LOSC_AUTO_SWITCH_EN) | LOSC_AUTO_SWITCH_BYPASS |
			    LOSC_SRC_EXTERNAL;
			wr(REG_LOSC_CTRL, v | LOSC_KEY);
			v = rd(REG_LOSC_CTRL) & 0xffffU & ~LOSC_EXT_EN;
			wr(REG_LOSC_CTRL, v | LOSC_KEY);
			wr(REG_HOSC_32K, rd(REG_HOSC_32K) | HOSC_32K_EN);
			v = rd(REG_LOSC_CTRL) & 0xffffU;
			wr(REG_LOSC_CTRL, v | LOSC_SRC_HOSC | LOSC_KEY);
			break;
		default:
			break;
		}
	}
	LOG_INF("control %08x, divider %08x, source status %08x (crystal %s), time %08x %08x",
		rd(REG_LOSC_CTRL), rd(REG_HOSC_32K), rd(REG_AUTO_STA), (rd(REG_AUTO_STA) & AUTO_STA_EXT_OK) ? "ok" : "not running",
		rd(REG_YMD), rd(REG_HMS));

#ifdef CONFIG_RTC_ALARM
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), sunxi_rtc_isr, DEVICE_DT_INST_GET(0),
		    0);
	irq_enable(DT_INST_IRQN(0));
#endif

	return 0;
}

static DEVICE_API(rtc, sunxi_rtc_api) = {
	.set_time = sunxi_rtc_set_time,
	.get_time = sunxi_rtc_get_time,
#ifdef CONFIG_RTC_ALARM
	.alarm_get_supported_fields = sunxi_rtc_alarm_get_supported_fields,
	.alarm_set_time = sunxi_rtc_alarm_set_time,
	.alarm_get_time = sunxi_rtc_alarm_get_time,
	.alarm_is_pending = sunxi_rtc_alarm_is_pending,
	.alarm_set_callback = sunxi_rtc_alarm_set_callback,
#endif
};

static struct rtc_sunxi_data rtc_data;

DEVICE_DT_INST_DEFINE(0, sunxi_rtc_init, NULL, &rtc_data, NULL, POST_KERNEL,
		      CONFIG_RTC_INIT_PRIORITY, &sunxi_rtc_api);
