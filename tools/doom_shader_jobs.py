"""DOOM (2016, Switch) shader jobs for the d2r-mobile offline translator (dmsh_xlate / dmsh_vk.py).

Input: the newest resource index (gameresources_003.pindex) -> every renderProg pair
    generated/nx/<name>_{vsps,vsgsps,cs}.sb      glslc output container (Maxwell code + NVN control sections)
    generated/renderprogs/<name>_nx.bin           the same program as GLSL source (#version 450 core + NV extensions)
(permutations: generated/nx/generated/renderprogs/permutations/<p>.decl_<kind>.sb <-> generated/renderprogs/permutations/<p>_nx.bin)

.sb container (verified on all 752 files: 689 vsps, 28 vsgsps, 35 cs; duplicates in the index are byte-identical)
    +0x00  u32 BE  id-Tech checksum of the payload (differs per file)
    +0x04  u32 BE  payload size (= file size - 8)
    +0x08  GLSLCoutput (little endian), g = file[8:]:
        g+0x00 u32 magic 0x19866891     g+0x0c..0x24: glslc / GPU-code versions (constant: 0x200004, 0, 0x11, 0x14, 1, 0xf, 0x3e)
        g+0x44 u32 total size (= len(g))  g+0x48 u32 data offset of the first section  g+0x4c u32 section count
        g+0x90 + i*0x90: section header i (36 x u32)
            [0] size  [1] data offset (from g)  [2] type (0 = GPU code; the only type present)
            [11] NVN stage (0 vertex, 1 fragment, 2 geometry, 5 compute)
            [12] control offset (from data, always 0)  [13] code offset (from data)  [14] code size  [15] control size
            [20] = size again; fragment headers sometimes carry [16]/[17] (not needed here)
        Section order: vsps = (fragment, vertex), vsgsps = (fragment, geometry, vertex), cs = (compute).
    Control section: magic 0x98761234, size 0x8b8..0x11110 (NVN control + embedded data); the game hands this pointer to
    nvnProgramSetShaders, so the d2r key (sha256(control[:0x880])[:16], xbg_shader_key.c) works unchanged.
    control+0x700 / +0x704: c1 start / end relative to the code area (end = code size, c1 = 0/256/512/2048 bytes).
    compute control+0x748: local size x,y,z; +0x754: shared memory bytes (cross-checked against the GLSL below).
    Code area: graphics = 0x30-byte header (starts 0x12345678) + 0x50 SPH + instructions (+ c1); compute = instructions (+ c1).

Record written per unique key = bytes(0x38) + code area + control section: exactly what dmsh_build.job_for / dmsh_vk.job_for
expect (code area at +0x38, control right after, c1 via control+0x700/+0x704; graphics strip 0x30, compute keep all).

Key decision: with the correct per-stage parse there are NO collisions (same key -> identical code area and identical full
control section for every stage), so the d2r key and xbg_shader_key.c stay as they are. The "11 collisions" reported by the
provisional exporter (doom_shaders.py) were the same stage shared by different programs. The tool still checks this and
exits 1 if a real collision ever appears.

Facts (d2r shader_facts.tsv schema, see dmsh_facts.py / dmsh_xlate Program.cs), derived from the GLSL source of the same stage:
    tex  {binding: 1d|1darray|2d|2darray|2dms|2dmsarray|3d|cube|cubearray|buffer}  NVN texture binding N = handle slot N
         (c2 0x168 + 8N); shadow samplers map to their base type (Ryujinx takes the compare flag from the instruction)
    img  {binding: {type, fmt}} fmt = Ryujinx TextureFormat name from the GLSL layout format
    attr {location: float|sint|uint} vertex inputs     fout {index: float|sint|uint} fragment outputs
    local [x,y,z], shared bytes (compute)
GLSL stage order inside _nx.bin: vertex, fragment, geometry / compute (checked: vertex writes gl_Position, geometry has
layout(triangles) in, compute has local_size).

Capture ingest (--capture DIR; the run-time capture format of nx2android's Vulkan backend, gfx_vk_shader.c vk_shader_missing,
with this engine's key): shaders the game hands to nvnProgramSetShaders that no renderProg .sb holds, saved on the device as
<key>.bin = "DMSC", u32 control length, u32 code-area length, control section, code area (graphics: 0x30-byte 0x12345678 header +
SPH + instructions + c1; compute: instructions + c1, as DOOM's glslc writes them). A file is taken only when its name is the engine
key of its control section (sha256(control[:0x880])[:16], xbg_shader_key.c), the control magic and c1 offsets (+0x700 / +0x704)
are consistent, and its stage can be read (graphics: SPH word 0 bits 10..13; compute: no header). A key the romfs already has is
checked for identical bytes and skipped. Captured stages have no GLSL, so their facts are the defaults (the translator notes every
query it answers that way); their jobs.tsv source is "capture:<file>".
Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/tools/shader_vk.py (captured()),
Copyright (c) 2026 seedds, MIT License.

Usage: doom_shader_jobs.py [romfs] [out] [--capture DIR]   (defaults work/romfs, work/shaders-jobs)
Output: <out>/rec/<key>.rec, <out>/jobs.tsv, <out>/shader_facts.tsv, <out>/glsl_bindings.json, <out>/facts_notes.tsv, <out>/report.txt
"""
import collections, hashlib, json, re, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from doom_index import parse

ROOT = Path(__file__).resolve().parents[1]
CTRL, GPU = bytes.fromhex('34127698'), bytes.fromhex('78563412')
GLSLC_MAGIC = 0x19866891
NVN_STAGE = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'hs', 4: 'ds', 5: 'cs'}
GLSL_ORDER = {'vsps': ['vs', 'ps'], 'vsgsps': ['vs', 'ps', 'gs'], 'cs': ['cs']}
SAMPLER = {'1D': '1d', '1DArray': '1darray', '2D': '2d', '2DArray': '2darray', '2DMS': '2dms', '2DMSArray': '2dmsarray',
           '3D': '3d', 'Cube': 'cube', 'CubeArray': 'cubearray', 'Buffer': 'buffer', '2DRect': '2d'}
IMGFMT = {'r8': 'R8Unorm', 'r8_snorm': 'R8Snorm', 'r8ui': 'R8Uint', 'r8i': 'R8Sint', 'r16f': 'R16Float', 'r16': 'R16Unorm',
          'r16_snorm': 'R16Snorm', 'r16ui': 'R16Uint', 'r16i': 'R16Sint', 'r32f': 'R32Float', 'r32ui': 'R32Uint', 'r32i': 'R32Sint',
          'rg8': 'R8G8Unorm', 'rg8_snorm': 'R8G8Snorm', 'rg8ui': 'R8G8Uint', 'rg8i': 'R8G8Sint', 'rg16f': 'R16G16Float',
          'rg16': 'R16G16Unorm', 'rg16_snorm': 'R16G16Snorm', 'rg16ui': 'R16G16Uint', 'rg16i': 'R16G16Sint', 'rg32f': 'R32G32Float',
          'rg32ui': 'R32G32Uint', 'rg32i': 'R32G32Sint', 'rgba8': 'R8G8B8A8Unorm', 'rgba8_snorm': 'R8G8B8A8Snorm',
          'rgba8ui': 'R8G8B8A8Uint', 'rgba8i': 'R8G8B8A8Sint', 'rgba16f': 'R16G16B16A16Float', 'rgba16': 'R16G16B16A16Unorm',
          'rgba16_snorm': 'R16G16B16A16Snorm', 'rgba16ui': 'R16G16B16A16Uint', 'rgba16i': 'R16G16B16A16Sint',
          'rgba32f': 'R32G32B32A32Float', 'rgba32ui': 'R32G32B32A32Uint', 'rgba32i': 'R32G32B32A32Sint',
          'rgb10_a2': 'R10G10B10A2Unorm', 'rgb10_a2ui': 'R10G10B10A2Uint', 'r11f_g11f_b10f': 'R11G11B10Float'}
SCALAR_BYTES = {'float': 4, 'int': 4, 'uint': 4, 'bool': 4, 'vec2': 8, 'vec3': 12, 'vec4': 16, 'ivec2': 8, 'ivec3': 12, 'ivec4': 16,
                'uvec2': 8, 'uvec3': 12, 'uvec4': 16, 'mat4': 64, 'mat3': 48}


def container(romfs, patch):
    return romfs / 'base' / ('gameresources.resources' if patch == 0 else
                             'gameresources.patch' if patch == 1 else f'gameresources_{patch:03d}.patch')


def load_renderprogs(romfs):
    ents = [x for x in parse(romfs / 'base' / 'gameresources_003.pindex') if x['type'] == 'renderProg'
            and (x['name2'].endswith('.sb') or x['name2'].endswith('_nx.bin'))]
    files, out = {}, {}
    for x in ents:
        f = files.get(x['patch']) or files.setdefault(x['patch'], container(romfs, x['patch']).open('rb'))
        f.seek(x['off'])
        raw = f.read(x['zsize'])
        d = raw if x['zsize'] == x['size'] else zlib.decompressobj(-15).decompress(raw)
        assert len(d) == x['size'], x['name2']
        if x['name2'] in out and out[x['name2']] != d: sys.exit(f'[red] {x["name2"]}: index lists different contents')
        out[x['name2']] = d
    return out


def parse_sb(d, name):
    """-> [(stage, code_area, control)] in container order"""
    size_be, = struct.unpack_from('>I', d, 4)
    assert size_be == len(d) - 8, name
    g = d[8:]
    magic, = struct.unpack_from('<I', g, 0)
    total, doff, nsec = struct.unpack_from('<3I', g, 0x44)
    assert magic == GLSLC_MAGIC and total == len(g), name
    secs = []
    for i in range(nsec):
        h = struct.unpack_from('<36I', g, 0x90 + i * 0x90)
        assert h[2] == 0 and h[0] == h[14] + h[15] == h[20] and h[12] == 0, (name, i)
        base = h[1]
        ctrl, code = g[base + h[12]:base + h[12] + h[15]], g[base + h[13]:base + h[13] + h[14]]
        stage = NVN_STAGE[h[11]]
        assert ctrl[:4] == CTRL, (name, i)
        assert (code[:4] == GPU) == (stage != 'cs'), (name, i)
        c1o, c1t = struct.unpack_from('<2I', ctrl, 0x700)
        assert c1o <= c1t == len(code), (name, i, c1o, c1t, len(code))
        secs.append((stage, code, ctrl))
    return secs


def glsl_sources(d):
    out = []
    for m in re.finditer(rb'#version', d):
        n, = struct.unpack_from('<I', d, m.start() - 4)
        out.append(d[m.start():m.start() + n].split(b'\0')[0].decode('latin1'))
    return out


def strip(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def toplevel(s):
    """top-level text only (function bodies and block member lists removed)"""
    out, depth = [], 0
    for ch in s:
        if ch == '{':
            if depth == 0: out.append('{')
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0: out.append('}')
        elif depth == 0: out.append(ch)
    return ''.join(out)


def const_eval(expr, defines):
    expr = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(defines.get(m.group(0), m.group(0))), expr)
    if not re.fullmatch(r'[\d\s+*()/-]+', expr): return None
    return int(eval(expr))  # digits and arithmetic only


def glsl_facts(src, stage):
    """-> facts dict (d2r schema), bindings dict (for slot verification), notes list"""
    s = strip(src)
    defines = {m.group(1): m.group(2) for m in re.finditer(r'^\s*#define\s+(\w+)\s+(\d+)\s*$', s, re.M)}
    top = toplevel(s)
    f = {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}
    b = {'tex': {}, 'img': {}, 'ubo': {}, 'ssbo': {}}
    notes = []
    for m in re.finditer(r'layout\s*\(([^)]*)\)\s*((?:\w+\s+)*)uniform\s+(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;', top):
        lay, ty, nm = m.group(1), m.group(3), m.group(4)
        bm = re.search(r'binding\s*=\s*(\d+)', lay)
        if not bm: notes.append(f'no binding: {ty} {nm}'); continue
        bn = bm.group(1)
        if m.group(5): notes.append(f'array {ty} {nm}{m.group(5)} (binding {bn})')
        sm = re.fullmatch(r'[iu]?sampler(\w+?)(Shadow)?', ty)
        im = re.fullmatch(r'([iu]?)image(\w+)', ty)
        if sm:
            t = SAMPLER.get(sm.group(1))
            if not t: notes.append(f'unknown sampler type {ty} {nm}'); continue
            if bn in f['tex'] and f['tex'][bn] != t: notes.append(f'texture binding {bn} declared twice ({f["tex"][bn]}/{t})')
            f['tex'][bn] = t; b['tex'][bn] = nm
            if ty[0] in 'iu': notes.append(f'integer sampler {ty} {nm} (binding {bn})')
        elif im:
            t = SAMPLER.get(im.group(2))
            fm = next((IMGFMT[x.strip()] for x in lay.split(',') if x.strip() in IMGFMT), None)
            if not t or not fm: notes.append(f'image without known type/format {lay} {ty} {nm}')
            f['img'][bn] = {'type': t or '2d', 'fmt': fm or 'R8G8B8A8Unorm'}; b['img'][bn] = nm
        else: notes.append(f'unknown uniform {ty} {nm}')
    for m in re.finditer(r'layout\s*\(([^)]*)\)\s*uniform\s+(\w+)\s*\{', top):
        bm = re.search(r'binding\s*=\s*(\d+)', m.group(1))
        if bm: b['ubo'][bm.group(1)] = m.group(2)
        else: notes.append(f'ubo without binding {m.group(2)}')
    for m in re.finditer(r'layout\s*\(([^)]*)\)\s*((?:\w+\s+)*)buffer\s+(\w+)\s*\{', top):
        bm = re.search(r'binding\s*=\s*(\d+)', m.group(1))
        if bm: b['ssbo'][bm.group(1)] = m.group(3)
        else: notes.append(f'ssbo without binding {m.group(3)}')
    CT = lambda t: 'sint' if t.startswith('i') else 'uint' if t.startswith('u') else 'float'
    if stage == 'vs':
        for m in re.finditer(r'(?:layout\s*\(([^)]*)\)\s*)?\bin\s+(\w+)\s+(\w+)\s*;', top):
            lm = re.search(r'location\s*=\s*(\d+)', m.group(1) or '')
            if not lm: notes.append(f'vertex input without location: {m.group(2)} {m.group(3)}'); continue
            f['attr'][lm.group(1)] = CT(m.group(2))
    if stage == 'ps':
        outs = [(m.group(1), m.group(2), m.group(3)) for m in re.finditer(r'(?:layout\s*\(([^)]*)\)\s*)?\bout\s+(\w+)\s+(\w+)\s*;', top)]
        for lay, ty, nm in outs:
            lm = re.search(r'location\s*=\s*(\d+)', lay or '')
            if lm: idx = lm.group(1)
            elif len(outs) == 1: idx = '0'
            else:
                dm = re.search(r'(\d+)$', nm)
                idx = dm.group(1) if dm else None
                notes.append(f'fragment output without location {nm}: index {idx} from name')
            if idx is not None: f['fout'][idx] = CT(ty)
    if stage == 'cs':
        lm = re.search(r'layout\s*\(\s*local_size_x\s*=\s*(\w+)\s*,\s*local_size_y\s*=\s*(\w+)\s*,\s*local_size_z\s*=\s*(\w+)\s*\)\s*in', top)
        f['local'] = [const_eval(x, defines) for x in lm.groups()] if lm else None
        if not lm: notes.append('no local_size')
        shared = 0
        for m in re.finditer(r'\bshared\s+(\w+)\s+(\w+)\s*(?:\[([^\]]+)\])?\s*;', top):
            n = const_eval(m.group(3), defines) if m.group(3) else 1
            sz = SCALAR_BYTES.get(m.group(1))
            if n is None or sz is None: notes.append(f'shared size unknown: {m.group(0)}'); continue
            al = 16 if sz >= 12 else sz
            shared = (shared + al - 1) // al * al + sz * n
        f['shared'] = (shared + 15) // 16 * 16
    return f, b, notes


def sb_to_bin(n):
    if n.startswith('generated/nx/generated/renderprogs/'):
        return re.sub(r'\.decl_(vsps|vsgsps|cs)\.sb$', '_nx.bin', n[len('generated/nx/'):])
    return 'generated/renderprogs/' + re.sub(r'_(vsps|vsgsps|cs)\.sb$', '_nx.bin', n[len('generated/nx/'):])


SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}   # SPH word 0 bits 10..13 (nxnative shader_split.py sph_stage)


def captured(d):
    """run-time captures in folder d -> ({key: (stage, code area, control)}, [rejection notes])"""
    out, bad = {}, []
    for f in sorted(Path(d).iterdir()) if d and Path(d).is_dir() else []:
        if not re.fullmatch(r'[0-9a-f]{16}\.bin', f.name): continue
        b = f.read_bytes()
        if len(b) < 12 or b[:4] != b'DMSC': bad.append(f'{f.name}: not a captured shader (no DMSC magic)'); continue
        nctl, ncode = struct.unpack_from('<2I', b, 4)
        ctl, code = b[12:12 + nctl], b[12 + nctl:12 + nctl + ncode]
        if len(ctl) != nctl or len(code) != ncode or nctl < 0x880 or ctl[:4] != CTRL: bad.append(f'{f.name}: truncated or bad control section'); continue
        if hashlib.sha256(ctl[:0x880]).hexdigest()[:16] != f.name[:16]: bad.append(f'{f.name}: its control section does not hash to its name'); continue
        c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
        if not c1o <= c1t == len(code): bad.append(f'{f.name}: c1 offsets {c1o:#x}/{c1t:#x} do not match the code area ({len(code):#x})'); continue
        if code[:4] == GPU:
            st = SPH_STAGE.get(struct.unpack_from('<I', code, 0x30)[0] >> 10 & 0xf) if len(code) > 0x80 else None
        elif code[:4] == bytes.fromhex('79563412'):
            bad.append(f'{f.name}: compute code area with a 0x12345679 header (not DOOM\'s glslc form; job_for keeps compute code as is)'); continue
        else: st = 'cs'
        if st not in ('vs', 'ps', 'gs', 'cs'): bad.append(f'{f.name}: no supported stage in its code header ({st})'); continue
        if (bytes(0x38) + code + ctl).find(CTRL) != 0x38 + len(code): bad.append(f'{f.name}: the control magic appears in its code area'); continue
        out[f.name[:16]] = (st, code, ctl)
    return out, bad


def main():
    args = [a for a in sys.argv[1:]]
    cap_dir = None
    if '--capture' in args:
        i = args.index('--capture'); cap_dir = args[i + 1]; del args[i:i + 2]
    romfs = Path(args[0]) if len(args) > 0 else ROOT / 'work' / 'romfs'
    out = Path(args[1]) if len(args) > 1 else ROOT / 'work' / 'shaders-jobs'
    (out / 'rec').mkdir(parents=True, exist_ok=True)
    rp = load_renderprogs(romfs)
    sbs = sorted(n for n in rp if n.endswith('.sb'))
    stages, facts, binds, notes = {}, {}, {}, collections.defaultdict(list)
    kinds, collisions, crosscheck = collections.Counter(), [], collections.Counter()
    for n in sbs:
        kind = re.search(r'_(vsps|vsgsps|cs)\.sb$', n).group(1)
        kinds[kind] += 1
        secs = parse_sb(rp[n], n)
        want = {'vsps': ['ps', 'vs'], 'vsgsps': ['ps', 'gs', 'vs'], 'cs': ['cs']}[kind]
        assert [s[0] for s in secs] == want, (n, [s[0] for s in secs])
        bn = sb_to_bin(n)
        srcs = glsl_sources(rp[bn]) if bn in rp else []
        by_stage = dict(zip(GLSL_ORDER[kind], srcs)) if len(srcs) == len(GLSL_ORDER[kind]) else {}
        if not by_stage: notes[n].append(f'no matching GLSL ({bn}: {len(srcs)} sources)')
        for st, code, ctrl in secs:
            key = hashlib.sha256(ctrl[:0x880]).hexdigest()[:16]
            rec = bytes(0x38) + code + ctrl
            assert rec.find(CTRL) == 0x38 + len(code), (n, st, 'control magic inside code area')
            if key in stages:
                o = stages[key]
                if (o['rec'], o['stage']) != (rec, st): collisions.append((key, o['sources'][0], n))
                o['sources'].append(n); continue
            c1o, c1t = struct.unpack_from('<2I', ctrl, 0x700)
            stages[key] = {'stage': st, 'rec': rec, 'sources': [n], 'code': len(code), 'ctrl': len(ctrl), 'c1': c1t - c1o}
            src = by_stage.get(st)
            if src is None: notes[key].append('no GLSL for this stage: default facts'); continue
            f, b, nt = glsl_facts(src, st)
            if st == 'cs':   # cross-check GLSL against the control section
                lc = list(struct.unpack_from('<3I', ctrl, 0x748)); sh, = struct.unpack_from('<I', ctrl, 0x754)
                crosscheck['local ok' if f.get('local') == lc else 'local MISMATCH'] += 1
                crosscheck['shared ok' if f['shared'] == sh else 'shared MISMATCH'] += 1
                if f.get('local') != lc: nt.append(f'local_size GLSL {f.get("local")} / control {lc} (control used)'); f['local'] = lc
                if f['shared'] != sh: nt.append(f'shared GLSL {f["shared"]} / control {sh} (control used)'); f['shared'] = sh
            facts[key], binds[key] = f, b
            notes[key].extend(nt)
    cap, cap_bad = captured(cap_dir) if cap_dir else ({}, [])
    cap_new = 0
    for key, (st, code, ctrl) in sorted(cap.items()):
        rec = bytes(0x38) + code + ctrl
        if key in stages:
            if (stages[key]['rec'], stages[key]['stage']) != (rec, st): collisions.append((key, stages[key]['sources'][0], 'capture:' + key + '.bin'))
            continue
        c1o, c1t = struct.unpack_from('<2I', ctrl, 0x700)
        stages[key] = {'stage': st, 'rec': rec, 'sources': ['capture:' + key + '.bin'], 'code': len(code), 'ctrl': len(ctrl), 'c1': c1t - c1o}
        facts[key] = {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}
        if st == 'cs':
            facts[key]['local'] = list(struct.unpack_from('<3I', ctrl, 0x748)); facts[key]['shared'], = struct.unpack_from('<I', ctrl, 0x754)
        notes[key].append('captured at run time: no GLSL, default facts'); cap_new += 1
    for key, v in stages.items():
        (out / 'rec' / f'{key}.rec').write_bytes(v['rec'])
    with open(out / 'jobs.tsv', 'w', newline='\n') as fp:
        fp.write('key\tstage\tcode_bytes\tctrl_bytes\tc1_bytes\tsources\n')
        for key in sorted(stages):
            v = stages[key]
            fp.write(f"{key}\t{v['stage']}\t{v['code']}\t{v['ctrl']}\t{v['c1']}\t{','.join(sorted(set(v['sources'])))}\n")
    with open(out / 'shader_facts.tsv', 'w', newline='\n') as fp:
        fp.write('# DOOM translation-query facts from the _nx.bin GLSL sources (tools/doom_shader_jobs.py): key <TAB> facts JSON\n')
        for key in sorted(facts): fp.write(key + '\t' + json.dumps(facts[key], sort_keys=True, separators=(',', ':')) + '\n')
    (out / 'glsl_bindings.json').write_text(json.dumps({k: binds[k] for k in sorted(binds)}, indent=0, sort_keys=True))
    with open(out / 'facts_notes.tsv', 'w', newline='\n') as fp:
        for k in sorted(notes):
            for x in notes[k]: fp.write(f'{k}\t{x}\n')
    sc = collections.Counter(v['stage'] for v in stages.values())
    shared_progs = sum(len(set(v['sources'])) > 1 for v in stages.values())
    nc = collections.Counter(re.sub(r'\d+', 'N', re.sub(r'\b\w+_\w+\b', 'ID', x)) for k in notes for x in notes[k])
    rep = [f'.sb files: {len(sbs)} ({dict(kinds)}); stages: {sum(len(v["sources"]) for v in stages.values())}',
           f'unique keys: {len(stages)} ({dict(sc)}); keys shared by several programs (identical stage): {shared_progs}',
           f'real collisions (same key, different code/control): {len(collisions)}',
           f'facts rows: {len(facts)}; compute cross-check vs control: {dict(crosscheck)}',
           'notes:'] + [f'  {v:5d}  {k}' for k, v in nc.most_common(20)]
    if cap_dir: rep.append(f'captures: {len(cap)} valid in {cap_dir}, {cap_new} new keys, {len(cap_bad)} rejected')
    for b in cap_bad: rep.append(f'  CAPTURE REJECTED {b}')
    for c in collisions: rep.append(f'  COLLISION {c}')
    (out / 'report.txt').write_text('\n'.join(rep) + '\n')
    print('\n'.join(rep))
    sys.exit(1 if collisions else 0)


if __name__ == '__main__':
    main()
