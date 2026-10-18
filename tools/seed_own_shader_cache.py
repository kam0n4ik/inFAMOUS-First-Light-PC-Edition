"""Seeds the own GCN translator's shader cache (play_rea_own.cmd) from the main one.

cache\\shaders\\<game>\\native\\<gen>\\shaders.pack -> ...\\native_own\\<gen>\\shaders.pack, every record but the modules
of the stages the own translator makes (--stages, default SSPC_OWN_SHADERS: cs, vs, ps) and the pipelines built of
them: with cs the compute modules ('M' of stage Cs) and compute pipelines ('L' compute); with vs or ps every graphics
module (the auxiliary tessellation stages of rect / quad lists too: the own translator gives them with its vertex
programs) and every graphics pipeline. vulkan.bin (the driver's pipeline cache) as it is, not the static prediction's
stamp (the preparation window then runs once for this cache: the left-out modules made by the own translator and
their pipelines created before the game). What stays the bridge's in both caches starts prepared. A generation already
seeded is left as it is unless --force, the own translator changed (build\\bin\\own_translator_generation.txt, written
by CMake: its modules from an earlier translator are dropped) or its stages did.

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


def seed(src_dir, dst_dir, stages):
    src = os.path.join(src_dir, 'shaders.pack')
    tmp = os.path.join(dst_dir, 'shaders.pack.seed')
    graphics_own = 'vs' in stages or 'ps' in stages
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
            if kind == ord('M') and size > IDENTITY_STAGE_AT:
                cs = payload[IDENTITY_STAGE_AT] == STAGE_CS
                if (cs and 'cs' in stages) or (not cs and graphics_own):
                    dropped_modules += 1
                    continue
            if kind == ord('L') and size > 0:
                compute = payload[0] == 1
                if (compute and 'cs' in stages) or (not compute and graphics_own):
                    dropped_pipelines += 1
                    continue
            out.write(head)
            out.write(payload)
            kept += 1
    os.replace(tmp, os.path.join(dst_dir, 'shaders.pack'))
    shutil.copyfile(os.path.join(src_dir, 'vulkan.bin'), os.path.join(dst_dir, 'vulkan.bin')) if os.path.exists(
        os.path.join(src_dir, 'vulkan.bin')) else None
    # not the static prediction's stamp: the preparation window runs once for this cache, the own translator's
    # modules made and their pipelines created before the game (no translation while playing)
    stamp = os.path.join(dst_dir, 'static_predict.stamp')
    if os.path.exists(stamp):
        os.remove(stamp)
    return kept, dropped_modules, dropped_pipelines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cache', 'shaders'))
    ap.add_argument('--game', default='CUSA00004')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--stages', default=os.environ.get('SSPC_OWN_SHADERS', 'cs'),
                    help="the own translator's stages, as SSPC_OWN_SHADERS (cs,vs,ps)")
    a = ap.parse_args()
    stages = sorted({s.strip() for s in a.stages.split(',')} & {'cs', 'vs', 'ps'})
    if not stages:
        print('no own translator stage in', repr(a.stages))
        return 1
    gen_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'bin', 'own_translator_generation.txt')
    own_gen = open(gen_file).read().strip() if os.path.isfile(gen_file) else ''
    native = os.path.join(a.cache, a.game, 'native')
    own = os.path.join(a.cache, a.game, 'native_own')
    if not os.path.isdir(native):
        print('no main shader cache to seed from:', native)
        return 0
    tag = 'own %s %s' % (own_gen, ','.join(stages))
    for gen in sorted(os.listdir(native)):
        src_dir = os.path.join(native, gen)
        if not os.path.isfile(os.path.join(src_dir, 'shaders.pack')):
            continue
        dst_dir = os.path.join(own, gen)
        mark = os.path.join(dst_dir, 'seeded')
        # (seeded again when the own translator or its stages change)
        if os.path.isfile(mark) and not a.force and open(mark).read().split('\n')[0] == tag:
            continue
        if os.path.exists(os.path.join(dst_dir, 'lock')):
            try:
                os.remove(os.path.join(dst_dir, 'lock'))  # (fails while a game holds it)
            except OSError:
                print(gen, ': in use by a running game - not seeded')
                continue
        os.makedirs(dst_dir, exist_ok=True)
        kept, dm, dp = seed(src_dir, dst_dir, stages)
        open(mark, 'w').write('%s\nfrom %s\n' % (tag, src_dir))
        print('%s: seeded from the main cache for the own %s - %d records kept, %d modules and %d pipelines left out'
              % (gen, ','.join(stages), kept, dm, dp))
    return 0


if __name__ == '__main__':
    sys.exit(main())
