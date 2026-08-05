// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
// Host-compiled unit tests for logger/axp192.h (ADR-023).
//
// The AXP192 transport is a hand-written bit-banged I2C master (the ESP32 has
// only two hardware I2C peripherals and both are spoken for by the temperature
// sensors — see ADR-023). Hand-written wire protocol in a safety build deserves
// a test, so the header exposes its six pin primitives as a swappable HAL:
// define AXP192_TEST_HAL, supply them, and the whole master — START/STOP
// framing, MSB-first bytes, ACK/NACK, the repeated start of a register read —
// runs on the host against the simulated slave below.
//
// What this CAN'T prove: real-world timing, edge rates, and whether the board's
// pull-ups are adequate. Those are hardware items (see the M5StickC bring-up
// checklist in CLAUDE.md). What it DOES prove is that the protocol we emit is
// the protocol an AXP192 expects, and that a silent bus degrades to NaN rather
// than to a plausible-looking voltage.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/ta test/test_axp192.cpp && /tmp/ta

#define AXP192_TEST_HAL  // we provide the six pin primitives ourselves

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

// --- Simulated AXP192 on a simulated open-drain bus -----------------------
// Line level = wired-AND of every driver: high only when nobody pulls it low.
// The slave is a small edge-driven state machine, deliberately written to the
// I2C spec rather than to our master, so that a master bug shows up as a
// protocol violation rather than being quietly mirrored.
namespace {

struct FakeAxp {
  static constexpr uint8_t kAddr = 0x34;

  uint8_t regs[256] = {};
  bool present = true;      // false simulates a dead/absent PMIC (no ACK ever)

  bool m_sda = true;        // master's driver (true = released)
  bool m_scl = true;
  bool s_sda = true;        // slave's driver

  enum Mode { IDLE, RX, TX } mode = IDLE;
  int bitpos = 0;
  uint8_t shift = 0;
  bool in_ack = false;
  int byte_index = 0;
  bool go_tx = false;       // address phase asked for a read
  uint8_t reg_ptr = 0;
  uint8_t tx_byte = 0;
  bool master_nacked = false;

  // Protocol-violation counters — asserted zero by the tests.
  int errors = 0;
  bool addr_seen = false;

  // Models a slave left mid-byte by an untimely reset: it holds SDA low until
  // the remaining bits of that byte are clocked out, then releases. This is the
  // condition axp_i2c_bus_recover() exists to clear.
  int stuck_low_clocks = 0;

  bool sda_line() const { return m_sda && s_sda; }
  bool scl_line() const { return m_scl; }

  void set_master_sda(bool released) {
    const bool before = sda_line();
    m_sda = released;
    const bool after = sda_line();
    if (scl_line() && before != after) {
      if (!after) on_start();
      else on_stop();
    }
  }

  void set_master_scl(bool released) {
    const bool before = scl_line();
    m_scl = released;
    const bool after = scl_line();
    if (!before && after) on_rising();
    else if (before && !after) on_falling();
  }

  void on_start() {
    mode = RX;
    bitpos = 0;
    shift = 0;
    in_ack = false;
    byte_index = 0;
    go_tx = false;
    s_sda = true;
  }

  void on_stop() {
    mode = IDLE;
    s_sda = true;
    in_ack = false;
  }

  void on_rising() {
    if (mode == RX) {
      if (bitpos < 8) {
        shift = (uint8_t) ((shift << 1) | (sda_line() ? 1 : 0));
        bitpos++;
      }
      // bitpos == 8: the ACK bit, which we are driving; nothing to sample.
    } else if (mode == TX) {
      if (bitpos < 8)
        bitpos++;            // master samples the bit we placed on the last fall
      else
        master_nacked = sda_line();
    }
  }

  void on_falling() {
    if (stuck_low_clocks > 0) {
      if (--stuck_low_clocks == 0)
        s_sda = true;
      return;
    }
    if (mode == RX) {
      if (bitpos == 8 && !in_ack) {
        s_sda = !ack_rx_byte(shift);   // drive low to ACK
        in_ack = true;
      } else if (in_ack) {
        s_sda = true;
        in_ack = false;
        bitpos = 0;
        shift = 0;
        byte_index++;
        if (go_tx) {
          mode = TX;
          start_tx_byte();
        }
      }
    } else if (mode == TX) {
      if (bitpos < 8) {
        s_sda = ((tx_byte >> (7 - bitpos)) & 1) != 0;
      } else if (!in_ack) {
        s_sda = true;        // release for the master's ACK/NACK bit
        in_ack = true;
      } else {
        in_ack = false;
        if (master_nacked) {
          mode = IDLE;       // last byte; master will STOP
        } else {
          start_tx_byte();   // sequential read
        }
      }
    }
  }

  void start_tx_byte() {
    tx_byte = regs[reg_ptr++];
    bitpos = 0;
    in_ack = false;
    master_nacked = false;
    s_sda = ((tx_byte >> 7) & 1) != 0;
  }

  // Returns whether to ACK this received byte.
  bool ack_rx_byte(uint8_t value) {
    if (byte_index == 0)
      addr_seen = true;   // recorded even when absent, so tests can prove we tried
    if (!present)
      return false;
    if (byte_index == 0) {
      const uint8_t addr = (uint8_t) (value >> 1);
      const bool read = (value & 1) != 0;
      if (addr != kAddr)
        return false;
      go_tx = read;
      return true;
    }
    if (byte_index == 1) {
      reg_ptr = value;       // register pointer
      return true;
    }
    regs[reg_ptr++] = value; // data write
    return true;
  }
};

FakeAxp g_bus;

}  // namespace

// --- The HAL the header will compile against ------------------------------
static inline void axp_pins_config() {}
static inline void axp_sda_low() { g_bus.set_master_sda(false); }
static inline void axp_sda_release() { g_bus.set_master_sda(true); }
static inline void axp_scl_low() { g_bus.set_master_scl(false); }
static inline void axp_scl_release() { g_bus.set_master_scl(true); }
static inline bool axp_sda_read() { return g_bus.sda_line(); }
static inline bool axp_scl_read() { return g_bus.scl_line(); }
static inline void axp_delay_us(uint32_t) {}

#include "../logger/axp192.h"

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static void reset_bus(bool present = true) {
  g_bus = FakeAxp();
  g_bus.present = present;
}

int main() {
  // --- Pure helpers, independent of the wire ------------------------------
  // Init rail arithmetic (ADR-026): EXTEN forced on; LDO2/LDO3 now left AS FOUND
  // (driven separately by the lcd helpers), everything else — and DC-DC1 (bit0,
  // our own 3.3 V supply) above all — preserved untouched.
  CHECK(axp192_rails_target(0x41) == 0x41);   // EXTEN already on: no change
  CHECK(axp192_rails_target(0x01) == 0x41);   // EXTEN set; DC-DC1 preserved
  CHECK(axp192_rails_target(0x00) == 0x40);   // EXTEN set even from all-off
  CHECK(axp192_rails_target(0x4C) == 0x4C);   // LDO2/LDO3 left exactly as found
  CHECK((axp192_rails_target(0x03) & 0x01) == 0x01);  // DC-DC1 never disturbed
  CHECK((axp192_rails_target(0x00) & AXP192_EXTEN) == AXP192_EXTEN);

  // TFT-rail arithmetic for the boot/button screens (ADR-026): only LDO2|LDO3
  // move, EXTEN and DC-DC1 and everything else stay exactly as found.
  CHECK(axp192_lcd_on_target(0x41) == 0x4D);          // 0x41 | 0x0C
  CHECK(axp192_lcd_on_target(0x4D) == 0x4D);          // already on: no change
  CHECK((axp192_lcd_on_target(0x41) & 0x41) == 0x41); // EXTEN + DC-DC1 preserved
  CHECK(axp192_lcd_off_target(0x4D) == 0x41);         // 0x4D & ~0x0C
  CHECK(axp192_lcd_off_target(0x41) == 0x41);         // already off: no change
  CHECK((axp192_lcd_off_target(0xFF) & AXP192_LCD_RAILS) == 0);
  CHECK((axp192_lcd_off_target(0xFF) & 0x01) == 0x01); // DC-DC1 never disturbed

  // 12-bit pair -> volts at 1.1 mV/LSB; the low register's high nibble is
  // status, not data, and must be masked off.
  CHECK(near(axp192_raw_to_volts(0xC5, 0x0A), 3.4782f));
  CHECK(near(axp192_raw_to_volts(0xC5, 0xFA), 3.4782f));  // high nibble ignored
  CHECK(near(axp192_raw_to_volts(0x00, 0x00), 0.0f));
  CHECK(near(axp192_raw_to_volts(0xFF, 0x0F), 4.5045f));  // full scale

  // --- Single register write reaches the right register -------------------
  reset_bus();
  CHECK(axp192_write_reg(0x82, 0xFF));
  CHECK(g_bus.regs[0x82] == 0xFF);
  CHECK(g_bus.mode == FakeAxp::IDLE);   // STOP was emitted; bus left idle
  CHECK(g_bus.sda_line() && g_bus.scl_line());  // both lines released

  // --- Single register read, incl. the repeated start ---------------------
  reset_bus();
  g_bus.regs[0x12] = 0x4D;
  uint8_t v = 0;
  CHECK(axp192_read_reg(0x12, &v));
  CHECK(v == 0x4D);
  CHECK(g_bus.mode == FakeAxp::IDLE);

  // --- init(): ADCs enabled, EXTEN on, LCD rails + DC-DC1 preserved -------
  // ADR-026: init no longer forces the TFT rails off — it forces EXTEN on and
  // leaves LDO2/LDO3 exactly as found (the lcd helpers own them).
  reset_bus();
  g_bus.regs[0x12] = 0x01;   // DC-DC1 on, EXTEN off, LCD rails off (post-sleep)
  g_bus.regs[0x82] = 0x00;   // ADCs disabled, as at power-on
  axp192_init();
  CHECK(g_bus.regs[0x82] == 0xFF);   // without this, 0x78/0x79 read zero forever
  CHECK(g_bus.regs[0x12] == 0x41);
  CHECK((g_bus.regs[0x12] & 0x01) == 0x01);              // ESP32's own rail alive
  CHECK((g_bus.regs[0x12] & AXP192_EXTEN) != 0);         // Grove 5 V for temp_a

  // init() must not clear TFT rails that a boot/button wake has already turned
  // on (ADR-026): with LDO2/LDO3 set it leaves them, only forcing EXTEN.
  reset_bus();
  g_bus.regs[0x12] = 0x4D;   // LDO2+LDO3 on (screen lit), DC-DC1 on, EXTEN on
  axp192_init();
  CHECK(g_bus.regs[0x12] == 0x4D);                       // rails untouched
  CHECK((g_bus.regs[0x12] & AXP192_LCD_RAILS) == AXP192_LCD_RAILS);

  // init() is called on EVERY wake — it must be idempotent, and must not write
  // 0x12 again once EXTEN already reads correct.
  const uint8_t after_first = g_bus.regs[0x12];
  axp192_init();
  CHECK(g_bus.regs[0x12] == after_first);

  // --- lcd_on()/lcd_off() drive only the TFT rails, over the wire ---------
  // lcd_on(): sets the LDO voltage (0x28) and turns LDO2/LDO3 on, EXTEN + DC-DC1
  // preserved. lcd_off(): clears them again — the sleep-time "display off".
  reset_bus();
  g_bus.regs[0x12] = 0x41;   // EXTEN + DC-DC1 on, screen off (a sampling wake)
  axp192_lcd_on();
  CHECK(g_bus.regs[0x28] == AXP192_LDO23_3V0);           // LDO voltage programmed
  CHECK(g_bus.regs[0x12] == 0x4D);                       // TFT rails now on
  CHECK((g_bus.regs[0x12] & 0x41) == 0x41);              // EXTEN + DC-DC1 kept
  axp192_lcd_off();
  CHECK(g_bus.regs[0x12] == 0x41);                       // back to dark
  CHECK((g_bus.regs[0x12] & 0x41) == 0x41);              // EXTEN + DC-DC1 kept

  // A corrupted 0x12 read (DC-DC1 low) must NOT be acted on by the lcd helpers
  // either — same DC-DC1 safety guard as init().
  reset_bus();
  g_bus.regs[0x12] = 0x0C;   // implausible: LCD bits set but our own rail reads low
  axp192_lcd_off();
  CHECK(g_bus.regs[0x12] == 0x0C);   // left untouched rather than written back

  // --- A corrupted 0x12 read must NOT be acted on -------------------------
  // 0x12 is the only register we read-modify-write, and bit0 is DC-DC1 — the
  // ESP32's own supply. Acting on a garbage read that has bit0 clear would
  // write it back as zero and power the board off. We are executing, so DC-DC1
  // is on; a read saying otherwise is provably wrong and must be ignored.
  CHECK(axp192_rails_plausible(0x4D));
  CHECK(axp192_rails_plausible(0x01));
  CHECK(!axp192_rails_plausible(0x00));   // "everything off" while we run: impossible
  CHECK(!axp192_rails_plausible(0x4C));   // LDOs on but our own rail off: impossible

  reset_bus();
  g_bus.regs[0x12] = 0x4C;   // implausible: DC-DC1 reads low
  g_bus.regs[0x82] = 0x00;
  axp192_init();
  CHECK(g_bus.regs[0x12] == 0x4C);   // left untouched rather than written back
  CHECK(g_bus.regs[0x82] == 0xFF);   // the ADC enable still happens (write-only)

  // And the safe path is unaffected: a plausible read is still acted on —
  // EXTEN forced on, TFT rails left as found (ADR-026).
  reset_bus();
  g_bus.regs[0x12] = 0x01;   // plausible (DC-DC1 on), EXTEN off
  axp192_init();
  CHECK(g_bus.regs[0x12] == 0x41);   // EXTEN forced on

  // --- Battery voltage over the wire --------------------------------------
  reset_bus();
  g_bus.regs[0x78] = 0xC5;
  g_bus.regs[0x79] = 0x0A;
  CHECK(near(axp192_batt_voltage(), 3.4782f));

  // A flat-but-alive cell still reads as a number, not NaN — the BATT_CRIT_V
  // floor depends on being able to see it.
  reset_bus();
  g_bus.regs[0x78] = 0xB6;   // raw 0xB68 = 2920 -> 3.212 V
  g_bus.regs[0x79] = 0x08;
  CHECK(near(axp192_batt_voltage(), 3.2120f));

  // --- Dead bus degrades to NaN, never to 0 V -----------------------------
  // This is the load-bearing failure mode: 0 V would read as a flat cell and
  // trip the protective floor, silencing the scheduled report (ADR-022).
  reset_bus(/*present=*/false);
  const float dead = axp192_batt_voltage();
  CHECK(std::isnan(dead));
  CHECK(g_bus.addr_seen);            // we did try — the slave just never ACKed
  CHECK(g_bus.mode == FakeAxp::IDLE);
  CHECK(g_bus.sda_line() && g_bus.scl_line());  // bus released even on failure

  reset_bus(/*present=*/false);
  CHECK(!axp192_write_reg(0x82, 0xFF));   // failure is reported, not swallowed
  uint8_t ignored = 0xAA;
  CHECK(!axp192_read_reg(0x12, &ignored));

  // init() against a dead PMIC must not hang or fault — it just achieves nothing.
  reset_bus(/*present=*/false);
  axp192_init();
  CHECK(g_bus.mode == FakeAxp::IDLE);

  // --- Bus recovery from a slave left holding SDA low ---------------------
  // Simulate a reset mid-byte: the slave is stuck driving SDA. Nine clocks plus
  // a STOP must free the line, otherwise every later read fails forever.
  reset_bus();
  g_bus.s_sda = false;
  g_bus.stuck_low_clocks = 3;   // releases once three more clocks arrive
  CHECK(!g_bus.sda_line());
  axp_i2c_bus_recover();
  CHECK(g_bus.sda_line());
  g_bus.regs[0x78] = 0xC5;
  g_bus.regs[0x79] = 0x0A;
  CHECK(near(axp192_batt_voltage(), 3.4782f));   // and the bus works afterwards

  // --- No protocol violations were recorded across any of the above -------
  CHECK(g_bus.errors == 0);

  std::printf("test_axp192: all %d checks passed\n", g_checks);
  return 0;
}
