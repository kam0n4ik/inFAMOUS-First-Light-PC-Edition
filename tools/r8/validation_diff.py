"""R8-V4: Vulkan Validation of a run against a reference (docs/NATIVE_ENGINE_R8_VERIFICATION.md §4).

Parses logs\\vk_validation.log (the backend's debug messenger, --gpu-native-validate) into messages, classifies them by
VUID and by a signature (the message text with numbers, handles and names taken out), and compares the run with one or
more reference logs (a baseline run of the same build without the change, or R6/R7's reference):

  known     - the VUID and signature occur in the reference (R6/R7: 04152 storage image layout, Flat-04744)
  variant   - the VUID is known, this signature is not (a new place / a new kind of the same rule): looked at
  new       - a VUID the reference does not have: a regression until explained
  gone      - in the reference, not in the run (fine; listed)

The layer reports one message ID at most duplicate_message_limit times (10 by default; its last report says so):
counts of a capped message say nothing about how often it happened. The table marks them "capped"; to count them run
with VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=<large> (the layer's setting by environment).

usage: py -I -X utf8 validation_diff.py <run vk_validation.log | run dir> --ref <log|dir> [--ref ...]
                                        [--known known.json] [--json out.json] [--show N]
exit: 0 nothing new, 1 a new VUID or a new variant, 2 bad input
"""
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HEADER = re.compile(r"^\[(?P<time>[\d:.]+)\]\s+(?P<sev>[A-Z]+)\s+submission\s+(?P<sub>\d+)\s+(?P<vuid>\S+)"
                    r"\s+\(id\s+(?P<id>0x[0-9a-fA-F]+)\)")
CAPPED = re.compile(r"has now been reported (\d+) times")
# what changes between two reports of the same fault: numbers, handles, names in quotes, array indices
NORMALIZE = [
    (re.compile(r"0x[0-9a-fA-F]+"), "<h>"),
    (re.compile(r"\"[^\"]*\""), "\"\""),
    (re.compile(r"\[\d+\]"), "[#]"),
    (re.compile(r"%\w+"), "%v"),
    (re.compile(r"\b\d+\b"), "#"),
    (re.compile(r"\s+"), " "),
]


def normalize(text):
    for rx, rep in NORMALIZE:
        text = rx.sub(rep, text)
    return text.strip()


def resolve(path):
    p = Path(path)
    if p.is_dir():
        p = p / "vk_validation.log"
    return p


def parse(path):
    """The messages of one log: dicts vuid, sev, sub, id, sig, text, capped."""
    p = resolve(path)
    if not p.exists():
        raise FileNotFoundError(str(p))
    msgs, cur = [], None
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        m = HEADER.match(line)
        if m:
            cur = {"vuid": m["vuid"], "sev": m["sev"], "sub": int(m["sub"]), "id": m["id"].lower(), "body": [],
                   "capped": False}
            msgs.append(cur)
            continue
        if cur is None:
            continue
        if CAPPED.search(line):
            cur["capped"] = True
            continue
        cur["body"].append(line)
    for m in msgs:
        # the signature: the call and the rule text (not "objects:", not the spec's sentence, not spirv-val's command)
        keep = [ln.strip() for ln in m["body"]
                if ln.strip() and not ln.lstrip().startswith(("objects:", "The Vulkan spec states", "Command to",
                                                                "spirv-val "))]
        m["text"] = "\n".join(keep)
        m["sig"] = m["vuid"] + " | " + normalize(" ".join(keep))[:300]
        del m["body"]
    return msgs


def summarize(msgs):
    by_vuid = defaultdict(lambda: {"count": 0, "capped": False, "sev": set(), "sigs": Counter(), "first_sub": None,
                                   "example": ""})
    for m in msgs:
        v = by_vuid[m["vuid"]]
        v["count"] += 1
        v["capped"] |= m["capped"]
        v["sev"].add(m["sev"])
        v["sigs"][m["sig"]] += 1
        if v["first_sub"] is None:
            v["first_sub"] = m["sub"]
            v["example"] = m["text"][:400]
    return by_vuid


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--ref", action="append", default=[], help="reference log or run dir (repeatable)")
    ap.add_argument("--known", help="JSON {vuid: note}: VUIDs known from R6/R7 (any signature is 'known')")
    ap.add_argument("--json", help="write the classification here")
    ap.add_argument("--show", type=int, default=2, help="example messages per new VUID/variant")
    a = ap.parse_args()
    try:
        run = parse(a.run)
        refs = [m for r in a.ref for m in parse(r)]
    except FileNotFoundError as e:
        print(f"no validation log: {e}", file=sys.stderr)
        return 2
    known = json.loads(Path(a.known).read_text(encoding="utf-8")) if a.known else {}
    rs, bs = summarize(run), summarize(refs)
    ref_sigs = {s for v in bs.values() for s in v["sigs"]}
    rows, bad = [], 0
    for vuid in sorted(set(rs) | set(bs)):
        r, b = rs.get(vuid), bs.get(vuid)
        if r is None:
            status = "gone"
        elif b is None and vuid not in known:
            status = "new"
        else:
            status = "known"
        variants = [] if r is None or (b is None and vuid in known) else [s for s in r["sigs"] if s not in ref_sigs]
        if status == "known" and variants:
            status = "variant"
        bad += status in ("new", "variant")
        rows.append({
            "vuid": vuid, "status": status, "run": r["count"] if r else 0, "ref": b["count"] if b else 0,
            "run_capped": bool(r and r["capped"]), "ref_capped": bool(b and b["capped"]),
            "severity": sorted(r["sev"]) if r else sorted(b["sev"]), "first_submission": r["first_sub"] if r else None,
            "signatures": len(r["sigs"]) if r else 0, "new_signatures": variants,
            "example": r["example"] if r else b["example"], "note": known.get(vuid, ""),
        })
    print(f"run: {resolve(a.run)} - {len(run)} messages, {len(rs)} VUIDs; reference: {len(a.ref)} log(s), "
          f"{len(refs)} messages, {len(bs)} VUIDs" + (f"; known list {len(known)}" if known else ""))
    print(f"{'status':8} {'run':>7} {'ref':>7}  vuid")
    for row in rows:
        cap = lambda n, c: f"{n}{'+' if c else ''}"  # noqa: E731
        print(f"{row['status']:8} {cap(row['run'], row['run_capped']):>7} {cap(row['ref'], row['ref_capped']):>7}  "
              f"{row['vuid']}" + (f"  ({row['note']})" if row["note"] else ""))
    print("('+': the layer's duplicate limit was reached - the real count is higher)")
    for row in rows:
        if row["status"] in ("new", "variant"):
            print(f"\n{row['status'].upper()}: {row['vuid']} (first at submission {row['first_submission']}, "
                  f"{row['run']} reports)")
            for s in (row["new_signatures"] or [row["vuid"]])[:a.show]:
                print(f"  signature: {s}")
            print("  example:\n    " + row["example"].replace("\n", "\n    "))
    if a.json:
        Path(a.json).write_text(json.dumps(rows, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"\nverdict: {'NEW MESSAGES' if bad else 'no new messages'} ({bad} new VUIDs/variants)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
