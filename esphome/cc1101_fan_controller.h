#pragma once
#include "esphome.h"
#include <CC1101_ESP_Arduino.h>

// ============================================================
// Fan RF Protocol Constants
// 29-bit OOK protocol: 20-bit device address + 9-bit command
// ============================================================
static const uint16_t FAN_CMD_SPEED1 = 0x1E8;  // 111101000
static const uint16_t FAN_CMD_SPEED2 = 0x1C8;  // 111001000
static const uint16_t FAN_CMD_SPEED3 = 0x1A9;  // 110101001
static const uint16_t FAN_CMD_SPEED4 = 0x189;  // 110001001
static const uint16_t FAN_CMD_SPEED5 = 0x16A;  // 101101010
static const uint16_t FAN_CMD_SPEED6 = 0x14A;  // 101001010
static const uint16_t FAN_CMD_TOGGLE = 0x191;  // 110010001
static const uint16_t FAN_CMD_LIGHT  = 0x0B5;  // 010110101
static const uint16_t FAN_CMD_INVERT = 0x12B;  // 100101011

// OOK timing (microseconds), derived from captured signals
static const int OOK_SHORT_US = 370;
static const int OOK_LONG_US  = 1100;
static const int OOK_GAP_US   = 6000;
static const int OOK_TX_REPS  = 12;
static const int OOK_MSG_BITS = 29;  // 20-bit address + 9-bit command
// The protocol is actually 30 bits: 29 data bits + 1 even-parity bit.
// The parity bit (trailing mark) is LONG=1 or SHORT=0 so the total
// number of '1' bits across all 30 is always even.

// ============================================================
// Hardware pin defaults — must match your wiring
// ============================================================
#ifndef CC1101_CS_PIN
#define CC1101_CS_PIN    5
#endif
#ifndef CC1101_GDO0_PIN
#define CC1101_GDO0_PIN  25
#endif
#ifndef CC1101_GDO2_PIN
#define CC1101_GDO2_PIN  26
#endif
#ifndef CC1101_SCK_PIN
#define CC1101_SCK_PIN   18
#endif
#ifndef CC1101_MISO_PIN
#define CC1101_MISO_PIN  19
#endif
#ifndef CC1101_MOSI_PIN
#define CC1101_MOSI_PIN  23
#endif

// Forward declaration for ISR
static void IRAM_ATTR rf_record_isr();

class CC1101FanController {
    CC1101 radio_;
    int gdo0_;
    int gdo2_;
    bool ready_ = false;
    portMUX_TYPE rf_mux_ = portMUX_INITIALIZER_UNLOCKED;

public:
    static constexpr int MAX_CHANGES = 512;
    static volatile int rec_timings_[MAX_CHANGES];
    static volatile int rec_count_;
    static volatile long rec_last_us_;
    static volatile bool rec_signal_detected_;
    static volatile bool rec_active_;

    CC1101FanController(int sck, int miso, int mosi, int cs, int gdo0, int gdo2)
        : radio_(sck, miso, mosi, cs, gdo0, gdo2), gdo0_(gdo0), gdo2_(gdo2) {}

    bool begin() {
        radio_.init();
        radio_.setMHZ(433.92);
        radio_.setTXPwr(TX_PLUS_10_DBM);
        radio_.setDataRate(2400);
        radio_.setRxBW(RX_BW_162_KHZ);
        radio_.setModulation(ASK_OOK);

        uint8_t partnum = radio_.getPartnum();
        uint8_t version = radio_.getVersion();

        if (version == 0x00 || version == 0xFF) {
            ESP_LOGE("cc1101", "CC1101 not detected (ver=0x%02X). Check wiring.", version);
            return false;
        }

        ready_ = true;
        ESP_LOGI("cc1101", "CC1101 OK  part=0x%02X  ver=0x%02X  433.92 MHz ASK/OOK", partnum, version);
        return true;
    }

    void send_command(uint32_t address, uint16_t command, bool odd_parity = false) {
        if (!ready_) {
            ESP_LOGW("cc1101", "Not initialised — skipping TX");
            return;
        }

        uint32_t msg = ((address & 0xFFFFF) << 9) | (command & 0x1FF);

        int ones = __builtin_popcount(msg);
        bool parity = odd_parity ? !(ones & 1) : (ones & 1);

        radio_.setTx();
        delayMicroseconds(500);

        // Preamble: alternating pulses to wake the receiver's AGC
        for (int i = 0; i < 12; i++) {
            digitalWrite(gdo0_, HIGH);
            delayMicroseconds(OOK_LONG_US);
            digitalWrite(gdo0_, LOW);
            delayMicroseconds(OOK_SHORT_US);
        }
        delayMicroseconds(OOK_GAP_US);

        for (int rep = 0; rep < OOK_TX_REPS; rep++) {
            portENTER_CRITICAL(&rf_mux_);

            for (int b = OOK_MSG_BITS - 1; b >= 0; b--) {
                bool one = msg & (1UL << b);
                digitalWrite(gdo0_, HIGH);
                delayMicroseconds(one ? OOK_LONG_US : OOK_SHORT_US);
                digitalWrite(gdo0_, LOW);
                delayMicroseconds(one ? OOK_SHORT_US : OOK_LONG_US);
            }
            // 30th bit: even-parity trailing mark
            digitalWrite(gdo0_, HIGH);
            delayMicroseconds(parity ? OOK_LONG_US : OOK_SHORT_US);
            digitalWrite(gdo0_, LOW);

            portEXIT_CRITICAL(&rf_mux_);

            delayMicroseconds(OOK_GAP_US);
        }

        digitalWrite(gdo0_, LOW);
        radio_.setIdle();
        ESP_LOGD("cc1101", "TX  addr=0x%05X  cmd=0x%03X", address, command);
    }

    void record_signal() {
        if (!ready_) {
            ESP_LOGW("cc1101", "Not initialised — skipping RX record");
            return;
        }

        ESP_LOGI("cc1101", "=== RECORDING: Put CC1101 in RX, waiting for signal (10s timeout) ===");

        rec_signal_detected_ = false;
        rec_count_ = 0;
        rec_last_us_ = micros();
        rec_active_ = true;

        radio_.setRx();
        pinMode(gdo2_, INPUT);
        attachInterrupt(digitalPinToInterrupt(gdo2_), rf_record_isr, CHANGE);

        unsigned long start = millis();
        while (!rec_signal_detected_ && (millis() - start < 10000)) {
            delay(1);
        }

        if (rec_signal_detected_) {
            delay(300);
        }

        rec_active_ = false;
        detachInterrupt(digitalPinToInterrupt(gdo2_));
        radio_.setIdle();

        int count = rec_count_;

        if (count < 20) {
            ESP_LOGW("cc1101", "Too few transitions (%d). No signal captured.", count);
            return;
        }

        ESP_LOGI("cc1101", "Captured %d transitions. Raw timings:", count);

        // Log timings in chunks so they fit in log buffers
        String timings_str = "[";
        for (int i = 0; i < count; i++) {
            if (i > 0) timings_str += ", ";
            timings_str += String(rec_timings_[i]);
            if (timings_str.length() > 400 || i == count - 1) {
                if (i < count - 1) timings_str += ",";
                ESP_LOGI("cc1101", "  %s", timings_str.c_str());
                timings_str = "";
            }
        }
        ESP_LOGI("cc1101", "]");

        decode_signal(count);
    }

private:
    void decode_signal(int count) {
        // Try to find repeated 30-bit frames in the captured data.
        // A SHORT pulse (~370us) = 0, a LONG pulse (~1100us) = 1.
        // Each bit is a mark+space pair; the mark duration encodes the bit.
        // Look for a gap (>3000us) as frame separator.

        for (int start = 0; start < count - 60; start++) {
            if (rec_timings_[start] < 3000) continue;

            // Found a gap — try to decode 30 bits starting after it
            uint32_t frame = 0;
            int bits_decoded = 0;
            int pos = start + 1;

            while (bits_decoded < 30 && pos + 1 < count) {
                int mark = rec_timings_[pos];
                if (mark < 100 || mark > 2000) break;

                bool is_one = (mark > 700);
                frame = (frame << 1) | (is_one ? 1 : 0);
                bits_decoded++;
                pos += 2; // skip mark + space
            }

            if (bits_decoded >= 29) {
                uint32_t full30 = frame;
                uint32_t msg29 = full30 >> 1;
                uint32_t address = (msg29 >> 9) & 0xFFFFF;
                uint16_t command = msg29 & 0x1FF;
                bool parity_bit = full30 & 1;

                ESP_LOGI("cc1101", "=== DECODED FRAME ===");
                ESP_LOGI("cc1101", "  30-bit frame : 0x%08X", full30);
                ESP_LOGI("cc1101", "  29-bit msg   : 0x%08X", msg29);
                ESP_LOGI("cc1101", "  Address (20b): 0x%05X", address);
                ESP_LOGI("cc1101", "  Command (9b) : 0x%03X", command);
                ESP_LOGI("cc1101", "  Parity bit   : %d", parity_bit);

                // Print command in binary for easy comparison
                char cmd_bin[10];
                for (int b = 8; b >= 0; b--) {
                    cmd_bin[8 - b] = (command & (1 << b)) ? '1' : '0';
                }
                cmd_bin[9] = '\0';
                ESP_LOGI("cc1101", "  Command (bin): %s", cmd_bin);
                return;
            }
        }

        ESP_LOGW("cc1101", "Could not decode a valid frame from the captured data.");
    }
};

// Static member definitions
volatile int CC1101FanController::rec_timings_[CC1101FanController::MAX_CHANGES];
volatile int CC1101FanController::rec_count_ = 0;
volatile long CC1101FanController::rec_last_us_ = 0;
volatile bool CC1101FanController::rec_signal_detected_ = false;
volatile bool CC1101FanController::rec_active_ = false;

static void IRAM_ATTR rf_record_isr() {
    if (!CC1101FanController::rec_active_ ||
        CC1101FanController::rec_count_ >= CC1101FanController::MAX_CHANGES)
        return;
    long now = micros();
    CC1101FanController::rec_timings_[CC1101FanController::rec_count_++] =
        (int)(now - CC1101FanController::rec_last_us_);
    CC1101FanController::rec_last_us_ = now;
    CC1101FanController::rec_signal_detected_ = true;
}

static CC1101FanController fan_ctrl(
    CC1101_SCK_PIN,
    CC1101_MISO_PIN,
    CC1101_MOSI_PIN,
    CC1101_CS_PIN,
    CC1101_GDO0_PIN,
    CC1101_GDO2_PIN
);
