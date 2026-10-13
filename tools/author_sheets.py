#!/usr/bin/env python3
"""One-time bootstrap of the design sheets (sheets/*.json). After this, edit the sheets directly.

Remnant II content is referenced, never copied: a text cell stores {asset, key} (the FText key inside that
package) and a texture cell stores the asset path. The English text is read from the player's own install at
runtime. Picks are made by text index inside the package, resolved here against Jalon's install.
usage: author_sheets.py <Remnant2 Paks dir> <sheets dir>
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text

C = "Remnant2/Content/"
ARCH = C + "World_Base/Items/Archetypes/"
WPN = C + "World_Base/Items/Weapons/"
ICON_W = C + "UI2/UI_Icons/UI_Icons_Gear/UI_Icons_Gear_Weapons/UI_Icons_Gear_Weapons_Standard/"
W13 = C + "World_Base/Quests/Quest_Ward13/"

idx = {}


def load(paks):
    for s in r2pak.open_all(paks):
        for k, v in s.files.items():
            idx[k] = (s, v)


def T(asset, i=None, key_prefix=None):
    """Text cell: pick by index among unique FTexts, or by key prefix. Stores only asset + key."""
    s, e = idx[asset]
    seen, uniq = set(), []
    for k, v in r2text.texts(s.read(e)):
        if k not in seen:
            seen.add(k); uniq.append(k)
    if key_prefix:
        key = next(k for k in uniq if k.startswith(key_prefix))
    else:
        key = uniq[i]
    return {"r2text": {"asset": asset, "key": key}}


def X(asset):
    """Texture cell: a Texture2D package in the player's install."""
    assert asset + ".uasset" in idx, asset
    return {"r2tex": asset}


def N(asset, name):
    """Name cell: an FName stored in a package's name map (used for proper nouns such as NPC names)."""
    s, e = idx[asset]
    assert name in r2text.names(s.read(e)), (asset, name)
    return {"r2name": {"asset": asset, "name": name}}


def sheet(name, columns, rows, phase="v0.1"):
    return {"sheet": name, "phase": phase, "columns": columns, "rows": rows}


def build():
    S = {}
    S["archetypes"] = sheet("archetypes", {
        "id": "id", "name": "r2text", "prime": "r2text", "desc": "r2text", "emblem": "r2tex",
        "skill": "ref:skills", "long_gun": "ref:weapons", "hand_gun": "ref:weapons",
        "ranged_mult": "num", "hp_bonus": "num", "damage_taken_mult": "num", "reload_mult": "num",
        "relic_heal_mult": "num"}, [
        dict(id="hunter", name=T(ARCH + "Hunter/Archetype_Hunter_UI.uasset", 0),
             prime=T(ARCH + "Hunter/Archetype_Hunter_UI.uasset", 1), desc=T(ARCH + "Hunter/Archetype_Hunter_UI.uasset", 2),
             emblem=X(C + "UI2/UI_Icons/UI_Icons_Archetype/T_UI_Icon_Archetype_Sharpshooter"), skill="hunters_mark",
             long_gun="huntmaster_m1", hand_gun="service_pistol", ranged_mult=1.10, hp_bonus=0,
             damage_taken_mult=1.0, reload_mult=1.0, relic_heal_mult=1.0),
        dict(id="medic", name=T(ARCH + "Medic/Archetype_Medic_UI.uasset", 0),
             prime=T(ARCH + "Medic/Archetype_Medic_UI.uasset", 1), desc=T(ARCH + "Medic/Archetype_Medic_UI.uasset", 2),
             emblem=X(C + "UI2/UI_Icons/UI_Icons_Archetype/T_UI_Icon_Archetype_Medic"), skill="wellspring",
             long_gun="blackmaw_ar47", hand_gun="service_pistol", ranged_mult=1.0, hp_bonus=15,
             damage_taken_mult=1.0, reload_mult=1.0, relic_heal_mult=1.25),
        dict(id="challenger", name=T(ARCH + "Challenger/Archetype_Challenger_UI.uasset", 0),
             prime=T(ARCH + "Challenger/Archetype_Challenger_UI.uasset", 1),
             desc=T(ARCH + "Challenger/Archetype_Challenger_UI.uasset", 2),
             emblem=X(C + "UI2/UI_Icons/UI_Icons_Archetype/T_UI_Icon_Archetype_Defender"), skill="juggernaut",
             long_gun="coach_gun", hand_gun="western_classic", ranged_mult=1.0, hp_bonus=25,
             damage_taken_mult=0.9, reload_mult=1.0, relic_heal_mult=1.0),
        dict(id="gunslinger", name=T(ARCH + "Gunslinger/Archetype_Gunslinger_UI.uasset", 0),
             prime=T(ARCH + "Gunslinger/Archetype_Gunslinger_UI.uasset", 1),
             desc=T(ARCH + "Gunslinger/Archetype_Gunslinger_UI.uasset", 2),
             emblem=X(C + "UI2/UI_Icons/UI_Icons_Archetype/T_UI_Icon_Archetype_Gunslinger"), skill="bulletstorm",
             long_gun="blackmaw_ar47", hand_gun="western_classic", ranged_mult=1.05, hp_bonus=0,
             damage_taken_mult=1.0, reload_mult=0.75, relic_heal_mult=1.0),
    ])
    sk = C + "UI2/UI_Icons/UI_Icons_Skills/"
    S["skills"] = sheet("skills", {
        "id": "id", "name": "r2text", "icon": "r2tex", "effect": "enum:monster_damage_mult|heal_over_time|damage_reduction|infinite_ammo",
        "magnitude": "num", "magnitude2": "num", "duration_s": "num", "cooldown_s": "num"}, [
        dict(id="hunters_mark", name=T(ARCH + "Hunter/Skills/HuntersMark/Skill_HuntersMark.uasset", 0),
             icon=X(sk + "UI_Icons_Skills_Hunter/T_UI_Icon_Skill_Hunter_HuntersMark"), effect="monster_damage_mult",
             magnitude=1.15, magnitude2=0.0, duration_s=25, cooldown_s=60),
        dict(id="wellspring", name=T(ARCH + "Medic/Skills/Wellspring/Skill_Wellspring.uasset", 0),
             icon=X(sk + "UI_Icons_Skills_Medic/T_UI_Icon_Skill_Medic_Wellspring"), effect="heal_over_time",
             magnitude=3.0, magnitude2=0.0, duration_s=15, cooldown_s=70),
        dict(id="juggernaut", name=T(ARCH + "Challenger/Skills/Battlecry_Juggernaut/Skill_Battlecry_Juggernaut.uasset", 0),
             icon=X(sk + "UI_Icons_Skills_Challenger/T_UI_Icon_Skill_Challenger_Juggernaut"), effect="damage_reduction",
             magnitude=0.6, magnitude2=1.25, duration_s=20, cooldown_s=90),
        dict(id="bulletstorm", name=T(ARCH + "Gunslinger/Skills/Bulletstorm/Skill_BulletStorm.uasset", 0),
             icon=X(sk + "UI_Icons_Skills_Gunslinger/T_UI_Icon_Skill_Gunslinger_Bulletstorm"), effect="infinite_ammo",
             magnitude=1.25, magnitude2=0.0, duration_s=20, cooldown_s=90),
    ])

    def gun(id_, slot, asset, icon, dmg, rpm, mag, reserve, reload_s, weak, rng, spread, pellets, mod, price):
        return dict(id=id_, slot=slot, name=T(asset, 0), desc=T(asset, 1), icon=X(icon), damage=dmg, rpm=rpm,
                    mag=mag, reserve=reserve, reload_s=reload_s, weakspot_mult=weak, range_m=rng,
                    spread_deg=spread, pellets=pellets, mod=mod, price_scrap=price, sold_by="brabus")

    S["weapons"] = sheet("weapons", {
        "id": "id", "slot": "enum:long|hand", "name": "r2text", "desc": "r2text", "icon": "r2tex",
        "damage": "num", "rpm": "num", "mag": "num", "reserve": "num", "reload_s": "num", "weakspot_mult": "num",
        "range_m": "num", "spread_deg": "num", "pellets": "num", "mod": "ref:mods|none", "price_scrap": "num",
        "sold_by": "ref:npcs"}, [
        gun("service_pistol", "hand", WPN + "HandGuns/Standard/ServicePistol/Weapon_ServicePistol.uasset",
            ICON_W + "T_UI_Weapon_ServicePistol_Icon_NEW", 24, 400, 10, 150, 1.5, 2.0, 45, 1.0, 1, "none", 0),
        gun("western_classic", "hand", WPN + "HandGuns/Standard/Revolver/Weapon_Revolver.uasset",
            ICON_W + "T_UI_Weaponl_WesternClassic_Icon", 50, 240, 6, 90, 2.2, 2.0, 50, 0.8, 1, "none", 250),
        gun("tech_22", "hand", WPN + "HandGuns/Standard/MachinePistol/Weapon_MachinePistol.uasset",
            ICON_W + "T_UI_Weapon_MachinePistol_Icon", 12, 900, 30, 240, 1.8, 1.6, 35, 2.5, 1, "none", 300),
        gun("blackmaw_ar47", "long", WPN + "LongGuns/Standard/AssaultRifle/Weapon_AssaultRifle.uasset",
            ICON_W + "T_UI_Weapon_AssaultRifle_Icon", 22, 600, 30, 240, 2.4, 1.8, 60, 1.5, 1, "hot_shot", 400),
        gun("coach_gun", "long", WPN + "LongGuns/Standard/Coachgun/Weapon_CoachGun.uasset",
            ICON_W + "T_UI_Weapon_CoachGun_Icon", 16, 120, 2, 40, 2.0, 1.5, 20, 6.0, 8, "concussive_shot", 400),
        gun("huntmaster_m1", "long", WPN + "LongGuns/Standard/Huntmaster/Weapon_Huntmaster.uasset",
            ICON_W + "T_UI_Weapon_HuntMaster_Icon", 85, 75, 6, 60, 2.6, 2.25, 120, 0.3, 1, "corrosive_rounds", 450),
        gun("widowmaker", "long", WPN + "LongGuns/Standard/Sniper_Rifle/Weapon_SniperRifle.uasset",
            ICON_W + "T_UI_Weapon_SniperRifle_Icon", 140, 50, 4, 40, 3.0, 2.5, 150, 0.1, 1, "healing_shot", 600),
    ])
    MOD = C + "World_Base/Items/Mods/"
    MI = C + "UI2/UI_Icons/UI_Icons_Mods/"
    S["mods"] = sheet("mods", {
        "id": "id", "name": "r2text", "desc": "r2text", "icon": "r2tex",
        "effect": "enum:burn|heal_burst|blast|corrode", "charge_damage": "num", "magnitude": "num", "duration_s": "num"}, [
        dict(id="hot_shot", name=T(MOD + "HotShot/Mod_Hotshot.uasset", 0), desc=T(MOD + "HotShot/Mod_Hotshot.uasset", 1),
             icon=X(MI + "T_UI_Mod_Hotshot"), effect="burn", charge_damage=600, magnitude=8, duration_s=15),
        dict(id="healing_shot", name=T(MOD + "HealingShot/Mod_HealingShot.uasset", 0),
             desc=T(MOD + "HealingShot/Mod_HealingShot.uasset", 1), icon=X(MI + "T_UI_Mod_HealingShot"),
             effect="heal_burst", charge_damage=700, magnitude=40, duration_s=0),
        dict(id="concussive_shot", name=T(MOD + "ConcussiveShot/Mod_ConcussiveShot.uasset", 0),
             desc=T(MOD + "ConcussiveShot/Mod_ConcussiveShot.uasset", 1), icon=X(MI + "T_UI_Mod_ConcussiveShot"),
             effect="blast", charge_damage=650, magnitude=180, duration_s=0),
        dict(id="corrosive_rounds", name=T(MOD + "CorrosiveRounds/Mod_CorrosiveRounds.uasset", 0),
             desc=T(MOD + "CorrosiveRounds/Mod_CorrosiveRounds.uasset", 1), icon=X(MI + "T_UI_Mod_CorrosiveRounds"),
             effect="corrode", charge_damage=650, magnitude=1.10, duration_s=20),
    ])
    DH = C + "Items/Common/Consumable_DragonHeart.uasset"
    S["relics"] = sheet("relics", {
        "id": "id", "name": "r2text", "desc": "r2text", "icon": "r2tex", "charges": "num",
        "heal_pct_max_hp": "num", "heal_over_s": "num", "use_time_s": "num", "refill_on": "enum:base_or_camp"}, [
        dict(id="dragon_heart", name=T(DH, key_prefix="89C817"), desc=T(DH, key_prefix="B562DB"),
             icon=X(C + "UI2/UI_Icons/UI_Icons_Gear/UI_Icons_Gear_Relics/T_UI_Relic_DragonHeart_Icon"),
             charges=3, heal_pct_max_hp=60, heal_over_s=1.5, use_time_s=0.8, refill_on="base_or_camp"),
    ])
    BR = W13 + "EventTree_Brabus_PlayerBase.uasset"
    MC = W13 + "EventTree_AvaMcCabe_PlayerBase.uasset"
    S["npcs"] = sheet("npcs", {
        "id": "id", "name": "r2name", "role": "enum:guns|upgrades", "greet_1": "r2text", "greet_2": "r2text",
        "greet_3": "r2text", "open_option": "r2text", "leave_option": "r2text", "shop_line": "r2text",
        "done_line": "r2text",
        "banner": "r2tex"}, [
        dict(id="brabus", name=N(W13 + "Quest_Ward13.uasset", "Brabus"), role="guns",
             greet_1=T(BR, key_prefix="77B6235F"), greet_2=T(BR, key_prefix="D12B2A1A"), greet_3=T(BR, key_prefix="5ED0BF74"),
             open_option=T(BR, key_prefix="B370F127"), leave_option=T(MC, key_prefix="DDBF3BD3"),
             shop_line=T(BR, key_prefix="1D01907E"), done_line=T(BR, key_prefix="20B7BAFB"),
             banner=X(C + "UI2/UI_Core/UI_Textures/UI_Textures_Travel/UI_Textures_Travel_Banners/UI_Textures_Travel_Banners_Waypoints_MainCampaign/Ward13_Waypoint_Town")),
        dict(id="mccabe", name=N(C + "World_Base/Global/Speakers/Speaker_AvaMcCabe.uasset", "Speaker_AvaMcCabe_C"),
             role="upgrades",
             greet_1=T(MC, key_prefix="221ED031"), greet_2=T(MC, key_prefix="F2240164"), greet_3=T(MC, key_prefix="E3FF9D28"),
             open_option=T(MC, key_prefix="CD8D5C15"), leave_option=T(MC, key_prefix="DDBF3BD3"),
             shop_line=T(MC, key_prefix="06491D8F"), done_line=T(MC, key_prefix="8095C752"),
             banner=X(C + "UI2/UI_Core/UI_Textures/UI_Textures_Travel/UI_Textures_Travel_Banners/UI_Textures_Travel_Banners_Waypoints_MainCampaign/Ward13_Waypoint_Town")),
    ])
    S["upgrades"] = sheet("upgrades", {"id": "id", "level": "num", "cost_scrap": "num", "damage_mult": "num",
                                       "done_by": "ref:npcs"},
                          [dict(id="plus_%d" % l, level=l, cost_scrap=c, damage_mult=m, done_by="mccabe")
                           for l, c, m in [(1, 150, 1.10), (2, 300, 1.20), (3, 500, 1.32), (4, 800, 1.45), (5, 1200, 1.60)]])
    return S


def main():
    load(sys.argv[1])
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name, sh in build().items():
        with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(sh, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print("wrote", name, len(sh["rows"]), "rows")


if __name__ == "__main__":
    main()
