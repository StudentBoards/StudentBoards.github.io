#!/usr/bin/env python3
"""
test_svf_parity.py — the page and the CLI must generate the same SVF.

There are two implementations of the avrsvf0 port: AVRSVF in index.html
and tools/avrsvf.py. Two copies of anything drift, and the way this one
would drift is quiet — a page that programs a board slightly differently
from the command line is a bug nobody reports as one, because each works
on its own.

So this pulls the AVRSVF block straight out of index.html, runs it under
node, and compares its output byte for byte against the Python. It fails
on the first difference, with the line that differs.

Needs node. Run it as part of tests/run_tests.sh.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import avrsvf                                          # noqa: E402

PAGE_HTML = os.path.join(HERE, "..", "index.html")


def extract_generator():
    """Lift the AVRSVF module out of index.html.

    Taken from the page rather than from a copy, so this cannot pass
    against a generator the page does not actually ship.
    """
    with open(PAGE_HTML, encoding="utf-8") as f:
        html = f.read()

    start = html.find("const AVRSVF = (function () {")
    if start < 0:
        raise SystemExit("Could not find the AVRSVF block in index.html.")

    # The module ends at the first `})();` sitting at the start of a line,
    # which is the IIFE's own close — nothing nested inside is unindented.
    end = html.find("\n})();", start)
    if end < 0:
        raise SystemExit("Could not find the end of the AVRSVF block.")
    return html[start:end + len("\n})();")]


HARNESS = """
%s

// Read from a file, not argv: a full 32 KB image does not fit in a
// Windows command line.
const cases = JSON.parse(require('fs').readFileSync(process.argv[2], 'utf8'));
const out = [];
for (const c of cases) {
  const opt = Object.assign({}, c);
  opt.image = (c.image === null) ? null : Uint8Array.from(c.image);
  try {
    out.push({ ok: true, svf: AVRSVF.build(opt) });
  } catch (e) {
    out.push({ ok: false, error: e.message });
  }
}
out.push({ modes: [AVRSVF.chooseVerifyMode(512), AVRSVF.chooseVerifyMode(1024)] });
process.stdout.write(JSON.stringify(out));
"""


def run_js(cases):
    src = HARNESS % extract_generator()
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "gen.js")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        cases_path = os.path.join(d, "cases.json")
        with open(cases_path, "w", encoding="utf-8") as f:
            json.dump(cases, f)
        proc = subprocess.run(["node", path, cases_path],
                              capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit("node failed:\n" + proc.stderr)
    return json.loads(proc.stdout)


def python_side(c):
    """The same case through tools/avrsvf.py."""
    kw = dict(
        image=None if c["image"] is None else bytes(c["image"]),
        max_stmt_bytes=c.get("maxStmt", avrsvf.LEGACY_MAX_STMT_BYTES),
        verify_mode=c.get("verifyMode", "auto"),
        erase=c.get("erase", True),
        write_flash=c.get("writeFlash", True),
        verify_flash=c.get("verifyFlash", True),
        verify_fuses=c.get("verifyFuses", True),
        check_idcode=c.get("checkIdcode", True),
        check_signature=c.get("checkSignature", True),
        note=c.get("note"),
    )
    f = c.get("fuses")
    kw["fuses"] = (f["l"], f["h"]) if f else None
    try:
        return {"ok": True, "svf": avrsvf.build_svf(**kw)}
    except avrsvf.SvfError as e:
        return {"ok": False, "error": str(e)}


def pattern(n, seed=0):
    return [((i * 37 + seed) ^ (i >> 5)) & 0xFF for i in range(n)]


CASES = [
    # name, options
    ("small image, page mode",
     dict(image=pattern(300), maxStmt=1024, verifyMode="page")),
    ("small image, word mode",
     dict(image=pattern(300), maxStmt=512, verifyMode="word")),
    ("auto picks for a big parser",
     dict(image=pattern(1000), maxStmt=1024, verifyMode="auto")),
    ("auto picks for a small parser",
     dict(image=pattern(1000), maxStmt=512, verifyMode="auto")),
    ("exactly one page",
     dict(image=pattern(128), maxStmt=1024, verifyMode="page")),
    ("one byte over a page",
     dict(image=pattern(129), maxStmt=1024, verifyMode="page")),
    ("single byte",
     dict(image=[0x42], maxStmt=1024, verifyMode="page")),
    # A blank interior page exercises the skip-a-blank-page rule, which
    # both sides have to agree about or the page counts diverge.
    ("blank page in the middle",
     dict(image=pattern(128) + [0xFF] * 128 + pattern(128, 9),
          maxStmt=1024, verifyMode="page")),
    ("blank page kept when not erasing",
     dict(image=pattern(128) + [0xFF] * 128 + pattern(128, 9),
          maxStmt=1024, verifyMode="page", erase=False)),
    ("verify only",
     dict(image=pattern(500), maxStmt=1024, verifyMode="page",
          erase=False, writeFlash=False, note="Verify only.")),
    ("fuses only",
     dict(image=None, maxStmt=1024, fuses={"l": 0xE4, "h": 0x99})),
    ("flash and fuses",
     dict(image=pattern(260), maxStmt=1024, verifyMode="page",
          fuses={"l": 0xE1, "h": 0x99})),
    ("fuses without readback",
     dict(image=None, maxStmt=1024, fuses={"l": 0xFF, "h": 0x89},
          verifyFuses=False)),
    ("no idcode check",
     dict(image=pattern(200), maxStmt=1024, verifyMode="page",
          checkIdcode=False)),
    ("no signature check",
     dict(image=pattern(200), maxStmt=1024, verifyMode="page",
          checkSignature=False)),
    ("full flash",
     dict(image=pattern(32768), maxStmt=1024, verifyMode="page")),
    # Both must refuse these, and it is worth checking they refuse the
    # same things rather than only that each refuses something.
    ("JTAGEN would be switched off",
     dict(image=None, maxStmt=1024, fuses={"l": 0xE4, "h": 0xD9})),
    ("page mode will not fit",
     dict(image=pattern(300), maxStmt=512, verifyMode="page")),
    ("image too big for the part",
     dict(image=[0] * 33000, maxStmt=1024)),
]


def main():
    opts = [c[1] for c in CASES]
    for o in opts:
        o.setdefault("image", None)

    js = run_js(opts)
    modes = js.pop()["modes"]

    failures = 0
    print("\nComparing index.html and tools/avrsvf.py\n")

    for (name, opt), got in zip(CASES, js):
        want = python_side(opt)

        if want["ok"] != got["ok"]:
            print(f"  {name:<40} *** FAIL ***")
            print(f"      python: {'built' if want['ok'] else want['error']}")
            print(f"      page  : {'built' if got['ok'] else got['error']}")
            failures += 1
            continue

        if not want["ok"]:
            # Both refused. The wording differs by design — one talks to a
            # terminal, the other to a student — so only the refusal counts.
            print(f"  {name:<40} both refused")
            continue

        a, b = want["svf"], got["svf"]
        if a == b:
            print(f"  {name:<40} identical ({len(a)} bytes)")
            continue

        failures += 1
        print(f"  {name:<40} *** FAIL ***")
        al, bl = a.splitlines(), b.splitlines()
        for i in range(max(len(al), len(bl))):
            x = al[i] if i < len(al) else "(end of file)"
            y = bl[i] if i < len(bl) else "(end of file)"
            if x != y:
                print(f"      first difference at line {i + 1}:")
                print(f"        python: {x[:100]}")
                print(f"        page  : {y[:100]}")
                break

    want_modes = [avrsvf.choose_verify_mode(512),
                  avrsvf.choose_verify_mode(1024)]
    if modes == want_modes:
        print(f"  {'verify-mode choice agrees':<40} {modes}")
    else:
        print(f"  {'verify-mode choice agrees':<40} *** FAIL ***  "
              f"python {want_modes}, page {modes}")
        failures += 1

    print(f"\n{len(CASES) + 1 - failures} passed, {failures} failed\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
