#!/usr/bin/env python3
"""Dev tool: give the arsenal Remnant II's status effects (BLEEDING, BURNING, CORRODED, OVERLOADED, EXPOSED, SLOW),
resistances, cures and the archetype skills' real mechanics, in the mod's effect language.

Reads the item descriptions in the Remnant 2 Toolkit data (.check/toolkit.json, the numbers only; no text is copied):
  - mods: "shots apply X, dealing N damage over Ts ... for Ds" -> use:imbue x N Ts Ds;
          explosions / auras / orbs applying X -> use:inflict x N Ts Rm[@target]
  - guns, rings, amulets, mutators: "hits / weakspot hits / on dodge ... apply X" -> [trigger:]inflict:x N Ts[ C%]
          "damage to X targets by N%" -> vs_x+N%
  - consumables: Bandage, Mud Rub, Antidote, Ethereal Orb -> use:cure x; bombs -> use:aoe + use:inflict
  - armor: bleed / fire / shock / blight+toxin resistance points -> res_bleed / res_fire / res_shock / res_acid (+2% each)
  - skills: written out below from the archetypes' Remnant II behaviour.
Clauses it wrote before are replaced, so it can be run again.
usage: author_statuses.py
"""
import json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SHEETS = os.path.join(ROOT, "sheets")
TK = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))
ST = {"BLEEDING": "bleeding", "BURNING": "burning", "CORRODED": "corroded", "OVERLOADED": "overloaded",
      "EXPOSED": "exposed", "SLOW": "slowed", "SLOWED": "slowed"}
OURS = re.compile(r"^(use:inflict|use:imbue|use:cure|use:aoe|use:mark|use:eruption|(\w+(\(\d+\))?:)?inflict:|res_|vs_)")


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def num(s):
    return float(s.replace(",", ""))


def sentences(desc):
    return [x.strip() for x in re.split(r"(?<=[.!])\s+", desc.replace("\n", " ")) if x.strip()]


def status_dmg(sent, word):
    """(damage, seconds) of a status in a sentence: the low end of 'N -> M' ranges."""
    i = sent.find(word)
    seg = sent[max(0, i - 60):]
    m = re.search(r"(\d[\d,]*(?:\.\d+)?)\s*(?:(?:→|->|-|–)\s*\d[\d,]*\s*)?(?:[A-Z]+\s+)*(?:[Dd]amage\s+)?over\s+(\d+(?:\.\d+)?)\s*s",
                  seg)
    if m:
        return num(m.group(1)), num(m.group(2))
    m = re.search(r"(\d[\d,]*(?:\.\d+)?)\s*(?:(?:→|->|-|–)\s*\d[\d,]*\s*)?(?:[A-Z]+\s+)*[Dd]amage\s+every\s+(\d+(?:\.\d+)?)\s*s"
                  r"(?:\s+for\s+(\d+(?:\.\d+)?)\s*s)?", seg)
    if m:
        secs = num(m.group(3)) if m.group(3) else 10.0
        return num(m.group(1)) * secs / max(0.5, num(m.group(2))), secs
    m = re.search(r"for\s+(\d+(?:\.\d+)?)\s*s", sent[i:])
    return 0.0, num(m.group(1)) if m else 10.0


def clauses_for(cat, desc):
    out = []
    for sent in sentences(desc):
        words = [w for w in ST if re.search(r"\b" + w + r"\b", sent)]
        words = [w for w in words if not (w == "SLOW" and "SLOWED" in words)]
        low = sent.lower()
        # damage against afflicted targets
        for w in words:
            m = re.search(r"(?:damage|Damage)[^.]*?(?:to|against)\s+(?:all\s+)?" + w + r"[^.]*?by\s+(\d+(?:\.\d+)?)%", sent)
            if m and ST[w] != "slowed":
                out.append(f"vs_{ST[w]}+{num(m.group(1)):g}%")
        if any(c.startswith("vs_") for c in out) and not re.search(r"\bapply|\bapplies|\binflict", low):
            continue
        if not re.search(r"\bappl(y|ies|ying)|\binflict|\bbegin\b|\bcaus", low):
            continue
        for w in words:
            st = ST[w]
            dmg, secs = status_dmg(sent, w)
            if st in ("exposed", "slowed"):
                dmg = 0
            if cat == "mod":
                span = re.search(r"(?:for|Lasts)\s+(\d+)\s*s", sent)
                rad = re.search(r"(\d+(?:\.\d+)?)\s*m\b", sent)
                if re.search(r"shots|ammunition|hits apply|rounds", low) and not re.search(r"explod|radius|within", low):
                    out.append(f"use:imbue {st} {dmg:g} {secs:g}s {num(span.group(1)) if span else 20:g}s")
                else:
                    around = re.search(r"aura|around (?:you|the wielder|the caster)|nearby", low)
                    out.append(f"use:inflict {st} {dmg:g} {secs:g}s {num(rad.group(1)) if rad else 5:g}m"
                               + ("" if around else "@target"))
            elif cat in ("weapon", "ring", "amulet", "mutator", "relic"):
                trig = "on_evade" if re.search(r"dodge|evade", low) else "on_weakspot" if "weakspot" in low else \
                    "on_crit" if "critical hit" in low else "on_kill" if re.search(r"\bkill", low) else "on_hit"
                if re.search(r"charge(d)? melee|melee attack|melee hit|explosi", low):
                    continue  # melee charge attacks and explosions: not what the Remnant guns do here
                ch = re.search(r"(\d+(?:\.\d+)?)%\s+chance", sent)
                out.append(f"{trig}:inflict:{st} {dmg:g} {secs:g}s" + (f" {num(ch.group(1)):g}%" if ch else ""))
            elif cat == "consumable":
                rad = re.search(r"(\d+(?:\.\d+)?)\s*m\b", sent)
                hit = re.search(r"dealing\s+(\d[\d,]*)", sent)
                if hit and rad and not any(c.startswith("use:aoe") for c in out):
                    out.append(f"use:aoe {num(hit.group(1)):g} {num(rad.group(1)):g}m@target")
                out.append(f"use:inflict {st} {dmg:g} {secs:g}s {num(rad.group(1)) if rad else 4:g}m@target")
    # cures
    if cat == "consumable":
        for w, st in (("BLEEDING", "bleeding"), ("BURNING", "burning"), ("CORRODED", "corroded"), ("OVERLOADED", "overloaded")):
            if re.search(r"(Stops|Removes|Extinguishes|Cures)\s+" + w, desc):
                out = [c for c in out if st not in c] + [f"use:cure {st}"]
    seen, uniq = set(), []
    for c in out:
        if c not in seen:
            seen.add(c)
            uniq.append(c)
    return uniq


SKILLS = {
    "realityrune": "use:inflict slowed 0 15s 12m;use:buff mod_gen+25% 15s",
    "havocform": "use:aoe 150 6m;use:buff dr+50% 15s",
    "warstomp": "use:aoe 150 10m",
    "deployable_flamethrower": "use:summon 40 20s;use:inflict burning 300 20s 0m",
    "deployable_impactcannon": "use:summon 40 20s;use:aoe 120 5m@target",
    "hunters_mark": "use:mark 25s 35m",
    "wormhole": "use:burst 300;use:inflict exposed 0 10s 0m",
    "eruption": "use:eruption 200 15m",
    "miasma": "use:inflict bleeding 225 10s 15m;use:inflict burning 225 10s 15m;use:inflict corroded 225 10s 15m;"
              "use:inflict overloaded 225 10s 15m",
    "attackdog": "use:summon 40 20s;use:inflict bleeding 200 10s 0m",
}

CAT_SHEETS = {"weapon": "weapons", "mod": "mods", "mutator": "mutators", "ring": "rings", "amulet": "amulets",
              "relic": "relics", "consumable": "consumables", "concoction": "consumables", "armor": "armor"}
sheets = {n: json.load(open(os.path.join(SHEETS, n + ".json"), encoding="utf-8"))
          for n in set(CAT_SHEETS.values()) | {"skills"}}
rows = {n: {norm(r["id"]): r for r in sh["rows"]} for n, sh in sheets.items()}
changed = {n: 0 for n in sheets}


def set_clauses(row, add, sheet):
    cur = [] if row["effects"] in ("none", "") else row["effects"].split(";")
    keep = [c for c in cur if not OURS.match(c)]
    # the arsenal's earlier stand-ins for these mechanics: a plain damage over time for a status, and flat damage
    # for "damage to <status> targets"
    if any(c.startswith(("use:imbue", "use:inflict")) and not c.startswith(("use:inflict exposed", "use:inflict slowed"))
           for c in add):
        keep = [c for c in keep if not c.startswith("use:dot")]
    for c in add:
        m = re.match(r"vs_\w+\+([\d.]+)%", c)
        if m:
            keep = [k for k in keep if k != f"dmg+{m.group(1)}%"]
    new = keep + add
    val = ";".join(new) if new else "none"
    if val != row["effects"]:
        row["effects"] = val
        changed[sheet] += 1


for cat, items in TK.items():
    sheet = CAT_SHEETS.get(cat)
    if not sheet:
        continue
    for it in items:
        slug = re.sub(r"^[A-Za-z]+_(?:Head_|Body_|Gloves_|Legs_)?|_C$", "", it.get("saveFileSlug") or "")
        row = rows[sheet].get(norm(it["name"])) or rows[sheet].get(norm(slug))
        if sheet == "armor" and not row:
            piece = {"helm": "head", "torso": "body", "legs": "legs", "gloves": "gloves"}.get(it.get("category"), "")
            row = rows[sheet].get(norm(piece + slug))
        if not row:
            continue
        if sheet == "armor":
            add = []
            for key, stat in (("bleedResistance", "res_bleed"), ("fireResistance", "res_fire"),
                              ("shockResistance", "res_shock")):
                if it.get(key):
                    add.append(f"{stat}{2 * it[key]:+g}%")
            acid = (it.get("blightResistance") or 0) + (it.get("toxinResistance") or 0)
            if acid:
                add.append(f"res_acid{2 * acid:+g}%")
        else:
            add = clauses_for("mod" if cat == "mod" else cat, it.get("description") or "")
        set_clauses(row, add, sheet)

for sid, dsl in SKILLS.items():
    r = rows["skills"].get(norm(sid))
    if r and r["effects"] != dsl:
        r["effects"] = dsl
        changed["skills"] += 1

for n, sh in sheets.items():
    if changed[n]:
        json.dump(sh, open(os.path.join(SHEETS, n + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print("rows changed:", {n: c for n, c in changed.items() if c})
