#!/usr/bin/env python3
"""
Gera main/core/ecu_profiles_gen.c a partir de arquivos JSON de ECU do ddt4all.

Uso:
    python3 -I tools/ddt2profile.py saida.c ECU1.json [ECU2.json ...]

Para cada ECU ele procura, nas requisicoes 21xx (ReadDataByLocalIdentifier
do KWP2000 Renault), os campos que o painel usa (rpm, velocidade, temperatura,
MAP, borboleta, bateria, tempo de injecao, consumo instantaneo, torque) e
escolhe o MENOR conjunto de requisicoes que cobre o maximo de campos.

Formula do ddt4all para valores escalados:  valor = (bruto * step + offset) / divideby
"firstbyte" e 1-based e conta a partir do byte de resposta positiva (0x61),
ou seja, em "61 A9 xx yy", xx e o byte 3.

Os arquivos JSON sao tratados como dados: so lemos numeros e nomes.
"""
import json
import re
import sys
import os

FIELDS = [
    # (id C,            regex do nome no ddt4all)
    ("F_RPM",     r"^r[ée]gime moteur( moyen)?$"),
    ("F_SPEED",   r"^vitesse (du )?v[ée]hicule$"),
    ("F_COOLANT", r"^temp[ée]rature (d.)?eau( mesur[ée]e)?$"),
    ("F_IAT",     r"^temp[ée]rature (d.)?air( mesur[ée]e)?$"),
    ("F_MAP",     r"^pression collecteur( absolue)?( mesur[ée]e)?$"),
    ("F_TPS",     r"^position (mesur[ée]e du potentiom[èe]tre papillon|papillon transmise.*)$"),
    ("F_VBAT",    r"^tension batterie( mesur[ée]e)?$"),
    ("F_INJ_MS",  r"^temps (d.)?injection r[ée]el$"),
    ("F_FUEL_LH", r"^consommation (instantan[ée]e de carburant|carburant instantan[ée]e)$"),
    ("F_TORQUE",  r"^couple moteur effectif$"),
    ("F_BARO",    r"^pression atmosph[ée]rique$"),
]


# Campos extras (sonda lambda, correcao de richesse, malha fechada...). Nao mexem
# na escolha das requisicoes: so entram se estiverem em uma requisicao ja escolhida
# para os campos acima. "." casa a letra acentuada, para nao depender da codificacao.
EXTRA_FIELDS = [
    ("F_O2_UP",   r"^tension de la sonde . oxyg.ne amont$"),
    ("F_O2_DOWN", r"^tension de la sonde . oxyg.ne aval$"),
    ("F_RICH",    r"^facteur enrichissement r.gulation (de )?richesse$"),
    ("F_LOOP",    r"^r.gulation de richesse active$"),
    ("F_ADV",     r"^avance . l.allumage( . chaque pmh)?$"),
    ("F_KNOCK",   r"^valeur max corr avance due au cliquetis$"),
    ("F_AC_REQ",  r"^demande de mise en route du compresseur de clim$"),
    ("F_AC_CMD",  r"^etat de la commande du compresseur de climatisation$"),
    ("F_FAN1",    r"^groupe motoventilateur actif en vitesse 1$"),
    ("F_FAN2",    r"^groupe motoventilateur actif en vitesse 2$"),
    ("F_PS_LEVEL", r"^niveau .lectrique du pressostat de direction assist.e$"),
    ("F_PS_CONN",  r"^pressostat de direction assist.e connect.e?$"),
    ("F_IDLE_ACTIVE", r"^r.gulation ralenti active$"),
    ("F_IDLE_ACC", r"^demande de ralenti acc.l.r.$"),
    ("F_IDLE_RCO", r"^rco th.orique r.gulation ralenti$"),
    ("F_IDLE_SET", r"^r.gime de consigne r.gulation ralenti$"),
]
FLAG_FIELDS = {"F_LOOP", "F_AC_REQ", "F_AC_CMD", "F_FAN1", "F_FAN2",
               "F_PS_LEVEL", "F_PS_CONN", "F_IDLE_ACTIVE", "F_IDLE_ACC"}   # 1 bit (ddt4all: bit 0 = o mais significativo)
# Estes ficam em outra requisicao (21A4 e 21A7 na Sirius 32): se nenhuma das ja escolhidas
# os traz, acrescenta mais uma (maximo 6 no total).
NEED_LID_IDS = {"F_AC_CMD", "F_FAN1", "F_FAN2", "F_IDLE_RCO", "F_IDLE_SET"}
BASE_IDS = {fid for fid, _ in FIELDS}
ALL_FIELDS = FIELDS + EXTRA_FIELDS


def norm(s):
    return re.sub(r"\s+", " ", s.strip().lower())


def scale_of(dd, unit_hint=None):
    step = float(dd.get("step", 1.0))
    off = float(dd.get("offset", 0.0))
    div = float(dd.get("divideby", 1.0)) or 1.0
    unit = dd.get("unit", "")
    # normaliza tempo de injecao para ms
    if unit in ("µs", "us"):
        step /= 1000.0
        off /= 1000.0
    return step, off, div, unit


def extract(path):
    with open(path, encoding="utf-8") as fh:
        d = json.load(fh)
    data = d.get("data", {})
    obd = d.get("obd", {})
    cands = {}  # field -> list of (lid, firstbyte, bits, signed, step, off, div)
    for r in d.get("requests", []):
        sb = r.get("sentbytes", "").upper()
        if not re.fullmatch(r"21[0-9A-F]{2}", sb):
            continue
        lid = int(sb[2:], 16)
        minbytes = int(r.get("minbytes", 0))
        for name, it in r.get("receivebyte_dataitems", {}).items():
            dd = data.get(name, {})
            bits = int(dd.get("bitscount", 8))
            bitoff = int(it.get("bitoffset", 0))
            if bits == 1:
                pass                    # 1 bit: so vale para os campos de flag
            elif "bitoffset" in it or bits not in (8, 16):
                continue
            n = norm(name)
            n = re.sub(r" copie trame \d+$", "", n)
            for fid, rx in ALL_FIELDS:
                if (fid in FLAG_FIELDS) != (bits == 1):
                    continue
                if re.match(rx, n):
                    step, off, div, unit = scale_of(dd)
                    cands.setdefault(fid, []).append(dict(
                        lid=lid, fb=int(it["firstbyte"]), bits=bits, bitoff=bitoff,
                        signed=bool(dd.get("signed", False)),
                        step=step, off=off, div=div, unit=unit,
                        copy=("copie" in norm(name)), minbytes=minbytes))
    # escolha gulosa de requisicoes
    chosen = []
    remaining = {f for f in cands if f in BASE_IDS}
    while remaining:
        best, best_cov = None, set()
        lids = {c["lid"] for f in remaining for c in cands[f]}
        for lid in sorted(lids):
            cov = {f for f in remaining if any(c["lid"] == lid for c in cands[f])}
            if len(cov) > len(best_cov):
                best, best_cov = lid, cov
        if best is None:
            break
        chosen.append(best)
        remaining -= best_cov
    missing = {f for f in NEED_LID_IDS
               if f in cands and not any(c["lid"] in chosen for c in cands[f])}
    while missing and len(chosen) < 6:
        best, best_cov = None, set()
        for lid in sorted({c["lid"] for f in missing for c in cands[f]}):
            cov = {f for f in missing if any(c["lid"] == lid for c in cands[f])}
            if len(cov) > len(best_cov):
                best, best_cov = lid, cov
        if best is None:
            break
        chosen.append(best)
        missing -= best_cov
    fields = {}
    for fid, lst in cands.items():
        for lid in chosen:
            opts = [c for c in lst if c["lid"] == lid]
            if opts:
                opts.sort(key=lambda c: c["copy"])
                fields[fid] = opts[0]
                break
    ids = [dict(supplier=a.get("supplier", a.get("supplier_code", "")),
                soft=a.get("soft", a.get("soft_version", "")),
                version=a.get("version", ""),
                diag=a.get("diagversion", a.get("diagnostic_version", "")))
           for a in d.get("autoidents", [])]
    return dict(name=d.get("ecuname", os.path.basename(path)), file=os.path.basename(path),
                fastinit=bool(obd.get("fastinit", True)), lids=chosen, fields=fields, ids=ids)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    profs = [extract(p) for p in sys.argv[2:]]
    profs = [p for p in profs if "F_RPM" in p["fields"]]
    L = []
    L.append("// GERADO AUTOMATICAMENTE por tools/ddt2profile.py a partir das definicoes do ddt4all.")
    L.append("// Nao edite a mao: rode o script de novo.")
    L.append('#include "ecu_profiles.h"\n')
    for i, p in enumerate(profs):
        L.append(f"static const ecu_ident_t ids_{i}[] = {{")
        for a in p["ids"]:
            L.append(f"    {{ {cstr(a['supplier'])}, {cstr(a['soft'])}, {cstr(a['version'])}, {cstr(str(a['diag']))} }},")
        L.append("    { NULL, NULL, NULL, NULL }\n};")
        L.append(f"static const ecu_field_t fields_{i}[] = {{")
        for fid, c in sorted(p["fields"].items()):
            if fid == "F_TPS" and c["unit"].startswith("°"):
                fid = "F_TPS_DEG"  # borboleta em graus (nao %)
            L.append(f"    {{ {fid}, 0x{c['lid']:02X}, {c['fb']}, {c['bits']}, {int(c['signed'])}, "
                     f"{c['step']!r}f, {c['off']!r}f, {c['div']!r}f, {c['bitoff']} }},")
        L.append("};")
    L.append("\nconst ecu_profile_t g_ecu_profiles[] = {")
    for i, p in enumerate(profs):
        lids = ", ".join(f"0x{x:02X}" for x in p["lids"][:6])
        L.append(f"    {{ {cstr(p['name'])}, {cstr(p['file'])}, {str(p['fastinit']).lower()}, "
                 f"{{ {lids} }}, {len(p['lids'][:6])}, fields_{i}, "
                 f"sizeof(fields_{i})/sizeof(fields_{i}[0]), ids_{i} }},")
    L.append("};")
    L.append(f"const int g_ecu_profile_count = {len(profs)};")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    for p in profs:
        print(f"{p['name']:45s} fast={p['fastinit']!s:5s} lids={[hex(x) for x in p['lids']]} campos={sorted(p['fields'])}")


if __name__ == "__main__":
    main()
