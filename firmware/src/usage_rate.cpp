#include "usage_rate.h"
#include <Arduino.h>

// Thresholds in %/min. A 5-hour (300 min) session ÷ 100% = 0.33 %/min to fill
// exactly at the same pace as the session itself resets — the user wants the
// "heavy" tier to start right there (filling in 4–5 hours).
//   < 0.10  →  Idle    (17h+ to fill, basically dormant)
//   < 0.20  →  Normal  (8–17h to fill, slow steady use)
//   < 0.33  →  Active  (5–8h, heavy but not yet pace-matching)
//   >=0.33  →  Heavy   (≤5h, matching or beating the session reset)
#define RATE_THRESH_NORMAL  0.10f
#define RATE_THRESH_ACTIVE  0.20f
#define RATE_THRESH_HEAVY   0.33f

// Minimum span between oldest and newest sample before we trust the computed
// rate. The whole point of the ring buffer is to smooth out single-sample
// jitter — at 60s daemon polling, a 1% bump between two consecutive samples
// looks like 1 %/min (Heavy) but really just means you grew 1% in the last
// minute. We require ~4 min of accumulated history so the rate reflects a
// real trend, not one noisy delta. Side-effect: ~4 min warm-up after boot
// during which we report Idle.
#define MIN_WINDOW_MS       240000UL

#define RING_SIZE 6

struct Sample { uint32_t ms; float pct; };

struct RateTracker {
    Sample ring[RING_SIZE];
    uint8_t count;
    uint8_t head;
};

static RateTracker claude_tracker = {};
static RateTracker codex_tracker = {};

static inline uint8_t oldest_idx(const RateTracker& tracker) {
    return (tracker.head + RING_SIZE - tracker.count) % RING_SIZE;
}

static void tracker_reset(RateTracker& tracker) {
    tracker.count = 0;
    tracker.head = 0;
}

static void tracker_sample(RateTracker& tracker, float session_pct) {
    uint32_t now = millis();

    if (tracker.count > 0) {
        uint8_t latest = (tracker.head + RING_SIZE - 1) % RING_SIZE;
        // Session reset: pct dropped substantially. Restart tracking.
        if (session_pct + 5.0f < tracker.ring[latest].pct) {
            tracker_reset(tracker);
        }
    }

    tracker.ring[tracker.head] = { now, session_pct };
    tracker.head = (tracker.head + 1) % RING_SIZE;
    if (tracker.count < RING_SIZE) tracker.count++;
}

static int tracker_group(const RateTracker& tracker) {
    if (tracker.count < 2) return 0;

    uint8_t o = oldest_idx(tracker);
    uint8_t l = (tracker.head + RING_SIZE - 1) % RING_SIZE;
    uint32_t dt = tracker.ring[l].ms - tracker.ring[o].ms;
    if (dt < MIN_WINDOW_MS) return 0;

    float dp = tracker.ring[l].pct - tracker.ring[o].pct;
    if (dp < 0.0f) dp = 0.0f;
    float rate = dp * 60000.0f / (float)dt;

    if (rate < RATE_THRESH_NORMAL) return 0;
    if (rate < RATE_THRESH_ACTIVE) return 1;
    if (rate < RATE_THRESH_HEAVY)  return 2;
    return 3;
}

void usage_rate_sample(float session_pct) {
    tracker_sample(claude_tracker, session_pct);
}

void usage_rate_sample_codex(float session_pct) {
    tracker_sample(codex_tracker, session_pct);
}

int usage_rate_group(void) {
    return tracker_group(claude_tracker);
}

int usage_rate_group_codex(void) {
    return tracker_group(codex_tracker);
}
