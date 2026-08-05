#pragma once
// AXP192 board-support for the M5StickC (ADR-020 / ADR-022 / ADR-023).
//
// The M5StickC has no ADC battery divider and no fuel gauge: the cell is behind
// an AXP192 PMIC on the StickC's INTERNAL I2C bus (G21/G22, addr 0x34), which is
// also what powers the ESP32, the LCD, and the Grove port's 5 V pin. This header
// is the M5StickC's equivalent of the Feather V2's logger/i2c_power.h — a small
// board-support shim called from on_boot, with no third-party dependency.
//
// Why this file has to exist: the AXP192 is NOT a core ESPHome component (there
// is no esphome/components/axp192 upstream), and ESPHome never calls M5's
// Axp.begin(). So on a StickC running ESPHome the PMIC sits in its power-on
// default state: it powers the ESP32 (obviously — we are running), but its ADCs
// are DISABLED, so the battery-voltage registers read zeros until we enable
// them. axp192_init() does that, and takes the two other actions this headless,
// battery-powered design needs.
//
// WHY THE TRANSPORT IS BIT-BANGED (ADR-023). This file used to take an
// esphome::i2c::I2CBus* for a third `bus_axp` hardware bus. That does not fit:
// the ESP32 (PICO-D4) has exactly TWO I2C peripherals, and ESPHome enforces it
// (components/i2c/__init__.py — VARIANT_ESP32: {"NUM": 2}). Both are spoken for
// by the two temperature sensors, which sit on two different physical
// connectors (Grove G32/G33 and the HAT header G0/G26) and therefore cannot
// share one. The PMIC is the right device to move off hardware I2C: it is read
// ONCE PER WAKE, at low speed, with no timing constraints and no other master on
// the line — whereas the sensors are ESPHome components that require a real bus.
// So G21/G22 are driven here as plain open-drain GPIOs.
//
// Register map (AXP192 datasheet + M5StickC's own AXP192.cpp begin() sequence):
//
//   0x12  DC-DC / LDO enable switches. Bit assignments:
//           bit6 EXTEN   bit4 DC-DC2  bit3 LDO3  bit2 LDO2  bit1 DC-DC3  bit0 DC-DC1
//         On the StickC: LDO2 = LCD backlight, LDO3 = LCD panel, DC-DC1 = the
//         ESP32's own 3.3 V rail, EXTEN = the 5 V boost feeding the Grove port.
//   0x28  LDO2/LDO3 output voltage. High nibble = LDO2 (LCD backlight), low
//         nibble = LDO3 (LCD panel); volts = 1.8 + 0.1 * nibble.
//   0x78  Battery voltage ADC, high 8 bits
//   0x79  Battery voltage ADC, low 4 bits (in bits 3:0) — 12-bit total, 1.1 mV/LSB
//   0x82  ADC enable 1. 0xFF = enable all (incl. the battery-voltage ADC we need).
#include <cmath>
#include <cstdint>

constexpr uint8_t AXP192_ADDR = 0x34;

constexpr uint8_t AXP192_REG_DCDC_LDO_EN = 0x12;
constexpr uint8_t AXP192_REG_LDO23_V = 0x28;
constexpr uint8_t AXP192_REG_BATT_V_HI = 0x78;
constexpr uint8_t AXP192_REG_BATT_V_LO = 0x79;
constexpr uint8_t AXP192_REG_ADC_EN1 = 0x82;

// 0x12 bit masks we touch. EXTEN is forced ON because it gates the 5 V boost that
// powers the Grove port, and the ENV II Unit carrying temp_a hangs off that port.
// LDO2|LDO3 are the TFT rails: no longer forced off at boot (ADR-026 supersedes
// the display half of ADR-002) — axp192_init() leaves them as found, and
// axp192_lcd_on()/axp192_lcd_off() drive them for the brief boot/button status
// screens. "Display off" is now enforced at sleep via axp192_lcd_off(), so all
// sampling wakes stay dark. Every other bit — critically DC-DC1, the ESP32's own
// supply — is left exactly as found.
constexpr uint8_t AXP192_LCD_RAILS = 0x0C;  // LDO3 | LDO2
constexpr uint8_t AXP192_EXTEN = 0x40;
constexpr uint8_t AXP192_DCDC1 = 0x01;      // the ESP32's own 3.3 V rail

// LDO2 (backlight) + LDO3 (panel) both to 3.0 V: each nibble 0xC → 1.8 + 0.1*12.
// Confirm/tune on hardware (M5's own begin() writes this register). Set once by
// axp192_lcd_on() before the rails are enabled.
constexpr uint8_t AXP192_LDO23_3V0 = 0xCC;

// The StickC's internal PMIC bus. Fixed by the board — these are not a choice.
constexpr int AXP192_PIN_SDA = 21;
constexpr int AXP192_PIN_SCL = 22;

// --- Pin-level HAL --------------------------------------------------------
// Six primitives, the only part of this file that touches hardware. Open-drain
// discipline throughout: "release" means stop driving and let the board's
// pull-up raise the line, never drive it high — two masters or a stuck slave
// would otherwise see a short.
//
// A host test defines AXP192_TEST_HAL and supplies its own six primitives
// BEFORE including this header, which lets the protocol below (start/stop/
// byte/ack framing, the register reads, the rail arithmetic) be unit-tested
// against a simulated AXP192 with no hardware. See test/test_axp192.cpp.
#ifndef AXP192_TEST_HAL

#include "driver/gpio.h"
#include "esp_rom_sys.h"

static inline void axp_pins_config() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << AXP192_PIN_SDA) | (1ULL << AXP192_PIN_SCL);
  // INPUT_OUTPUT_OD: we can drive low and still read the line back, which is
  // what ACK sampling and clock-stretch detection need.
  cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;   // belt and braces; the board fits its own
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

static inline void axp_sda_low() { gpio_set_level((gpio_num_t) AXP192_PIN_SDA, 0); }
static inline void axp_sda_release() { gpio_set_level((gpio_num_t) AXP192_PIN_SDA, 1); }
static inline void axp_scl_low() { gpio_set_level((gpio_num_t) AXP192_PIN_SCL, 0); }
static inline void axp_scl_release() { gpio_set_level((gpio_num_t) AXP192_PIN_SCL, 1); }
static inline bool axp_sda_read() { return gpio_get_level((gpio_num_t) AXP192_PIN_SDA) != 0; }
static inline bool axp_scl_read() { return gpio_get_level((gpio_num_t) AXP192_PIN_SCL) != 0; }
static inline void axp_delay_us(uint32_t us) { esp_rom_delay_us(us); }

#endif  // AXP192_TEST_HAL

// --- Bit-banged I2C master ------------------------------------------------
// ~100 kHz (5 µs half-period). The AXP192 handles 400 kHz, but nothing here is
// throughput-bound — two register reads per wake — so the slower, more
// forgiving edge rate is free.
constexpr uint32_t AXP192_HALF_PERIOD_US = 5;

// Release SCL and wait for it to actually read high, so a slave holding the
// clock down (stretching) is honoured rather than clocked over. The AXP192 does
// not stretch; this is cheap insurance against a wedged bus. Returns false on
// timeout, which propagates all the way out as a NaN reading.
static inline bool axp_scl_release_and_wait() {
  axp_scl_release();
  for (int i = 0; i < 1000; i++) {   // ~1 ms ceiling
    if (axp_scl_read())
      return true;
    axp_delay_us(1);
  }
  return false;
}

static inline bool axp_i2c_start() {
  axp_sda_release();
  if (!axp_scl_release_and_wait())
    return false;
  axp_delay_us(AXP192_HALF_PERIOD_US);
  axp_sda_low();                       // SDA falls while SCL is high = START
  axp_delay_us(AXP192_HALF_PERIOD_US);
  axp_scl_low();
  return true;
}

static inline bool axp_i2c_stop() {
  axp_sda_low();
  axp_delay_us(AXP192_HALF_PERIOD_US);
  if (!axp_scl_release_and_wait())
    return false;
  axp_delay_us(AXP192_HALF_PERIOD_US);
  axp_sda_release();                   // SDA rises while SCL is high = STOP
  axp_delay_us(AXP192_HALF_PERIOD_US);
  return true;
}

// Clock out one bit, MSB-first framing handled by the caller.
static inline bool axp_i2c_write_bit(bool bit) {
  if (bit)
    axp_sda_release();
  else
    axp_sda_low();
  axp_delay_us(AXP192_HALF_PERIOD_US);
  if (!axp_scl_release_and_wait())
    return false;
  axp_delay_us(AXP192_HALF_PERIOD_US);
  axp_scl_low();
  return true;
}

static inline bool axp_i2c_read_bit(bool *bit) {
  axp_sda_release();                   // let the slave drive
  axp_delay_us(AXP192_HALF_PERIOD_US);
  if (!axp_scl_release_and_wait())
    return false;
  *bit = axp_sda_read();               // sample while SCL is high
  axp_delay_us(AXP192_HALF_PERIOD_US);
  axp_scl_low();
  return true;
}

// Returns true only if the slave ACKed (pulled SDA low on the 9th clock).
static inline bool axp_i2c_write_byte(uint8_t value) {
  for (int i = 7; i >= 0; i--) {
    if (!axp_i2c_write_bit((value >> i) & 1))
      return false;
  }
  bool nack = true;
  if (!axp_i2c_read_bit(&nack))
    return false;
  return !nack;
}

static inline bool axp_i2c_read_byte(uint8_t *out, bool ack) {
  uint8_t value = 0;
  for (int i = 7; i >= 0; i--) {
    bool bit = false;
    if (!axp_i2c_read_bit(&bit))
      return false;
    value = (uint8_t) (value | ((uint8_t) (bit ? 1 : 0) << i));
  }
  *out = value;
  // Master ACK = drive low (more bytes wanted); NACK = release (last byte).
  return axp_i2c_write_bit(!ack);
}

// If a previous transaction was cut short (a reset mid-byte), a slave can be
// left holding SDA low. Nine clocks with SDA released walks it out of that byte,
// then a STOP resynchronises the bus. Runs once at init; harmless when idle.
static inline void axp_i2c_bus_recover() {
  axp_sda_release();
  for (int i = 0; i < 9 && !axp_sda_read(); i++) {
    axp_scl_low();
    axp_delay_us(AXP192_HALF_PERIOD_US);
    axp_scl_release();
    axp_delay_us(AXP192_HALF_PERIOD_US);
  }
  axp_i2c_stop();
}

// --- Register access ------------------------------------------------------

static inline bool axp192_write_reg(uint8_t reg, uint8_t value) {
  if (!axp_i2c_start())
    return false;
  const bool ok = axp_i2c_write_byte((uint8_t) (AXP192_ADDR << 1)) &&  // W
                  axp_i2c_write_byte(reg) && axp_i2c_write_byte(value);
  axp_i2c_stop();
  return ok;
}

static inline bool axp192_read_reg(uint8_t reg, uint8_t *out) {
  // Write the register pointer, then a REPEATED START into the read — the
  // AXP192 expects the address phase without an intervening STOP.
  if (!axp_i2c_start())
    return false;
  if (!axp_i2c_write_byte((uint8_t) (AXP192_ADDR << 1)) || !axp_i2c_write_byte(reg)) {
    axp_i2c_stop();
    return false;
  }
  if (!axp_i2c_start()) {   // repeated start
    axp_i2c_stop();
    return false;
  }
  if (!axp_i2c_write_byte((uint8_t) ((AXP192_ADDR << 1) | 1))) {  // R
    axp_i2c_stop();
    return false;
  }
  const bool ok = axp_i2c_read_byte(out, /*ack=*/false);  // single byte → NACK
  axp_i2c_stop();
  return ok;
}

// Pure: what register 0x12 should become at init, given what it currently reads.
// Grove 5 V boost on (temp_a hangs off it); the TFT rails (LDO2/LDO3) are left
// exactly as found (ADR-026 — driven separately by axp192_lcd_on/off()), as is
// every other bit — critically DC-DC1, our own supply. Split out from
// axp192_init() so the rail arithmetic is host-testable.
static inline uint8_t axp192_rails_target(uint8_t current) {
  return (uint8_t) (current | AXP192_EXTEN);
}

// Pure: register 0x12 with the TFT rails (LDO2|LDO3) ON, everything else as found.
static inline uint8_t axp192_lcd_on_target(uint8_t current) {
  return (uint8_t) (current | AXP192_LCD_RAILS);
}

// Pure: register 0x12 with the TFT rails (LDO2|LDO3) OFF, everything else as found.
static inline uint8_t axp192_lcd_off_target(uint8_t current) {
  return (uint8_t) (current & (uint8_t) ~AXP192_LCD_RAILS);
}

// Is a value read from 0x12 believable? Register 0x12 is the one register we
// READ-MODIFY-WRITE, which makes it the one place a corrupted read can do real
// damage: we would compute a mask from garbage and write it back, and bit0 of
// that register is DC-DC1 — the ESP32's OWN 3.3 V supply. Writing a zero there
// powers the board off mid-operation, and it stays off until the AXP192 is
// poked by the power button. Nothing else in this file can brick anything; a
// bad ADC read is just a NaN.
//
// The check is free and exact: this code is EXECUTING, which is only possible
// if DC-DC1 is on. So a read claiming otherwise is provably a bad read (bus
// glitch, marginal pull-up, missed ACK), and the only safe response is to leave
// the register alone. The cost of skipping is trivial and self-correcting — the
// LCD stays powered and Grove 5 V stays as-is for ONE wake, and init() runs
// again on the next one.
static inline bool axp192_rails_plausible(uint8_t current) {
  return (current & AXP192_DCDC1) != 0;
}

// Put the PMIC into the state this build needs. Call once per wake from on_boot.
// Unlike the retired hardware-bus version there is no component to wait for —
// this configures its own two GPIOs — so it has no ordering constraint beyond
// running before the first battery read.
//
// Note the ADC needs a moment to produce its first conversion after being
// enabled. wake_cycle reads batt_v well after this (a settling delay plus both
// temperature reads), which covers it; a boot-time read taken immediately would
// not.
static inline void axp192_init() {
  axp_pins_config();
  axp_i2c_bus_recover();

  // Enable all ADCs — without this 0x78/0x79 read as zero forever.
  axp192_write_reg(AXP192_REG_ADC_EN1, 0xFF);

  uint8_t rails = 0;
  if (axp192_read_reg(AXP192_REG_DCDC_LDO_EN, &rails) && axp192_rails_plausible(rails)) {
    const uint8_t want = axp192_rails_target(rails);
    if (want != rails)
      axp192_write_reg(AXP192_REG_DCDC_LDO_EN, want);
  }
}

// Power the TFT rails ON (LDO2 backlight + LDO3 panel) for the brief boot/button
// status screens (ADR-026). Sets the LDO voltage first (harmless if already set),
// then the enable bits — guarded by axp192_rails_plausible() so a corrupted 0x12
// read never RMWs DC-DC1 (our own supply) to zero. Callers must have run
// axp_pins_config() first (the high-priority on_boot lambda does, before the
// ESPHome display component's ST7735 init runs).
static inline void axp192_lcd_on() {
  axp192_write_reg(AXP192_REG_LDO23_V, AXP192_LDO23_3V0);
  uint8_t rails = 0;
  if (axp192_read_reg(AXP192_REG_DCDC_LDO_EN, &rails) && axp192_rails_plausible(rails)) {
    const uint8_t want = axp192_lcd_on_target(rails);
    if (want != rails)
      axp192_write_reg(AXP192_REG_DCDC_LDO_EN, want);
  }
}

// Power the TFT rails OFF. This is where "display off" is now enforced (ADR-026
// supersedes the display half of ADR-002): call before every deep_sleep.enter so
// the panel is dark in sleep and on all sampling wakes. Idempotent; guarded like
// axp192_lcd_on().
static inline void axp192_lcd_off() {
  uint8_t rails = 0;
  if (axp192_read_reg(AXP192_REG_DCDC_LDO_EN, &rails) && axp192_rails_plausible(rails)) {
    const uint8_t want = axp192_lcd_off_target(rails);
    if (want != rails)
      axp192_write_reg(AXP192_REG_DCDC_LDO_EN, want);
  }
}

// Pure: 12-bit ADC pair → volts, at the AXP192's 1.1 mV/LSB.
static inline float axp192_raw_to_volts(uint8_t hi, uint8_t lo) {
  const uint16_t raw = (uint16_t) (((uint16_t) hi << 4) | (uint16_t) (lo & 0x0F));
  return (float) raw * 0.0011f;
}

// Cell voltage in volts, or NaN if the PMIC does not answer. NaN matters: every
// downstream battery path (the report's battery line, alert_footer(), the
// BATT_CRIT_V floor, the low-battery warning) already treats NaN as "battery
// n/a" and degrades gracefully, so a dead bus must not read as 0 V — that would
// look like a flat cell and trip the protective floor.
static inline float axp192_batt_voltage() {
  uint8_t hi = 0, lo = 0;
  if (!axp192_read_reg(AXP192_REG_BATT_V_HI, &hi))
    return NAN;
  if (!axp192_read_reg(AXP192_REG_BATT_V_LO, &lo))
    return NAN;
  return axp192_raw_to_volts(hi, lo);
}
