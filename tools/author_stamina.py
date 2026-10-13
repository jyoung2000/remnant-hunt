#!/usr/bin/env python3
"""Dev tool: split the arsenal's one "stamina" stat into the three that act on MHW's own stamina bar.

  stamina_max    flat points of max stamina          ("Increases Max Stamina by 20")
  stamina_regen  percent faster stamina regeneration ("Increases Stamina Regen by 25", flat /s as a share of 33/s)
  stamina_cost   percent change of stamina costs, - is cheaper ("Reduces Stamina Cost by 50%"; armor weight and
                 encumbrance make it dearer: half a percent per point)

Reads the item descriptions in the Remnant 2 Toolkit data (.check/toolkit.json, the numbers only; no text is copied).
Each old stamina clause keeps its trigger or buff duration. Rows without a readable description are converted as the
old authoring meant them: a percent was a cheaper stamina cost (or movement), a flat number was max stamina.
usage: author_stamina.py
"""
import json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SHEETS = os.path.join(ROOT, "sheets")
TK = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))
CAT_SHEETS = {"weapon": "weapons", "mod": "mods", "mutator": "mutators", "ring": "rings", "amulet": "amulets",
              "relic": "relics", "consumable": "consumables", "concoction": "consumables", "armor": "armor",
              "perk": "perks", "trait": "traits", "relic-fragment": "fragments", "skill": "skills"}
OLD = re.compile(r"^(?P<pre>(?:[a-z_]+(?:\(\d+\))?:)?|use:buff )stamina(?P<v>[+-][\d.]+)(?P<pct>%?)(?P<rest>.*)$")
N = r"(\d+(?:\.\d+)?)(?:\s*-+>\s*(\d+(?:\.\d+)?))?\s*(%?)"
REGEN_BASE = 33.0  # Remnant II's stamina regeneration per second, for flat bonuses


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def fmt(v):
    return f"{v:+g}" if abs(v - round(v)) < 1e-6 else f"{v:+.1f}"


def facts(desc):
    """(stat, value, pct) for each stamina change the description states."""
    out = []
    d = desc.replace("\n", " ")
    for sent in re.split(r"(?<=[.!])\s+", d):
        low = sent.lower()
        if "stamina" not in low and "encumbrance" not in low:
            continue
        verbs = [(v.start(), v.group(1)) for v in re.finditer(r"\b(increas|reduc|decreas|lower|negat)\w*", low)]
        for m in re.finditer(r"(max(?:imum)? (?:health and )?stamina|stamina regen(?:eration)?(?: penalty| delay)?|stamina recovery"
                             r"(?: delay)?|stamina (?:cost|consumption)|encumbrance)[a-z ]*? by " + N, low):
            what, v, top, pct = m.group(1), float(m.group(2)), m.group(3), m.group(4)
            before = [w for at, w in verbs if at < m.start(2)]  # the verb this number belongs to
            sign = -1 if before and before[-1] in ("reduc", "decreas", "lower", "negat") else 1
            if top:  # "3 -> 30": levels; the top one
                v = float(top)
            rng = re.match(r"\s*-\s*(\d+(?:\.\d+)?)%", low[m.end():])  # "25% - 50%": the middle
            if rng:
                v = (v + float(rng.group(1))) / 2.0
            if "penalty" in what or "delay" in what:
                out.append(("stamina_regen", -sign * v * 0.4, True))  # less penalty or delay: faster regen
            elif what.startswith("max"):
                out.append(("stamina_max", sign * v, False))
            elif "regen" in what or "recovery" in what:
                out.append(("stamina_regen", sign * (v if pct else v / REGEN_BASE * 100.0), True))
            elif what == "encumbrance":
                out.append(("stamina_cost", sign * v / 2.0, True))
            else:
                out.append(("stamina_cost", sign * v, True))
        for m in re.finditer(r"costs? (\d+(?:\.\d+)?)% (?:additional|more|extra) stamina", low):
            out.append(("stamina_cost", float(m.group(1)), True))
        for m in re.finditer(r"(\d+(?:\.\d+)?) stamina regen(?:eration)? per second", low):
            out.append(("stamina_regen", float(m.group(1)) / REGEN_BASE * 100.0, True))
    return out


sheets = {n: json.load(open(os.path.join(SHEETS, n + ".json"), encoding="utf-8")) for n in set(CAT_SHEETS.values())}
rows = {n: {norm(r["id"]): r for r in sh["rows"]} for n, sh in sheets.items()}
desc_of = {}
for cat, items in TK.items():
    sheet = CAT_SHEETS.get(cat)
    if not sheet:
        continue
    for it in items:
        slug = re.sub(r"^[A-Za-z]+_(?:Head_|Body_|Gloves_|Legs_)?|_C$", "", it.get("saveFileSlug") or "")
        row = rows[sheet].get(norm(it["name"])) or rows[sheet].get(norm(slug))
        if row and it.get("description"):
            desc_of[(sheet, row["id"])] = it["description"]
SHOW = os.environ.get("SHOW_DESC")

changed, report = {n: 0 for n in sheets}, {"description": 0, "fallback": 0}
for n, sh in sheets.items():
    for row in sh["rows"]:
        eff = row.get("effects")
        if not isinstance(eff, str) or "stamina" not in eff or "stamina_" in eff:
            continue
        desc = desc_of.get((n, row["id"]), "") if n != "armor" else ""
        found = facts(desc)
        # The old authoring read movement speed as stamina. Movement is MHW's own, so it stands in as a cheaper stamina
        # cost (half the percent); bonuses while downed have nothing to act on.
        moves = bool(desc) and not re.search(r"(?i)stamina|encumbrance", desc) and "movement speed" in desc.lower()
        downed = bool(desc) and not re.search(r"(?i)stamina|encumbrance", desc) and "downed" in desc.lower()
        out = []
        for c in eff.split(";"):
            m = OLD.match(c)
            if not m:
                out.append(c)
                continue
            pre, rest = m.group("pre"), m.group("rest")
            if found:
                for stat, v, pct in found:
                    out.append(f"{pre}{stat}{fmt(v)}{'%' if pct else ''}{rest}")
                found = []  # one description, written once
                report["description"] += 1
            elif downed:
                report["fallback"] += 1
            elif moves and m.group("pct"):
                v = float(m.group("v"))
                out.append(f"{pre}stamina_cost{fmt(-min(abs(v), 40.0) / 2.0 * (1 if v > 0 else -1))}%{rest}")
                report["fallback"] += 1
            else:
                v, pct = float(m.group("v")), m.group("pct")
                if pct:  # the old "+ is good" percent: a cheaper (or, negative, dearer) stamina cost
                    out.append(f"{pre}stamina_cost{fmt(-v)}%{rest}")
                else:
                    out.append(f"{pre}stamina_max{fmt(v)}{rest}")
                report["fallback"] += 1
        new = ";".join(dict.fromkeys(out))
        if new != eff:
            print(f"{n:11} {row['id'][:24]:24} {eff[:60]:60} -> {new[:80]}")
            if SHOW and (n, row["id"]) in desc_of:
                print("            ", " ".join(x for x in re.split(r"(?<=[.!])\s+", desc_of[(n, row["id"])].replace("\n", " "))
                                            if re.search(r"(?i)stamina|encumbrance", x))[:230])
            row["effects"] = new
            changed[n] += 1
for n, sh in sheets.items():
    if changed[n]:
        json.dump(sh, open(os.path.join(SHEETS, n + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print("rows changed:", {n: c for n, c in changed.items() if c}, report)
