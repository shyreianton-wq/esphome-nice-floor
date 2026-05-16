#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace nice_flor_s {

class NiceFlorS : public Component {
 public:
  // Configuration (appelee depuis __init__.py via setters)
  void set_cs_pin(uint8_t pin) { cs_pin_ = pin; }
  void set_gdo0_pin(uint8_t pin) { gdo0_pin_ = pin; }
  void set_serial(uint32_t serial) { serial_ = serial; }
  void set_button(uint8_t btn) { button_ = btn; }
  void set_initial_code(uint16_t code) { initial_code_ = code; }
  void set_tx_power(int8_t dbm) { tx_power_ = dbm; }

  void setup() override;
  void loop() override {}  // pas de traitement periodique
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void dump_config() override;

  // Action principale: emet une commande "ouvrir" (incremente rolling code, sauve, transmet 14 trames)
  void send_open();

  // Variante: emet avec un rolling code explicite (ne touche pas le compteur sauvegarde)
  void send_command(uint16_t code);

  // Permet de modifier le rolling code depuis HA (ex: lambda set_rolling_code(0x1234))
  void set_rolling_code(uint16_t code);

  uint16_t get_rolling_code() const { return rolling_code_; }

 protected:
  // Config
  uint8_t cs_pin_{5};
  uint8_t gdo0_pin_{2};
  uint32_t serial_{0};
  uint8_t button_{1};
  uint16_t initial_code_{0};
  int8_t tx_power_{12};

  // Etat runtime
  uint16_t rolling_code_{0};
  ESPPreferenceObject rolling_code_pref_;

  // CC1101
  void cc1101_init_();
  void cc1101_start_tx_();
  void cc1101_stop_();

  // Crypto + framing
  void send_bit_(bool bit);
  void send_nice_frame_(const uint8_t *encbuff);
  void nice_flor_s_encode_(uint16_t cnt, uint8_t repeat, uint8_t *encbuff);
  uint64_t nice_flor_s_encrypt_(uint64_t data);
};

}  // namespace nice_flor_s
}  // namespace esphome
