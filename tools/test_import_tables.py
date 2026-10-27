"""Regression test of the binding tables (docs/ARCHITECTURE.md, section 4).

1. gen_import_table.py run on Second Son's eboot reproduces Second Son's table row for row, in any order (keys are
   unique, part 3; the generator is the one that
   made First Light's, so this is what makes First Light's trustworthy).
2. First Light's committed table equals what the generator makes from First Light's eboot now.
3. Every import of each eboot has exactly one row in its table (a missing row would bind a logging stub returning 0).

    python -I tools/test_import_tables.py [<Second Son game dir>]      (default E:\\shdps4\\CUSA00004; part 1 is
                                                                         skipped when it has no eboot.bin)
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_import_table  # noqa: E402
from ps4elf import Image  # noqa: E402

ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, 'core', 'src', 'loader', 'generated')
SS_TABLE = os.path.join(GEN, 'import_table.inc')
FL_TABLE = os.path.join(GEN, 'import_table_cusa00897.inc')
AEROLIB = os.path.join(ROOT, 'core', 'third_party', 'shadPS4', 'src', 'core', 'aerolib', 'aerolib.inl')


def rows(path):
    return [l for l in open(path, encoding='utf-8').read().splitlines() if l.lstrip().startswith('{"')]


def generate(eboot):
    fd, out = tempfile.mkstemp(suffix='.inc')
    os.close(fd)
    try:
        gen_import_table.main(eboot, SS_TABLE, AEROLIB, out)
        return rows(out)
    finally:
        os.remove(out)


def check_coverage(eboot, table):
    keys = [tuple(r.split('"')[1:6:2]) for r in rows(table)]
    return [i[:3] for i in Image(eboot).analyze()['imports'] if keys.count(tuple(i[:3])) != 1]


def main():
    failures = 0
    ss_dir = sys.argv[1] if len(sys.argv) > 1 else r'E:\shdps4\CUSA00004'
    ss_eboot = os.path.join(ss_dir, 'eboot.bin')
    if os.path.isfile(ss_eboot):
        same = sorted(generate(ss_eboot)) == sorted(rows(SS_TABLE))
        print(f"[{'ok' if same else 'FAIL'}] generator on Second Son's eboot == import_table.inc")
        failures += not same
    else:
        print(f'[skip] no {ss_eboot}')
    fl_eboot = os.path.join(ROOT, 'eboot.bin')
    same = sorted(generate(fl_eboot)) == sorted(rows(FL_TABLE))
    print(f"[{'ok' if same else 'FAIL'}] generator on First Light's eboot == import_table_cusa00897.inc")
    failures += not same
    missing = check_coverage(fl_eboot, FL_TABLE)
    print(f"[{'ok' if not missing else 'FAIL'}] every First Light import has exactly one row ({len(missing)} without)")
    failures += bool(missing)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
