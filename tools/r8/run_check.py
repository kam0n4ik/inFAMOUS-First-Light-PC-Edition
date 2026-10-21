r"""R8-V5: the checks of one stand run (docs/NATIVE_ENGINE_R8_VERIFICATION.md §6) - what a stress scenario must pass.

usage: py -I -X utf8 run_check.py <run dir> [--from S] [--to S] [--ref-validation <run dir|log>] [--min-fps F]
                                  [--max-hitch-ms MS] [--json out.json]

Criteria (each PASS / FAIL / n/a, the run passes when none fails):
  exit        the game ended by the harness's close (run.json: closed-by-test, exit 0), lived the whole run
  log         no [Error] / [Critical] in SecondSonPC.log (validation runs: only "[vk validation]" errors allowed, they
              are judged by the validation criterion), no "device lost", no fatal exception
  frames      frames were presented through the window (gpu_frames.csv); FPS over the window >= --min-fps; no frame
              over --max-hitch-ms in the window (a stall / a hang)
  verify      R8 verify (gpu_native.txt "R8 verify"): 0 mismatches in every site; native engine (native_engine/
              summary.txt): 0 mismatches, 0 fallbacks
  validation  with vk_validation.log: tools/r8/validation_diff.py against --ref-validation (or the R6/R7 known list):
              no new VUID / variant
  fallback    fallbacks / not verifiable counts are listed (a diagnosable fallback is no failure, an unexplained one
              is looked at)
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FRAME_STATS = HERE.parent / "frame_stats.py"
VALIDATION = HERE / "validation_diff.py"
KNOWN = HERE / "validation_known.json"


def check_exit(run):
    rj = run / "run.json"
    if not rj.exists():
        return "n/a", "no run.json"
    j = json.loads(rj.read_text(encoding="utf-8-sig"))
    ok = not j.get("exited_before_deadline", True) and j.get("action") == "close-requested"
    return ("PASS" if ok else "FAIL"), f"action {j.get('action')}, early exit {j.get('exited_before_deadline')}, " \
                                       f"lifetime {j.get('lifetime_s')} s"


def check_log(run):
    log = run / "SecondSonPC.log"
    if not log.exists():
        return "FAIL", "no SecondSonPC.log"
    errors, validation, fatal = [], 0, []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[Error]" in line or "[Critical]" in line:
            if "[vk validation]" in line:
                validation += 1
            else:
                errors.append(line.strip()[:200])
        low = line.lower()
        if "[warning]" in low or "[info]" in low:
            continue  # (a diagnostic's note naming the fault filter is no fault)
        if "device lost" in low or "device_lost" in low or "fatal" in low:
            fatal.append(line.strip()[:200])
    status = "FAIL" if errors or fatal else "PASS"
    detail = f"errors {len(errors)}, validation errors {validation}, fatal {len(fatal)}"
    if errors or fatal:
        detail += "; first: " + (fatal or errors)[0]
    return status, detail


def check_frames(run, lo, hi, min_fps, max_hitch):
    csv = run / "gpu_frames.csv"
    if not csv.exists():
        return "FAIL", "no gpu_frames.csv", {}
    out = run / "stats.json"
    subprocess.run([sys.executable, "-I", str(FRAME_STATS), str(csv), "--from", str(lo), "--to", str(hi), "--json",
                    str(out)], check=True, stdout=subprocess.DEVNULL)
    s = json.loads(out.read_text(encoding="utf-8"))
    fps, worst = s.get("avg_fps", 0.0), s.get("ft_max_ms", s.get("ft_p99_ms", 0.0))
    fails = []
    if not fps or fps < min_fps:
        fails.append(f"FPS {fps:.2f} < {min_fps}")
    if max_hitch and worst and worst > max_hitch:
        fails.append(f"worst frame {worst:.1f} ms > {max_hitch}")
    detail = f"window {lo}..{hi} s: FPS {fps:.2f}, 1 % low {s.get('low1_fps', 0):.2f}, p99 {s.get('ft_p99_ms', 0):.1f} " \
             f"ms, worst {worst:.1f} ms, CP {s.get('mean_cp_ms', 0):.2f} ms"
    return ("FAIL" if fails else "PASS"), detail + ("; " + "; ".join(fails) if fails else ""), s


def check_verify(run):
    notes, fails = [], []
    gn = run / "gpu_native.txt"
    if gn.exists():
        text = gn.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"^  (\S+) \[([^\]]+)\]: checks (\d+), mismatches (\d+), not verifiable (\d+)", text, re.M):
            name, mode, checks, mism, unv = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
            notes.append(f"{name}: {checks} checks, {mism} mismatches, {unv} not verifiable ({mode})")
            if mism:
                fails.append(f"R8 verify {name}: {mism} mismatches")
    summ = run / "native_engine" / "summary.txt"
    if summ.exists():
        for line in summ.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.search(r"^(\S+) \[.*?verified (\d+) mismatches (\d+) live-diff (\d+) skipped (\d+) fallbacks (\d+)",
                          line)
            if m:
                if int(m.group(3)) or int(m.group(6)):
                    fails.append(f"native engine {m.group(1)}: mismatches {m.group(3)}, fallbacks {m.group(6)}")
                notes.append(f"{m.group(1)}: verified {m.group(2)}, mismatches {m.group(3)}, fallbacks {m.group(6)}")
    if not notes:
        return "n/a", "no verification sites in this run", notes
    return ("FAIL" if fails else "PASS"), "; ".join(fails) if fails else f"{len(notes)} sites, 0 mismatches", notes


def check_validation(run, ref):
    log = run / "vk_validation.log"
    if not log.exists():
        return "n/a", "not a validation run"
    args = [sys.executable, "-I", "-X", "utf8", str(VALIDATION), str(log), "--known", str(KNOWN)]
    if ref:
        args += ["--ref", ref]
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    verdict = (r.stdout.strip().splitlines() or ["?"])[-1]
    return ("PASS" if r.returncode == 0 else "FAIL"), verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--from", dest="lo", type=float, default=70)
    ap.add_argument("--to", dest="hi", type=float, default=148)
    ap.add_argument("--ref-validation")
    ap.add_argument("--min-fps", type=float, default=10.0)
    ap.add_argument("--max-hitch-ms", type=float, default=1000.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    run = Path(a.run)
    res = {}
    res["exit"] = check_exit(run)
    res["log"] = check_log(run)
    st, detail, stats = check_frames(run, a.lo, a.hi, a.min_fps, a.max_hitch_ms)
    res["frames"] = (st, detail)
    st, detail, notes = check_verify(run)
    res["verify"] = (st, detail)
    res["validation"] = check_validation(run, a.ref_validation)
    print(f"run {run.name}")
    for k, (s, d) in res.items():
        print(f"  {k:10} {s:4}  {d}")
    for n in notes:
        print(f"    {n}")
    failed = [k for k, (s, _) in res.items() if s == "FAIL"]
    print(f"  verdict: {'FAIL (' + ', '.join(failed) + ')' if failed else 'PASS'}")
    if a.json:
        Path(a.json).write_text(json.dumps({"run": str(run), "checks": res, "notes": notes, "stats": stats,
                                            "pass": not failed}, indent=1), encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
