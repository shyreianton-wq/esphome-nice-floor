#!/usr/bin/env python3
"""
Decodeur Nice FLOR-S utilisant l'approche Jev1337 (tables precalculees).

Contrairement a l'approche rtl_433 (Feistel + leaf_node[32]) qui ne fait qu'une
transformation partielle, cette approche utilise les tables TABLE_KI[256] et
TABLE_ENCODE[65536] qui ENCAPSULENT la cle Nice complete (precalculees).

Si le serial decode est CONSTANT entre plusieurs appuis sur la meme telecommande,
c'est que cette approche est la bonne.

Usage:
  python nice_flor_s_jev1337.py 0x08E7B967E9ACB0
  python nice_flor_s_jev1337.py 08 E7 B9 67 E9 AC B0
"""

import sys
import os
import re


def load_tables(header_path):
    """Parse nice_flor_s_tables.h pour extraire TABLE_ENCODE et TABLE_KI."""
    with open(header_path, "r") as f:
        content = f.read()

    # Trouver toutes les valeurs hex dans l'ordre
    encode_match = re.search(
        r"NICE_FLOR_S_TABLE_ENCODE\[\]\s*=\s*\{(.*?)\};", content, re.DOTALL
    )
    ki_match = re.search(
        r"NICE_FLOR_S_TABLE_KI\[\]\s*=\s*\{(.*?)\};", content, re.DOTALL
    )

    if not encode_match or not ki_match:
        raise RuntimeError("Tables introuvables dans le header")

    encode_values = re.findall(r"0x([0-9A-Fa-f]+)", encode_match.group(1))
    ki_values = re.findall(r"0x([0-9A-Fa-f]+)", ki_match.group(1))

    table_encode = [int(v, 16) for v in encode_values]
    table_ki = [int(v, 16) for v in ki_values]

    if len(table_encode) != 65536:
        raise RuntimeError(f"TABLE_ENCODE a {len(table_encode)} entries, attendu 65536")
    if len(table_ki) != 256:
        raise RuntimeError(f"TABLE_KI a {len(table_ki)} entries, attendu 256")

    # Construire la table inverse: enccode -> liste de codes possibles
    decode_map = {}
    for code, enccode in enumerate(table_encode):
        decode_map.setdefault(enccode, []).append(code)

    return table_encode, table_ki, decode_map


def parse_input(args):
    raw = "".join(args).replace("0x", "").replace("0X", "").replace(" ", "")
    if len(raw) != 14:
        raise ValueError(
            f"Entree doit faire 14 hex digits (7 octets), recu {len(raw)}: {raw!r}"
        )
    return [int(raw[i : i + 2], 16) for i in range(0, 14, 2)]


def decode_jev1337(encbuff, table_ki, decode_map):
    """
    Decode selon l'algo Jev1337.
    Pour chaque candidat code possible, calcule ki et le serial associe.
    """
    button = encbuff[0] & 0x0F
    enccode = (encbuff[2] << 8) | encbuff[3]

    candidates = decode_map.get(enccode, [])
    if not candidates:
        return {
            "button": button,
            "enccode": enccode,
            "candidates": [],
        }

    decoded = []
    for code in candidates:
        ki = table_ki[code & 0xFF] ^ (enccode & 0xFF)
        sn0 = encbuff[6] ^ ki
        sn1 = encbuff[5] ^ ki
        sn2 = encbuff[4] ^ ki
        sn3 = (encbuff[1] & 0x0F) ^ (ki & 0x0F)
        serial = (sn3 << 24) | (sn2 << 16) | (sn1 << 8) | sn0
        decoded.append({
            "code": code,
            "ki": ki,
            "serial": serial,
        })

    return {
        "button": button,
        "enccode": enccode,
        "candidates": decoded,
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    here = os.path.dirname(os.path.abspath(__file__))
    header = os.path.join(here, "include", "nice_flor_s_tables.h")
    print(f"Chargement des tables depuis {header} ...", file=sys.stderr)
    table_encode, table_ki, decode_map = load_tables(header)
    print(f"  TABLE_ENCODE: {len(table_encode)} entries", file=sys.stderr)
    print(f"  TABLE_KI: {len(table_ki)} entries", file=sys.stderr)
    print(f"  decode_map (uniques): {len(decode_map)} enccodes\n", file=sys.stderr)

    encbuff = parse_input(sys.argv[1:])
    raw_hex = " ".join(f"{x:02X}" for x in encbuff)
    print(f"Trame  : {raw_hex}")

    result = decode_jev1337(encbuff, table_ki, decode_map)
    print(f"  Button   : 0x{result['button']:X}")
    print(f"  Enccode  : 0x{result['enccode']:04X}")
    print(f"  {len(result['candidates'])} candidat(s) code:")
    for c in result["candidates"]:
        print(
            f"    code={c['code']:5d} (0x{c['code']:04X})  "
            f"ki=0x{c['ki']:02X}  serial=0x{c['serial']:08X}"
        )


if __name__ == "__main__":
    main()
