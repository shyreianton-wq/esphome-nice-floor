#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <Preferences.h>

// ============================================================
// CONFIGURATION - A MODIFIER AVEC VOS VALEURS
// ============================================================
// Serial 28-bit de TA telecommande (a remplacer apres capture en mode sniff)
uint32_t remote_serial = 0x12345678;
// Bouton (1..4 selon le modele de telecommande)
uint8_t button_id = 1;
// Rolling code initial; sera incremente avant chaque envoi par send_open()
// Apres une capture, mettre ici la valeur captee + marge pour etre sur d'etre
// devant le compteur du portail
#define ROLLING_CODE_INIT 0x0001

// ============================================================
// CABLAGE CC1101 -> ESP32 (Wemos D1 Mini 32)
// ============================================================
// CC1101 VCC  -> 3.3V
// CC1101 GND  -> GND
// CC1101 CSN  -> GPIO 5  (SS)
// CC1101 SCK  -> GPIO 18 (SCK)
// CC1101 MOSI -> GPIO 23 (MOSI)
// CC1101 MISO -> GPIO 19 (MISO)
// CC1101 GDO0 -> GPIO 2  (TX/RX data en mode async OOK)
// ============================================================

#define CC1101_GDO0 2

// Nice FLOR-S RF timing (microsecondes) - mesures vraie telecommande:
// element = 527us, donc bit PWM = 3 elements = 1581us
// sync header = 1581us HIGH puis 1581us LOW
#define NICE_HI_PREFIX  1581    // sync HIGH (1 bit period)
#define NICE_INITSEQ    1581    // sync LOW  (1 bit period)
#define NICE_SHORT      527     // element court (= 1 element)
#define NICE_LONG       1054    // element long  (= 2 elements)
#define NICE_LO_LAST    1581    // end pulse HIGH (1 bit period)
#define NICE_SEP        30000   // gap inter-trames (LOW)
#define NICE_NBITS      52

Preferences prefs;
uint16_t rolling_code = 0;

// ============================================================
// NICE FLOR-S - Algorithme Feistel 2 rounds (port Flipper Zero / rtl_433)
// Reference: rtl_433/src/devices/nice_flor_s.c (PR #3159)
// Keystore: 32 octets seulement (les index sont p[0]>>3 et p[0]&0x1F, max 31)
// ============================================================

static const uint8_t NICE_KEYSTORE[32] = {
     25,   5,  63,  97, 203, 109,  69,  10,
      3,   7,  64,   5,  71, 134, 180,  74,
     41, 158, 102, 199,  93, 118, 175, 101,
     60,  77, 143, 174, 103, 148,  29,  85
};

// Helper: XOR p[1..5] avec k
static inline void nice_magic_xor(uint8_t *p, uint8_t k) {
    for (uint8_t i = 1; i < 6; i++) p[i] ^= k;
}

// Encrypt: prend les 64 bits decryptes (cnt | serial | btn) et renvoie les 64 bits chiffres
// Layout decrypte (uint64_t LE) :
//   bits 0-15  : cnt (rolling counter)
//   bits 16-43 : serial (28 bits)
//   bits 44-47 : ?? (zero ou compteur de repeat selon la variante)
//   bits 48-51 : button (bitmask 1,2,4,8)
uint64_t nice_flor_s_encrypt(uint64_t data) {
    uint8_t *p = (uint8_t*)&data;
    uint8_t k;

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
            uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
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

// Decrypt: prend les 64 bits chiffres et renvoie les 64 bits decryptes
uint64_t nice_flor_s_decrypt(uint64_t data) {
    uint8_t *p = (uint8_t*)&data;
    uint8_t k;

    // Pre-shuffle
    k    = ~p[4];
    p[5] = ~p[5];
    p[4] = ~p[2];
    p[2] = ~p[0];
    p[0] = k;
    k    = ~p[3];
    p[3] = ~p[1];
    p[1] = k;

    for (uint8_t y = 0; y < 2; y++) {
        k = NICE_KEYSTORE[p[0] >> 3] + 0x25;
        nice_magic_xor(p, k);
        p[5] &= 0x0F;
        p[0] ^= k & 0x07;

        k = NICE_KEYSTORE[p[0] & 0x1F];
        nice_magic_xor(p, k);
        p[5] &= 0x0F;
        p[0] ^= k & 0xE0;

        if (y == 0) {
            uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
        }
    }

    return data;
}

// Convertit encbuff[7] (ordre TX, MSB-first) -> uint64_t pour algo Flipper
// p[0] = encbuff[6], ..., p[6] = encbuff[0]
// Pas d'invert ici : la fonction sniff_nice_flor_s skip le bit fantome en debut
// de trame, donc encbuff contient deja les vrais bits dans le bon sens.
uint64_t encbuff_to_data(const uint8_t *encbuff) {
    uint64_t data = 0;
    uint8_t *p = (uint8_t*)&data;
    for (int i = 0; i < 7; i++) p[i] = encbuff[6 - i];
    p[7] = 0;
    return data;
}

// Convertit uint64_t (apres encrypt) -> encbuff[7] pour TX
void data_to_encbuff(uint64_t data, uint8_t *encbuff) {
    uint8_t *p = (uint8_t*)&data;
    for (int i = 0; i < 7; i++) encbuff[6 - i] = p[i];
}

// Encode une trame TX : rolling counter + serial + bouton + repeat
void nice_flor_s_encode(uint32_t serial, uint16_t cnt, uint8_t button, uint8_t repeat, uint8_t *encbuff) {
    // Construction des 64 bits decryptes
    // Layout : btn(4b) | ?(4b) | serial(28b) | cnt(16b)
    uint64_t data = 0;
    data |= (uint64_t)(cnt & 0xFFFF);
    data |= ((uint64_t)(serial & 0x0FFFFFFF)) << 16;
    data |= ((uint64_t)(button & 0x0F)) << 48;

    uint64_t encrypted = nice_flor_s_encrypt(data);
    data_to_encbuff(encrypted, encbuff);

    // Le compteur de repeat est dans le high nibble de encbuff[1]
    // Formule Flipper: encbuff[1] high nibble = repeat ^ btn ^ 0xF
    // (apparait apres le chiffrement car non touche par le Feistel)
    encbuff[1] = (encbuff[1] & 0x0F) | (((repeat ^ button ^ 0x0F) & 0x0F) << 4);
}

// ============================================================
// CC1101 - INIT / TX / RX
// ============================================================

void cc1101_setup() {
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setGDO0(CC1101_GDO0);
    ELECHOUSE_cc1101.setMHZ(434.);
    ELECHOUSE_cc1101.setModulation(2);      // ASK/OOK
    ELECHOUSE_cc1101.setDRate(512);
    ELECHOUSE_cc1101.setPA(12);
    ELECHOUSE_cc1101.setCCMode(false);      // Desactiver le mode CC (pour raw async)
    ELECHOUSE_cc1101.setPktFormat(3);       // Mode serie asynchrone
    ELECHOUSE_cc1101.setSyncMode(0);        // Pas de preamble/sync word
    ELECHOUSE_cc1101.setLengthConfig(2);    // Longueur infinie
    ELECHOUSE_cc1101.setDcFilterOff(false); // Filtre DC actif
    ELECHOUSE_cc1101.setManchester(false);
    ELECHOUSE_cc1101.setCrc(false);
    ELECHOUSE_cc1101.setSidle();
    Serial.println("CC1101 initialise.");
}

// CC1101 register IOCFG0 controle qui pilote la pin GDO0 :
//   0x2E = High-Z (le MCU ecrit dans GDO0 -> pour TX async)
//   0x0D = Serial Data Output (le chip ecrit dans GDO0 -> pour RX async)
// La lib SmartRC met IOCFG0=0x0D dans setCCMode(false), ce qui bloque la TX
// car le chip et le MCU se battent sur GDO0.
#define CC1101_REG_IOCFG0   0x02
#define IOCFG0_TX_HIZ       0x2E
#define IOCFG0_RX_DATA_OUT  0x0D

void cc1101_start_tx() {
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_REG_IOCFG0, IOCFG0_TX_HIZ);
    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);
    ELECHOUSE_cc1101.SetTx();
    delay(1);
}

void cc1101_start_rx() {
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_REG_IOCFG0, IOCFG0_RX_DATA_OUT);
    ELECHOUSE_cc1101.setRxBW(325.00);      // Bande passante large pour OOK
    pinMode(CC1101_GDO0, INPUT);
    ELECHOUSE_cc1101.SetRx();
    delay(10);
}

void cc1101_stop() {
    ELECHOUSE_cc1101.setSidle();
}

// ============================================================
// TRANSMISSION OOK
// ============================================================

void send_bit(bool bit) {
    if (bit) {
        digitalWrite(CC1101_GDO0, HIGH);
        delayMicroseconds(NICE_LONG);
        digitalWrite(CC1101_GDO0, LOW);
        delayMicroseconds(NICE_SHORT);
    } else {
        digitalWrite(CC1101_GDO0, HIGH);
        delayMicroseconds(NICE_SHORT);
        digitalWrite(CC1101_GDO0, LOW);
        delayMicroseconds(NICE_LONG);
    }
}

void send_nice_frame(uint8_t *encbuff) {
    // Sync: 1 bit period HIGH puis 1 bit period LOW
    digitalWrite(CC1101_GDO0, HIGH);
    delayMicroseconds(NICE_HI_PREFIX);
    digitalWrite(CC1101_GDO0, LOW);
    delayMicroseconds(NICE_INITSEQ);

    // 52 bits de data (button 4 bits + 6 octets)
    for (int i = 3; i >= 0; i--)
        send_bit((encbuff[0] >> i) & 1);
    for (int b = 1; b <= 6; b++)
        for (int i = 7; i >= 0; i--)
            send_bit((encbuff[b] >> i) & 1);

    // End pulse + gap inter-trames
    digitalWrite(CC1101_GDO0, HIGH);
    delayMicroseconds(NICE_LO_LAST);
    digitalWrite(CC1101_GDO0, LOW);
    delayMicroseconds(NICE_SEP);
}

void send_command(uint32_t serial, uint8_t button, uint16_t code) {
    Serial.printf("Envoi commande: serial=0x%08X button=%d code=%d\n", serial, button, code);
    cc1101_start_tx();

    // Sequence de count observee sur la vraie telecommande:
    // wire high nibble = A B 8 9 6 7 4 5 2 3 0 1 E F (14 trames)
    // avec btn=1, count_interne = high_nibble XOR 1 XOR 0xF = high_nibble XOR 0xE
    // => sequence count_interne = 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1
    static const uint8_t REPEAT_SEQUENCE[14] = {
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1
    };

    for (uint8_t i = 0; i < 14; i++) {
        uint8_t repeat = REPEAT_SEQUENCE[i];
        uint8_t encbuff[7];
        nice_flor_s_encode(serial, code, button, repeat, encbuff);
        Serial.printf("  TX #%2d count=0x%X : %02X %02X %02X %02X %02X %02X %02X\n",
                       i + 1, repeat, encbuff[0], encbuff[1], encbuff[2],
                       encbuff[3], encbuff[4], encbuff[5], encbuff[6]);
        send_nice_frame(encbuff);
    }

    cc1101_stop();
    Serial.println("Envoi termine.");
}

void send_open() {
    rolling_code++;
    prefs.putUShort("cnt2", rolling_code);
    send_command(remote_serial, button_id, rolling_code);
    Serial.printf("Rolling code incremente: %d\n", rolling_code);
}

// ============================================================
// RECEPTION / SNIFFING NICE FLOR-S
// ============================================================

// Decode une trame capturee via l'algo Feistel et affiche cnt/serial/btn
void decode_nice_frame(uint8_t *encbuff) {
    uint64_t data = encbuff_to_data(encbuff);
    uint64_t decrypted = nice_flor_s_decrypt(data);

    uint16_t cnt    = (uint16_t)(decrypted & 0xFFFF);
    uint32_t serial = (uint32_t)((decrypted >> 16) & 0x0FFFFFFF);
    // Le bouton n'est pas chiffre, on le lit directement depuis encbuff[0]
    uint8_t  btn    = encbuff[0] & 0x0F;

    Serial.printf("  DECRYPT: cnt=%5u serial=0x%08X btn=0x%X\n", cnt, serial, btn);

    if (serial != 0) {
        remote_serial = serial;
        rolling_code = cnt;
        prefs.putUShort("cnt2", rolling_code);
        Serial.printf("  -> Sauvegarde: serial=0x%08X cnt=%u\n", serial, cnt);
    }
}

// Mesurer la duree d'un etat du pin (HIGH ou LOW), avec timeout
unsigned long measure_pulse_dur(int pin, int level, unsigned long timeout_us) {
    unsigned long start = micros();
    while (digitalRead(pin) == level) {
        if (micros() - start > timeout_us) return 0;
    }
    return micros() - start;
}

void sniff_nice_flor_s() {
    Serial.println("=== MODE CAPTURE / SNIFFING ===");
    Serial.println("Appuyez sur votre telecommande pres du CC1101.");
    Serial.println("Envoyez 'q' pour quitter.\n");

    cc1101_start_rx();

    int frame_count = 0;
    uint8_t last_encbuff[7] = {0};

    while (true) {
        // Verifier si l'utilisateur veut quitter
        if (Serial.available()) {
            char c = Serial.read();
            if (c == 'q' || c == 'Q') break;
        }

        // Attendre un front montant (debut d'impulsion)
        if (digitalRead(CC1101_GDO0) != HIGH) continue;

        // Mesurer la duree du sync HIGH (~1581us)
        unsigned long hi_dur = measure_pulse_dur(CC1101_GDO0, HIGH, 3000);
        if (hi_dur < 1200 || hi_dur > 2000) continue;

        // Mesurer le sync LOW (~1581us)
        unsigned long lo_dur = measure_pulse_dur(CC1101_GDO0, LOW, 3000);
        if (lo_dur < 1200 || lo_dur > 2000) continue;

        // Header detecte! Lire 52 bits PWM directement (pas de phantom)
        uint8_t bits[52];
        bool valid = true;

        for (int i = 0; i < 52; i++) {
            unsigned long h = measure_pulse_dur(CC1101_GDO0, HIGH, 2500);
            if (h == 0) { valid = false; break; }

            unsigned long l = measure_pulse_dur(CC1101_GDO0, LOW, 2500);
            // Pour le dernier bit, le LOW peut etre tres long (suivi du end pulse).
            if (l == 0 && i < 51) { valid = false; break; }

            // Seuil ~790us: midway entre NICE_SHORT (527) et NICE_LONG (1054)
            bits[i] = (h > 790) ? 1 : 0;
        }

        if (!valid) continue;

        // Convertir les bits en encbuff[7] (MSB-first par byte)
        uint8_t encbuff[7];
        encbuff[0] = 0;
        for (int b = 0; b < 4; b++)
            encbuff[0] |= bits[b] << (3 - b);
        for (int idx = 1; idx <= 6; idx++) {
            encbuff[idx] = 0;
            for (int b = 0; b < 8; b++)
                encbuff[idx] |= bits[4 + (idx - 1) * 8 + b] << (7 - b);
        }

        // Filtre repeat (meme encbuff[2..6] = meme appui) - on n'appelle decode que sur la 1ere
        bool is_repeat = (memcmp(encbuff + 2, last_encbuff + 2, 5) == 0);
        memcpy(last_encbuff, encbuff, 7);

        frame_count++;
        Serial.printf("  RX #%d  : %02X %02X %02X %02X %02X %02X %02X%s\n",
                       frame_count, encbuff[0], encbuff[1], encbuff[2], encbuff[3],
                       encbuff[4], encbuff[5], encbuff[6],
                       is_repeat ? "  (repeat)" : "");

        if (!is_repeat) decode_nice_frame(encbuff);
    }

    cc1101_stop();
    Serial.printf("\nCapture terminee. %d trames recues.\n", frame_count);
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("  Nice FLOR-S Remote - ESP32 + CC1101");
    Serial.println("========================================");

    cc1101_setup();

    Serial.printf("Serial: 0x%08X\n", remote_serial);
    Serial.printf("Button: %d\n", button_id);

    prefs.begin("nice", false);
    rolling_code = prefs.getUShort("cnt2", ROLLING_CODE_INIT);
    Serial.printf("Rolling code actuel: %d\n", rolling_code);

    Serial.println();
    Serial.println("Commandes:");
    Serial.println("  c - Capturer/sniffer une telecommande");
    Serial.println("  o - Ouvrir/Fermer (envoyer)");
    Serial.println("  s - Modifier le serial (hex)");
    Serial.println("  r - Afficher le rolling code");
    Serial.println("========================================");
}

void loop() {
    if (Serial.available()) {
        char cmd = Serial.read();

        switch (cmd) {
            case 'c':
            case 'C':
                sniff_nice_flor_s();
                break;

            case 'o':
            case 'O':
                send_open();
                break;

            case 'r':
            case 'R':
                Serial.printf("Rolling code: %d\n", rolling_code);
                break;

            case 't':
            case 'T': {
                Serial.println("[DEBUG] Porteuse continue HIGH 3s ...");
                cc1101_start_tx();
                digitalWrite(CC1101_GDO0, HIGH);
                delay(3000);
                digitalWrite(CC1101_GDO0, LOW);
                cc1101_stop();
                Serial.println("[DEBUG] Porteuse off.");
                break;
            }

            case 'm':
            case 'M': {
                Serial.println("[DEBUG] Monitor GDO0 3s (compte les fronts) ...");
                cc1101_start_rx();
                unsigned long t_end = millis() + 3000;
                int prev = digitalRead(CC1101_GDO0);
                int initial = prev;
                unsigned long edges = 0, high_us = 0, low_us = 0;
                unsigned long t_last = micros();
                while (millis() < t_end) {
                    int cur = digitalRead(CC1101_GDO0);
                    if (cur != prev) {
                        unsigned long now = micros();
                        unsigned long delta = now - t_last;
                        if (prev == HIGH) high_us += delta; else low_us += delta;
                        t_last = now;
                        prev = cur;
                        edges++;
                    }
                }
                cc1101_stop();
                Serial.printf("[DEBUG] Etat initial GDO0: %s\n", initial ? "HIGH" : "LOW");
                Serial.printf("[DEBUG] Fronts: %lu  |  HIGH cumul: %lu us  |  LOW cumul: %lu us\n",
                              edges, high_us, low_us);
                break;
            }

            case 's':
            case 'S': {
                Serial.println("Entrez le serial (hex, ex: 00E48DCA):");
                while (!Serial.available()) delay(10);
                delay(100);
                String input = Serial.readStringUntil('\n');
                input.trim();
                uint32_t new_serial = strtoul(input.c_str(), NULL, 16);
                remote_serial = new_serial;
                Serial.printf("Nouveau serial: 0x%08X\n", remote_serial);
                Serial.println("Entrez le rolling code de depart:");
                while (!Serial.available()) delay(10);
                delay(100);
                input = Serial.readStringUntil('\n');
                input.trim();
                rolling_code = atoi(input.c_str());
                prefs.putUShort("cnt2", rolling_code);
                Serial.printf("Rolling code: %d\n", rolling_code);
                break;
            }

            default:
                break;
        }
    }
}
