"""Seeds the own GCN translator's shader cache (play_rea_own.cmd) from the main one.

cache\\shaders\\<game>\\native\\<gen>\\shaders.pack -> ...\\native_own\\<gen>\\shaders.pack, every record but the compute
modules ('M' of stage Cs: the own translator makes those) and the compute pipelines ('L' compute: their stages are
those modules); vulkan.bin (the driver's pipeline cache) and the static prediction stamp as they are. The graphics
modules and pipelines are the bridge's in both caches, so the own run starts with them prepared (no draws skipped
while they build). A generation already seeded is left as it is unless --force or the own translator changed
(build\\bin\\own_translator_generation.txt, written by CMake: its modules from an earlier translator are dropped).

Format: src/runtime/gpu/shader/shader_disk_cache.h (64-byte header, then {u32 'SPRC', u8 type, 3 pad, u32 size,
u32 reserved, u64 XXH3 of the payload} + payload; records are copied whole, their hashes unchanged).
"""
import argparse
import os
import shutil
import struct
import sys

REC = struct.Struct('<IB3xIIQ')
REC_MAGIC = 0x43525053  # "SPRC"
HEADER = 64
STAGE_CS = 6  # HwStage::Cs
IDENTITY_STAGE_AT = 20  # u64 lo, u64 hi, u32 length, u8 stage


def seed(src_dir, dst_dir):
    src = os.path.join(src_dir, 'shaders.pack')
    tmp = os.path.join(dst_dir, 'shaders.pack.seed')
    kept = dropped_modules = dropped_pipelines = 0
    with open(src, 'rb') as f, open(tmp, 'wb') as out:
        out.write(f.read(HEADER))
        while True:
            head = f.read(REC.size)
            if len(head) < REC.size:
                break
            magic, kind, size, _, _ = REC.unpack(head)
            if magic != REC_MAGIC:
                break  # (a torn tail: the cache cuts it off too)
            payload = f.read(size)
            if len(payload) < size:
                break
            if kind == ord('M') and size > IDENTITY_STAGE_AT and payload[IDENTITY_STAGE_AT] == STAGE_CS:
                dropped_modules += 1
                continue
            if kind == ord('L') and size > 0 and payload[0] == 1:
                dropped_pipelines += 1
                continue
            out.write(head)
            out.write(payload)
            kept += 1
    os.replace(tmp, os.path.join(dst_dir, 'shaders.pack'))
    for name in ('vulkan.bin', 'static_predict.stamp'):
        if os.path.exists(os.path.join(src_dir, name)):
            shutil.copyfile(os.path.join(src_dir, name), os.path.join(dst_dir, name))
    return kept, dropped_modules, dropped_pipelines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cache', 'shaders'))
    ap.add_argument('--game', default='CUSA00004')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    gen_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'bin', 'own_translator_generation.txt')
    own_gen = open(gen_file).read().strip() if os.path.isfile(gen_file) else ''
    native = os.path.join(a.cache, a.game, 'native')
    own = os.path.join(a.cache, a.game, 'native_own')
    if not os.path.isdir(native):
        print('no main shader cache to seed from:', native)
        return 0
    for gen in sorted(os.listdir(native)):
        src_dir = os.path.join(native, gen)
        if not os.path.isfile(os.path.join(src_dir, 'shaders.pack')):
            continue
        dst_dir = os.path.join(own, gen)
        mark = os.path.join(dst_dir, 'seeded')
        if os.path.isfile(mark) and not a.force and open(mark).read().split('\n')[0] == 'own ' + own_gen:
            continue
        if os.path.exists(os.path.join(dst_dir, 'lock')):
            try:
                os.remove(os.path.join(dst_dir, 'lock'))  # (fails while a game holds it)
            except OSError:
                print(gen, ': in use by a running game - not seeded')
                continue
        os.makedirs(dst_dir, exist_ok=True)
        kept, dm, dp = seed(src_dir, dst_dir)
        open(mark, 'w').write('own %s\nfrom %s\n' % (own_gen, src_dir))
        print('%s: seeded from the main cache - %d records, %d compute modules and %d compute pipelines left out'
              % (gen, kept, dm, dp))
    return 0


if __name__ == '__main__':
    sys.exit(main())
