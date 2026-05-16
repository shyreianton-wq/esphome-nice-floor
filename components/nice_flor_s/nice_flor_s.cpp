#include "nice_flor_s.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <ELECHOUSE_CC1101_SRC_DRV.h>

namespace esphome {
namespace nice_flor_s {

static const char *const TAG = "nice_flor_s";

// ---------------------------------------------------------------------------
// Timings Nice FLOR-S (microsecondes, mesures sur vraie telecommande)
// element = 527us, bit PWM = 3 * 527 = 1581us
// ---------------------------------------------------------------------------
static constexpr uint32_t NICE_HI_PREFIX = 1581;
static constexpr uint32_t NICE_INITSEQ   = 1581;
static constexpr uint32_t NICE_SHORT     = 527;
static constexpr uint32_t NICE_LONG      = 1054;
static constexpr uint32_t NICE_LO_LAST   = 1581;
static constexpr uint32_t NICE_SEP       = 30000;

// Registres CC1101 a modifier directement (la lib SmartRC met IOCFG0=0x0D
// en mode raw, qui empeche la modulation OOK pilotee par MCU)
static constexpr uint8_t CC1101_REG_IOCFG0  = 0x02;
static constexpr uint8_t IOCFG0_TX_HIZ      = 0x2E;

// Keystore Nice (32 octets), copie verbatim de rtl_433/src/devices/nice_flor_s.c
static const uint8_t NICE_KEYSTORE[32] = {
     25,   5,  63,  97, 203, 109,  69,  10,
      3,   7,  64,   5,  71, 134, 180,  74,
     41, 158, 102, 199,  93, 118, 175, 101,
     60,  77, 143, 174, 103, 148,  29,  85
};

// Sequence de count observee sur la vraie telecommande:
// wire high nibble = A B 8 9 6 7 4 5 2 3 0 1 E F (14 trames)
// count_interne = high_nibble XOR btn XOR 0xF
static constexpr uint8_t COUNT_SEQUENCE_LEN = 14;
static const uint8_t COUNT_SEQUENCE[COUNT_SEQUENCE_LEN] = {
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1
};

// ---------------------------------------------------------------------------
// Helpers crypto
// ---------------------------------------------------------------------------
static inline void nice_magic_xor(uint8_t *p, uint8_t k) {
    for (uint8_t i = 1; i < 6; i++) p[i] ^= k;
}

uint64_t NiceFlorS::nice_flor_s_encrypt_(uint64_t data) {
    uint8_t *p = (uint8_t *) &data;
    uint8_t k = 0;

    for (uint8_t y = 0; y < 2; y++) {
        k = NICE_KEYSTORE[p[0] & 0x1F];
        nice_magic_xor(p, k);
        p[5] &= 0x0F;
        p[0] ^= k & 0xE0;

        k = NICE_KEYSTORE[p[0] >> 3] + 0x25;
        nice_magic_xor(p, k);
        p[5] &= 0x0F;
        p[0] ^= k & 0x07;

        if (y == 0) {
            uint8_t t = p[0];
            p[0] = p[1];
            p[1] = t;
        }
    }

    // Post-shuffle (inverse de la pre-shuffle du decrypt)
    p[5] = ~p[5] & 0x0F;
    k    = ~p[4];
    p[4] = ~p[0];
    p[0] = ~p[2];
    p[2] = k;
    k    = ~p[3];
    p[3] = ~p[1];
    p[1] = k;

    return data;
}

void NiceFlorS::nice_flor_s_encode_(uint16_t cnt, uint8_t repeat, uint8_t *encbuff) {
    // Construction des 64 bits decryptes
    // Layout : btn(4b) | ?(4b) | serial(28b) | cnt(16b)
    uint64_t data = 0;
    data |= (uint64_t)(cnt & 0xFFFF);
    data |= ((uint64_t)(this->serial_ & 0x0FFFFFFF)) << 16;
    data |= ((uint64_t)(this->button_ & 0x0F)) << 48;

    uint64_t encrypted = this->nice_flor_s_encrypt_(data);
    uint8_t *p = (uint8_t *) &encrypted;
    for (int i = 0; i < 7; i++) encbuff[6 - i] = p[i];

    // Le high nibble de encbuff[1] est le compteur de retransmission,
    // applique APRES le chiffrement (pas touche par le Feistel)
    encbuff[1] = (encbuff[1] & 0x0F) | (((repeat ^ this->button_ ^ 0x0F) & 0x0F) << 4);
}

// ---------------------------------------------------------------------------
// CC1101 init / TX
// ---------------------------------------------------------------------------
void NiceFlorS::cc1101_init_() {
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setGDO0(this->gdo0_pin_);
    ELECHOUSE_cc1101.setMHZ(433.92);
    ELECHOUSE_cc1101.setModulation(2);      // ASK/OOK
    ELECHOUSE_cc1101.setDRate(512);
    ELECHOUSE_cc1101.setPA(this->tx_power_);
    ELECHOUSE_cc1101.setCCMode(false);
    ELECHOUSE_cc1101.setPktFormat(3);       // mode serie async
    ELECHOUSE_cc1101.setSyncMode(0);
    ELECHOUSE_cc1101.setLengthConfig(2);
    ELECHOUSE_cc1101.setDcFilterOff(false);
    ELECHOUSE_cc1101.setManchester(false);
    ELECHOUSE_cc1101.setCrc(false);
    ELECHOUSE_cc1101.setSidle();
}

void NiceFlorS::cc1101_start_tx_() {
    ELECHOUSE_cc1101.setSidle();
    // Important: forcer GDO0 en high impedance cote chip, sinon le chip et
    // le MCU se battent sur la pin et la modulation OOK ne s'applique pas
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_REG_IOCFG0, IOCFG0_TX_HIZ);
    pinMode(this->gdo0_pin_, OUTPUT);
    digitalWrite(this->gdo0_pin_, LOW);
    ELECHOUSE_cc1101.SetTx();
    delay(1);
}

void NiceFlorS::cc1101_stop_() {
    ELECHOUSE_cc1101.setSidle();
}

// ---------------------------------------------------------------------------
// PWM TX
// ---------------------------------------------------------------------------
void NiceFlorS::send_bit_(bool bit) {
    if (bit) {
        digitalWrite(this->gdo0_pin_, HIGH);
        delayMicroseconds(NICE_LONG);
        digitalWrite(this->gdo0_pin_, LOW);
        delayMicroseconds(NICE_SHORT);
    } else {
        digitalWrite(this->gdo0_pin_, HIGH);
        delayMicroseconds(NICE_SHORT);
        digitalWrite(this->gdo0_pin_, LOW);
        delayMicroseconds(NICE_LONG);
    }
}

void NiceFlorS::send_nice_frame_(const uint8_t *encbuff) {
    // Sync header: 1 bit period HIGH + 1 bit period LOW
    digitalWrite(this->gdo0_pin_, HIGH);
    delayMicroseconds(NICE_HI_PREFIX);
    digitalWrite(this->gdo0_pin_, LOW);
    delayMicroseconds(NICE_INITSEQ);

    // 52 bits de data
    for (int i = 3; i >= 0; i--)
        this->send_bit_((encbuff[0] >> i) & 1);
    for (int b = 1; b <= 6; b++)
        for (int i = 7; i >= 0; i--)
            this->send_bit_((encbuff[b] >> i) & 1);

    // End pulse + gap inter-trames
    digitalWrite(this->gdo0_pin_, HIGH);
    delayMicroseconds(NICE_LO_LAST);
    digitalWrite(this->gdo0_pin_, LOW);
    delayMicroseconds(NICE_SEP);
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------
void NiceFlorS::setup() {
    ESP_LOGCONFIG(TAG, "Init Nice FLOR-S (serial=0x%08X btn=%u)", this->serial_, this->button_);

    // Restore le rolling code depuis NVS
    uint32_t hash = (uint32_t) this->serial_;  // hash unique par serial
    this->rolling_code_pref_ = global_preferences->make_preference<uint16_t>(hash);
    if (!this->rolling_code_pref_.load(&this->rolling_code_)) {
        this->rolling_code_ = this->initial_code_;
        ESP_LOGI(TAG, "Aucun rolling code en NVS, init a 0x%04X", this->rolling_code_);
    } else {
        ESP_LOGI(TAG, "Rolling code restaure depuis NVS: 0x%04X", this->rolling_code_);
    }

    this->cc1101_init_();
}

void NiceFlorS::dump_config() {
    ESP_LOGCONFIG(TAG, "Nice FLOR-S:");
    ESP_LOGCONFIG(TAG, "  CS pin:        GPIO%u", this->cs_pin_);
    ESP_LOGCONFIG(TAG, "  GDO0 pin:      GPIO%u", this->gdo0_pin_);
    ESP_LOGCONFIG(TAG, "  Serial:        0x%08X", this->serial_);
    ESP_LOGCONFIG(TAG, "  Button:        %u", this->button_);
    ESP_LOGCONFIG(TAG, "  TX power:      %d dBm", this->tx_power_);
    ESP_LOGCONFIG(TAG, "  Rolling code:  0x%04X", this->rolling_code_);
}

// ---------------------------------------------------------------------------
// Actions exposees
// ---------------------------------------------------------------------------
void NiceFlorS::send_open() {
    this->rolling_code_++;
    this->rolling_code_pref_.save(&this->rolling_code_);
    this->send_command(this->rolling_code_);
}

void NiceFlorS::send_command(uint16_t code) {
    ESP_LOGI(TAG, "TX Nice FLOR-S: serial=0x%08X btn=%u code=0x%04X",
             this->serial_, this->button_, code);

    this->cc1101_start_tx_();

    for (uint8_t i = 0; i < COUNT_SEQUENCE_LEN; i++) {
        uint8_t repeat = COUNT_SEQUENCE[i];
        uint8_t encbuff[7];
        this->nice_flor_s_encode_(code, repeat, encbuff);
        this->send_nice_frame_(encbuff);
    }

    this->cc1101_stop_();
}

void NiceFlorS::set_rolling_code(uint16_t code) {
    this->rolling_code_ = code;
    this->rolling_code_pref_.save(&this->rolling_code_);
    ESP_LOGI(TAG, "Rolling code reglé sur 0x%04X", code);
}

}  // namespace nice_flor_s
}  // namespace esphome
