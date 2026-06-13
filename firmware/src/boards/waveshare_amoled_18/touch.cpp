#include "../../hal/touch_hal.h"
#include "board.h"
#include "board_i2c.h"
#include "board_rev.h"
#include <Arduino.h>
#include "driver/i2c_master.h"
#include "esp32-hal-i2c.h"

// Minimal capacitive-touch reader. Both shipping panel revisions expose the
// FocalTech-style touch-data layout, so one read path serves both — only the
// I2C address differs (FT3168 @ 0x38 vs CST816 @ 0x15), chosen from board_rev().
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static uint8_t           touch_addr = FT3168_ADDR;
static i2c_master_dev_handle_t touch_dev = nullptr;
static uint32_t          last_touch_poll_ms = 0;

// Some panel revisions pulse INT on touch-down but not reliably on release.
// Once pressed, poll briefly until the controller reports zero fingers so
// LVGL cannot remain stuck in a permanent PRESSED state.
static const uint32_t TOUCH_POLL_MS = 20;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static bool touch_write(uint8_t reg, uint8_t value) {
    if (!touch_dev) return false;
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(touch_dev, data, sizeof(data), 20) == ESP_OK;
}

static bool touch_read(uint8_t reg, uint8_t* data, size_t len) {
    if (!touch_dev) return false;
    return i2c_master_transmit_receive(touch_dev, &reg, 1, data, len, 20) == ESP_OK;
}

static void touch_read_into_shared_state(void) {
    uint8_t point[5];
    if (!touch_read(0x02, point, sizeof(point))) {
        touch_pressed = false;
        return;
    }
    uint8_t fingers = point[0] & 0x0F;
    uint8_t xH = point[1];
    uint8_t xL = point[2];
    uint8_t yH = point[3];
    uint8_t yL = point[4];
    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
}

void touch_hal_init(void) {
    bool is_cst816 = (board_rev() == REV_CO5300_CST816);
    touch_addr = is_cst816 ? CST816_ADDR : FT3168_ADDR;

    i2c_master_bus_handle_t bus =
        static_cast<i2c_master_bus_handle_t>(i2cBusHandle(1));
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = touch_addr;
    config.scl_speed_hz = 100000;
    if (!bus || i2c_master_bus_add_device(bus, &config, &touch_dev) != ESP_OK) {
        Serial.printf("Touch bus handle failed (addr 0x%02X)\n", touch_addr);
        return;
    }

    if (!is_cst816) {
        // FT3168 power-mode register 0xA5 = 0x00: active scanning.
        // Register 0xA4 = 0x01 enables trigger mode on the INT pin; its
        // reset default is polling mode, which never produces a GPIO edge.
        touch_write(0xA5, 0x00);
        touch_write(0xA4, 0x01);
    }

    // Verify the controller answers. FT3168 chip-id is reg 0xA0; CST816 reg 0xA7.
    uint8_t id_reg = is_cst816 ? 0xA7 : 0xA0;
    uint8_t id = 0;
    if (touch_read(id_reg, &id, 1)) {
        Serial.printf("Touch %s ID=0x%02X (addr 0x%02X)\n",
                      is_cst816 ? "CST816" : "FT3168", id, touch_addr);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", touch_addr);
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    uint32_t now = millis();
    // Some FT3168 panel batches do not drive INT even in trigger mode.
    // Polling is safe while the boot-time audio codec remains dormant.
    if (touch_data_ready || now - last_touch_poll_ms >= TOUCH_POLL_MS) {
        touch_data_ready = false;
        last_touch_poll_ms = now;
        touch_read_into_shared_state();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
