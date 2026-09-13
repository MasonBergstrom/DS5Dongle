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
#include "wake.h"
#include "pico/time.h"

uint8_t mute[2] = {}; // 0: SPEAKER(0x02) 1: MIC(0x05)
float volume[2] = {0.0f,48.0f}; // 0: SPEAKER(0x02) 1: MIC(0x05)

namespace {
enum class UsbIdentityTarget : uint8_t { Detached, Idle, Full };
enum class UsbIdentityPhase : uint8_t { Detached, WaitingAttach, Connecting, Served };

constexpr uint64_t USB_REENUMERATE_DELAY_US = 250'000; // exceeds USB's 100 ms debounce

UsbIdentityTarget identity_target = UsbIdentityTarget::Detached;
UsbIdentityPhase identity_phase = UsbIdentityPhase::Detached;
bool descriptor_idle = false;
bool served_idle = false;
bool reconnect_requested = false;
uint64_t detached_at_us = 0;

void request_identity(UsbIdentityTarget target) {
#if ENABLE_SERIAL
    (void) target; // Keep the CDC diagnostic identity attached in serial builds.
#else
    identity_target = target;
#endif
}
}

void usb_identity_init() {
#if ENABLE_SERIAL
    identity_target = UsbIdentityTarget::Full;
    identity_phase = UsbIdentityPhase::Served;
    descriptor_idle = false;
    served_idle = false;
#else
    if (!get_config().enable_wake) {
        identity_target = UsbIdentityTarget::Detached;
    } else if (get_config().enable_idle_usb) {
        identity_target = UsbIdentityTarget::Idle;
    } else {
        identity_target = UsbIdentityTarget::Full;
    }
    identity_phase = identity_target == UsbIdentityTarget::Detached
                         ? UsbIdentityPhase::Detached
                         : UsbIdentityPhase::WaitingAttach;
    descriptor_idle = identity_target == UsbIdentityTarget::Idle;
    served_idle = false;
    detached_at_us = time_us_64();
#endif
}

void usb_identity_request_full() { request_identity(UsbIdentityTarget::Full); }
void usb_identity_request_idle() { request_identity(UsbIdentityTarget::Idle); }
void usb_identity_request_detached() { request_identity(UsbIdentityTarget::Detached); }

void usb_identity_reconnect() {
#if ENABLE_SERIAL
    wake_note_usb_reconnect();
    tud_disconnect();
    sleep_ms(150);
    tud_connect();
#else
    if (!get_config().enable_wake) {
        identity_target = UsbIdentityTarget::Detached;
    } else if (!bt_is_connected() && get_config().enable_idle_usb) {
        identity_target = UsbIdentityTarget::Idle;
    } else {
        identity_target = UsbIdentityTarget::Full;
    }
    // Defer the detach until after the SET_REPORT control transfer completes.
    reconnect_requested = true;
#endif
}

bool usb_idle_descriptor_requested() { return descriptor_idle; }

bool usb_idle_identity_active() {
    return identity_phase == UsbIdentityPhase::Served && served_idle;
}

bool usb_gamepad_available() {
    return identity_phase == UsbIdentityPhase::Served && !served_idle;
}

void usb_identity_note_descriptor_served(bool idle) {
    served_idle = idle;
    identity_phase = UsbIdentityPhase::Served;
}

void usb_identity_task() {
#if ENABLE_SERIAL
    return;
#else
    // Re-enumerating while suspended can wake the host before the wake HID
    // sequence is ready. Likewise, do not remove the keyboard while F15 is
    // still held or awaiting its key-up report.
    if (tud_suspended() || wake_owns_keyboard()) return;

    const uint64_t now = time_us_64();
    if (reconnect_requested) {
        reconnect_requested = false;
        wake_note_usb_reconnect();
        if (identity_phase != UsbIdentityPhase::Detached) tud_disconnect();

        if (identity_target == UsbIdentityTarget::Detached) {
            identity_phase = UsbIdentityPhase::Detached;
            return;
        }

        descriptor_idle = identity_target == UsbIdentityTarget::Idle;
        served_idle = false;
        identity_phase = UsbIdentityPhase::WaitingAttach;
        detached_at_us = now;
        return;
    }

    if (identity_target == UsbIdentityTarget::Detached) {
        if (identity_phase != UsbIdentityPhase::Detached) {
            wake_note_usb_reconnect();
            tud_disconnect();
            identity_phase = UsbIdentityPhase::Detached;
            detached_at_us = now;
        }
        return;
    }

    const bool want_idle = identity_target == UsbIdentityTarget::Idle;
    if (identity_phase == UsbIdentityPhase::Served && served_idle == want_idle) return;

    if (identity_phase == UsbIdentityPhase::Served ||
        (identity_phase == UsbIdentityPhase::Connecting && descriptor_idle != want_idle)) {
        wake_note_usb_reconnect();
        tud_disconnect();
        identity_phase = UsbIdentityPhase::WaitingAttach;
        descriptor_idle = want_idle;
        detached_at_us = now;
        return;
    }

    if (identity_phase == UsbIdentityPhase::Detached) {
        identity_phase = UsbIdentityPhase::WaitingAttach;
        descriptor_idle = want_idle;
        detached_at_us = now;
        return;
    }

    if (identity_phase == UsbIdentityPhase::WaitingAttach) {
        descriptor_idle = want_idle;
        if (now - detached_at_us >= USB_REENUMERATE_DELAY_US) {
            identity_phase = UsbIdentityPhase::Connecting;
            tud_connect();
        }
    }
#endif
}

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

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void) instance;
    (void) len;
}

#ifndef ENABLE_WAKE_HID

void tud_suspend_cb(bool remote_wakeup_en) {
    printf("[USB PM] invoke tud_suspend_cb\n");
    if (!get_config().enable_wake) return;   // wake off: leave the controller's BT alone on a USB suspend (see wake.cpp)
    bt_disconnect();
}

#endif
