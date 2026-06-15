#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// AMOLED-1.8 has only the BOOT button as a secondary input — the PWR
// button comes through power_hal (XCA9554 EXIO4). No secondary button.

static bool primary_state = false;
static bool primary_candidate = false;
static uint32_t primary_candidate_since = 0;

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
    primary_state = digitalRead(BTN_BACK_GPIO) == LOW;
    primary_candidate = primary_state;
    primary_candidate_since = millis();
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY: {
        bool raw = digitalRead(BTN_BACK_GPIO) == LOW;
        if (raw != primary_candidate) {
            primary_candidate = raw;
            primary_candidate_since = millis();
        } else if (raw != primary_state && millis() - primary_candidate_since >= 25) {
            primary_state = raw;
        }
        return primary_state;
    }
    case INPUT_BTN_SECONDARY:
        return false;   // not present on this board
    }
    return false;
}
