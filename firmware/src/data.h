#pragma once
#include <Arduino.h>

struct ServiceUsage {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets; 0 = N/A (hide)
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets; 0 = N/A (hide)
    bool ok;                 // source available
    bool present;            // key was present in svc object
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100) — Claude / legacy top-level
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    bool has_svc;            // true when optional "svc" object was present
    ServiceUsage cl;         // Claude Code  (svc.cl)
    ServiceUsage cx;         // Codex        (svc.cx)
    ServiceUsage cu;         // Cursor       (svc.cu)
};
