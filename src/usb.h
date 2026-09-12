//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#include <cstdint>

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

// Record the bInterval actually sent to the host during enumeration. It is the
// authoritative poll period until the next USB reconnect.
void usb_note_enumerated_binterval(uint8_t binterval);
uint32_t usb_hid_poll_period_us();

#endif //DS5_BRIDGE_USB_H