"""ESPHome external component: Nice FLOR-S remote emulator via CC1101.

Usage YAML (voir example.yaml dans le repo) :

    external_components:
      - source:
          type: local
          path: components

    nice_flor_s:
      id: portail
      cs_pin: 5
      gdo0_pin: 2
      serial: 0x12345678
      button: 1
      initial_code: 0x0001
      tx_power: 12      # dBm, valeurs supportees: -30,-20,-15,-10,0,5,7,10,11,12

    button:
      - platform: template
        name: "Ouvrir portail"
        on_press:
          - lambda: id(portail).send_open();
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@user"]
DEPENDENCIES = []
MULTI_CONF = True

CONF_CS_PIN = "cs_pin"
CONF_GDO0_PIN = "gdo0_pin"
CONF_SERIAL = "serial"
CONF_BUTTON = "button"
CONF_INITIAL_CODE = "initial_code"
CONF_TX_POWER = "tx_power"

# Puissances dBm acceptees par la lib SmartRC pour la bande 433 MHz
VALID_TX_POWERS = [-30, -20, -15, -10, 0, 5, 7, 10, 11, 12]

nice_flor_s_ns = cg.esphome_ns.namespace("nice_flor_s")
NiceFlorS = nice_flor_s_ns.class_("NiceFlorS", cg.Component)


def _validate_tx_power(value):
    value = cv.int_(value)
    if value not in VALID_TX_POWERS:
        raise cv.Invalid(
            f"tx_power doit etre dans {VALID_TX_POWERS} (dBm), recu {value}"
        )
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NiceFlorS),
        cv.Required(CONF_CS_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_GDO0_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_SERIAL): cv.hex_uint32_t,
        cv.Optional(CONF_BUTTON, default=1): cv.int_range(min=1, max=15),
        cv.Optional(CONF_INITIAL_CODE, default=0): cv.uint16_t,
        cv.Optional(CONF_TX_POWER, default=12): _validate_tx_power,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_gdo0_pin(config[CONF_GDO0_PIN]))
    cg.add(var.set_serial(config[CONF_SERIAL]))
    cg.add(var.set_button(config[CONF_BUTTON]))
    cg.add(var.set_initial_code(config[CONF_INITIAL_CODE]))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))

    # SPI Arduino (necessaire a la lib SmartRC mais pas declare dans son library.json)
    cg.add_library("SPI", None)
    # Lib SmartRC pour piloter le CC1101 en mode async OOK
    cg.add_library("SmartRC-CC1101-Driver-Lib", "3.0.1")
