#!/usr/bin/env python3
"""Dev tool: author the v0.2 arsenal sheets (every Remnant II archetype, weapon and item, offered at MHW's NPCs).

Inputs (local only, never committed or packaged):
  .check/catalog.json   tools/r2catalog.py: asset paths, FText keys and icons found in the player's Remnant II install
  .check/toolkit.json   tools/r2toolkit_read.py: item stats and effect descriptions from the Remnant 2 Toolkit
                        (GPL-3.0, github.com/joshpayette/remnant2-toolkit), used for numbers only
Outputs: sheets/{archetypes,skills,perks,traits,weapons,mods,mutators,rings,amulets,relics,fragments,consumables,
armor,materials,menus}.json. Sheets hold where Remnant II's names, descriptions and icons live in the install, plus our
own numbers: gun stats, effects written in our effect language (see EFFECTS below), tiers, zenny prices and recipes in
MHW materials. Remnant II's text is never written into a sheet.

usage: author_arsenal.py --paks <Remnant2/Content/Paks> [--keep-ids <dir of the v0.1 sheets>]   (from the project root)

EFFECTS (column "effects"), read by src/core/effects.cpp:
  passive   [trigger:]stat+N[%][ Ts][ xS]          e.g. on_evade:crit+10% 15s
            [trigger:]proc+N cdTs                    deal N damage to the target when the trigger fires
  active    use:heal N% [Ts] | use:shield N% Ts | use:burst N | use:dot N Ts | use:summon N Ts
            | use:buff stat+N% Ts | use:ammo N% | use:grenade N
  several effects are separated by ';'.
"""
import json, os, re, sys

ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "tools"))
import r2pak, r2text  # noqa: E402

PAKS = os.environ.get("R2PAKS") or sys.argv[sys.argv.index("--paks") + 1]
_STORES = r2pak.open_all(PAKS)
_FILES = {}
for _s in _STORES:
    for _k, _v in _s.files.items():
        _FILES[_k] = (_s, _v)
_TEXTS = {}


def pkg_texts(asset):
    """Text keys exactly as the mod's reader finds them (r2text.texts), so every key we write resolves at runtime."""
    if asset not in _TEXTS:
        s, e = _FILES[asset]
        _TEXTS[asset] = dict(r2text.texts(s.read(e)))
    return _TEXTS[asset]


def tex_exists(path):
    return path + ".uasset" in _FILES


def tex_variant(path):
    """Icons are sometimes renamed in later patches (T_UI_X_Icon -> T_UI_X_Icon_2): find the one that ships."""
    if tex_exists(path):
        return path
    d, b = path.rsplit("/", 1)
    cands = sorted(k[:-7] for k in _FILES if k.startswith(d + "/" + b) and k.endswith(".uasset"))
    return cands[0] if cands else None
CAT = json.load(open(os.path.join(ROOT, ".check", "catalog.json"), encoding="utf-8"))
TK = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))
SHEETS = os.path.join(ROOT, "sheets")
# ids to keep come from the sheets before the first authoring run (saves and code refer to them)
KEEP = sys.argv[sys.argv.index("--keep-ids") + 1] if "--keep-ids" in sys.argv else SHEETS

STATS = ["proc", "dmg", "dmg_ranged", "dmg_melee", "dmg_mod", "dmg_skill", "dmg_elem", "dmg_status", "dmg_explosive",
         "weakspot", "crit", "crit_dmg", "fire_rate", "reload", "mag", "ammo", "accuracy", "mod_gen", "mod_dur", "cdr",
         "skill_dur", "relic_speed", "relic_heal", "relic_charges", "heal", "lifesteal", "dr", "max_hp", "regen",
         "stamina", "shield"]
TRIGGERS = ["on_kill", "on_crit", "on_weakspot", "on_hit", "on_evade", "on_relic", "on_skill", "on_mod", "on_reload",
            "on_hurt", "low_hp", "high_hp"]

# phrase -> (stat, sign when the text says "increases"); "reduces" flips the sign except where noted in GOOD_WHEN_REDUCED
PHRASES = [
    (r"all (?:incoming )?damage (?:taken|received)|incoming damage|damage taken|damage received", "dr"),
    (r"damage reduction", "dr"),
    (r"critical (?:hit )?chance|crit chance", "crit"),
    (r"critical (?:hit )?damage|crit damage", "crit_dmg"),
    (r"weakspot damage|weak spot damage", "weakspot"),
    (r"ranged (?:and|&) melee damage|melee and ranged damage", "dmg"),
    (r"ranged damage|firearm damage|gun damage", "dmg_ranged"),
    (r"melee damage", "dmg_melee"),
    (r"mod damage", "dmg_mod"),
    (r"skill damage|summon damage|companion damage", "dmg_skill"),
    (r"(?:fire|shock|acid|elemental|corrosive) damage", "dmg_elem"),
    (r"status (?:effect )?damage|bleed(?:ing)? damage|burn(?:ing)? damage", "dmg_status"),
    (r"explosive damage|explosion damage|area damage", "dmg_explosive"),
    (r"all damage(?: dealt)?|damage dealt|outgoing damage|damage", "dmg"),
    (r"fire rate|rate of fire|melee (?:attack )?speed|attack speed", "fire_rate"),
    (r"reload speed|reload time", "reload"),
    (r"magazine(?: capacity| size)?", "mag"),
    (r"ammo reserves?|ammo capacity|reserve ammo", "ammo"),
    (r"spread|recoil|accuracy", "accuracy"),
    (r"mod (?:power )?generation|mod power", "mod_gen"),
    (r"mod duration", "mod_dur"),
    (r"(?:all )?skill cooldowns?|cooldowns? of skills|cooldown", "cdr"),
    (r"skill duration", "skill_dur"),
    (r"(?:relic|consumable)(?: and (?:relic|consumable))? use speed|use speed", "relic_speed"),
    (r"relic (?:healing )?(?:efficacy|effectiveness)|relic healing", "relic_heal"),
    (r"relic charges?", "relic_charges"),
    (r"healing(?: effectiveness| received)?", "heal"),
    (r"lifesteal", "lifesteal"),
    (r"max(?:imum)? health|health", "max_hp"),
    (r"health regen(?:eration)?", "regen"),
    (r"stamina(?: cost)?|evade (?:cost|speed)|movement speed|move speed", "stamina"),
    (r"shield(?: amount)?", "shield"),
    (r"armor(?: effectiveness)?", "dr"),
]
# phrases where "increases" is a penalty for the wearer (cooldowns, spread, damage taken, costs)
INVERTED = re.compile(r"cooldown|spread|recoil|damage taken|damage received|incoming damage|cost|requirement")

TRIGGER_RX = [
    (r"\bperfect dodge|\bevad|\bdodg|combat slide", "on_evade"),
    (r"\bon kill|\bkilling\b|\bkills?\b|\bslay", "on_kill"),
    (r"critical hits?\b|\bon crit|\bcrits\b", "on_crit"),
    (r"weakspot hits?|on weakspot|hitting a weakspot|weakspot kills", "on_weakspot"),
    (r"relic use|using a relic|after using a relic|on relic", "on_relic"),
    (r"skill (?:use|activation)|using a skill|casting a skill|on skill|activating a skill|skill is activated", "on_skill"),
    (r"mod use|using a mod|after (?:using )?(?:a |this )?mod|on mod", "on_mod"),
    (r"after reloading|on reload|reloading|when reloading", "on_reload"),
    (r"taking damage|when hit|receiving damage|after being hit|when damaged", "on_hurt"),
    (r"\bon hit\b|each hit|hitting an enemy|shots? (?:that )?hit", "on_hit"),
]
TAG_FALLBACK = [
    ("All Damage", "dmg+5%"), ("Ranged Damage", "dmg_ranged+8%"), ("Melee Damage", "dmg_melee+8%"),
    ("Critical Chance", "crit+5%"), ("Critical Damage", "crit_dmg+10%"), ("Weakspot Damage", "weakspot+10%"),
    ("Lifesteal", "lifesteal+3%"), ("Damage Reduction", "dr+5%"), ("Armor Increase", "dr+5%"),
    ("Fire Rate", "fire_rate+5%"), ("Reload Speed", "reload+10%"), ("Mod Power", "mod_gen+10%"),
    ("Mod Damage", "dmg_mod+10%"), ("Mod Duration", "mod_dur+10%"), ("Skill Cooldown", "cdr+5%"),
    ("Skill Damage", "dmg_skill+10%"), ("Skill Duration", "skill_dur+10%"), ("Summon", "dmg_skill+10%"),
    ("Heal", "heal+10%"), ("Healing", "heal+10%"), ("Grey Health", "regen+1"), ("Health", "max_hp+10"),
    ("Status Effect", "dmg_status+10%"), ("Status Duration", "dmg_status+5%"), ("Ammo Reserves", "ammo+20%"),
    ("Relic Speed", "relic_speed+10%"), ("Relic Efficacy", "relic_heal+10%"), ("Stamina", "stamina+10%"),
    ("Movement Speed", "stamina+5%"), ("Perfect Dodge", "on_evade:dmg+10% 10s"), ("Explosive Damage", "dmg_explosive+10%"),
    ("Elemental Damage", "dmg_elem+10%"), ("Shield", "shield+10"),
]
NUM = r"(\d+(?:\.\d+)?)"


def snake(s):
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", s)
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def strip_markup(s):
    return re.sub(r"<[^>]+>", "", s or "")


def first_num(s):
    """'10%', '10% - 30%', '10 -> 100%', '200 → 600' -> the first (base) value."""
    m = re.search(NUM, s)
    return float(m.group(1)) if m else None


def unrange(s):
    """'10% - 30%' / '10 -> 100%' / '200 → 600' -> the middle value, so the rest of the parser sees one number."""
    def mid(m):
        a, b = float(m.group(1)), float(m.group(3))
        return fmt(round((a + b) / 2, 1)) + (m.group(4) or "")
    return re.sub(r"(\d+(?:\.\d+)?)(%?)\s*(?:-|->|→|to)\s*(\d+(?:\.\d+)?)(%?)", mid, s)


def sentences(desc):
    return [x.strip() for x in re.split(r"(?<=[.!])\s+|\n+", unrange(strip_markup(desc))) if x.strip()]


def trigger_of(sent):
    s = sent.lower()
    m = re.search(r"(?:below|under|less than) " + NUM + r"%\s*(?:max )?health|health (?:is )?(?:below|under) " + NUM + "%", s)
    if m:
        return f"low_hp({int(float(m.group(1) or m.group(2)))})"
    if re.search(r"(?:above|over) " + NUM + r"% (?:max )?health|at full health|at max(?:imum)? health", s):
        m = re.search(r"(?:above|over) " + NUM + "%", s)
        return f"high_hp({int(float(m.group(1))) if m else 95})"
    for rx, tr in TRIGGER_RX:
        if re.search(rx, s):
            return tr
    return ""


def fmt(v):
    return str(int(v)) if float(v).is_integer() else f"{v:g}"


def parse_passive(desc):
    """Our effect language from a description; returns (dsl, fidelity)."""
    out = []
    carry = ""
    for sent in sentences(desc):
        s = sent.lower()
        tr = trigger_of(sent) or (carry if re.match(r"^[A-Z][A-Z' ]{3,}:", sent) else "")
        dur = re.search(r"(?:for|lasts?|lasting) " + NUM + r"\s*s\b", s)
        stacks = re.search(r"stacks? (?:up to )?" + NUM + r"\s*x|up to " + NUM + r" (?:times|stacks)|max " + NUM + r" stacks", s)
        before = len(out)
        # "<stat phrase> is increased by N%"
        for m in re.finditer(r"([a-z ]+?) (?:is|are) (increased|reduced|decreased) by " + NUM + r"\s*(%?)", s):
            for rx, stat in PHRASES:
                if re.search(rx, m.group(1)):
                    sign = -1 if m.group(2) != "increased" else 1
                    if INVERTED.search(m.group(1)):
                        sign = -sign
                    out.append((tr, stat, sign * float(m.group(3)), m.group(4), dur, stacks))
                    break
        # "grants a SHIELD for N% of Max Health"
        m = re.search(r"shield (?:for|of|equal to) " + NUM + r"%", s)
        if m:
            out.append((tr, "shield", float(m.group(1)), "%", dur, stacks))
        # damage procs: "creates a blast which deals N damage", "deal N damage"
        m = re.search(r"deals? " + NUM + r" (?:\w+ )?damage", s)
        if m and tr:
            cd = re.search(r"once every " + NUM + r"\s*s", s)
            out.append((tr, "proc", float(m.group(1)), "", None, None, float(cd.group(1)) if cd else 1.0))
        # "<verb> <stat phrase>[, <stat phrase> and <stat phrase>] by N%"
        for m in re.finditer(r"(increas\w*|reduc\w*|decreas\w*|grants?|gains?|deals?|adds?)\s+([a-z ,&]+?)\s+by\s+" + NUM + r"\s*(%?)", s):
            verb, what, val, pct = m.group(1), m.group(2), float(m.group(3)), m.group(4)
            parts = re.split(r",\s*|\s+and\s+|\s*&\s*", what)
            for part in parts:
                for rx, stat in PHRASES:
                    if re.search(rx, part):
                        sign = -1 if verb.startswith(("reduc", "decreas")) else 1
                        if INVERTED.search(part):
                            sign = -sign
                        out.append((tr, stat, sign * val, pct, dur, stacks))
                        break
        if tr and len(out) == before:
            carry = tr
        elif len(out) > before:
            carry = ""
        # "increases fire rate 20% and reload speed 50%" (no "by")
        for m in re.finditer(r"(increas\w*|reduc\w*)\s+((?:[a-z ]+?\s+" + NUM + r"%(?:,\s*|\s+and\s+)?)+)", s):
            for part in re.finditer(r"([a-z ]+?)\s+" + NUM + r"%", m.group(2)):
                what = re.sub(r"^(?:and|the|all)\s+", "", part.group(1).strip())
                for rx, stat in PHRASES:
                    if re.fullmatch(r"(?:" + rx + r")", what):
                        sign = -1 if m.group(1).startswith("reduc") else 1
                        if INVERTED.search(what):
                            sign = -sign
                        out.append((tr, stat, sign * float(part.group(2)), "%", dur, stacks))
                        break
        # "N% additional <stat>" / "gain N% <stat>"
        for m in re.finditer(NUM + r"\s*%\s+(?:additional |increased |more |bonus )?([a-z ]+?)(?:\s+for|\.|,|$)", s):
            val, what = float(m.group(1)), m.group(2)
            if any(x[2] == val for x in out):
                continue
            for rx, stat in PHRASES:
                if re.fullmatch(r"(?:" + rx + r")(?: dealt| against \w+)?", what.strip()):
                    out.append((tr, stat, val, "%", dur, stacks))
                    break
    dsl, seen = [], set()
    for o in out:
        tr, stat, val, pct, dur, stacks = o[:6]
        if (tr, stat) in seen or len(dsl) >= 4:
            continue
        seen.add((tr, stat))
        e = (tr + ":" if tr else "") + f"{stat}{'+' if val >= 0 else '-'}{fmt(abs(val))}{pct}"
        if stat == "proc":
            dsl.append(e + f" cd{fmt(o[6])}s")
            continue
        if dur:
            e += f" {fmt(float(dur.group(1)))}s"
        elif tr and not tr.startswith(("low_hp", "high_hp")):
            e += " 10s"  # a triggered effect without a stated duration lasts 10s
        if stacks:
            e += f" x{int(float(stacks.group(1) or stacks.group(2)))}"
        dsl.append(e)
    return ";".join(dsl)


def fallback(tags, default="dmg+3%"):
    for t in tags or []:
        for tag, e in TAG_FALLBACK:
            if t == tag:
                return e
    return default


def passive(item, default="dmg+3%"):
    d = parse_passive(item.get("description", ""))
    if d:
        return d, "approx" if len(d.split(";")) < 1 else "exact"
    return fallback(item.get("tags"), default), "fallback"


def parse_active(desc, tags, kind):
    s = strip_markup(desc).lower()
    eff = []
    m = re.search(r"heals? (?:for )?" + NUM + r"%(?: of (?:max(?:imum)? )?health)?(?:.*?over " + NUM + r"\s*s)?", s)
    if m:
        eff.append(f"use:heal {fmt(float(m.group(1)))}%" + (f" {fmt(float(m.group(2)))}s" if m.group(2) else ""))
    m = re.search(r"(?:restores?|regenerates?|heals?|grants?) " + NUM + r" (?:health|hp)(?: regeneration)? per second.*?lasts? " + NUM + r"\s*s", s)
    if m:
        eff.append(f"use:heal {fmt(min(300.0, float(m.group(1)) * float(m.group(2))))}% {fmt(float(m.group(2)))}s")
    m = re.search(r"(?:restores?|regenerates?|heals?) " + NUM + r" (?:health|hp)(?:.*?over " + NUM + r"\s*s)?", s)
    if m and not eff:
        eff.append(f"use:heal {fmt(min(100.0, float(m.group(1))))}%" + (f" {fmt(float(m.group(2)))}s" if m.group(2) else ""))
    m = re.search(r"shield (?:for |equal to |of )?" + NUM + r"%", s)
    if m:
        dur = re.search(r"lasts? " + NUM + r"\s*s", s)
        eff.append(f"use:shield {fmt(float(m.group(1)))}% {fmt(float(dur.group(1))) if dur else 10}s")
    m = re.search(NUM + r"(?:\s*(?:→|->|-)\s*" + NUM + r")? (?:\w+ )?damage over " + NUM + r"\s*s", s)
    if m:
        eff.append(f"use:dot {fmt(float(m.group(1)) / float(m.group(3)))} {fmt(float(m.group(3)))}s")
    elif re.search(r"summon|companion|minion|turret|drone|spirit|familiar|root|bots?\b|crab|dog", s) and kind in ("skill", "mod"):
        dur = re.search(r"(?:lasts?|for) " + NUM + r"\s*s", s)
        eff.append(f"use:summon 40 {fmt(float(dur.group(1))) if dur else 20}s")
    m = re.search(r"deals? " + NUM + r"(?:\s*(?:→|->|-)\s*" + NUM + r")? (?:\w+ )*?damage", s)
    if m and not any(e.startswith("use:dot") for e in eff):
        n = float(m.group(1))
        cnt = re.search(r"(?:burst of|fires?|launch(?:es)?) " + NUM + r" ", s)
        if cnt:
            n *= float(cnt.group(1))
        eff.append(f"use:burst {fmt(n)}")
    if re.search(r"refills?|replenish(?:es)? .*ammo|ammo (?:box|crate)", s):
        eff.append("use:ammo 50%")
    # timed buffs from the passive grammar
    for e in parse_passive(desc).split(";"):
        if e and not e.startswith(("on_", "low_hp", "high_hp")):
            stat_part = e.split(" ")[0]
            dur = re.search(r" (\d+(?:\.\d+)?)s", e)
            eff.append(f"use:buff {stat_part} {dur.group(1) if dur else 15}s")
    eff = eff[:4]
    if eff:
        return ";".join(eff), "approx"
    fb = fallback(tags, "")
    if fb:
        stat = fb.split(" ")[0].split(":")[-1]
        return f"use:buff {stat} 15s", "fallback"
    return "use:burst 300" if kind == "mod" else "use:buff dmg+15% 15s", "fallback"


# ---------------------------------------------------------------- install side

def install(cat):
    out = {}
    for it in CAT:
        if it["cat"] == cat:
            base = it["path"].rsplit("/", 1)[1][:-7]
            out[base.lower()] = it
    return out


def texts_of(it):
    have = pkg_texts(it["path"])
    t = [x for x in it["texts"] if x[0] in have]
    if not t:
        return None, "none"
    name = {"r2text": {"asset": it["path"], "key": t[0][0]}}
    desc = "none"
    for k, s in t[1:]:
        if len(s) > 30 and not s.lstrip().startswith(('"', "“", "”", "'")) and s != t[0][1] and "Needs " not in s:
            desc = {"r2text": {"asset": it["path"], "key": k}}
            break
    return name, desc


def icon_of(it):
    icons = sorted(it["icons"], key=lambda i: "Icon" not in i.rsplit("/", 1)[-1])
    for i in icons:
        v = tex_variant(i)
        if v:
            return {"r2tex": v}
    return "none"


# toolkit slug -> install asset name where the two differ
SLUG_ALIAS = {"skill_droneshield": "skill_summonshielddrone", "skill_droneheal": "skill_summonhealdrone",
              "skill_dronecombat": "skill_summoncombatdrone"}
ID_ALIAS = {"battlecry_juggernaut": "juggernaut"}


def tk_index(cat):
    out = {}
    for x in TK.get(cat, []):
        k = (x.get("saveFileSlug") or "").lower().removesuffix("_c")
        out[SLUG_ALIAS.get(k, k)] = x
    return out


def tier_of(x, cat):
    dlc = x.get("dlc", "base")
    loc = x.get("location") or {}
    dung = loc.get("dungeon")
    name = x.get("name", "")
    if name.startswith("Corrupted") or dlc == "dlc3":
        return 5
    if dlc in ("dlc1", "dlc2"):
        return 4
    if dung == "Vendor":
        return 1
    if dung in ("World Drop", None, "Quest"):
        return 2
    return 3


TIER_ZENNY = {1: 1500, 2: 4000, 3: 12000, 4: 25000, 5: 50000}
TIER_RECIPE = {
    1: ["ore_low*3", "bone_low*2"],
    2: ["ore_low2*4", "bone_low2*3", "crystal_low*2"],
    3: ["ore_high*5", "bone_high*4", "part_high*2"],
    4: ["ore_high2*6", "elder_high*3", "part_high2*3"],
    5: ["ore_master*6", "bone_master*4", "elder_master*3", "gem_master*1"],
}
MATERIALS = [
    ("ore_low", "low", "ore", "common low rank ore"), ("bone_low", "low", "bone", "small monster bone"),
    ("ore_low2", "low", "ore", "better low rank ore"), ("bone_low2", "low", "bone", "medium monster bone"),
    ("crystal_low", "low", "crystal", "low rank crystal"), ("ore_high", "high", "ore", "high rank ore"),
    ("bone_high", "high", "bone", "high rank bone"), ("part_high", "high", "part", "high rank wyvern part"),
    ("ore_high2", "high", "ore", "rare high rank ore"), ("elder_high", "high", "elder", "elder dragon material"),
    ("part_high2", "high", "part", "rare high rank wyvern part"), ("ore_master", "master", "ore", "master rank ore"),
    ("bone_master", "master", "bone", "master rank bone"), ("elder_master", "master", "elder", "master rank elder dragon material"),
    ("gem_master", "master", "gem", "master rank gem"),
]


def scaled(recipe, k):
    return [f"{r.split('*')[0]}*{max(1, round(int(r.split('*')[1]) * k))}" for r in recipe]


def commerce(row, tier, menu, zk=1.0, rk=1.0, recipe=True):
    row["tier"] = tier
    row["menu"] = menu
    row["zenny"] = int(round(TIER_ZENNY[tier] * zk / 50.0) * 50)
    row["recipe"] = scaled(TIER_RECIPE[tier], rk) if recipe else ["none"]


def sheet(name, columns, rows, phase="v0.2"):
    return {"sheet": name, "phase": phase, "columns": columns, "rows": rows}


COMMON = {"name": "r2text", "desc": "r2text|none", "icon": "r2tex|none", "effects": "effects", "fidelity": "enum:exact|approx|fallback"}
SHOP = {"tier": "num", "menu": "enum:smithy|melder|provisions|archetype", "zenny": "num", "recipe": "recipe"}


def old_ids(name):
    """Keep the ids of rows the v0.1 sheets already had (code and saves refer to them)."""
    p = os.path.join(KEEP, name + ".json")
    m = {}
    if os.path.exists(p):
        for r in json.load(open(p, encoding="utf-8"))["rows"]:
            v = r.get("name")
            if isinstance(v, dict) and "r2text" in v:
                m[v["r2text"]["asset"].rsplit("/", 1)[1][:-7].lower()] = r["id"]
    return m


def simple(cat, tkcat, prefix, menu, zk, rk, default_effect, active=False, recipe=True):
    rows, used = [], set()
    tk = tk_index(tkcat)
    keep = old_ids({"ring": "rings", "amulet": "amulets", "mutator": "mutators", "fragment": "fragments", "relic": "relics",
                    "consumable": "consumables", "mod": "mods"}.get(cat, cat))
    for base, it in sorted(install(cat).items()):
        x = tk.get(base)
        if not x:
            continue
        name, desc = texts_of(it)
        if not name:
            continue
        rid = keep.get(base) or snake(re.sub("^" + prefix, "", it["path"].rsplit("/", 1)[1][:-7]))
        if rid in used:
            continue
        used.add(rid)
        if active:
            eff, fid = parse_active(x.get("description", ""), x.get("tags"), cat)
        else:
            eff, fid = passive(x, default_effect)
        row = {"id": rid, "name": name, "desc": desc, "icon": icon_of(it), "effects": eff, "fidelity": fid}
        commerce(row, tier_of(x, cat), menu, zk, rk, recipe)
        rows.append((row, x))
    return rows


def main():
    out = {}
    # rings, amulets, mutators, fragments
    out["rings"] = sheet("rings", {"id": "id", **COMMON, **SHOP}, [r for r, _ in simple("ring", "ring", "Ring_", "melder", 0.6, 0.6, "dmg+3%")])
    out["amulets"] = sheet("amulets", {"id": "id", **COMMON, **SHOP}, [r for r, _ in simple("amulet", "amulet", "Amulet_", "melder", 0.8, 0.8, "dmg+5%")])
    mut = simple("mutator", "mutator", "MetaGem_", "smithy", 0.6, 0.6, "dmg+5%")
    for r, x in mut:
        r["slot"] = "melee" if x.get("type") == "melee" else "gun"
    out["mutators"] = sheet("mutators", {"id": "id", "slot": "enum:gun|melee", **COMMON, **SHOP}, [r for r, _ in mut])
    frag = simple("fragment", "relic-fragment", "RelicFragment_", "melder", 0.4, 0.4, "dmg+2%")
    for r, x in frag:
        r["tier"] = 4 if x.get("color") in ("legendary", "purple") else 2
        commerce(r, r["tier"], "melder", 0.4, 0.4)
    out["fragments"] = sheet("fragments", {"id": "id", **COMMON, **SHOP}, [r for r, _ in frag])
    rel = simple("relic", "relic", "Relic_Consumable_", "melder", 1.0, 0.8, "", active=True)
    for r, x in rel:
        if r["id"] == "dragon_heart" or "dragonheart" in r["id"]:
            commerce(r, 1, "melder", recipe=False)
            r["zenny"] = 0
    out["relics"] = sheet("relics", {"id": "id", **COMMON, **SHOP}, [r for r, _ in rel])
    con = simple("consumable", "consumable", "Consumable_", "provisions", 0.05, 0, "", active=True, recipe=False)
    con += [c for c in simple("consumable", "concoction", "Consumable_", "provisions", 0.1, 0, "", active=True, recipe=False)
            if c[0]["id"] not in {r["id"] for r, _ in con}]
    for r, x in con:
        r["tier"] = 1
        r["zenny"] = max(100, r["zenny"])
    out["consumables"] = sheet("consumables", {"id": "id", **COMMON, **SHOP}, [r for r, _ in con])

    # armor: Remnant armor adds damage reduction and costs stamina by weight
    arm = []
    tk = tk_index("armor")
    for base, it in sorted(install("armor").items()):
        x = tk.get(base)
        if not x:
            continue
        name, desc = texts_of(it)
        if not name:
            continue
        piece = {"helm": "head", "torso": "body", "gloves": "gloves", "legs": "legs"}.get(x.get("category"), "body")
        row = {"id": snake(base.removeprefix("armor_")), "piece": piece, "name": name, "desc": desc, "icon": icon_of(it),
               "effects": f"dr+{fmt(round(float(x.get('armor', 10)) / 5.0, 1))}%;stamina-{fmt(round(float(x.get('weight', 5)) / 2.0, 1))}%",
               "fidelity": "approx"}
        commerce(row, tier_of(x, "armor"), "smithy", 0.5, 0.5)
        arm.append(row)
    out["armor"] = sheet("armor", {"id": "id", "piece": "enum:head|body|gloves|legs", **COMMON, **SHOP}, arm)

    # mods (gun mods carried by boss guns and standalone ones)
    mods = simple("mod", "mod", "Mod_", "smithy", 1.0, 1.0, "", active=True)
    for r, x in mods:
        r["charge_damage"] = [600, 750, 900, 1050, 1200][r["tier"] - 1]
    out["mods"] = sheet("mods", {"id": "id", **COMMON, "charge_damage": "num", **SHOP}, [r for r, _ in mods])
    mod_by_name = {x.get("name", "").lower(): r["id"] for r, x in mods}

    # weapons: guns (stats) and melee (an infusion for the hunter's MHW weapon)
    keep = old_ids("weapons")
    tk = tk_index("weapon")
    weps = []
    for base, it in sorted(install("weapon").items()):
        x = tk.get(base)
        if not x:
            continue
        name, desc = texts_of(it)
        if not name:
            continue
        typ = x.get("type")
        slot = {"long gun": "long", "hand gun": "hand", "melee": "melee"}[typ]
        rid = keep.get(base) or snake(base.removeprefix("weapon_"))
        dmg = float(x.get("damage") or 20)
        rps = float(x.get("rps") or 2)
        mag = int(x.get("magazine") or 1) if slot != "melee" else 0
        ideal = float(x.get("ideal") or 20)
        acc = float(x.get("accuracy") or 90)
        shotgun = slot != "melee" and ideal <= 12 and mag <= 12 and rps < 4
        pellets = 8 if shotgun else 1
        reload = 0 if slot == "melee" else (0.7 if mag == 1 else 3.0 if shotgun else 1.4 if slot == "hand" else 2.4)
        linked = ((x.get("linkedItems") or {}).get("mod") or {}).get("name", "").lower()
        eff = parse_passive(x.get("description", "")) or ("dmg_melee+" + fmt(5 * tier_of(x, "weapon")) + "%" if slot == "melee" else "")
        row = {"id": rid, "slot": slot, "name": name, "desc": desc, "icon": icon_of(it),
               "damage": round(dmg / pellets, 2) if slot != "melee" else dmg,
               "rpm": round(rps * 60, 1), "mag": mag, "reserve": int(x.get("ammo") or 0),
               "reload_s": reload, "weakspot_mult": round(1 + float(x.get("weakspot") or 100) / 100.0, 2),
               "range_m": round(float(x.get("falloff") or ideal * 2), 1),
               "spread_deg": round(max(0.1, (100 - acc) / 15.0), 2) if not shotgun else 6.0, "pellets": pellets,
               "crit": float(x.get("crit") or 5),
               "mod": mod_by_name.get(linked, "none"),
               "effects": eff or "none", "fidelity": "exact" if eff else "approx"}
        commerce(row, tier_of(x, "weapon"), "smithy")
        weps.append(row)
    out["weapons"] = sheet("weapons", {"id": "id", "slot": "enum:long|hand|melee", "name": "r2text", "desc": "r2text|none",
                                       "icon": "r2tex|none", "damage": "num", "rpm": "num", "mag": "num", "reserve": "num",
                                       "reload_s": "num", "weakspot_mult": "num", "range_m": "num", "spread_deg": "num",
                                       "pellets": "num", "crit": "num", "mod": "ref:mods|none", "effects": "effects|none",
                                       "fidelity": "enum:exact|approx|fallback", **SHOP}, weps)

    # archetypes with their skills, perks and traits
    tk_sk, tk_pk, tk_tr = tk_index("skill"), tk_index("perk"), tk_index("trait")
    ins_sk, ins_pk, ins_tr = install("skill"), install("perk"), install("trait")
    keep_sk = old_ids("skills")
    skills, perks, traits = [], [], []
    by_name = lambda tk: {v.get("name", "").lower(): (k, v) for k, v in tk.items()}
    sk_names, pk_names, tr_names = by_name(tk_sk), by_name(tk_pk), by_name(tk_tr)

    def add_skill(nm):
        k, x = sk_names.get(nm.lower(), (None, None))
        k = SLUG_ALIAS.get(k, k)
        it = ins_sk.get(k) if k else None
        if not it:
            return None
        rid = keep_sk.get(k) or snake(k.removeprefix("skill_"))
        rid = ID_ALIAS.get(rid, rid)
        if any(s["id"] == rid for s in skills):
            return rid
        name, _ = texts_of(it)
        if not name:
            return None
        eff, fid = parse_active(x.get("description", ""), x.get("tags"), "skill")
        dur = re.search(r"lasts? " + NUM + r"\s*s", (x.get("description") or "").lower())
        skills.append({"id": rid, "name": name, "icon": icon_of(it), "effects": eff, "fidelity": fid,
                       "cooldown_s": float(x.get("cooldown") or 60), "duration_s": float(dur.group(1)) if dur else 15.0})
        return rid

    def add_perk(nm, kind):
        k, x = pk_names.get(nm.lower(), (None, None))
        it = ins_pk.get(k) if k else None
        if not it:
            return None
        rid = snake(k.removeprefix("perk_"))
        if not any(p["id"] == rid for p in perks):
            name, _ = texts_of(it)
            if not name:
                return None
            eff, fid = passive(x, "dmg+3%")
            perks.append({"id": rid, "kind": kind, "name": name, "icon": icon_of(it), "effects": eff, "fidelity": fid})
        return rid

    def add_trait(nm):
        k, x = tr_names.get(nm.lower(), (None, None))
        it = ins_tr.get(k) if k else None
        if not it:
            return None
        rid = snake(k.removeprefix("trait_"))
        if not any(t["id"] == rid for t in traits):
            name, _ = texts_of(it)
            if not name:
                return None
            d = (x.get("description") or "")
            m = re.search(NUM + r"\s*(?:->|→|-)\s*" + NUM + r"\s*(%?)", d)
            # traits are described as "1.5% -> 15%" (1 to 10 points): store the 10-point value; the code scales by points
            d10 = re.sub(NUM + r"(%?)\s*(?:->|→)\s*" + NUM, lambda mm: mm.group(3), d)
            eff, fid = passive({"description": d10, "tags": x.get("tags")}, "dmg+1%")
            traits.append({"id": rid, "name": name, "icon": icon_of(it), "effects": eff, "fidelity": fid})
        return rid

    keep_ar = old_ids("archetypes")
    ins_ar_ui = {b.removesuffix("_ui"): it for b, it in install("archetype_ui").items()}
    ins_eng = install("engram")
    ars = []
    STARTERS = {"challenger", "hunter", "medic", "handler"}
    # the guns each archetype arrives with (v0.1's choices for the first four)
    STARTER_GEAR = {"hunter": ("huntmaster_m1", "service_pistol", "hatchet"),
                    "medic": ("blackmaw_ar47", "service_pistol", "hatchet"),
                    "challenger": ("coach_gun", "western_classic", "sword"),
                    "gunslinger": ("blackmaw_ar47", "western_classic", "hatchet"),
                    "handler": ("leveraction", "service_pistol", "hatchet")}
    for x in TK.get("archetype", []):
        nm = x["name"]
        key = "archetype_" + re.sub(r"[^a-z]", "", nm.lower())
        it = ins_ar_ui.get(key)
        eng = ins_eng.get("engram_" + re.sub(r"[^a-z]", "", nm.lower()))
        if not it:
            continue
        have = pkg_texts(it["path"])
        t = [tt for tt in it["texts"] if tt[0] in have]
        if not t:
            continue
        li = x.get("linkedItems") or {}
        sk = [add_skill(s["name"]) for s in li.get("skills", [])]
        pk = [add_perk(p["name"], "prime" if i == 0 else "perk") for i, p in enumerate(li.get("perks", []))]
        tr = [add_trait(tt["name"]) for tt in li.get("traits", [])[:1]]
        rid = keep_ar.get(key + "_ui") or snake(nm)
        row = {"id": rid, "name": {"r2text": {"asset": it["path"], "key": t[0][0]}},
               "prime": {"r2text": {"asset": it["path"], "key": t[1][0]}} if len(t) > 1 else "none",
               "desc": {"r2text": {"asset": it["path"], "key": t[2][0]}} if len(t) > 2 else "none",
               "emblem": icon_of(eng) if eng else "none",
               "skills": [s for s in sk if s], "perks": [p for p in pk if p], "trait": tr[0] if tr and tr[0] else "none"}
        lg, hg, ml = STARTER_GEAR.get(rid, ("blackmaw_ar47", "service_pistol", "hatchet"))
        row["starter_long"], row["starter_hand"], row["starter_melee"] = lg, hg, ml
        tier = 1 if rid in STARTERS else 5 if x.get("dlc") != "base" else 3
        commerce(row, tier, "archetype", 1.5, 1.0)
        if rid in STARTERS:
            row["zenny"] = 0
            row["recipe"] = ["none"]
        ars.append(row)
    out["skills"] = sheet("skills", {"id": "id", "name": "r2text", "icon": "r2tex|none", "effects": "effects",
                                     "fidelity": "enum:exact|approx|fallback", "cooldown_s": "num", "duration_s": "num"}, skills)
    out["perks"] = sheet("perks", {"id": "id", "kind": "enum:prime|perk", "name": "r2text", "icon": "r2tex|none",
                                   "effects": "effects", "fidelity": "enum:exact|approx|fallback"}, perks)
    out["traits"] = sheet("traits", {"id": "id", "name": "r2text", "icon": "r2tex|none", "effects": "effects",
                                     "fidelity": "enum:exact|approx|fallback"}, traits)
    out["archetypes"] = sheet("archetypes", {"id": "id", "name": "r2text", "prime": "r2text|none", "desc": "r2text|none",
                                             "emblem": "r2tex|none", "skills": "list:ref:skills", "perks": "list:ref:perks",
                                             "trait": "ref:traits|none", "starter_long": "ref:weapons",
                                             "starter_hand": "ref:weapons", "starter_melee": "ref:weapons", **SHOP}, ars)

    out["materials"] = sheet("materials", {"id": "id", "rank": "enum:low|high|master", "kind": "enum:ore|bone|crystal|part|elder|gem",
                                           "mhw_item_id": "num", "note": "str"},
                             [{"id": i, "rank": r, "kind": k, "mhw_item_id": 0, "note": n} for i, r, k, n in MATERIALS])
    menus = []
    for town, zone in (("astera", 301), ("seliana", 305)):
        for role, host in (("smithy", "mccabe"), ("melder", "wallace"), ("provisions", "brabus")):
            menus.append({"id": f"{role}_{town}", "role": role, "zone": zone, "x": 0, "y": 0, "z": 0, "radius": 350, "host": host})
    out["menus"] = sheet("menus", {"id": "id", "role": "enum:smithy|melder|provisions", "zone": "num", "x": "num", "y": "num",
                                   "z": "num", "radius": "num", "host": "ref:npcs"}, menus)

    # hand-checked values (sheets/_overrides.json: "sheet.row.column": value) win over the parser
    op = os.path.join(SHEETS, "_overrides.json")
    if os.path.exists(op):
        for where, val in json.load(open(op, encoding="utf-8")).items():
            if where.startswith("_"):
                continue
            n, rid, col = where.split(".")
            for r in out.get(n, {"rows": []})["rows"]:
                if r["id"] == rid:
                    r[col] = val
                    if col == "effects":
                        r["fidelity"] = "exact"
    for n, sh in out.items():
        json.dump(sh, open(os.path.join(SHEETS, n + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    fid = {}
    for n, sh in out.items():
        for r in sh["rows"]:
            f = r.get("fidelity")
            if f:
                fid[f] = fid.get(f, 0) + 1
    print({n: len(s["rows"]) for n, s in out.items()}, fid)


if __name__ == "__main__":
    main()
