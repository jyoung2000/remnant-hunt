#!/usr/bin/env python3
"""Dev tool: the sheets for Remnant II's traits, prisms, prism fusions and legendary prism bonuses.

  traits.json       every trait in the player's Remnant II install (.check/catalog.json): core traits are owned from
                    the start, archetype traits come with the archetype, the others are learned from the Elder Melder
                    or from Remnant bosses. effects are written at trait level 10 and scaled by level in the mod.
  prisms.json       the seven prisms (Remnant2/Content/Events/Paragon/PrismStone/UniquePrisms), from the Elder Melder.
  fusions.json      the 23 fusions: two relic fragments (fragments.json rows) that merge in a prism.
  legendaries.json  the legendary prism bonuses (PrismStoneMythicDataTable); those MHW has nothing to act on have no
                    effects and are never offered.
Names and descriptions are references into the player's install (r2text): no Remnant II text is copied. Numbers come
from the Remnant 2 Toolkit data (.check/toolkit.json) and are written in the mod's effect language by hand below.
usage: author_progression.py
"""
import json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SHEETS = os.path.join(ROOT, "sheets")
CAT = json.load(open(os.path.join(ROOT, ".check", "catalog.json"), encoding="utf-8"))
TK = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))
PT = json.load(open(os.path.join(ROOT, ".check", "prism_texts.json"), encoding="utf-8"))
PRISM_DIR = "Remnant2/Content/Events/Paragon/PrismStone/"
TABLE = PRISM_DIR + "PrismStoneDataTable.uasset"
MYTHIC = PRISM_DIR + "PrismStoneMythicDataTable.uasset"
SHOP = {"tier": "num", "menu": "enum:smithy|melder|provisions|archetype", "zenny": "num", "recipe": "recipe"}
TIER_ZENNY = {1: 1500, 2: 4000, 3: 12000, 4: 25000, 5: 50000}
TIER_RECIPE = {
    1: ["ore_low*3", "bone_low*2"],
    2: ["ore_low2*4", "bone_low2*3", "crystal_low*2"],
    3: ["ore_high*5", "bone_high*4", "part_high*2"],
    4: ["ore_high2*6", "elder_high*3", "part_high2*3"],
    5: ["ore_master*6", "bone_master*4", "elder_master*3", "gem_master*1"],
}


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def shop(row, tier, menu, sell=True):
    row.update({"tier": tier, "menu": menu, "zenny": TIER_ZENNY[tier] if sell else 0,
                "recipe": TIER_RECIPE[tier] if sell else ["none"]})
    return row


# ---------------------------------------------------------------- traits (effects at level 10)
TRAITS = {
    # core: owned from the start
    "vigor": ("max_hp+30", "exact"), "endurance": ("stamina_max+30", "exact"), "spirit": ("mod_gen+20%", "exact"),
    "expertise": ("cdr+20%", "exact"),
    # archetype traits (the primary archetype's, at full strength)
    "affliction": ("status_dur+100%", "exact"), "ammoreserves": ("ammo+50%", "exact"), "barrier": ("shield+15%", "exact"),
    "flashcaster": ("cdr+10%;mod_gen+10%", "approx"), "fortify": ("dr+10%", "approx"), "kinship": ("dr+5%", "fallback"),
    "longshot": ("dmg_ranged+10%", "approx"), "potency": ("heal+20%", "approx"), "regrowth": ("regen+1.5", "exact"),
    "strongback": ("stamina_cost-15%", "approx"), "swiftness": ("stamina_cost-7.5%", "approx"),
    "triage": ("heal+50%", "exact"), "untouchable": ("on_evade:dr+30% 2s", "approx"),
    # traits learned from the Elder Melder or Remnant bosses
    "amplitude": ("dmg_skill+10%", "approx"), "arcanestrike": ("mod_gen+15%", "approx"),
    "barkskin": ("dr+10%", "exact"), "bloodbond": ("dr+5%", "approx"), "bloodstream": ("regen+1", "approx"),
    "chakra": ("mod_dur+30%", "exact"), "darkpact": ("lifesteal+2%", "approx"), "fitness": ("stamina_cost-10%", "approx"),
    "footwork": ("accuracy+15%", "approx"), "glutton": ("relic_speed+35%", "exact"), "handling": ("accuracy+30%", "exact"),
    "insight": ("mod_gen+15%", "approx"), "leech": ("lifesteal+2%", "approx"),
    "perception": ("fire_rate+3%;reload+3%", "approx"), "preservation": ("shield+10%", "approx"),
    "recovery": ("stamina_regen+90%", "exact"), "resolute": ("dr+3%", "fallback"), "resonance": ("dmg_skill+5%", "fallback"),
    "revivalist": ("heal+10%", "fallback"), "rugged": ("dmg_skill+10%", "fallback"), "scholar": ("xp+15%", "exact"),
    "shadeskin": ("res_fire+20%;res_shock+20%;res_acid+20%", "approx"), "siphoner": ("lifesteal+3%", "exact"),
    "softbreak": ("dmg+3%", "fallback"), "wayfarer": ("stamina_cost-5%", "approx"),
}

traits = []
for it in sorted((x for x in CAT if x["cat"] == "trait"), key=lambda x: x["path"]):
    base = it["path"].rsplit("/", 1)[1][:-7]
    tid = base.replace("Trait_", "").lower()
    texts = [t for t in it["texts"] if "Needs Lore" not in t[1] and "Needs Flavor" not in t[1]]
    if tid not in TRAITS or not texts:
        print("skipped trait", tid)
        continue
    kind_text = texts[1][1] if len(texts) > 1 else ""
    kind = "core" if kind_text == "Core Trait" else "archetype" if kind_text == "Archetype Trait" else "trait"
    desc = next((t for t in texts[1:] if t[1] not in ("Core Trait", "Archetype Trait", "Trait")), None)
    eff, fid = TRAITS[tid]
    row = {"id": tid, "kind": kind, "name": {"r2text": {"asset": it["path"], "key": texts[0][0]}},
           "desc": {"r2text": {"asset": it["path"], "key": desc[0]}} if desc else "none", "icon": "none",
           "effects": eff, "fidelity": fid}
    traits.append(shop(row, 2 if fid != "fallback" else 1, "melder", sell=kind == "trait"))
    if kind != "trait":
        row["menu"] = "archetype"

# ---------------------------------------------------------------- prisms, fusions, legendaries
PRISMS = ["Greed", "Hatred", "Jealousy", "Lethargy", "Passion", "Pride", "Voracity"]
PRISM_TEXTS = json.load(open(os.path.join(ROOT, ".check", "prism_texts.json"), encoding="utf-8"))
prisms = []
for i, p in enumerate(PRISMS):
    asset = f"{PRISM_DIR}UniquePrisms/PrismOf{p}.uasset"
    key = PRISM_TEXTS.get("names", {}).get(asset)
    if not key:
        raise SystemExit(f"prism text key missing for {asset}: run the device step that fills .check/prism_texts.json")
    row = {"id": p.lower(), "name": {"r2text": {"asset": asset, "key": key}}, "icon": "none"}
    prisms.append(shop(row, 3 if i < 4 else 4, "melder"))

frag_rows = {r["id"]: r for r in json.load(open(os.path.join(SHEETS, "fragments.json"), encoding="utf-8"))["rows"]}
by_name = {}
for r in frag_rows.values():
    by_name[norm(r["id"])] = r["id"]
TK_FRAG = {f["id"]: f for f in TK["relic-fragment"]}
SLUG_FIX = {"armorbonus": "armor_effectiveness", "armor": "armor_flat", "healthbonus": "health", "health": "health_flat",
            "stamina": "stamina_flat", "staminabonus": "stamina", "heatreduction": "heat_gen_reduction",
            "firearmchargetime": "ranged_charge_speed", "totalmeleespeed": "melee_attack_speed",
            "greyhealthconversion": "grey_health_rate", "weaponidealrange": "ideal_range", "firearmswapspeed": "weapon_swap_speed",
            "shieldamount": "shield_effectiveness", "usespeed": "consumable_speed", "healthregeneration": "health_regen",
            "firerate": "ranged_fire_rate", "firearmchargespeed": "ranged_charge_speed", "castspeed": "cast_speed",
            "statuseffectdamage": "status_damage", "modpowergeneration": "mod_generation", "skillcooldown": "skill_cooldown"}


def frag_of(tk_id):
    f = TK_FRAG.get(tk_id)
    if not f:
        return None
    slug = re.sub(r"^RelicFragment_|_C$", "", f.get("saveFileSlug") or "")
    for cand in (norm(slug), norm(f["name"])):
        if cand in by_name:
            return by_name[cand]
        if cand in SLUG_FIX:
            return SLUG_FIX[cand]
    return None


table = dict((v, k) for k, v in PT[TABLE])
fusions = []
for f in TK["fusion"]:
    a, b = (frag_of(x) for x in f["fragmentIds"])
    key = table.get(f["name"])
    if not a or not b or not key:
        print("skipped fusion", f["name"], a, b, bool(key))
        continue
    fusions.append({"id": norm(f["name"]), "name": {"r2text": {"asset": TABLE, "key": key}}, "a": a, "b": b})

LEGENDARY = {
    "Master Killer": ("weakspot+35%", "exact"), "Full Hearted": ("relic_charges+3", "approx"),
    "Outlaw": ("reload+50%", "approx"), "Power Trip": ("mod_gen+50%", "approx"), "Unbridled": ("cdr+50%", "exact"),
    "Defense Measures": ("shield+100%", "exact"), "Reverberation": ("stamina_regen+300%", "approx"),
    "Overpowered": ("mod_gen+50%", "exact"), "Gigantic": ("max_hp+50;stamina_max+50", "exact"),
    "Traitor": ("none", "exact"),  # core traits at level 10 (progress.cpp)
    "Prime Time": ("none", "exact"),  # the second archetype's prime perk (remnant.cpp)
    "Insult to Injury": ("on_hit:inflict:exposed 0 8s 20%", "approx"),
    "Brutality": ("dmg+100%;dr-100%", "exact"), "Wrecking Ball": ("dmg_explosive+50%", "approx"),
    "Steel Plating": ("dr+15%", "approx"), "Critical Situation": ("crit+25%", "approx"),
    "Impervious": ("dr+30%", "exact"), "Jack of All Trades": ("dmg+40%", "exact"), "Physician": ("heal+100%", "exact"),
    "Power Fantasy": ("dmg_explosive+30%;crit_dmg+15%", "approx"), "Sharpshooter": ("dmg_ranged+60%", "exact"),
    "Size Matters": ("mag+100%", "exact"), "Sadistic": ("dmg_status+50%", "approx"),
    "Vaccinated": ("res_bleed+100%;res_fire+100%;res_shock+100%;res_acid+100%", "approx"),
    "Peak Conditioning": ("stamina_cost-100%", "exact"), "Speed Demon": ("stamina_cost-30%", "fallback"),
}
SPECIAL = {"Traitor", "Prime Time"}
mythic = PT[MYTHIC]
legendaries = []
for i in range(0, len(mythic) - 1, 2):
    (nk, name), (dk, _) = mythic[i], mythic[i + 1]
    eff, fid = {norm(k): v for k, v in LEGENDARY.items()}.get(norm(name), ("none", "fallback"))
    legendaries.append({"id": norm(name), "name": {"r2text": {"asset": MYTHIC, "key": nk}},
                        "desc": {"r2text": {"asset": MYTHIC, "key": dk}}, "effects": eff, "fidelity": fid,
                        "special": "yes" if norm(name) in {norm(x) for x in SPECIAL} else "no"})


def write(name, columns, rows):
    json.dump({"sheet": name, "phase": "v0.2", "columns": columns, "rows": rows},
              open(os.path.join(SHEETS, name + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)


write("traits", {"id": "id", "kind": "enum:core|archetype|trait", "name": "r2text", "desc": "r2text|none",
                 "icon": "r2tex|none", "effects": "effects", "fidelity": "enum:exact|approx|fallback", **SHOP}, traits)
write("prisms", {"id": "id", "name": "r2text", "icon": "r2tex|none", **SHOP}, prisms)
write("fusions", {"id": "id", "name": "r2text", "a": "ref:fragments", "b": "ref:fragments"}, fusions)
write("legendaries", {"id": "id", "name": "r2text", "desc": "r2text|none", "effects": "effects|none",
                      "fidelity": "enum:exact|approx|fallback", "special": "enum:no|yes"}, legendaries)
print(f"traits {len(traits)} ({sum(t['kind'] == 'trait' for t in traits)} to learn), prisms {len(prisms)}, "
      f"fusions {len(fusions)}, legendaries {len(legendaries)} ({sum(l['effects'] != 'none' or l['special'] == 'yes' for l in legendaries)} offered)")
