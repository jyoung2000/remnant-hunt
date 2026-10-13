#!/usr/bin/env python3
"""Dev tool: point the sheets at Remnant II's own sound events (read from the player's install at runtime).

Adds columns of type r2snd (an event package path, or none) to weapons (snd_fire, snd_reload), mods, skills, relics
and consumables (snd_use), and a ui_sounds sheet for the Remnant menus. An event is kept only when its embedded
soundbank lists Vorbis sounds whose media exist. Run after tools/author_arsenal.py.
usage: author_sounds.py <Remnant2/Content/Paks>
"""
import json, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak

PAKS = sys.argv[1]
SHEETS = os.path.join(os.getcwd(), "sheets")
STORES = r2pak.open_all(PAKS)
FILES = {}
for s in STORES:
    for k, v in s.files.items():
        FILES[k] = (s, v)
EV = sorted(k for k in FILES if k.startswith("Remnant2/Content/WwiseAudio/Events/") and k.endswith(".uasset")
            and not k.endswith("_FOLDER.uasset"))


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def playable(ev):
    """The event's soundbank has Vorbis sounds with media in the install."""
    s, e = FILES[ev]
    d = s.read(e)
    p = d.find(b"BKHD")
    if p < 0:
        return False
    while p + 8 <= len(d):
        tag, sz = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if tag == b"HIRC":
            n = struct.unpack_from("<I", d, p + 8)[0]
            q = p + 12
            for _ in range(n):
                t, ln = d[q], struct.unpack_from("<I", d, q + 1)[0]
                if t == 2:
                    plug, src = struct.unpack_from("<I", d, q + 9)[0], struct.unpack_from("<I", d, q + 14)[0]
                    if plug == 0x00040001 and f"Remnant2/Content/WwiseAudio/Media/{src}.ubulk" in FILES:
                        return True
                q += 5 + ln
            return False
        if not tag.isalpha():
            return False
        p += 8 + sz
    return False


def pick(cands, *patterns):
    # one-shot sounds first: loops, stops and impacts only when nothing else fits
    for avoid in (r"Loop|STOP|_Stop|Impact|Dissipate", None):
        for pat in patterns:
            for ev in cands:
                b = ev.rsplit("/", 1)[1]
                if avoid and re.search(avoid, b):
                    continue
                if re.search(pat, b, re.I) and playable(ev):
                    return ev
    return "none"


def folder_events(dirname_norm, roots):
    out = []
    for ev in EV:
        parts = ev.split("/")
        if any(r in ev for r in roots) and any(norm(x) == dirname_norm for x in parts[4:-1]):
            out.append(ev)
    return out


def load(n):
    return json.load(open(os.path.join(SHEETS, n + ".json"), encoding="utf-8"))


def save(n, sh):
    json.dump(sh, open(os.path.join(SHEETS, n + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)


def asset_dir(row):
    a = row["name"]["r2text"]["asset"]
    return norm(a.split("/")[-2])


hits = {}
# weapons: fire and reload from the gun's own event folder
w = load("weapons")
w["columns"]["snd_fire"] = "r2snd|none"
w["columns"]["snd_reload"] = "r2snd|none"
for r in w["rows"]:
    r["snd_fire"] = r["snd_reload"] = "none"
    if r["slot"] == "melee":
        continue
    keys = {asset_dir(r), norm(r["id"]), norm(r["name"]["r2text"]["asset"].rsplit("/", 1)[1][:-7].replace("Weapon_", ""))}
    cands = []
    for k in keys:
        cands = folder_events(k, ["/Weapons/"])
        if cands:
            break
    r["snd_fire"] = pick(cands, r"_Shoot\.uasset$", r"_Shoot_Start", r"_Shoot", r"_Fire")
    r["snd_reload"] = pick(cands, r"Reload\.uasset$", r"Mag_In", r"Reload", r"Load")
    hits["weapons"] = hits.get("weapons", 0) + (r["snd_fire"] != "none")
save("weapons", w)

# mods: activate/shoot events in the mod's folder (standalone mods) or in the boss gun's folder
m = load("mods")
m["columns"]["snd_use"] = "r2snd|none"
for r in m["rows"]:
    k = norm(r["name"]["r2text"]["asset"].rsplit("/", 1)[1][:-7].replace("Mod_", ""))
    cands = folder_events(k, ["/Weapons/Mods/"]) or [e for e in EV if "/Weapons/" in e and norm(e.rsplit("/", 1)[1]).startswith("mod" + k)]
    r["snd_use"] = pick(cands, r"Activate\.uasset$", r"Activate", r"_Shoot\.uasset$", r"Shoot", r"Cast", r"Fire")
    hits["mods"] = hits.get("mods", 0) + (r["snd_use"] != "none")
save("mods", m)

# skills: Player_Archetypes/<Archetype>/<Archetype>_<Skill>_Cast
sk = load("skills")
sk["columns"]["snd_use"] = "r2snd|none"
arch = load("archetypes")
owner = {}
for a in arch["rows"]:
    for s in a["skills"]:
        owner[s] = a["id"]
for r in sk["rows"]:
    a = owner.get(r["id"], "")
    base = norm(r["name"]["r2text"]["asset"].rsplit("/", 1)[1][:-7].replace("Skill_", "").replace(a, "", 1)) if a else ""
    cands = [e for e in EV if "/Player/Player_Archetypes" in e and norm(e.split("/")[-2]) == norm(a)]
    words = [x for x in re.findall(r"[A-Z][a-z]+", r["name"]["r2text"]["asset"].rsplit("/", 1)[1][:-7].replace("Skill_", "")) if x.lower() != a]
    pats = [r"(?i)" + "".join(words) + r".*(Cast|Activate)", r"(?i)" + "".join(words)] if words else []
    pats += [r"(?i)" + "".join(words[-1:]) + r".*Cast\.uasset$"] if words else []
    pats += [r"(?i)" + w_ + r".*Cast" for w_ in words] + [r"(?i)" + w_ for w_ in words]
    r["snd_use"] = pick(cands, *pats) if cands and pats else "none"
    hits["skills"] = hits.get("skills", 0) + (r["snd_use"] != "none")
save("skills", sk)

# relics share the Dragon Heart's use sound (Remnant II plays the heart being absorbed)
heart = pick([e for e in EV if "/DragonHeart/" in e], r"DragonHeart_Absorb\.uasset$")
rl = load("relics")
rl["columns"]["snd_use"] = "r2snd|none"
for r in rl["rows"]:
    r["snd_use"] = heart
save("relics", rl)

# consumables: their own folder, else drinking / eating / throwing from the shared foley
c = load("consumables")
c["columns"]["snd_use"] = "r2snd|none"
shared = [e for e in EV if "/Consumables/Consumables_Shared/" in e]
drink = pick(shared, r"Consumable_Foley_Drink\.uasset$")
eat = pick(shared, r"Food_Eat_Meat")
throw = pick(shared, r"Whoosh_Throw")
magic = pick(shared, r"Buff_Magic")
for r in c["rows"]:
    k = norm(r["name"]["r2text"]["asset"].rsplit("/", 1)[1][:-7].replace("Consumable_", ""))
    own = folder_events(k, ["/Consumables/"])
    snd = pick(own, r"Use", r"Throw", r"Activate", r".")
    if snd == "none":
        e = r["effects"]
        snd = throw if "grenade" in e or "burst" in e else eat if re.search(r"jerky|meat|fruit|stew|pellet|leaf", r["id"]) else drink
    r["snd_use"] = snd
save("consumables", c)

# the Remnant menus' own sounds
ui = {"sheet": "ui_sounds", "phase": "v0.2", "columns": {"id": "id", "event": "r2snd"}, "rows": []}
for rid, pat in (("craft", r"UI/Widget_Crafting/UI_Crafting_Craft\.uasset$"), ("buy", r"UI_BuySellConfirm_Buy\.uasset$"),
                 ("upgrade", r"UI_UpgradeConfirm\.uasset$"), ("equip", r"UI/Equip/UI_Equip_Weapon_Ranged_AssaultRifle\.uasset$"),
                 ("open", r"UI/Widget_Screen_Terminal/.*\.uasset$"), ("socket", r"UI_RelicFragments_Gem_Socket\.uasset$")):
    ev = pick([e for e in EV if re.search(pat, e)], r".")
    if ev != "none":
        ui["rows"].append({"id": rid, "event": ev})
save("ui_sounds", ui)
print("sound events:", hits, "relic", heart != "none", "ui", len(ui["rows"]))
