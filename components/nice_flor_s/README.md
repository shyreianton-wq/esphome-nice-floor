# nice_flor_s - composant ESPHome

Clone d'une telecommande Nice FLOR-S via un module CC1101.

## Installation

Copie le dossier `nice_flor_s/` dans `<ton-projet-esphome>/components/`, puis dans ton YAML :

```yaml
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
  tx_power: 12
```

Voir `example.yaml` pour un exemple complet avec WiFi + bouton HA.

## Options

| Option | Defaut | Description |
|--------|--------|-------------|
| `cs_pin` | requis | GPIO du CS (SS) du CC1101 |
| `gdo0_pin` | requis | GPIO du GDO0 du CC1101 (data async) |
| `serial` | requis | Serial 28-bit de la telecommande clonee (decoder avec rtl_433 ou notre script Python) |
| `button` | 1 | Numero du bouton (1..15) |
| `initial_code` | 0 | Rolling code initial, persiste en NVS apres |
| `tx_power` | 12 | Puissance dBm : -30, -20, -15, -10, 0, 5, 7, 10, 11, 12 |

## Cablage CC1101

| CC1101 | ESP32 |
|--------|-------|
| VCC    | 3.3V  |
| GND    | GND   |
| CSN    | GPIO5 (`cs_pin`) |
| SCK    | GPIO18 (SCK SPI) |
| MOSI   | GPIO23 (MOSI SPI) |
| MISO   | GPIO19 (MISO SPI) |
| GDO0   | GPIO2 (`gdo0_pin`, data async OOK) |

Antenne 433 MHz obligatoire (sinon portee de quelques cm).

## API C++

- `void send_open()` : incremente le rolling code, sauve, transmet 14 trames
- `void send_command(uint16_t code)` : transmet avec un code explicite (ne touche pas le compteur)
- `void set_rolling_code(uint16_t code)` : force le rolling code et sauvegarde
- `uint16_t get_rolling_code()` : lit le code actuel

## Recuperer le serial/code initial d'une vraie telecommande

Hors ESPHome, flashe le firmware standalone (`src/main.cpp` du repo parent) sur
un ESP32 + CC1101, lance le mode sniff (`c` en serie), appuie sur ta vraie
telecommande. Note les valeurs `serial=0x...` et `cnt=...` decodees, puis
reporte-les ici dans `serial:` et `initial_code:`.
