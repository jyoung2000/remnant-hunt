#!/usr/bin/env python3
"""Check Remnant II player animation data and MHW bone-map coverage from the installed game.

Reads RSKL/RANM responses from r2read.exe in memory. No game assets are written or packaged.
Usage: check_player_anims.py <r2read.exe> <Remnant II install dir> [sheets dir]
"""
import json
import os
import struct
import subprocess
import sys


def main():
    exe, game = sys.argv[1:3]
    sheets_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "sheets")

    def sheet(name):
        with open(os.path.join(sheets_dir, name), encoding="utf-8") as f:
            return json.load(f)

    animations = sheet("player_anims.json")
    hunter = sheet("hunter_bones.json")
    paths = {r["id"]: r["asset"] for r in animations["rows"]}
    frames = {r["id"]: r["frames"] for r in animations["rows"]}
    hunter_names = {r["r2"] for r in hunter["rows"] if r.get("r2") not in (None, "none", "")}
    used = set()
    for name in ("relics.json", "skills.json", "consumables.json"):
        for row in sheet(name)["rows"]:
            anim = row.get("anim", "none")
            if anim not in (None, "", "none"):
                used.add(anim)
    # Aim-pose rows are activated continuously by hunter_tick rather than a relic/skill sheet reference.
    used.update(r["id"] for r in animations["rows"] if r.get("use") == "aim")
    missing = sorted(used - set(paths))
    if missing:
        raise SystemExit("unresolved player animation references: " + ", ".join(missing))

    proc = subprocess.Popen([exe, game], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def ask(command):
        proc.stdin.write((command + "\n").encode("utf-8"))
        proc.stdin.flush()
        head = proc.stdout.readline().decode("utf-8", "replace").strip()
        if not head.startswith("OK "):
            raise RuntimeError(head)
        size = int(head.split()[-1])
        body = proc.stdout.read(size)
        if len(body) != size:
            raise RuntimeError("short r2read response")
        return body

    bad = []
    try:
        raw = ask("SKEL\tRemnant2/Content/Characters/Player/Male/Mesh/Player_M_Skeleton.uasset")
        if raw[:4] != b"RSKL":
            raise RuntimeError("player skeleton response is not RSKL")
        count = struct.unpack_from("<I", raw, 4)[0]
        p = 8
        bones = []
        for _ in range(count):
            n = struct.unpack_from("<H", raw, p)[0]
            p += 2
            bones.append(raw[p:p + n].decode("utf-8", "replace"))
            p += n + 44  # name, parent, quaternion, translation and scale

        print(f"Remnant II player skeleton: {len(bones)} bones; {len(hunter_names)} map to the Monster Hunter hunter rig")
        for row in animations["rows"]:
            blob = ask("ANIM\t" + row["asset"])
            if blob[:4] != b"RANM" or len(blob) < 16:
                bad.append(row["id"] + ": invalid RANM header")
                continue
            tracks, samples, rate = struct.unpack_from("<IIf", blob, 4)
            end = 16 + tracks * 4 + tracks * samples * 40
            if tracks <= 0 or samples <= 0 or rate <= 0 or end != len(blob):
                bad.append(row["id"] + ": invalid track/sample payload")
                continue
            track_bones = struct.unpack_from("<%di" % tracks, blob, 16)
            names = {bones[i] for i in track_bones if 0 <= i < len(bones)}
            mapped = names & hunter_names
            if frames.get(row["id"]) != samples:
                bad.append(f"{row['id']}: sheet frames={frames.get(row['id'])}, asset samples={samples}")
            if row["id"] in used and not mapped:
                bad.append(row["id"] + ": animation has no tracks on mapped hunter bones")
            print(f"{row['id']}: {samples} frames, {tracks} tracks, {len(mapped)} mapped hunter bones")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
    if bad:
        raise SystemExit("FAILED: " + "; ".join(bad))
    print(f"PASS: {len(animations['rows'])} animation assets parse; all used animations drive mapped hunter bones.")


if __name__ == "__main__":
    main()
