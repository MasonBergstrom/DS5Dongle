//
// Created by awalol on 2026/3/4.
//

#include <algorithm>

#include "bt.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "config.h"
#include "utils.h"
#include "usb.h"
#include "debug.h"
#include "pico/time.h"

uint8_t mute[2] = {}; // 0: SPEAKER(0x02) 1: MIC(0x05)
float volume[2] = {0.0f,48.0f}; // 0: SPEAKER(0x02) 1: MIC(0x05)

#define UAC1_ENTITY_SPK_FEATURE_UNIT    0x02
#define UAC1_ENTITY_MIC_FEATURE_UNIT    0x05

/*int main() {
    board_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    board_init_after_tusb();

    while (1) {
        tud_task();
    }
}*/

//--------------------------------------------------------------------+
// Audio Callback Functions
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// UAC1 Helper Functions
//--------------------------------------------------------------------+

static bool audio10_set_req_entity(tusb_control_request_t const *p_request, uint8_t *pBuff) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t index = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    // If request is for our speaker feature unit
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT || entityID == UAC1_ENTITY_MIC_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_SET_CUR: {
                        // Only 1st form is supported
                        TU_VERIFY(p_request->wLength == 1);

                        mute[index] = pBuff[0];

                        SetStateData state = {
                            .AllowAudioMute = 1,
                            .MicMute = mute[1],
                            .SpeakerMute = mute[0],
                            .HeadphoneMute = mute[0],
                        };
                        update_state(state);

                        TU_LOG2("    Set Mute: %d of entity: %u\r\n", mute[index], entityID);
                        return true;
                    }

                    default:
                        return false; // not supported
                }

            case AUDIO10_FU_CTRL_VOLUME:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_SET_CUR:
                        // Only 1st form is supported
                        TU_VERIFY(p_request->wLength == 2);

                        volume[index] = static_cast<float>(*reinterpret_cast<int16_t const *>(pBuff)) / 256;
                        if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                            SetStateData state = {
                                .AllowHeadphoneVolume = 1,
                                .AllowSpeakerVolume = 1,
                                .VolumeHeadphones = static_cast<uint8_t>(100.0f + volume[index]),
                                .VolumeSpeaker = static_cast<uint8_t>(100.0f + volume[index]),
                            };
                            update_state(state);
                        }
                        if (entityID == UAC1_ENTITY_MIC_FEATURE_UNIT) {
                            SetStateData state = {
                                .AllowMicVolume = 1,
                                .VolumeMic = static_cast<uint8_t>(volume[index]),
                            };
                            update_state(state);
                        }

                        TU_LOG2("    Set Volume: %d dB of entity: %u\r\n", volume[index], entityID);
                        return true;

                    default:
                        return false; // not supported
                }

            // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
        }
    }

    return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    uint8_t index = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    // If request is for our speaker feature unit
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT || entityID == UAC1_ENTITY_MIC_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
                // There does not exist a range parameter block for mute
                TU_LOG2("    Get Mute of entity: %u\r\n", entityID);
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[index], 1);

            case AUDIO10_FU_CTRL_VOLUME:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_GET_CUR:
                        TU_LOG2("    Get Volume of entity: %u\r\n", entityID); {
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                volume[index] = -100.0f + std::min(static_cast<int>(get_config().speaker_volume),100);
                            }
                            int16_t vol = volume[index] * 256; // convert to 1/256 dB units
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
                        }

                    case AUDIO10_CS_REQ_GET_MIN:
                        TU_LOG2("    Get Volume min of entity: %u\r\n", entityID); {
                            uint8_t min[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                min[0] = 0x00;
                                min[1] = 0x9c;
                            }else {
                                min[0] = 0x00;
                                min[1] = 0x00;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
                        }

                    case AUDIO10_CS_REQ_GET_MAX:
                        TU_LOG2("    Get Volume max of entity: %u\r\n", entityID); {
                            uint8_t max[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                max[0] = 0x00;
                                max[1] = 0x00;
                            }else {
                                max[0] = 0x00;
                                max[1] = 0x30;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
                        }

                    case AUDIO10_CS_REQ_GET_RES:
                        TU_LOG2("    Get Volume res of entity: %u\r\n", entityID); {
                            uint8_t res[2];
                            if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
                                res[0] = 0x00;
                                res[1] = 0x01;
                            }else {
                                res[0] = 0x7a;
                                res[1] = 0x00;
                            }
                            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
                        }
                    // Unknown/Unsupported control
                    default:
                        TU_BREAKPOINT();
                        return false;
                }
                break;

            // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
        }
    }

    return false;
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;

    return audio10_get_req_entity(rhport, p_request);
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void) rhport;

    return audio10_set_req_entity(p_request, buf);
}

static volatile uint32_t hid_poll_period_us = 0;
static volatile uint64_t hid_last_complete_us = 0;

// Additive-increase/multiplicative-decrease-style controller for duplicate
// padding. Completion callbacks are dispatched from tud_task(), so individual
// interval measurements are noisy; evaluate completion rate over a window.
static constexpr uint32_t HID_DEFER_UP_US = 25;
static constexpr uint32_t HID_DEFER_DOWN_US = 100;
static constexpr uint32_t HID_RATE_WINDOW = 256;
static volatile uint32_t hid_defer_us = 0;
static uint32_t hid_rate_count = 0;
static uint64_t hid_rate_start_us = 0;

void usb_note_enumerated_binterval(uint8_t binterval) {
    hid_poll_period_us = binterval ? static_cast<uint32_t>(binterval) * 1000u : 0u;
}

uint32_t usb_hid_poll_period_us() {
    return hid_poll_period_us;
}

uint64_t usb_hid_last_complete_us() {
    return hid_last_complete_us;
}

uint32_t usb_hid_defer_us() {
    return hid_defer_us;
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void) report;
    (void) len;
    if (instance != 0) return;

    const uint64_t now = time_us_64();
    const uint32_t period = hid_poll_period_us;
    if (period != 0) {
        if (hid_rate_start_us == 0) {
            hid_rate_start_us = now;
        } else if (++hid_rate_count >= HID_RATE_WINDOW) {
            const uint64_t elapsed = now - hid_rate_start_us;
            const uint64_t expected = static_cast<uint64_t>(hid_rate_count) * period;
            const uint32_t defer_cap = period - period / 8u;

            if (elapsed > expected + expected / 8u) {
                hid_defer_us = hid_defer_us > HID_DEFER_DOWN_US
                                   ? hid_defer_us - HID_DEFER_DOWN_US
                                   : 0;
            } else if (hid_defer_us < defer_cap) {
                hid_defer_us += HID_DEFER_UP_US;
            }

            hid_rate_count = 0;
            hid_rate_start_us = now;
        }
    }

    hid_last_complete_us = now;
#if ENABLE_DEBUG
    debug_usb_report_delivered(now);
#endif
}

#ifndef ENABLE_WAKE_HID

void tud_suspend_cb(bool remote_wakeup_en) {
    printf("[USB PM] invoke tud_suspend_cb\n");
    if (!get_config().enable_wake) return;   // wake off: leave the controller's BT alone on a USB suspend (see wake.cpp)
    bt_disconnect();
}

#endif
