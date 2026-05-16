#!/usr/bin/env python3
"""
Nice FLOR-S frame decoder — port verbatim de rtl_433/src/devices/nice_flor_s.c

Prend en entree une trame de 52 bits captee par notre CC1101 (format encbuff
serialise en hex, ex: 0x08DDC67F93D6CA = 7 octets ou le bouton est dans le
nibble bas du premier octet) et applique l'algo de rtl_433 pour en extraire
button, count, serial (chiffre) et code (chiffre).

Usage:
  python nice_flor_s_decode.py 0x08DDC67F93D6CA
  python nice_flor_s_decode.py 08DDC67F93D6CA
  python nice_flor_s_decode.py 08 DD C6 7F 93 D6 CA
"""

import sys


# Keystore 32 octets - copie verbatim de rtl_433/src/devices/nice_flor_s.c
LEAF_NODE = [
    25, 5, 63, 97, 203, 109, 69, 10, 3, 7, 64, 5, 71, 134, 180, 74,
    41, 158, 102, 199, 93, 118, 175, 101, 60, 77, 143, 174, 103, 148, 29, 85,
]


def xor_array(p, k):
    """Equivalent du xor_array de rtl_433: XOR p[1..5] avec k."""
    for i in range(1, 6):
        p[i] = (p[i] ^ k) & 0xFF


def pl_reverse(p):
    """
    Port verbatim de rtl_433::pl_reverse().
    Modifie p (liste de 6 octets) en place. Retourne le code 16 bits.
    """
    # Pre-shuffle
    k = (~p[4]) & 0xFF
    p[5] = (~p[5]) & 0xFF
    p[4] = (~p[2]) & 0xFF
    p[2] = (~p[0]) & 0xFF
    p[0] = k
    k = (~p[3]) & 0xFF
    p[3] = (~p[1]) & 0xFF
    p[1] = k

    for y in range(2):
        k = (LEAF_NODE[p[0] >> 3] + 0x25) & 0xFF
        xor_array(p, k)
        p[5] &= 0x0F
        p[0] = (p[0] ^ (k & 0x07)) & 0xFF

        k = LEAF_NODE[p[0] & 0x1F]
        xor_array(p, k)
        p[5] &= 0x0F
        p[0] = (p[0] ^ (k & 0xE0)) & 0xFF

        if y == 0:
            p[0], p[1] = p[1], p[0]

    return ((p[1] << 8) | p[0]) & 0xFFFF


def encbuff_to_rtl_bytes(encbuff):
    """
    Notre encbuff stocke le bouton dans le nibble BAS de encbuff[0].
    rtl_433 attend le bouton dans le nibble HAUT de b[0]. C'est juste
    un decalage de 4 bits de tout le buffer.
    """
    if len(encbuff) != 7:
        raise ValueError(f"encbuff doit faire 7 octets, recu {len(encbuff)}")
    b = [0] * 7
    b[0] = ((encbuff[0] & 0x0F) << 4) | ((encbuff[1] >> 4) & 0x0F)
    for i in range(1, 6):
        b[i] = ((encbuff[i] & 0x0F) << 4) | ((encbuff[i + 1] >> 4) & 0x0F)
    b[6] = (encbuff[6] & 0x0F) << 4  # nibble bas = padding (les 52 bits utiles s'arretent ici)
    return b


def decode_nice_flor_s(encbuff):
    """
    Decode une trame Nice FLOR-S de 52 bits.

    encbuff: liste de 7 octets dans notre format (bouton dans nibble bas
    du premier octet).

    Retourne un dict {button, count, serial, code, raw_hex}.
    """
    # 1) Conversion vers le format de bitbuffer rtl_433
    b = encbuff_to_rtl_bytes(encbuff)

    # 2) bitbuffer_invert: inversion bit-a-bit de TOUS les octets
    b_inv = [(~x) & 0xFF for x in b]

    # 3) Reconstruction du t_buf via decalage de nibble (rtl_433 lignes 92-95)
    t_buf = [0] * 7
    t_buf[0] = (b_inv[0] >> 4) & 0x0F
    for i in range(6):
        t_buf[i + 1] = ((b_inv[i] << 4) & 0xF0) | ((b_inv[i + 1] >> 4) & 0x0F)

    # 4) Mapping vers p[] (rtl_433 lignes 97-102)
    p = [0] * 6
    p[5] = t_buf[1] & 0x0F
    p[4] = t_buf[2]
    p[3] = t_buf[3]
    p[2] = t_buf[4]
    p[1] = t_buf[5]
    p[0] = t_buf[6]

    # 5) Dechiffrement Feistel
    code = pl_reverse(p)

    # 6) Extraction des champs (rtl_433 lignes 106-108)
    serial = (p[5] << 24) | (p[4] << 16) | (p[3] << 8) | p[2]
    button = t_buf[0] & 0x0F
    count = ((t_buf[1] >> 4) & 0x0F) ^ (t_buf[0] & 0x0F) ^ 0x0F

    return {
        "button": button,
        "count": count,
        "serial": serial,
        "code": code,
        "raw_hex": " ".join(f"{x:02X}" for x in encbuff),
    }


def parse_input(args):
    """Accepte 0x08DDC67F93D6CA, 08DDC67F93D6CA, ou 08 DD C6 7F 93 D6 CA."""
    raw = "".join(args).replace("0x", "").replace("0X", "").replace(" ", "")
    if len(raw) != 14:
        raise ValueError(
            f"Entree doit faire 14 hex digits (7 octets), recu {len(raw)}: {raw!r}"
        )
    return [int(raw[i : i + 2], 16) for i in range(0, 14, 2)]


def main():
    args = sys.argv[1:]
    rtl_only = False
    if args and args[0] in ("--rtl", "-r"):
        rtl_only = True
        args = args[1:]

    if not args:
        print(__doc__)
        print("Options:")
        print("  --rtl  Affiche la commande rtl_433 -y equivalente puis decode")
        sys.exit(1)

    encbuff = parse_input(args)
    rtl_bytes = encbuff_to_rtl_bytes(encbuff)
    rtl_hex = "".join(f"{x:02X}" for x in rtl_bytes)[:13]  # 52 bits = 13 hex digits

    if rtl_only:
        print(f'rtl_433 -R 169 -y "{{52}} 0x{rtl_hex} {{0}}"')
        return

    result = decode_nice_flor_s(encbuff)
    print(f"Trame encbuff  : {result['raw_hex']}")
    print(f"Equivalent rtl_433 :")
    print(f'  rtl_433 -R 169 -y "{{52}} 0x{rtl_hex} {{0}}"')
    print(f"Decode :")
    print(f"  Button : {result['button']}")
    print(f"  Count  : {result['count']}")
    print(f"  Serial : 0x{result['serial']:08X}  ({result['serial']})")
    print(f"  Code   : 0x{result['code']:04X}  ({result['code']})")


if __name__ == "__main__":
    main()
