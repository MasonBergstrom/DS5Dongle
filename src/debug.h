//
// Created by Codex on 2026/7/7.
//

#ifndef DS5_BRIDGE_DEBUG_H
#define DS5_BRIDGE_DEBUG_H

#include <cstdint>

#ifndef ENABLE_DEBUG
#define ENABLE_DEBUG 0
#endif

#if ENABLE_DEBUG

void debug_fill_core1_stack_watermark(uint32_t *stack, uint32_t word_count);
void debug_log_core1_stack_usage();

// Call once per DualSense controller-state input report (0x31, non-mic) to
// measure the Bluetooth report arrival cadence. Accumulates min/max/mean
// inter-report interval and logs a summary over UART every ~2 s.
void debug_bt_report_arrival();

// Polling-rate instrumentation, mode-agnostic (works for all polling_rate_mode
// values without the send path having to know which mode is active).
//
//   debug_note_bt_frame_staged() -- a fresh controller frame is now available.
//   debug_usb_report_sent()      -- a HID report was accepted by TinyUSB.
//
// Together they answer the questions raw "reports per second" cannot:
//   sends  -- raw poll rate (what a naive rate tester sees)
//   fresh  -- reports that actually carried new controller data
//   dup    -- padding (mode 3 repeats the last value to keep an even cadence)
//   drop   -- BT frames replaced before they ever reached the host
//   lat    -- BT arrival -> USB handoff, for fresh reports only
// Logged over UART every ~2 s.
//
// NOTE: `lat` ends at tud_hid_report(), which only QUEUES the report. It is
// firmware-side responsiveness, not end-to-end delivery to the host (which adds
// up to one poll interval on top).
//
// debug_note_bt_frame_staged() also inspects the controller's own
// SensorTimestamp (USBGetStateData offset 27) to report how many BT frames
// carry genuinely new sensor data versus repeats -- this is what decides
// whether a lower polling mode discards real information or just duplicates.
// `state` must point at the 63-byte USBGetStateData payload (data + 3).
void debug_note_bt_frame_staged(const uint8_t *state, uint16_t len);
void debug_usb_report_sent();

// Called from tud_hid_report_complete_cb when the host has actually COLLECTED a
// report. This is the metric that matters: `stale` is the age of the newest BT
// sample in the delivered report, measured at the moment the host got it --
// unlike `lat`, which stops at queueing.
// `poll` is the spacing between collections; `late` counts intervals long
// enough to mean a poll slot was missed (a cadence gap).
void debug_usb_report_delivered(uint64_t now_us);

#endif

#endif // DS5_BRIDGE_DEBUG_H
