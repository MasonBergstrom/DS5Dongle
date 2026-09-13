//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#include <cstdint>

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

// Dynamic USB identity. The full identity remains byte-for-byte DualSense.
// When wake is enabled and no controller is connected, the idle identity
// exposes only an inert vendor HID plus the wake keyboard; no gamepad or audio.
void usb_identity_init();
void usb_identity_request_full();
void usb_identity_request_idle();
void usb_identity_request_detached();
void usb_identity_reconnect();
void usb_identity_task();

// Descriptor/callback routing state. The requested descriptor is changed only
// while physically detached; the served state records what the host received.
bool usb_idle_descriptor_requested();
bool usb_idle_identity_active();
bool usb_gamepad_available();
void usb_identity_note_descriptor_served(bool idle);

#endif //DS5_BRIDGE_USB_H