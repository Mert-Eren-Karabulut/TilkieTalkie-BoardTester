#!/usr/bin/env python3
"""Export the fault catalogue from src/Faults.h as JSON (for the portal's fallback table and docs).

    tools/faults.py                       # prints JSON to stdout
    tools/faults.py --out path.json       # writes it
    tools/faults.py --markdown            # prints the docs table
"""
import json
import re
import sys
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "src", "Faults.h")

ROW = re.compile(r'X\((\d+),\s*([A-Z0-9_]+),\s*(INFO|WARNING|ERROR|CRITICAL),\s*"([^"]*)",\s*(EVENT|ON_NETWORK|ON_PLAY|ON_BOOT),\s*"([^"]*)",\s*"([^"]*)"\)')


def catalogue():
    text = open(SRC, encoding="utf-8").read()
    start = text.index("#define TT_FAULT_LIST(X)")
    end = text.index("enum class Severity")
    rows = []
    for m in ROW.finditer(text[start:end]):
        code, slug, sev, fruit, clears, desc, remedy = m.groups()
        rows.append({"code": int(code), "slug": slug, "severity": sev.lower(), "fruit": fruit,
                     "clears": clears.lower(), "description": desc, "remedy": remedy})
    return rows


def main():
    rows = catalogue()
    if "--markdown" in sys.argv:
        print("| code | slug | severity | fruit | description | remedy |")
        print("|---|---|---|---|---|---|")
        for r in rows:
            print(f"| {r['code']} | {r['slug']} | {r['severity']} | {r['fruit']} | {r['description']} | {r['remedy']} |")
        return
    out = json.dumps(rows, ensure_ascii=False, indent=2)
    if "--out" in sys.argv:
        path = sys.argv[sys.argv.index("--out") + 1]
        with open(path, "w", encoding="utf-8") as f:
            f.write(out + "\n")
        print(f"{len(rows)} codes -> {path}")
    else:
        print(out)


if __name__ == "__main__":
    main()
