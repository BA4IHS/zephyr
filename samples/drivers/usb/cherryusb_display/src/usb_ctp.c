/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The capacitive touch panel (the touch node of the board, a GT967 on I2C1 with its driver in the
 * input subsystem) as the touch screen of the USB display: the input events of the panel are
 * collected per finger and turned into reports of the HID touch interface.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>

#include "usb_ctp.h"
#include "usb_touch.h"

#define TOUCH_NODE	DT_NODELABEL(touch)
#define CTP_MAX_X	DT_PROP(TOUCH_NODE, screen_width)
#define CTP_MAX_Y	DT_PROP(TOUCH_NODE, screen_height)
#define POINTS		USB_TOUCH_MAX_CONTACTS
#define SEND_WAIT_MS	20

static const struct device *const touch_dev = DEVICE_DT_GET(TOUCH_NODE);

static bool swap_xy = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_SWAP);
static bool flip_x = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_FLIP_X);
static bool flip_y = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_FLIP_Y);

/*
 * A finger the panel loses for a moment would end a drag selection or a press and hold on the PC,
 * so a lift is reported only when the finger stays away for UP_DEBOUNCE_MS. While a finger is
 * down the report is repeated every BEAT_MS, as the PC expects from a touch screen.
 */
#define UP_DEBOUNCE_MS	50
#define BEAT_MS		20

/* what the input events tell about each finger (slot) */
static struct {
	uint16_t x, y;		/* in panel pixels */
	bool down;		/* as the PC knows it */
	bool lifted;		/* one more report with the tip switch off */
	uint32_t up_at;		/* the panel lost the finger: when to tell the PC, 0 when it is down */
} slot[POINTS];
static int cur_slot;
static struct k_spinlock lock;
static struct k_work_q ctp_q;
K_THREAD_STACK_DEFINE(ctp_q_stack, 2048);
static void ctp_beat(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ctp_work, ctp_beat);

static void scale(uint32_t x, uint32_t y, uint16_t *ox, uint16_t *oy)
{
	uint32_t max_x = CTP_MAX_X, max_y = CTP_MAX_Y;

	if (swap_xy) {
		uint32_t t = x;

		x = y;
		y = t;
		max_x = CTP_MAX_Y;
		max_y = CTP_MAX_X;
	}
	if (flip_x) {
		x = max_x - 1 - MIN(x, max_x - 1);
	}
	if (flip_y) {
		y = max_y - 1 - MIN(y, max_y - 1);
	}
	x = MIN(x, max_x - 1);
	y = MIN(y, max_y - 1);
	*ox = x * USB_TOUCH_MAX / (max_x - 1);
	*oy = y * USB_TOUCH_MAX / (max_y - 1);
}

/*
 * One report with the slots 0..count-1, a lifted finger once with its tip switch off. It runs in
 * a queue of its own: the input events come in the system work queue, which must not wait for the
 * USB endpoint.
 */
static void ctp_beat(struct k_work *work)
{
	struct usb_touch_point out[POINTS];
	uint32_t now = k_uptime_get_32();
	bool any = false;
	int count = 0;
	k_spinlock_key_t key;

	ARG_UNUSED(work);
	memset(out, 0, sizeof(out));
	key = k_spin_lock(&lock);
	for (int i = 0; i < POINTS; i++) {
		if (slot[i].down && slot[i].up_at != 0 && (int32_t)(now - slot[i].up_at) >= 0) {
			slot[i].down = false;
			slot[i].lifted = true;
			slot[i].up_at = 0;
		}
		out[i].id = i;
		if (slot[i].down || slot[i].lifted) {
			out[i].down = slot[i].down;
			scale(slot[i].x, slot[i].y, &out[i].x, &out[i].y);
			count = i + 1;
		}
		slot[i].lifted = false;
		any |= slot[i].down;
	}
	k_spin_unlock(&lock, key);

	if (count != 0) {
		for (int n = 0; usb_touch_send(out, count) == -EBUSY && n < SEND_WAIT_MS; n++) {
			k_msleep(1);
		}
	}
	if (any) {
		k_work_schedule_for_queue(&ctp_q, &ctp_work, K_MSEC(BEAT_MS));
	}
}

static void ctp_event(struct input_event *evt, void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	switch (evt->code) {
	case INPUT_ABS_MT_SLOT:
		cur_slot = CLAMP(evt->value, 0, POINTS - 1);
		break;
	case INPUT_ABS_X:
		slot[cur_slot].x = evt->value;
		break;
	case INPUT_ABS_Y:
		slot[cur_slot].y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		if (evt->value) {
			slot[cur_slot].down = true;
			slot[cur_slot].up_at = 0;
		} else if (slot[cur_slot].down && slot[cur_slot].up_at == 0) {
			slot[cur_slot].up_at = k_uptime_get_32() + UP_DEBOUNCE_MS;
		}
		break;
	default:
		break;
	}
	k_spin_unlock(&lock, key);
	if (evt->sync) {
		k_work_reschedule_for_queue(&ctp_q, &ctp_work, K_NO_WAIT);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), ctp_event, NULL);

void usb_ctp_start(void)
{
	if (!device_is_ready(touch_dev)) {
		printk("usb ctp: the touch panel is not ready\n");
		return;
	}
	k_work_queue_init(&ctp_q);
	k_work_queue_start(&ctp_q, ctp_q_stack, K_THREAD_STACK_SIZEOF(ctp_q_stack), 6, NULL);
	printk("usb ctp: touch panel %s, %ux%u\n", touch_dev->name, CTP_MAX_X, CTP_MAX_Y);
}
