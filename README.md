# esphome-nice-floor

ESPHome / ESP32 component that can replace your Nice FLOR-S rolling code remote for garage and portal — pilotable depuis Home Assistant ou en standalone via moniteur série.

Clone d'une télécommande Nice FLOR-S (portails de garage / portails coulissants) avec un ESP32 et un module CC1101.

Deux modes d'usage :
1. **Firmware standalone PlatformIO** (`src/main.cpp`) avec moniteur série pour capturer, décoder et émettre.
2. **Composant ESPHome** (`components/nice_flor_s/`) pour intégration directe dans Home Assistant.

## Matériel requis

- ESP32 (testé sur Wemos D1 Mini ESP32)
- Module CC1101 433 MHz avec antenne soudée
- Câblage SPI standard

| CC1101 | ESP32 |
|--------|-------|
| VCC    | 3.3V  |
| GND    | GND   |
| CSN    | GPIO5 |
| SCK    | GPIO18 |
| MOSI   | GPIO23 |
| MISO   | GPIO19 |
| GDO0   | GPIO2 (data async OOK) |

## Mode 1 : firmware standalone (capture + clonage)

```bash
pio run -t upload
pio device monitor
```

Commandes série :
- `c` — mode sniff (capture les trames d'une vraie télécommande)
- `o` — émet une commande "ouvrir" avec le serial/rolling code configurés
- `s` — modifie serial + rolling code interactivement
- `r` — affiche le rolling code actuel
- `t`, `m` — commandes de diagnostic bas niveau (porteuse continue / monitor de GDO0)

### Procédure de clonage

1. Flash le firmware sur l'ESP32 (`pio run -t upload`)
2. Ouvre le moniteur série (`pio device monitor` à 115200 baud)
3. Tape `c` pour entrer en sniff
4. Appuie 1 fois sur le bouton de ta vraie télécommande
5. Note le `serial` et le `cnt` décodés
6. Tape `q` pour sortir
7. Tape `s`, entre ton serial puis ton rolling code de départ
8. Tape `o` à proximité du portail — il doit s'ouvrir

## Mode 2 : composant ESPHome

Voir [`components/nice_flor_s/README.md`](components/nice_flor_s/README.md) et [`components/nice_flor_s/example.yaml`](components/nice_flor_s/example.yaml).

```yaml
external_components:
  - source:
      type: local
      path: components

nice_flor_s:
  id: portail
  cs_pin: 5
  gdo0_pin: 2
  serial: 0x12345678          # à remplacer après capture
  button: 1
  initial_code: 0x0001        # à remplacer après capture
  tx_power: 12

button:
  - platform: template
    name: "Ouvrir portail"
    on_press:
      - lambda: id(portail).send_open();
```

## Algorithme

- **Modulation** : OOK 433.92 MHz, encodage PWM
- **Bit period** : 1581 µs (mesuré sur télécommande réelle)
  - Bit "1" : long HIGH 1054 µs + court LOW 527 µs
  - Bit "0" : court HIGH 527 µs + long LOW 1054 µs
- **Frame** : sync (1581 HIGH + 1581 LOW) + 52 bits de data + end pulse
- **Crypto** : Feistel 2 rounds avec keystore de 32 octets, port verbatim de [rtl_433](https://github.com/merbanan/rtl_433/blob/master/src/devices/nice_flor_s.c)
- **Compteur de retransmission** : séquence de 14 valeurs identiques à la vraie télécommande

## Outils Python

- `nice_flor_s_decode.py` — décodeur faithful de rtl_433 pour valider une trame
- `nice_flor_s_jev1337.py` — alternative basée sur les tables précalculées de [Jev1337/NiceFlor-Encoder](https://github.com/Jev1337/NiceFlor-Encoder)

```bash
python nice_flor_s_decode.py 0x08DDC67F93D6CA
```

## Limitations / honesty

- Le `leaf_node[32]` de rtl_433 n'est PAS la vraie clé de chiffrement Nice ; c'est une transformation partielle. Notre cipher est mathématiquement réversible mais peut différer du chiffrement réel utilisé par certains portails Nice.
- Les timings (527 µs / 1054 µs / 1581 µs) ont été mesurés sur une télécommande spécifique. Ils peuvent varier légèrement selon le modèle.
- Le clonage suppose que le portail accepte des trames émises avec un rolling code supérieur au dernier observé. Si le portail rejette, vérifier l'antenne et la puissance TX.

## Crédits

- Algorithme et keystore : [rtl_433/devices/nice_flor_s.c](https://github.com/merbanan/rtl_433/blob/master/src/devices/nice_flor_s.c)
- Tables précalculées alternatives : [Jev1337/NiceFlor-Encoder](https://github.com/Jev1337/NiceFlor-Encoder)
- Reverse engineering original : [phreakerclub.com/1615](http://phreakerclub.com/1615)
- Lib CC1101 : [LSatan/SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)
