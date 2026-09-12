//
// Created by Codex on 2026/7/7.
//

#include "debug.h"

#if ENABLE_DEBUG

#include <cstdio>
#include <cstring>

#include "pico/time.h"
#include "usb.h"

static constexpr uint32_t DEBUG_STACK_CANARY = 0xA5A5A5A5u;
static constexpr uint64_t DEBUG_STACK_LOG_PERIOD_US = 5'000'000;

static uint32_t *debug_core1_stack = nullptr;
static uint32_t debug_core1_stack_words = 0;
static bool debug_core1_stack_watermark_active = false;

void debug_fill_core1_stack_watermark(uint32_t *stack, uint32_t word_count) {
    if (stack == nullptr || word_count == 0) {
        return;
    }

    for (uint32_t i = 0; i < word_count; i++) {
        stack[i] = DEBUG_STACK_CANARY;
    }

    debug_core1_stack = stack;
    debug_core1_stack_words = word_count;
    debug_core1_stack_watermark_active = true;
}

static uint32_t debug_core1_stack_used_bytes() {
    if (!debug_core1_stack_watermark_active) {
        return 0;
    }

    const volatile uint32_t *stack = debug_core1_stack;
    uint32_t unused_words = 0;
    while (unused_words < debug_core1_stack_words && stack[unused_words] == DEBUG_STACK_CANARY) {
        unused_words++;
    }

    return (debug_core1_stack_words - unused_words) * sizeof(debug_core1_stack[0]);
}

void debug_log_core1_stack_usage() {
    if (!debug_core1_stack_watermark_active) {
        return;
    }

    static uint64_t next_log_us = 0;
    const uint64_t now = time_us_64();
    if (now < next_log_us) {
        return;
    }
    next_log_us = now + DEBUG_STACK_LOG_PERIOD_US;

    const uint32_t used = debug_core1_stack_used_bytes();
    const uint32_t total = debug_core1_stack_words * sizeof(debug_core1_stack[0]);
    printf("[Audio] core1 stack used %lu / %lu bytes, free %lu bytes\n",
           static_cast<unsigned long>(used),
           static_cast<unsigned long>(total),
           static_cast<unsigned long>(total - used));
}

static constexpr uint64_t DEBUG_BT_LOG_PERIOD_US = 2'000'000;

void debug_bt_report_arrival() {
    const uint64_t now = time_us_64();

    static uint64_t last_us = 0;
    static uint64_t window_start_us = 0;
    static uint32_t count = 0;
    static uint32_t min_us = UINT32_MAX;
    static uint32_t max_us = 0;
    static uint64_t sum_us = 0;

    if (last_us != 0) {
        const uint32_t delta = static_cast<uint32_t>(now - last_us);
        if (delta < min_us) min_us = delta;
        if (delta > max_us) max_us = delta;
        sum_us += delta;
        count++;
    } else {
        window_start_us = now;
    }
    last_us = now;

    if (now - window_start_us >= DEBUG_BT_LOG_PERIOD_US && count > 0) {
        const uint32_t mean = static_cast<uint32_t>(sum_us / count);
        const uint32_t hz = mean ? (1'000'000u / mean) : 0;
        // reports: input reports seen this window; mean/min/max: inter-report
        // interval in us; hz: 1/mean; jitter: max-min spread.
        printf("[BT] reports=%lu mean=%luus (%luHz) min=%luus max=%luus jitter=%luus\n",
               static_cast<unsigned long>(count),
               static_cast<unsigned long>(mean),
               static_cast<unsigned long>(hz),
               static_cast<unsigned long>(min_us),
               static_cast<unsigned long>(max_us),
               static_cast<unsigned long>(max_us - min_us));
        count = 0;
        sum_us = 0;
        min_us = UINT32_MAX;
        max_us = 0;
        window_start_us = now;
    }
}

// ---------------------------------------------------------------------------
// Polling-rate instrumentation (see debug.h)
// ---------------------------------------------------------------------------
static constexpr uint64_t DEBUG_USB_LOG_PERIOD_US = 2'000'000;
static constexpr uint64_t DEBUG_SENSOR_LOG_PERIOD_US = 2'000'000;

// Written by the BT callback, read by the USB send path. Both run on core0
// (core1 is the audio core), so plain statics are sufficient here.
static uint32_t usb_bt_seq = 0;        // bumped once per staged controller frame
static uint64_t usb_bt_staged_us = 0;  // when the newest frame was staged
static bool     usb_bt_pending = false; // staged frame not yet delivered
static uint32_t usb_dropped = 0;       // frames replaced before delivery
static uint64_t usb_inflight_data_us = 0; // BT sample carried by the queued report
static bool     usb_inflight_fresh = false; // did that report carry NEW data?

// ---------------------------------------------------------------------------
// Delivered staleness (the metric that actually matters)
// ---------------------------------------------------------------------------
static constexpr uint64_t DEBUG_DLV_LOG_PERIOD_US = 2'000'000;
// A "late" interval means a poll slot produced nothing -- a cadence gap, which
// is exactly what the smoothed mode exists to avoid. Threshold is relative to
// the measured poll period so this is meaningful at any bInterval.
static constexpr uint32_t DLV_LATE_FALLBACK_US = 1500;

void debug_usb_report_delivered(uint64_t now_us) {
    static uint64_t window_start_us = 0;
    static uint64_t last_delivery_us = 0;
    static uint32_t reports = 0;
    static uint32_t fresh_reports = 0;
    static uint64_t stale_sum_us = 0;
    static uint32_t stale_min_us = UINT32_MAX;
    static uint32_t stale_max_us = 0;
    static uint64_t fresh_stale_sum_us = 0;
    static uint32_t fresh_stale_min_us = UINT32_MAX;
    static uint32_t fresh_stale_max_us = 0;
    static uint64_t poll_sum_us = 0;
    static uint32_t poll_min_us = UINT32_MAX;
    static uint32_t poll_max_us = 0;
    static uint32_t poll_n = 0;
    static uint32_t late = 0;

    if (window_start_us == 0) {
        window_start_us = now_us;
    }

    reports++;

    if (usb_inflight_data_us != 0 && now_us >= usb_inflight_data_us) {
        const uint32_t stale = static_cast<uint32_t>(now_us - usb_inflight_data_us);
        stale_sum_us += stale;
        if (stale < stale_min_us) stale_min_us = stale;
        if (stale > stale_max_us) stale_max_us = stale;

        // Padding repeats a sample the host already has, so its age is not a
        // latency anyone experiences. Only reports carrying NEW data matter.
        if (usb_inflight_fresh) {
            fresh_reports++;
            fresh_stale_sum_us += stale;
            if (stale < fresh_stale_min_us) fresh_stale_min_us = stale;
            if (stale > fresh_stale_max_us) fresh_stale_max_us = stale;
        }
    }

    if (last_delivery_us != 0) {
        const uint32_t poll = static_cast<uint32_t>(now_us - last_delivery_us);
        const uint32_t period = usb_hid_poll_period_us();
        const uint32_t late_us = period ? period + period / 2u : DLV_LATE_FALLBACK_US;
        poll_sum_us += poll;
        if (poll < poll_min_us) poll_min_us = poll;
        if (poll > poll_max_us) poll_max_us = poll;
        poll_n++;
        if (poll >= late_us) late++;
    }
    last_delivery_us = now_us;

    if (now_us - window_start_us < DEBUG_DLV_LOG_PERIOD_US) {
        return;
    }

    printf("[DLV] reports=%lu fstale mean=%luus min=%luus max=%luus | stale mean=%luus | poll mean=%luus min=%luus max=%luus late=%lu period=%luus\n",
           static_cast<unsigned long>(reports),
           static_cast<unsigned long>(fresh_reports ? fresh_stale_sum_us / fresh_reports : 0),
           static_cast<unsigned long>(fresh_stale_min_us == UINT32_MAX ? 0 : fresh_stale_min_us),
           static_cast<unsigned long>(fresh_stale_max_us),
           static_cast<unsigned long>(reports ? stale_sum_us / reports : 0),
           static_cast<unsigned long>(poll_n ? poll_sum_us / poll_n : 0),
           static_cast<unsigned long>(poll_min_us == UINT32_MAX ? 0 : poll_min_us),
           static_cast<unsigned long>(poll_max_us),
           static_cast<unsigned long>(late),
           static_cast<unsigned long>(usb_hid_poll_period_us()));

    reports = 0;
    fresh_reports = 0;
    stale_sum_us = 0;
    stale_min_us = UINT32_MAX;
    stale_max_us = 0;
    fresh_stale_sum_us = 0;
    fresh_stale_min_us = UINT32_MAX;
    fresh_stale_max_us = 0;
    poll_sum_us = 0;
    poll_min_us = UINT32_MAX;
    poll_max_us = 0;
    poll_n = 0;
    late = 0;
    window_start_us = now_us;
}

// Offset of uint32_t SensorTimestamp within USBGetStateData (see utils.h).
static constexpr uint16_t SENSOR_TIMESTAMP_OFFSET = 27;

// ---------------------------------------------------------------------------
// SensorTimestamp gap distribution
// ---------------------------------------------------------------------------
// Deliberately assumption-free: raw microsecond buckets, so the shape of the
// distribution speaks for itself (single cluster / bimodal / smeared). An
// earlier version bucketed by multiples of the smallest observed gap to test
// whether the controller samples on a fixed grid -- it does not, so that
// framing was removed rather than left to mislead.
//
// Gaps at or beyond DT_STALL_US are also timed against each other, because the
// observed outliers repeat to the tick (41374/41375, 22568), which points at a
// periodic stall rather than random interference.
static constexpr uint32_t DT_BUCKET_COUNT = 10;
static const uint32_t dt_bucket_edge_us[DT_BUCKET_COUNT - 1] = {
    800, 1000, 1200, 1400, 1600, 2000, 3000, 5000, 10000
};
static uint32_t dt_bucket[DT_BUCKET_COUNT] = {0};

// Measured timestamp unit, refined from the previous window (seeded at the
// observed ~3 ticks/us) so bucket edges stay in real microseconds without
// hardcoding the controller's clock rate.
static uint32_t dt_ticks_per_us_x100 = 300;

static constexpr uint32_t DT_STALL_US = 10000;
static uint32_t dt_stall_count = 0;
static uint64_t dt_stall_last_us = 0; // persists across windows
static uint32_t dt_stall_gap_n = 0;
static uint64_t dt_stall_gap_sum_ms = 0;
static uint32_t dt_stall_gap_min_ms = UINT32_MAX;
static uint32_t dt_stall_gap_max_ms = 0;

static void dt_histogram_add(uint32_t dt, uint64_t now_us) {
    if (dt == 0 || dt_ticks_per_us_x100 == 0) {
        return;
    }

    // 64-bit intermediate: a timestamp wraparound would overflow 32 bits here.
    const uint32_t dt_us =
        static_cast<uint32_t>((static_cast<uint64_t>(dt) * 100u) / dt_ticks_per_us_x100);

    uint32_t i = 0;
    while (i < DT_BUCKET_COUNT - 1 && dt_us >= dt_bucket_edge_us[i]) {
        i++;
    }
    dt_bucket[i]++;

    if (dt_us < DT_STALL_US) {
        return;
    }

    dt_stall_count++;
    if (dt_stall_last_us != 0) {
        const uint32_t gap_ms = static_cast<uint32_t>((now_us - dt_stall_last_us) / 1000u);
        dt_stall_gap_sum_ms += gap_ms;
        if (gap_ms < dt_stall_gap_min_ms) dt_stall_gap_min_ms = gap_ms;
        if (gap_ms > dt_stall_gap_max_ms) dt_stall_gap_max_ms = gap_ms;
        dt_stall_gap_n++;
    }
    dt_stall_last_us = now_us;
}

// Does every BT frame actually carry a new sensor sample, or does the
// controller repeat itself? This is what decides whether a lower polling mode
// discards real information or merely redundant copies.
static void debug_note_sensor_timestamp(const uint8_t *state, uint16_t len) {
    if (state == nullptr || len < SENSOR_TIMESTAMP_OFFSET + sizeof(uint32_t)) {
        return;
    }

    uint32_t ts = 0;
    memcpy(&ts, state + SENSOR_TIMESTAMP_OFFSET, sizeof(ts)); // unaligned-safe

    const uint64_t now = time_us_64();

    static uint64_t window_start_us = 0;
    static bool     has_last = false;
    static uint32_t last_ts = 0;
    static uint32_t frames = 0;
    static uint32_t unique = 0;
    static uint64_t dt_sum = 0;
    static uint32_t dt_min = UINT32_MAX;
    static uint32_t dt_max = 0;

    if (window_start_us == 0) {
        window_start_us = now;
    }

    frames++;

    if (has_last && ts != last_ts) {
        unique++;
        const uint32_t dt = ts - last_ts; // unsigned wraparound is well defined
        dt_sum += dt;
        if (dt < dt_min) dt_min = dt;
        if (dt > dt_max) dt_max = dt;
        dt_histogram_add(dt, now);
    }
    if (!has_last) {
        has_last = true;
        unique++;
    }
    last_ts = ts;

    const uint64_t elapsed_us = now - window_start_us;
    if (elapsed_us < DEBUG_SENSOR_LOG_PERIOD_US) {
        return;
    }

    const uint32_t elapsed_ms = static_cast<uint32_t>(elapsed_us / 1000);
    const uint32_t unique_hz = elapsed_ms ? (unique * 1000u / elapsed_ms) : 0;
    const uint32_t dt_mean = unique ? static_cast<uint32_t>(dt_sum / unique) : 0;
    // Self-calibrate the timestamp unit rather than assuming one: total ticks
    // elapsed over wall-clock microseconds, in hundredths.
    const uint32_t ticks_per_us_x100 =
        elapsed_us ? static_cast<uint32_t>((dt_sum * 100u) / elapsed_us) : 0;

    // frames: BT frames staged. unique: frames whose SensorTimestamp advanced.
    // repeat: frames carrying a stale sensor sample. dt: raw tick delta between
    // distinct samples. ticks/us: measured timestamp unit.
    printf("[SENSOR] frames=%lu unique=%lu (%luHz) repeat=%lu dt mean=%lu min=%lu max=%lu ticks (%lu.%02lu ticks/us)\n",
           static_cast<unsigned long>(frames),
           static_cast<unsigned long>(unique),
           static_cast<unsigned long>(unique_hz),
           static_cast<unsigned long>(frames - unique),
           static_cast<unsigned long>(dt_mean),
           static_cast<unsigned long>(unique ? dt_min : 0),
           static_cast<unsigned long>(dt_max),
           static_cast<unsigned long>(ticks_per_us_x100 / 100),
           static_cast<unsigned long>(ticks_per_us_x100 % 100));

    // Raw gap distribution in microseconds -- no assumed structure. A single
    // tight cluster means steady sampling; a second cluster at the low end means
    // delivery bunching (retransmits arriving back to back).
    printf("[DT] us <800=%lu 800=%lu 1.0k=%lu 1.2k=%lu 1.4k=%lu 1.6k=%lu 2k=%lu 3k=%lu 5k=%lu 10k+=%lu\n",
           static_cast<unsigned long>(dt_bucket[0]),
           static_cast<unsigned long>(dt_bucket[1]),
           static_cast<unsigned long>(dt_bucket[2]),
           static_cast<unsigned long>(dt_bucket[3]),
           static_cast<unsigned long>(dt_bucket[4]),
           static_cast<unsigned long>(dt_bucket[5]),
           static_cast<unsigned long>(dt_bucket[6]),
           static_cast<unsigned long>(dt_bucket[7]),
           static_cast<unsigned long>(dt_bucket[8]),
           static_cast<unsigned long>(dt_bucket[9]));

    // Spacing between >=10 ms gaps. A tight mean/min/max means the stalls are
    // periodic (something with a fixed duty cycle); a wide spread means they are
    // sporadic, e.g. interference.
    if (dt_stall_gap_n > 0) {
        printf("[STALL] count=%lu gap mean=%lums min=%lums max=%lums\n",
               static_cast<unsigned long>(dt_stall_count),
               static_cast<unsigned long>(dt_stall_gap_sum_ms / dt_stall_gap_n),
               static_cast<unsigned long>(dt_stall_gap_min_ms),
               static_cast<unsigned long>(dt_stall_gap_max_ms));
    } else if (dt_stall_count > 0) {
        printf("[STALL] count=%lu (need >=2 to measure spacing)\n",
               static_cast<unsigned long>(dt_stall_count));
    }

    // Refine the timestamp unit for next window's bucket edges.
    if (ticks_per_us_x100 > 0) {
        dt_ticks_per_us_x100 = ticks_per_us_x100;
    }
    for (uint32_t i = 0; i < DT_BUCKET_COUNT; i++) {
        dt_bucket[i] = 0;
    }
    dt_stall_count = 0;
    dt_stall_gap_n = 0;
    dt_stall_gap_sum_ms = 0;
    dt_stall_gap_min_ms = UINT32_MAX;
    dt_stall_gap_max_ms = 0;

    frames = 0;
    unique = 0;
    dt_sum = 0;
    dt_min = UINT32_MAX;
    dt_max = 0;
    window_start_us = now;
}

void debug_note_bt_frame_staged(const uint8_t *state, uint16_t len) {
    // Still pending means the previous frame was overwritten before any USB
    // poll picked it up -- the host never saw it. This is the metric that
    // distinguishes "slower" from "actually lossy" polling modes.
    if (usb_bt_pending) {
        usb_dropped++;
    }
    usb_bt_seq++;
    usb_bt_staged_us = time_us_64();
    usb_bt_pending = true;

    debug_note_sensor_timestamp(state, len);
}
void debug_usb_report_sent() {
    const uint64_t now = time_us_64();

    static uint64_t window_start_us = 0;
    static uint32_t last_seq = 0;
    static uint32_t sends = 0;
    static uint32_t fresh = 0;
    static uint64_t lat_sum_us = 0;
    static uint32_t lat_min_us = UINT32_MAX;
    static uint32_t lat_max_us = 0;

    if (window_start_us == 0) {
        window_start_us = now;
    }

    sends++;

    // Freshness is derived from the staged-frame sequence rather than the send
    // path's own state, so this works identically in every polling mode.
    const bool is_fresh = (usb_bt_seq != last_seq);
    if (is_fresh) {
        last_seq = usb_bt_seq;
        usb_bt_pending = false;
        fresh++;

        const uint32_t lat = static_cast<uint32_t>(now - usb_bt_staged_us);
        lat_sum_us += lat;
        if (lat < lat_min_us) lat_min_us = lat;
        if (lat > lat_max_us) lat_max_us = lat;
    }

    // Remember which BT sample this queued report carries, so the completion
    // callback can age it at the moment the host collects it.
    usb_inflight_data_us = usb_bt_staged_us;
    usb_inflight_fresh = is_fresh;

    const uint64_t elapsed_us = now - window_start_us;
    if (elapsed_us < DEBUG_USB_LOG_PERIOD_US) {
        return;
    }

    const uint32_t elapsed_ms = static_cast<uint32_t>(elapsed_us / 1000);
    const uint32_t send_hz = elapsed_ms ? (sends * 1000u / elapsed_ms) : 0;
    const uint32_t fresh_hz = elapsed_ms ? (fresh * 1000u / elapsed_ms) : 0;
    const uint32_t lat_mean = fresh ? static_cast<uint32_t>(lat_sum_us / fresh) : 0;

    // sends: raw poll rate. fresh: reports carrying new data (the real ceiling
    // is the BT arrival rate -- compare against the [BT] line). dup: padding.
    // dropped: BT frames the host never saw. lat: BT arrival -> USB handoff.
    printf("[USB] sends=%lu (%luHz) fresh=%lu (%luHz) dup=%lu dropped=%lu lat mean=%luus min=%luus max=%luus\n",
           static_cast<unsigned long>(sends),
           static_cast<unsigned long>(send_hz),
           static_cast<unsigned long>(fresh),
           static_cast<unsigned long>(fresh_hz),
           static_cast<unsigned long>(sends - fresh),
           static_cast<unsigned long>(usb_dropped),
           static_cast<unsigned long>(lat_mean),
           static_cast<unsigned long>(fresh ? lat_min_us : 0),
           static_cast<unsigned long>(lat_max_us));

    sends = 0;
    fresh = 0;
    usb_dropped = 0;
    lat_sum_us = 0;
    lat_min_us = UINT32_MAX;
    lat_max_us = 0;
    window_start_us = now;
}

#endif
