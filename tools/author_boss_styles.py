#!/usr/bin/env python3
"""Dev tool: how each Remnant boss fights (bosses.json style / prefer_m / combo), and the difficulty table.

Styles (bosses.cpp): brute closes in and hits hard; hunter circles at a distance, pounces and backs off; duelist
stays close and chains attacks, stepping around between them; ranged keeps its distance and shoots, backing away when
pressed; caster holds a middle distance and favours area attacks; juggernaut advances relentlessly and shrugs off
flinches; sentinel does not move and turns to face. Chosen from each boss's Remnant II fight and its attack list.
difficulties.json: Remnant II's four difficulties for the Remnant monsters, chosen at Ward 13.
usage: author_boss_styles.py
"""
import json, os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
P = os.path.join(ROOT, "sheets", "bosses.json")
STYLE = {
    "corrupted_ravager": "hunter", "nightweaver": "caster", "faerin": "duelist", "faelin": "duelist",
    "shahala": "sentinel", "tal_ratha": "brute", "one_true_king": "duelist", "lydusa": "hunter",
    "alepsis_taura": "sentinel", "annihilation": "juggernaut", "abomination": "juggernaut", "amalgam": "brute",
    "bloat_king": "caster", "custodians_eye": "ranged", "fae_archon": "duelist", "harvester": "brute",
    "hatchery": "caster", "impaler": "duelist", "kiln": "juggernaut", "phantom": "ranged", "root_horror": "brute",
    "shifting_soldier": "duelist", "shrewd": "hunter", "sunken_witch": "caster", "mother_mind": "ranged",
    "legion": "caster", "gwendil": "duelist", "root_mantis": "hunter", "venom": "hunter",
}
PREFER = {"brute": 2, "hunter": 10, "duelist": 4, "ranged": 17, "caster": 12, "juggernaut": 2, "sentinel": 0}
COMBO = {"brute": 0.2, "hunter": 0.3, "duelist": 0.5, "ranged": 0.25, "caster": 0.15, "juggernaut": 0.3, "sentinel": 0.35}
sh = json.load(open(P, encoding="utf-8"))
cols = list(sh["columns"].items())
at = [k for k, _ in cols].index("zones")
for k, v in (("style", "enum:brute|hunter|duelist|ranged|caster|juggernaut|sentinel"), ("prefer_m", "num"), ("combo", "num")):
    if k not in sh["columns"]:
        cols.insert(at, (k, v))
        at += 1
sh["columns"] = dict(cols)
for r in sh["rows"]:
    st = STYLE.get(r["id"], "brute")
    if r["walk_mps"] <= 0:
        st = "sentinel"
    r["style"], r["prefer_m"], r["combo"] = st, PREFER[st], COMBO[st]
json.dump(sh, open(P, "w", encoding="utf-8"), indent=1, ensure_ascii=False)

ICON = "Remnant2/Content/UI2/UI_Icons/UI_Icons_Travel/T_UI_Travel_Icon_Difficulty_0{}.uasset"
rows = [
    # id, label, hp, damage, tempo, stagger, enrage at, combo bonus, status, experience
    ("survivor", "Survivor", 0.75, 0.70, 0.85, 0.80, 0.0, -0.10, 0.70, 0.8),
    ("veteran", "Veteran", 1.00, 1.00, 1.00, 1.00, 0.30, 0.00, 1.00, 1.0),
    ("nightmare", "Nightmare", 1.40, 1.45, 1.15, 1.25, 0.45, 0.10, 1.25, 1.3),
    ("apocalypse", "Apocalypse", 1.90, 2.00, 1.30, 1.50, 0.60, 0.20, 1.50, 1.6),
]
diff = {"sheet": "difficulties", "phase": "v0.2",
        "columns": {"id": "id", "label": "str", "icon": "r2tex|none", "hp_mult": "num", "dmg_mult": "num", "tempo": "num",
                    "stagger_mult": "num", "enrage_frac": "num", "combo_bonus": "num", "status_mult": "num", "xp_mult": "num"},
        "rows": [{"id": i, "label": l, "icon": {"r2tex": ICON.format(n + 1)}, "hp_mult": h, "dmg_mult": d, "tempo": te,
                  "stagger_mult": sg, "enrage_frac": en, "combo_bonus": cb, "status_mult": stt, "xp_mult": xp}
                 for n, (i, l, h, d, te, sg, en, cb, stt, xp) in enumerate(rows)]}
json.dump(diff, open(os.path.join(ROOT, "sheets", "difficulties.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
from collections import Counter
print(Counter(r["style"] for r in sh["rows"]))
