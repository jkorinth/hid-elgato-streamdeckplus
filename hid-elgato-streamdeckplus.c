// SPDX-License-Identifier: GPL-2.0+
/*
 *  HID driver for Elgato Stream Deck+.
 *
 *  Copyright (c) 2025 Jens Korinth <jens.korinth@tuta.io>
 */
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/version.h>

#include "hid-ids.h"

#define SDP_TOUCHSCREEN_RES_X (4 * 256 - 1)
#define SDP_TOUCHSCREEN_RES_Y 128
#define SDP_TOUCHSCREEN_WIDTH_MM 108
#define SDP_TOUCHSCREEN_HEIGHT_MM 14

typedef enum {
	SDP_EV_UNKNOWN,
	SDP_EV_TAP_SHORT,
	SDP_EV_TAP_LONG,
	SDP_EV_DRAG,
} streamdeck_ev_t;

struct streamdeck_coord {
	u8 region;
	u8 x;
	u8 y;
};

struct streamdeck_event {
	streamdeck_ev_t type;
	struct streamdeck_coord coord[2];
};

static int streamdeck_probe(struct hid_device *hdev,
			    const struct hid_device_id *id)
{
	int ret = 0;
	struct usb_interface *usbif;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	usbif = to_usb_interface(hdev->dev.parent);
	ret = hid_parse(hdev);
	if (ret != 0) {
		hid_err(hdev, "parse failed! (%d)\n", ret);
		return ret;
	}
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret != 0) {
		hid_err(hdev, "hw start failed! (%d)\n", ret);
		return ret;
	}

	return 0;
}

static void streamdeck_remove(struct hid_device *hdev)
{
	struct hid_input *hidinput =
		list_first_entry(&hdev->inputs, struct hid_input, list);
	struct input_dev *input = hidinput->input;
	if (input->absinfo) {
		kfree(input->absinfo);
		input->absinfo = NULL;
	}
	hid_hw_stop(hdev);
}

/*
 * The Elgatoo StreamDeck+ only reports custom usages, leading to unusable
 * events with identical keycodes for every button. This deactivates those
 * usages, they are replaced by raw event processing below.
 */
static
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	const
#endif
	__u8 *
	streamdeck_report_fixup(struct hid_device *hdev, __u8 *rdesc,
				unsigned int *rsize)
{
	for (unsigned int i = 0; i < *rsize - 2; i++) {
		if (rdesc[i] == 0x0A && rdesc[i + 1] == 0x00 &&
		    rdesc[i + 2] == 0xFF) {
			rdesc[i + 2] = 0x00; // deactivate
		}
	}
	return rdesc;
}

static int streamdeck_input_configured(struct hid_device *hdev,
				       struct hid_input *hidinput)
{
	struct input_dev *input = hidinput->input;
	for (int i = KEY_MACRO1; i <= KEY_MACRO12; i++) {
		input_set_capability(input, EV_KEY, i);
	}
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	input_set_capability(input, EV_REL, REL_Z);
	input_set_capability(input, EV_REL, REL_MISC);
	input_set_capability(input, EV_ABS, ABS_X);
	input_set_capability(input, EV_ABS, ABS_Y);
	input_set_abs_params(input, ABS_X, 0, SDP_TOUCHSCREEN_RES_X, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, SDP_TOUCHSCREEN_RES_Y, 0, 0);
	input_abs_set_res(input, ABS_X,
			  SDP_TOUCHSCREEN_RES_X / SDP_TOUCHSCREEN_WIDTH_MM);
	input_abs_set_res(input, ABS_Y,
			  SDP_TOUCHSCREEN_RES_Y / SDP_TOUCHSCREEN_HEIGHT_MM);
	return 0;
}

static int streamdeck_parse_button_report(struct hid_device *hdev,
					  struct input_dev *input, u8 *data,
					  int size)
{
	for (int i = 0; i < 8; ++i) {
		hid_dbg(hdev, "button %d: 0x%08x\n", i, data[4 + i]);
		input_event(input, EV_KEY, KEY_MACRO1 + i, data[4 + i] > 0);
	}
	input_sync(input);
	return 1; // stop processing
}

static int streamdeck_parse_rotenc_report(struct hid_device *hdev,
					  struct input_dev *input, u8 *data,
					  int size)
{
	if (data[4] == 0) {
		for (int i = 5; i < 9; ++i) {
			hid_dbg(hdev, "push button %d: 0x%08x\n", i, data[i]);
			input_event(input, EV_KEY, KEY_MACRO9 + i - 5,
				    data[i] > 0);
		}
	} else if (data[4] == 1) {
		if (data[5]) {
			hid_dbg(hdev, "rel_x: %d\n", (s8)data[5]);
			input_event(input, EV_REL, REL_X, (s8)data[5]);
		}
		if (data[6]) {
			hid_dbg(hdev, "rel_y: %d\n", (s8)data[6]);
			input_event(input, EV_REL, REL_Y, (s8)data[6]);
		}
		if (data[7]) {
			hid_dbg(hdev, "rel_z: %d\n", (s8)data[7]);
			input_event(input, EV_REL, REL_Z, (s8)data[7]);
		}
		if (data[8]) {
			hid_dbg(hdev, "rel_misc: %d\n", (s8)data[8]);
			input_event(input, EV_REL, REL_MISC, (s8)data[8]);
		}
	} else {
		hid_warn(hdev, "received unknown rotary encoder report: %u\n",
			 data[4]);
	}
	input_sync(input);
	return 0;
}

static int streamdeck_parse_touch_report(struct hid_device *hdev,
					 struct input_dev *input, u8 *data,
					 int size)
{
	struct streamdeck_event ev = {
		.type = data[4],
		.coord = {
			{ .region = data[7], .x = data[6], .y = data[8] },
			{ .region = data[11], .x = data[10], .y = data[12] },
		},
	};
	hid_dbg(hdev,
		"ev = %d\n"
		"region0 = %u: x0 = %u y0 = %u nx0 = %u ny0 = %u\n"
		"region1 =  %u: x1 = %u y1 = %u nx0 = %u ny0 = %u\n",
		ev.type, ev.coord[0].region, ev.coord[0].x, ev.coord[0].y,
		ev.coord[0].region * 256 + ev.coord[0].x, ev.coord[0].y,
		ev.coord[1].region, ev.coord[1].x, ev.coord[1].y,
		ev.coord[1].region * 256 + ev.coord[1].x, ev.coord[1].y);
	switch (ev.type) {
	case SDP_EV_TAP_SHORT:
		input_event(input, EV_KEY, BTN_TOUCH, 1);
		input_event(input, EV_ABS, ABS_X,
			    ev.coord[0].region * 255 + ev.coord[0].x);
		input_event(input, EV_ABS, ABS_Y, ev.coord[0].y);
		input_sync(input);
		input_event(input, EV_KEY, BTN_TOUCH, 0);
		break;
	case SDP_EV_TAP_LONG:
		input_event(input, EV_KEY, BTN_TOUCH, 1);
		input_event(input, EV_ABS, ABS_X,
			    ev.coord[0].region * 255 + ev.coord[0].x);
		input_event(input, EV_ABS, ABS_Y, ev.coord[0].y);
		input_event(input, EV_KEY, BTN_TOOL_FINGER, 1);
		input_sync(input);
		input_event(input, EV_KEY, BTN_TOOL_FINGER, 0);
		input_event(input, EV_KEY, BTN_TOUCH, 0);
		break;
	case SDP_EV_DRAG:
		input_event(input, EV_KEY, BTN_TOUCH, 1);
		input_event(input, EV_ABS, ABS_X,
			    ev.coord[0].region * 255 + ev.coord[0].x);
		input_event(input, EV_ABS, ABS_Y, ev.coord[0].y);
		input_sync(input);

		input_event(input, EV_ABS, ABS_X,
			    ev.coord[1].region * 255 + ev.coord[1].x);
		input_event(input, EV_ABS, ABS_Y, ev.coord[1].y);
		input_event(input, EV_KEY, BTN_TOUCH, 0);
		break;
	default:
		/* unknown event */
		hid_warn(hdev, "received unknown touch event type: %d\n",
			 ev.type);
		break;
	}
	input_sync(input);
	return 0;
}

/*
 * Replaces custom usages with "normal" event codes:
 *  1. 8 programmable buttons on top generate KEY_MACRO1 to KEY_MACRO9.
 *  2. 4 rotary encoders generate KEY_MACRO10 to KEY_MACRO13 on push,
 *     axes REL_X, REL_Y, REL_Z and REL_MISC (left to right) on
 *     turn.
 *  3. Touchscreen above rotary encoders is 1024x128, reports
 *     BTN_TOUCH + EV_ABS coordinates + BTN_TOOL_FINGER for long press.
 *
 * All input events are handled in the report #1 in a proprietary format.
 * First byte defines type of event:
 *   0x00 => button events
 *   0x01 => touch events
 *   0x02 => rotary encoder events
 * Generates input events EV_KEY, EV_REL and EV_ABS.
 */
static int streamdeck_raw_event(struct hid_device *hdev,
				struct hid_report *report, u8 *data, int size)
{
	struct hid_input *hidinput;
	struct input_dev *input;

	if (list_empty(&hdev->inputs)) {
		hid_err(hdev, "no inputs found\n");
		return -ENODEV;
	}

	hidinput = list_first_entry(&hdev->inputs, struct hid_input, list);
	input = hidinput->input;

	switch (report->id) {
	case 0x01:
		if (data[1] == 0x00 && data[2] == 0x08) {
			return streamdeck_parse_button_report(hdev, input, data,
							      size);
		} else if (data[1] == 0x02 && data[2] == 0x0e) {
			return streamdeck_parse_touch_report(hdev, input, data,
							     size);
		} else if (data[1] == 0x03 && data[2] == 0x05) {
			return streamdeck_parse_rotenc_report(hdev, input, data,
							      size);
		} else {
			hid_warn(
				hdev,
				"received unknown event with header 0x%02x%02x%02x%02x\n",
				data[0], data[1], data[2], data[3]);
		}
		break;
	default:
		hid_dbg(hdev, "received report #%u\n", report->id);
		break;
	}

	return 0;
}

static const struct hid_device_id streamdeck_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ELGATO,
			 USB_DEVICE_ID_ELGATO_STREAMDECKPLUS) },
};

static struct hid_driver streamdeck_driver = {
	.name = "streamdeck",
	.id_table = streamdeck_devices,
	.probe = streamdeck_probe,
	.remove = streamdeck_remove,
	.raw_event = streamdeck_raw_event,
	.input_configured = streamdeck_input_configured,
	.report_fixup = streamdeck_report_fixup,
};

module_hid_driver(streamdeck_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jens Korinth <jens.korinth@tuta.io>");
MODULE_DESCRIPTION("HID driver for Elgato StreamDeck+");
