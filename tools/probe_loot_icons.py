#!/usr/bin/env python3
"""Read only the Remnant icon assets referenced by boss loot rows; report formats for the MHW atlas spike."""
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / "build/win/r2read.exe"


def rows():
    sheets = ROOT / "sheets"
    bosses = json.loads((sheets / "bosses.json").read_text(encoding="utf-8"))["rows"]
    refs = {ref for boss in bosses for ref in boss["loot"] if ref != "none"}
    result = []
    for ref in sorted(refs):
        sheet, row = ref.split(".", 1)
        data = json.loads((sheets / f"{sheet}.json").read_text(encoding="utf-8"))["rows"]
        found = next((r for r in data if r["id"] == row), None)
        if found is None:
            raise ValueError(f"broken boss loot reference: {ref}")
        icon = found.get("icon", "none")
        if isinstance(icon, dict):
            icon = icon.get("r2tex", "none")
        name = found.get("name", "none")
        if isinstance(name, dict):
            name = name.get("r2text", "none")
        result.append((ref, icon, name))
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: probe_loot_icons.py <Remnant II install>", file=sys.stderr)
        return 2
    sources = rows()
    with subprocess.Popen([str(HELPER), sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, bufsize=0) as proc:
        assert proc.stdin and proc.stdout
        formats = Counter()
        unavailable = []
        for ref, icon, name in sources:
            if name == "none":
                formats["no-sheet-name"] += 1
                unavailable.append(ref + " (name)")
            else:
                proc.stdin.write(f"TEXT\t{name['asset']}\t{name['key']}\n".encode("utf-8"))
                head = proc.stdout.readline().decode("utf-8", "replace").strip().split()
                if len(head) != 2 or head[0] != "OK":
                    formats["name-unavailable"] += 1
                    unavailable.append(ref + " (name)")
                else:
                    body = proc.stdout.read(int(head[1]))
                    if body:
                        formats["name-resolved"] += 1
                    else:
                        formats["name-empty"] += 1
                        unavailable.append(ref + " (name)")
            if icon == "none":
                formats["no-sheet-icon"] += 1
                unavailable.append(ref)
                continue
            asset = icon.removesuffix(".uasset")
            proc.stdin.write(f"TEX\t{asset}\t64\n".encode("utf-8"))
            head = proc.stdout.readline().decode("utf-8", "replace").strip()
            bits = head.split()
            if len(bits) != 5 or bits[0] != "OK":
                formats["unavailable"] += 1
                unavailable.append(ref)
                continue
            fmt, w, h, size = bits[1], int(bits[2]), int(bits[3]), int(bits[4])
            blob = proc.stdout.read(size)
            if len(blob) != size:
                raise RuntimeError(f"short texture read: {ref}")
            formats[f"{fmt}:{w}x{h}"] += 1
        proc.stdin.write(b"QUIT\n")
    print(f"boss-loot-rows={len(sources)}")
    for key, count in sorted(formats.items()):
        print(f"{key}: {count}")
    if unavailable:
        print("missing:", ", ".join(unavailable))
    return 0 if not unavailable else 1


if __name__ == "__main__":
    sys.exit(main())
