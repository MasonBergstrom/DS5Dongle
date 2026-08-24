#include "button_shortcut.h"

#include "bt.h"
#include "button_utils.h"
#include "config.h"
#include "tusb.h"
#include "pico/time.h"
#include "utils.h"

// Kept outside the ENABLE_WAKE_HID guard below: config_load() normalizes the
// stored slots on every boot regardless of the wake HID interface.
bool shortcut_slot_valid(const ButtonShortcut &shortcut) {
    if (shortcut.trigger_a >= Disable) return false;
    if (shortcut.flags & ~SHORTCUT_FLAG_MASK) return false;
    if (shortcut.trigger_b == SHORTCUT_TRIGGER_TAP ||
        shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP) {
        // Tap slots spell their tap count out in trigger_b.
        if (shortcut.flags & SHORTCUT_FLAG_DOUBLE_TAP) return false;
    } else {
        if (shortcut.trigger_b >= Disable || shortcut.trigger_a == shortcut.trigger_b) {
            return false;
        }
        // The DPad reports a single direction, so two directions can never be held
        // at once and such a chord would silently never fire.
        if (shortcut.trigger_a <= DPadNorthWest && shortcut.trigger_b <= DPadNorthWest) {
            return false;
        }
    }

    switch (shortcut.action) {
        case ShortcutActionKeyboard:
            return shortcut.keyboard.key <= SHORTCUT_KEY_USAGE_MAX &&
                   (shortcut.keyboard.modifiers != 0 || shortcut.keyboard.key != 0);
        case ShortcutActionConsumer:
            return shortcut.consumer.usage != 0 &&
                   shortcut.consumer.usage <= SHORTCUT_CONSUMER_USAGE_MAX;
        case ShortcutActionBtDisconnect:
            return true;
    }
    return false;
}

#ifdef ENABLE_WAKE_HID

static constexpr uint8_t KEYBOARD_INSTANCE = 1;
static constexpr uint8_t CONSUMER_INSTANCE = 2;
static constexpr uint32_t KEY_PRESS_MS = 30;

static bool release_pending = false;
static absolute_time_t release_time = nil_time;
static uint8_t release_instance = KEYBOARD_INSTANCE; // Which interface owes a release report.
static uint16_t pending_mask = 0; // Triggered slots waiting to send their HID report.

static_assert(BUTTON_SHORTCUT_COUNT <= 16, "slot masks are 16 bits wide");

// A slot is "engaged" while its trigger is pressed, which for a chord means both
// buttons held; taps and chords share one state machine from there on.
static uint8_t tap_count[BUTTON_SHORTCUT_COUNT]{};
static absolute_time_t tap_deadline[BUTTON_SHORTCUT_COUNT]{};
static uint16_t engaged_mask = 0; // Last-tick engagement of each slot; fire on 0 -> 1.
// Tap slots whose current press was taken by a chord on the same button. Sticky
// until the button comes back up, so a chord that has already come and gone
// still keeps the lone-tap action from firing on release.
static uint16_t chord_claimed_mask = 0;

// Triggers are ButtonIds below Disable (27), so one word holds them all.
static constexpr uint32_t button_bit(uint8_t button) {
    return button < Disable ? (1u << button) : 0u;
}

static bool is_tap_slot(const ButtonShortcut &shortcut) {
    return shortcut.trigger_b == SHORTCUT_TRIGGER_TAP ||
           shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP;
}

// Taps spell the count out in trigger_b, chords in the flags byte.
static bool wants_double(const ButtonShortcut &shortcut) {
    return shortcut.trigger_b == SHORTCUT_TRIGGER_DOUBLE_TAP ||
           (shortcut.flags & SHORTCUT_FLAG_DOUBLE_TAP);
}

// Same physical trigger? Chords are an unordered pair, so Create+Options and
// Options+Create are the same chord.
static bool same_trigger(const ButtonShortcut &a, const ButtonShortcut &b) {
    if (is_tap_slot(a) != is_tap_slot(b)) return false;
    if (is_tap_slot(a)) return a.trigger_a == b.trigger_a;
    return (a.trigger_a == b.trigger_a && a.trigger_b == b.trigger_b) ||
           (a.trigger_a == b.trigger_b && a.trigger_b == b.trigger_a);
}

// A single-press slot may only fire immediately when no other slot wants the
// double press of the same trigger; otherwise it has to wait the window out.
static bool has_double_tap_partner(uint8_t slot, const ButtonShortcut &shortcut) {
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        if (i == slot) continue;
        const auto &other = get_button().shortcuts[i];
        if (!shortcut_slot_valid(other)) continue;
        if (wants_double(other) && same_trigger(shortcut, other)) return true;
    }
    return false;
}

// Both interfaces emit an array field, so "nothing pressed" is an all-zero report.
static bool send_release(uint8_t instance) {
    if (instance == CONSUMER_INSTANCE) {
        uint16_t usage = 0;
        return tud_hid_n_report(CONSUMER_INSTANCE, 0, &usage, sizeof(usage));
    }
    return tud_hid_n_keyboard_report(KEYBOARD_INSTANCE, 0, 0, nullptr);
}

static bool send_keyboard(const ButtonShortcut &shortcut) {
    if (release_pending || !tud_hid_n_ready(KEYBOARD_INSTANCE)) return false;

    uint8_t keys[6]{};
    keys[0] = shortcut.keyboard.key;
    if (!tud_hid_n_keyboard_report(KEYBOARD_INSTANCE, 0,
                                   shortcut.keyboard.modifiers, keys)) return false;

    release_pending = true;
    release_instance = KEYBOARD_INSTANCE;
    release_time = make_timeout_time_ms(KEY_PRESS_MS);
    return true;
}

static bool send_consumer(const ButtonShortcut &shortcut) {
    if (release_pending || !tud_hid_n_ready(CONSUMER_INSTANCE)) return false;

    // Copy out of the packed struct before taking an address.
    uint16_t usage = shortcut.consumer.usage;
    if (!tud_hid_n_report(CONSUMER_INSTANCE, 0, &usage, sizeof(usage))) return false;

    release_pending = true;
    release_instance = CONSUMER_INSTANCE;
    release_time = make_timeout_time_ms(KEY_PRESS_MS);
    return true;
}

// Returns true when the slot's action should fire.
// A "guarded" slot is a tap whose button a configured chord also uses: it cannot
// judge a press while the button is down, because the chord's second button may
// still arrive, so it defers every decision to the release.
static bool tap_tick(uint8_t slot, const ButtonShortcut &shortcut, bool engaged,
                     bool guarded, bool chord_claimed) {
    const uint16_t bit = static_cast<uint16_t>(1u << slot);
    const bool edge = engaged && !(engaged_mask & bit);
    const bool want_double = wants_double(shortcut);

    if (chord_claimed) chord_claimed_mask |= bit;
    if (chord_claimed_mask & bit) {
        // The chord took this press; the tap slot sits out the rest of the hold.
        if (!engaged) {
            chord_claimed_mask &= static_cast<uint16_t>(~bit);
            tap_count[slot] = 0;
        }
        return false;
    }

    if (edge) {
        if (tap_count[slot] == 0) {
            if (!guarded && !want_double && !has_double_tap_partner(slot, shortcut)) {
                return true; // Nothing else claims the double press: fire right away.
            }
            tap_count[slot] = 1;
            tap_deadline[slot] = make_timeout_time_ms(SHORTCUT_TAP_WINDOW_MS);
            return false;
        }
        // Second press inside the window: the double-press slot fires, the
        // single-press slot on the same trigger stands down.
        if (guarded) {
            tap_count[slot] = 2; // Decided on release, in case the chord forms.
            return false;
        }
        tap_count[slot] = 0;
        return want_double;
    }

    if (guarded) {
        // Still held: a chord can still claim this press, so nothing is decided.
        if (engaged || tap_count[slot] == 0) return false;
        if (tap_count[slot] == 2) {
            tap_count[slot] = 0;
            return want_double; // Two presses, and no chord came of either.
        }
        if (!want_double && !has_double_tap_partner(slot, shortcut)) {
            tap_count[slot] = 0;
            return true; // Released without the chord forming, and nothing wants a second press.
        }
        if (time_reached(tap_deadline[slot])) {
            tap_count[slot] = 0;
            return !want_double; // The window closed on a lone press.
        }
        return false;
    }

    if (tap_count[slot] && time_reached(tap_deadline[slot])) {
        tap_count[slot] = 0;
        return !want_double; // The window closed on a lone press.
    }
    return false;
}

static void process_shortcuts(const USBGetStateData &state) {
    uint16_t new_engaged_mask = 0;
    uint16_t valid_mask = 0;
    uint32_t chord_buttons = 0; // Buttons some configured chord uses.
    uint32_t chord_held = 0; // Buttons of the chords held right now.
    bool engaged_now[BUTTON_SHORTCUT_COUNT]{};

    // Chords are resolved first: a tap slot sharing one of their buttons has to
    // know whether a chord took this press before it decides anything.
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const auto &shortcut = get_button().shortcuts[i];
        if (!shortcut_slot_valid(shortcut)) continue;

        const uint16_t bit = static_cast<uint16_t>(1u << i);
        valid_mask |= bit;

        bool engaged = button_is_pressed(state, shortcut.trigger_a);
        if (!is_tap_slot(shortcut)) {
            const uint32_t buttons =
                    button_bit(shortcut.trigger_a) | button_bit(shortcut.trigger_b);
            chord_buttons |= buttons;
            engaged = engaged && button_is_pressed(state, shortcut.trigger_b);
            if (engaged) chord_held |= buttons;
        }
        engaged_now[i] = engaged;
        if (engaged) new_engaged_mask |= bit;
    }

    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << i);
        if (!(valid_mask & bit)) continue;

        const auto &shortcut = get_button().shortcuts[i];
        // The chord wins over a lone tap on either of its buttons.
        const bool guarded = is_tap_slot(shortcut) &&
                             (chord_buttons & button_bit(shortcut.trigger_a));
        const bool claimed = guarded && (chord_held & button_bit(shortcut.trigger_a));

        if (tap_tick(i, shortcut, engaged_now[i], guarded, claimed)) pending_mask |= bit;
    }
    engaged_mask = new_engaged_mask;
    // A slot reconfigured out from under a pending action never gets to fire it.
    pending_mask &= valid_mask;
    chord_claimed_mask &= valid_mask;

    if (!pending_mask) return;
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << i);
        if (!(pending_mask & bit)) continue;
        const auto &shortcut = get_button().shortcuts[i];
        bool handled = false;
        switch (shortcut.action) {
            case ShortcutActionKeyboard:
                handled = send_keyboard(shortcut);
                break;
            case ShortcutActionConsumer:
                handled = send_consumer(shortcut);
                break;
            case ShortcutActionBtDisconnect:
                handled = bt_disconnect();
                break;
        }
        if (handled) {
            pending_mask &= static_cast<uint16_t>(~bit);
            // Send at most one action per input report.
            break;
        }
    }
}

void button_shortcut_reset() {
    if (release_pending && tud_hid_n_ready(release_instance)) {
        send_release(release_instance);
    }
    release_pending = false;
    release_instance = KEYBOARD_INSTANCE;
    release_time = nil_time;
    pending_mask = 0;
    engaged_mask = 0;
    chord_claimed_mask = 0;
    for (uint8_t i = 0; i < BUTTON_SHORTCUT_COUNT; ++i) {
        tap_count[i] = 0;
        tap_deadline[i] = nil_time;
    }
}

void button_shortcut_tick(const USBGetStateData &state) {
    // Let the held key go before process_shortcuts() looks for the next action.
    if (release_pending && time_reached(release_time) &&
        tud_hid_n_ready(release_instance) && send_release(release_instance)) {
        release_pending = false;
    }
    process_shortcuts(state);
}

#endif // ENABLE_WAKE_HID
