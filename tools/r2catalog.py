#!/usr/bin/env python3
"""Dev tool: list every Remnant II item the mashup can offer (archetypes, weapons, mods, mutators, rings, amulets,
relics, fragments, consumables, armor) from the player's own install, read in place.

Writes .check/catalog.json (local only, never committed or packaged): for each item its asset path, category, the
FText keys found in it and the icon texture it references. The English source strings are kept in that local file
only so an author can tell the name key from the description key; the design sheets store keys and paths, never the
text, which the mod reads from the install at runtime.
usage: r2catalog.py <Paks dir> [out.json]
"""
import json, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text

KEY = re.compile(rb"\x21\x00\x00\x00([0-9A-F]{32})\x00")
SKIP = re.compile(r"(Base|Skinned|Template|Test|Inspect|Dummy|Pickup|_UI$|_Unlocks|VisualSet|Generic|Default|Nude)", re.I)
RULES = [
    ("archetype", r"^Archetype_[A-Za-z]+$", "/Archetype"),
    ("engram", r"^Engram_[A-Za-z]+$", "/Archetype"),
    ("skill", r"^Skill_[A-Za-z0-9]+(?:_(?!Action|Aura|Actor|Buff|Ghost|Player|Cloak|Projectile|Mesh|Anim)[A-Za-z0-9]+)*$", "/Archetype"),
    ("perk", r"^(?:Prime)?Perk_[A-Za-z0-9]+$", "/Archetype"),
    ("trait", r"^Trait_[A-Za-z]+$", "/Traits/"),
    ("weapon", r"^Weapon_[A-Za-z0-9]+(_[A-Za-z0-9]+)?$", "/Weapons/"),
    ("mod", r"^Mod_[A-Za-z0-9]+$", "/Mods/"),
    ("mutator", r"^MetaGem_[A-Za-z0-9]+$", "/Gems/"),
    ("fragment", r"^RelicFragment_[A-Za-z0-9_]+$", "/Gems/"),
    ("ring", r"^Ring_[A-Za-z0-9_]+$", "/Trinkets/Rings/"),
    ("amulet", r"^Amulet_[A-Za-z0-9_]+$", "/Trinkets/Amulets/"),
    ("relic", r"^Relic_Consumable_[A-Za-z0-9_]+$", "/Items/"),
    ("relic", r"^Consumable_DragonHeart$", "/Items/"),
    ("consumable", r"^Consumable_[A-Za-z0-9_]+$", "/Items/"),
    ("armor", r"^Armor_(Head|Body|Gloves|Legs)_[A-Za-z0-9_]+$", "/Armor/"),
]
RULES = [(c, re.compile(r), d) for c, r, d in RULES]


def ftexts(data):
    out = []
    for m in KEY.finditer(data):
        p = m.end()
        if p + 4 > len(data):
            continue
        ln = struct.unpack_from("<i", data, p)[0]
        if 1 < ln < 4096 and data[p + 4 + ln - 1:p + 4 + ln] == b"\0":
            s = data[p + 4:p + 4 + ln - 1].decode("latin1")
        elif -4096 < ln < -1:
            s = data[p + 4:p + 4 - 2 * ln - 2].decode("utf-16-le", "replace")
        else:
            continue
        if s and not re.fullmatch(r"[0-9A-F]{32}", s):
            out.append([m.group(1).decode(), s])
    return out


def classify(path):
    if "/Items/" not in path and "/Archetype" not in path:
        return None
    base = path.rsplit("/", 1)[1][:-7]
    if re.match(r"^Archetype_[A-Za-z]+_UI$", base) and "/Archetype" in path:
        return "archetype_ui"
    if SKIP.search(base):
        return None
    for cat, rx, need in RULES:
        if rx.match(base) and need in path:
            return cat
    return None


def main():
    paks = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else ".check/catalog.json"
    t0 = time.time()
    items = []
    for s in r2pak.open_all(paks):
        for path in list(s.files):
            if not path.endswith(".uasset"):
                continue
            cat = classify(path)
            if cat == "consumable" and "_Action" in path:
                continue
            if not cat:
                continue
            try:
                data = s.read(s.files[path])
            except Exception as e:  # noqa: BLE001
                print("read fail", path, e, file=sys.stderr)
                continue
            texts = ftexts(data)
            if not texts:
                continue
            try:
                nm = r2text.names(data)
            except Exception:  # noqa: BLE001
                nm = []
            icons = [n for n in nm if n.startswith("/Game/") and re.search(r"(Icon|T_UI)", n.rsplit("/", 1)[-1])]
            refs = [n for n in nm if n.startswith("/Game/")]
            items.append({"cat": cat, "path": path, "texts": texts,
                          "icons": ["Remnant2/Content/" + i[len("/Game/"):] for i in icons], "refs": refs[:80]})
    json.dump(items, open(out, "w"), indent=0)
    by = {}
    for it in items:
        by[it["cat"]] = by.get(it["cat"], 0) + 1
    print(len(items), "items in", round(time.time() - t0), "s", by)


if __name__ == "__main__":
    main()
