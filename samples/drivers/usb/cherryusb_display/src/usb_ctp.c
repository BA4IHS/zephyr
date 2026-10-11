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

/* what the input events tell about each finger (slot) */
static struct {
	uint16_t x, y;		/* in panel pixels */
	bool down;
	bool lifted;		/* went up since the last report: one report with the tip switch off */
} slot[POINTS];
static int cur_slot;

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

/* one report with the slots 0..count-1, a lifted finger once with its tip switch off */
static void ctp_send(void)
{
	struct usb_touch_point out[POINTS];
	int count = 0;

	memset(out, 0, sizeof(out));
	for (int i = 0; i < POINTS; i++) {
		out[i].id = i;
		if (slot[i].down || slot[i].lifted) {
			out[i].down = slot[i].down;
			scale(slot[i].x, slot[i].y, &out[i].x, &out[i].y);
			count = i + 1;
		}
		slot[i].lifted = false;
	}
	if (count == 0) {
		return;
	}
	for (int n = 0; usb_touch_send(out, count) == -EBUSY && n < SEND_WAIT_MS; n++) {
		k_msleep(1);
	}
}

static void ctp_event(struct input_event *evt, void *user_data)
{
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
		if (!evt->value && slot[cur_slot].down) {
			slot[cur_slot].lifted = true;
		}
		slot[cur_slot].down = evt->value;
		break;
	default:
		break;
	}
	if (evt->sync) {
		ctp_send();
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), ctp_event, NULL);

void usb_ctp_start(void)
{
	if (!device_is_ready(touch_dev)) {
		printk("usb ctp: the touch panel is not ready\n");
		return;
	}
	printk("usb ctp: touch panel %s, %ux%u\n", touch_dev->name, CTP_MAX_X, CTP_MAX_Y);
}
