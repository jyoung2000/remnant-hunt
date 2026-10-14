#!/usr/bin/env python3
"""Dev tool: the archetypes as Remnant II runs them.

  perks.json       every archetype's five perks (the Warden's four and the Explorer's Scavenger were missing), with
                   their kind of perk (prime / damage / team / utility / relic, which sets when they unlock and
                   upgrade with the archetype's level) and effects written by hand at their top level.
  archetypes.json  the perks in Remnant II's order, the Invoker's Gifted trait, and the five core trait points each
                   primary archetype assigns (core_vigor, core_endurance, core_spirit, core_expertise).
Names come from the player's install (.check/catalog.json, r2text references); the core trait split comes from the
Remnant 2 Toolkit data (.check/toolkit.json); effects are the mod's own approximations where MHW has no equivalent.
usage: author_archetypes.py
"""
import json, os, re

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SHEETS = os.path.join(ROOT, "sheets")
CAT = json.load(open(os.path.join(ROOT, ".check", "catalog.json"), encoding="utf-8"))
TK = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


# Effects at each perk's top level ("exact" where MHW can do what Remnant II does, "approx" otherwise).
PERKS = {
    "spirited": ("relic_heal+15%", "approx"), "liquidcourage": ("dmg+25%;crit+5%", "exact"),
    "panacea": ("res_bleed+15%;res_fire+15%;res_shock+15%;res_acid+15%", "approx"), "goldtolead": ("ammo+15%", "approx"),
    "experimentalist": ("on_relic:dmg+10% 60s", "approx"),
    "tempest": ("mod_gen+50%", "approx"), "amplify": ("dmg_mod+50%;crit+10%", "exact"), "powercreep": ("mod_gen+10%", "approx"),
    "spiritwithin": ("mod_gen+10%", "exact"), "powerleak": ("on_relic:mod_gen+100% 10s", "approx"),
    "diehard": ("low_hp(20):dr+40%", "approx"), "closequarters": ("dmg+35%;crit+5%", "approx"),
    "intimidatingpresence": ("on_skill:dr+10% 15s", "approx"), "powerlifter": ("stamina_cost-50%", "approx"),
    "faceofdanger": ("on_relic:dmg+10% 10s;on_relic:dr+10% 10s", "approx"),
    "hightech": ("on_skill:dmg+25% 15s", "approx"), "metalworker": ("dmg_skill+50%;dmg_ranged+25%;crit+5%", "exact"),
    "magneticfield": ("dr+15%", "approx"), "heavymobility": ("stamina_cost-25%", "approx"), "surplus": ("ammo+15%", "approx"),
    "lucky": ("xp+15%", "approx"), "scavenger": ("on_kill:dmg+5% 20s x5;on_kill:crit+5% 20s", "approx"),
    "metaldetector": ("ammo+10%", "approx"), "prospector": ("xp+5%", "approx"), "selfdiscovery": ("on_relic:dmg+15% 30s", "approx"),
    "loaded": ("on_skill:reload+100% 8s", "approx"), "swiftshot": ("fire_rate+15%;dmg_ranged+25%;crit+5%", "exact"),
    "posseup": ("ammo+30%", "approx"), "quickhands": ("reload+15%", "approx"), "sleightofhand": ("on_relic:dmg_ranged+15% 10s", "exact"),
    "bonded": ("low_hp(30):dr+20%", "approx"), "packhunter": ("dmg+30%;crit+5%", "approx"),
    "heartofthepack": ("stamina_cost-15%", "approx"), "teamwork": ("dr+5%", "approx"), "bestfriend": ("on_relic:dmg+25% 15s", "approx"),
    "deadtorightsv3": ("on_weakspot:skill_dur+20% 10s", "approx"), "deadeye": ("dmg_ranged+40%;weakspot+15%;crit+5%", "exact"),
    "returntosender": ("ammo+25%", "approx"), "urgency": ("on_kill:reload+15% 5s;on_kill:stamina_cost-15% 5s", "approx"),
    "intuition": ("on_relic:skill_dur+50% 10s", "approx"),
    "shadownew": ("dmg+15%", "approx"), "shark": ("on_evade:dmg+7% 10s x5;on_evade:crit+1% 10s x5", "approx"),
    "loophole": ("lifesteal+3%", "approx"), "bypass": ("stamina_cost-15%", "exact"), "override": ("on_relic:dr+15% 10s", "approx"),
    "primeperk_visionary": ("cdr+15%", "approx"), "entranced": ("dmg_skill+30%;dmg_elem+15%;crit+5%", "exact"),
    "communion": ("on_skill:regen+1 15s", "approx"), "mindandbody": ("on_skill:dr+5% 15s;on_skill:stamina_cost-5% 15s", "exact"),
    "soothsayer": ("relic_speed+25%;on_relic:skill_dur+20% 10s", "approx"),
    "regenerator": ("relic_charges+1", "approx"), "invigorated": ("dmg+25%;crit+5%", "exact"),
    "benevolence": ("relic_heal+15%", "exact"), "backbone": ("dr+5%", "approx"), "benefactor": ("relic_speed+20%", "exact"),
    "primeperk_vile": ("dmg_status+15%", "approx"), "wrath": ("vs_status+25%;crit+5%", "approx"), "terrify": ("ammo+10%", "approx"),
    "darkblood": ("res_bleed+25%;res_fire+25%;res_shock+25%;res_acid+25%", "approx"), "purge": ("on_relic:dr+10% 10s", "approx"),
    "ruthless": ("dmg_skill+20%", "approx"), "dominator": ("dmg_mod+35%;dmg_skill+35%;dmg_explosive+35%;crit+5%", "exact"),
    "residue": ("heal+20%", "approx"), "outrage": ("on_skill:lifesteal+3% 10s;on_skill:stamina_cost-15% 10s", "approx"),
    "incite": ("on_relic:dmg_skill+15% 30s", "approx"),
    "dynamic": ("shield+20%", "approx"), "galvanize": ("dmg+25%;crit+5%", "exact"), "safeguard": ("heal+10%", "approx"),
    "contingency": ("low_hp(25):dr+25%", "approx"), "energize": ("on_relic:dr+20% 10s", "approx"),
}
MISSING = {  # perks the sheet did not have: id -> (asset, archetype)
    "scavenger": "Remnant2/Content/World_Base/Items/Archetypes/Explorer/Perks/Perk_Scavenger/Perk_Scavenger.uasset",
    "galvanize": "Remnant2/Content/World_DLC3/Items/Archetypes/Warden/Perks/Perk_Galvanize.uasset",
    "safeguard": "Remnant2/Content/World_DLC3/Items/Archetypes/Warden/Perks/Perk_Safeguard.uasset",
    "contingency": "Remnant2/Content/World_DLC3/Items/Archetypes/Warden/Perks/Perk_Contingency.uasset",
    "energize": "Remnant2/Content/World_DLC3/Items/Archetypes/Warden/Perks/Perk_Energize.uasset",
}
ORDER = {"explorer": ["lucky", "scavenger", "metaldetector", "prospector", "selfdiscovery"],
         "warden": ["dynamic", "galvanize", "safeguard", "contingency", "energize"]}
KINDS = {"Prime Perk": "prime", "Damage Perk": "damage", "Team Perk": "team", "Utility Perk": "utility", "Relic Perk": "relic"}

by_path = {x["path"]: x for x in CAT if x["cat"] == "perk"}
pk = json.load(open(os.path.join(SHEETS, "perks.json"), encoding="utf-8"))
rows = {r["id"]: r for r in pk["rows"]}
for pid, path in MISSING.items():
    if pid not in rows:
        x = by_path[path]
        rows[pid] = {"id": pid, "kind": "perk", "name": {"r2text": {"asset": path, "key": x["texts"][0][0]}}, "icon": "none"}
        pk["rows"].append(rows[pid])
for r in pk["rows"]:
    x = by_path.get(r["name"]["r2text"]["asset"])
    kind = KINDS.get(x["texts"][1][1] if x and len(x["texts"]) > 1 else "", "prime" if r["kind"] == "prime" else "damage")
    r["type"] = kind
    r["kind"] = "prime" if kind == "prime" else "perk"
    if r["id"] not in PERKS:
        raise SystemExit(f"no effects written for perk {r['id']}")
    r["effects"], r["fidelity"] = PERKS[r["id"]]
cols = dict(pk["columns"])
cols = {k: v for k, v in cols.items()}
cols["type"] = "enum:prime|damage|team|utility|relic"
pk["columns"] = {k: cols[k] for k in ["id", "kind", "type", "name", "icon", "effects", "fidelity"]}
pk["rows"] = [{k: r[k] for k in pk["columns"]} for r in pk["rows"]]
json.dump(pk, open(os.path.join(SHEETS, "perks.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)

ar = json.load(open(os.path.join(SHEETS, "archetypes.json"), encoding="utf-8"))
tk = {norm(a["name"]): a for a in TK["archetype"]}
cols = list(ar["columns"].items())
at = [k for k, _ in cols].index("trait") + 1
for k in ("core_vigor", "core_endurance", "core_spirit", "core_expertise"):
    if k not in ar["columns"]:
        cols.insert(at, (k, "num"))
        at += 1
ar["columns"] = dict(cols)
for r in ar["rows"]:
    if r["id"] in ORDER:
        r["perks"] = ORDER[r["id"]]
    if r["id"] == "invoker":
        r["trait"] = "gifted"
    core = {t["name"].lower(): t["amount"] for t in tk[norm(r["id"])]["linkedItems"]["traits"]}
    for k in ("vigor", "endurance", "spirit", "expertise"):
        r["core_" + k] = core.get(k, 0)
    assert sum(r["core_" + k] for k in ("vigor", "endurance", "spirit", "expertise")) == 5, r["id"]
    assert len(r["perks"]) == 5 and len(r["skills"]) == 3, r["id"]
json.dump(ar, open(os.path.join(SHEETS, "archetypes.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print(f"perks {len(pk['rows'])}, archetypes {len(ar['rows'])}: every archetype has 3 skills, 5 perks and its trait")
