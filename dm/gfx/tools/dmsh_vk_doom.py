#!/usr/bin/env python3
# [DOOM] Thin wrapper around dmsh_vk.py for DOOM (2016, Switch): same translation and export, other inputs.
# dmsh_vk.py reads D2R's shader_map.tsv / main NSO / nvn/shader_facts.tsv at module level, so this script does not import
# it: it takes dmsh_vk.py's own function definitions (job_for, rewrite, add_format_caps, spirv_info, image_desc, decode_tex,
# export, helper_has_subgroup) and constants from its source with ast and runs them unchanged, with
#   - records and facts from tools/doom_shader_jobs.py (<jobs>/rec/<key>.rec in dmsh_build.job_for's layout,
#     <jobs>/shader_facts.tsv from the _nx.bin GLSL sources, stage per key from <jobs>/jobs.tsv);
#   - one source-level patch of export(): the c0 storage-buffer table base of the geometry stage (0x410) is added to the
#     D2R table {'vs': 0x110, 'cs': 0x310, 'ps': 0x510} (NVN: 0x110 + 0x100 x hardware stage; DOOM's GPU-culling
#     geometry shaders use SSBOs; verified below against the GLSL bindings);
#   - decode_tex replaced by decode_tex_doom: DOOM's 2017 glslc puts combined texture handles at c2 0x20 + 8N and image
#     handles at 0x120 + 8N (D2R: textures 0x168, samplers 0x568);
#   - spirv-opt --trim-capabilities when the spirv-opt in use has it; the NDK r29 one (SPIRV-Tools v2022.4) does not, then a
#     conservative Python trim of the same unconditional Ryujinx capabilities is used (py_trim below; reported in the summary).
# Translator: the DOOM build of dmsh_xlate (dm/gfx/tools/dmsh_xlate_doom, built by tools/build_dmsh_xlate_doom.py), passed as XLATE.
# Stage linking: each vertex / geometry job carries "next" = its next-stage partners in every DOOM program (.sb) that uses the key
#   (vertex -> pixel, or geometry + pixel for vsgsps; geometry -> pixel), so the translator links the stages as Ryujinx's runtime does
#   (outputs the partners read are declared and start at (0, 0, 0, 1); a passthrough geometry shader passes those attributes through).
#   DOOM_LINK=0 turns it off (the pre-2026-10-27 output). Checked after export by iface_check: every pixel input location / component
#   must be declared / written by the stage before it.
# c1 guard (the idea of nxnative dm/gfx/tools/c1_guard.py, redone on SPIR-V): a shader that indexes c1 with a non-constant index gets
#   its c1 bytes zero-padded to 64 KiB in the bundle, so the engine binds c1 with a 64 KiB range and an index past the data reads 0, as
#   Maxwell's out-of-range constant-buffer reads do (the engine binds exactly the c1 bytes the bundle carries). DOOM_C1_GUARD=0: off.
# Chain manifest: <out>/_chain.json records the translator source sha and dll fingerprints, the Ryujinx shader-source digest, the
#   SPIRV-Tools versions, the tool scripts' and job tables' digests, the options, and a chain version (sha256 of all of it, 12 hex
#   digits). The bundle format (XGS1 version 1) and the key are unchanged; the engine does not read the manifest.
# After export every NVN slot is checked against the GLSL layout(binding=N) of the same stage (uniform -> UBO binding,
# storage_pointer -> SSBO binding, uniform_pointer (compute) -> UBO binding, texture -> sampler binding, image -> image binding).
# Usage: dmsh_vk_doom.py <jobs dir> <output dir> [keys...]
#   environment: XLATE (dmsh_xlate executable), J (parallelism, default 8), SPIRV_OPT / SPIRV_VAL, DM_VK_INLINE_CS,
#                DMSH_VK_KEEP=1 (keep the work dir), META=<dir> (also write <key>.vk.json / .res.json / .spv there),
#                DOOM_LINK=0 (no stage linking), DOOM_C1_GUARD=0 (no c1 padding)
import ast, collections, concurrent.futures as cf, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, pathlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pack_game_shaders

# ---- dmsh_vk.py's functions and constants, unchanged except the documented export() patch ----
_src = open(os.path.join(HERE, 'dmsh_vk.py'), encoding='utf-8').read()
_SB_D2R = "{'vs': 0x110, 'cs': 0x310, 'ps': 0x510}.get(stage)"
_SB_DOOM = "{'vs': 0x110, 'gs': 0x410, 'cs': 0x310, 'ps': 0x510}.get(stage)"
NS = {'__name__': 'dmsh_vk_doom_ns'}
exec('import os, re, sys, json, struct, hashlib, shutil, subprocess, tempfile, collections', NS)
WANT_FUNCS = {'helper_has_subgroup', 'job_for', 'insts', 'rewrite', 'add_format_caps', 'spirv_info', 'image_desc', 'decode_tex', 'export'}
WANT_CONST = {'STAGE_NO', 'TEX_BASE', 'SAMP_BASE', 'IMG_BASE'}
_got = set()
for node in ast.parse(_src).body:
    seg = ast.get_source_segment(_src, node)
    if isinstance(node, ast.FunctionDef) and node.name in WANT_FUNCS:
        if node.name == 'export':
            if seg.count(_SB_D2R) != 1: sys.exit('[red] dmsh_vk.py export(): storage-buffer base table changed, update dmsh_vk_doom.py')
            seg = seg.replace(_SB_D2R, _SB_DOOM)
        exec(seg, NS); _got.add(node.name)
    elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in WANT_CONST for t in node.targets) or \
            isinstance(node, ast.Assign) and any(isinstance(t, ast.Tuple) and any(isinstance(e, ast.Name) and e.id in WANT_CONST for e in t.elts) for t in node.targets):
        exec(seg, NS); _got.update(n.id for t in node.targets for n in ast.walk(t) if isinstance(n, ast.Name))
if not WANT_FUNCS <= _got or not WANT_CONST <= _got: sys.exit(f'[red] dmsh_vk.py is missing {(WANT_FUNCS | WANT_CONST) - _got}')

# DOOM's (2017) glslc c2 handle layout differs from D2R's: combined texture handles at 0x20 + 8N (N = GLSL sampler binding,
# 32 slots), image handles at 0x120 + 8N (checked against every stage's GLSL bindings by check_slots). Same decoding rule as
# dmsh_vk.decode_tex otherwise; separate sampler handles (type != 0) do not occur in DOOM and are refused.
DOOM_TEX_BASE, DOOM_IMG_BASE, DOOM_IMG_END = 0x20, 0x120, 0x168


def decode_tex_doom(slots, h, image):
    tb = (slots & 0xffff) if slots >= 0 else 2
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if ty: return None, None, ty
    def slot(word, base, end): return (word * 4 - base) // 8 if tb == 2 and base <= word * 4 < end and (word * 4 - base) % 8 == 0 else None
    return slot(o0, DOOM_IMG_BASE, DOOM_IMG_END) if image else slot(o0, DOOM_TEX_BASE, DOOM_IMG_BASE), None, ty
NS['decode_tex'] = decode_tex_doom


OPT, VAL = (os.environ.get(k, d) for k, d in (('SPIRV_OPT', 'spirv-opt'), ('SPIRV_VAL', 'spirv-val')))
INLINE_CS = os.environ.get('DM_VK_INLINE_CS') == '1'


# ---- capability trim fallback (spirv-opt without --trim-capabilities) ----
BALLOT_BUILTINS = {4416, 4417, 4418, 4419, 4420}   # Subgroup{Eq,Ge,Gt,Le,Lt}Mask
GROUP_BUILTINS = {36, 37, 38, 39, 40, 41}           # SubgroupSize, NumSubgroups, SubgroupId, ... SubgroupLocalInvocationId
DRAW_BUILTINS = {4424, 4425, 4426}                  # BaseVertex, BaseInstance, DrawIndex
EXT_CAPS = {'SPV_KHR_shader_draw_parameters': {4427}, 'SPV_EXT_fragment_shader_interlock': {5363, 5372, 5378}}


def py_trim(spv):
    """Remove capabilities Ryujinx declares unconditionally when nothing in the module needs them: Float64 / Int64 / Int16 /
    Float16 (no type of that width), GroupNonUniform* (no instruction / builtin of that class), DrawParameters (no
    BaseVertex / BaseInstance / DrawIndex builtin), fragment shader interlock (no begin/end interlock, no interlock execution
    mode); then extensions whose only capabilities were removed."""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    caps, ops, builtins, fw, iw, modes = set(), set(), set(), set(), set(), set()
    for i, op, a in NS['insts'](w):
        ops.add(op)
        if op == 17: caps.add(a[0])
        elif op == 22: fw.add(a[1])
        elif op == 21: iw.add(a[1])
        elif op == 71 and len(a) >= 3 and a[1] == 11: builtins.add(a[2])          # OpDecorate ... BuiltIn b
        elif op == 72 and len(a) >= 4 and a[2] == 11: builtins.add(a[3])          # OpMemberDecorate ... BuiltIn b
        elif op in (16, 331): modes.add(a[1])                                      # OpExecutionMode(Id)
    need = {10: 64 in fw, 9: 16 in fw, 11: 64 in iw, 22: 16 in iw,
            61: bool(ops & set(range(333, 367))) or bool(builtins & (GROUP_BUILTINS | BALLOT_BUILTINS)),
            62: bool(ops & {334, 335, 336}), 63: bool(ops & set(range(349, 365))),
            64: bool(ops & set(range(337, 345))) or bool(builtins & BALLOT_BUILTINS),
            65: bool(ops & {345, 346}), 66: bool(ops & {347, 348}), 68: bool(ops & {365, 366}), 4427: bool(builtins & DRAW_BUILTINS),
            **{c: bool(ops & {5364, 5365}) or bool(modes & set(range(5366, 5372))) for c in (5363, 5372, 5378)}}   # fragment interlock
    drop = {c for c, n in need.items() if c in caps and not n}
    out, i = w[:5], 5
    for j, op, a in NS['insts'](w):
        n = w[j] >> 16
        if op == 17 and a[0] in drop: continue
        if op == 10:
            name = bytes(struct.pack('<%dI' % len(a), *a)).split(b'\0')[0].decode()
            if name in EXT_CAPS and EXT_CAPS[name] <= drop | (EXT_CAPS[name] - caps): continue
        out.extend(w[j:j + n])
    return struct.pack('<%dI' % len(out), *out), sorted(drop)


def opt_has_trim():
    r = subprocess.run([OPT, '--help'], capture_output=True, text=True, errors='replace')
    return '--trim-capabilities' in r.stdout + r.stderr


# ---- binding check against the GLSL source ----
def check_slots(vk, gb):
    bad = []
    for d in vk['descriptors']:
        n = d['source'].get('nvn') or {}
        k, s = n.get('kind'), n.get('slot')
        want = {'uniform': 'ubo', 'uniform_pointer': 'ubo', 'storage_pointer': 'ssbo', 'texture': 'tex', 'image': 'img'}.get(k)
        if want and str(s) not in gb.get(want, {}): bad.append(f'{k} slot {s} not a GLSL {want} binding')
        if k == 'texture' and n.get('sampler_slot') is not None and str(n['sampler_slot']) not in gb.get('tex', {}):
            bad.append(f'sampler slot {n["sampler_slot"]} not a GLSL sampler binding')
    return bad


# ---- stage linking: next-stage partners from the programs (jobs.tsv sources) ----
def partners(meta):
    """{key: sorted next-stage partner keys}: vs -> gs + ps of its programs (the gs passes the pixel inputs through), gs -> ps"""
    progs = collections.defaultdict(dict)
    for k, v in meta.items():
        for src in v['sources']: progs[src][v['stage']] = k
    nxt = collections.defaultdict(set)
    for p in progs.values():
        if 'ps' not in p: continue
        if 'gs' in p: nxt[p['gs']].add(p['ps'])
        if 'vs' in p: nxt[p['vs']].update(x for x in (p.get('gs'), p['ps']) if x)
    return {k: sorted(v) for k, v in nxt.items()}, progs


def spv_io(spv):
    """(declared output locations, {loc: components written}, declared input locations, {loc: components read}) of a stage's
    user-defined interface (Location-decorated, not BuiltIn); the last access-chain index is the component"""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    loc, bi, var, consts, ac = {}, set(), {}, {}, {}
    for i, op, a in NS['insts'](w):
        if op == 71 and a[1] == 30: loc[a[0]] = a[2]
        elif op == 71 and a[1] == 11: bi.add(a[0])
        elif op == 59: var[a[1]] = a[2]
        elif op == 43 and len(a) >= 3: consts[a[1]] = a[2]
    outs = {v: loc[v] for v, sc in var.items() if sc == 3 and v in loc and v not in bi}
    ins = {v: loc[v] for v, sc in var.items() if sc == 1 and v in loc and v not in bi}
    wr, rd = collections.defaultdict(set), collections.defaultdict(set)
    for i, op, a in NS['insts'](w):
        if op in (65, 66) and (a[2] in outs or a[2] in ins):
            ac[a[1]] = (a[2], consts.get(a[-1]) if len(a) > 3 else None)
        elif op == 62:
            if a[0] in outs: wr[outs[a[0]]] |= {0, 1, 2, 3}
            elif a[0] in ac and ac[a[0]][0] in outs: wr[outs[ac[a[0]][0]]].add(ac[a[0]][1])
        elif op == 61:
            if a[2] in ins: rd[ins[a[2]]] |= {0, 1, 2, 3}
            elif a[2] in ac and ac[a[2]][0] in ins: rd[ins[ac[a[2]][0]]].add(ac[a[2]][1])
    return set(outs.values()), wr, set(ins.values()), rd


def iface_check(progs, spvs):
    """per program with a pixel stage: pixel input locations the stage before it does not declare, and components it does not write"""
    out = collections.Counter(); bad = []
    for src, p in sorted(progs.items()):
        up = p.get('gs') or p.get('vs')
        if 'ps' not in p or up not in spvs or p['ps'] not in spvs: continue
        od, wr, _, _ = spv_io(spvs[up]); _, _, idl, rd = spv_io(spvs[p['ps']])
        miss = sorted(l for l in idl if l not in od)
        part = sorted((l, sorted(c for c in rd[l] - wr[l] if c is not None)) for l in idl if l in od and rd[l] - wr[l] - {None})
        out['pairs'] += 1
        if miss: out['pixel input not declared upstream'] += 1; bad.append(f'{src}: {up} -> {p["ps"]} undeclared {miss}')
        if part: out['pixel input component not written upstream'] += 1; bad.append(f'{src}: {up} -> {p["ps"]} unwritten {part}')
    return out, bad


# ---- c1 guard ----
def c1_dynamic(spv, vk):
    """number of access chains into the c1 uniform block with a non-constant index"""
    c1b = {d['binding'] for d in vk['descriptors'] if (d['source'].get('nvn') or {}).get('kind') == 'c1'}
    if not c1b: return 0
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    bind, consts = {}, set()
    for i, op, a in NS['insts'](w):
        if op == 71 and a[1] == 33: bind[a[0]] = a[2]
        elif op == 43: consts.add(a[1])
    c1v = {v for v, b in bind.items() if b in c1b}
    return sum(1 for i, op, a in NS['insts'](w) if op in (65, 66) and a[2] in c1v and any(x not in consts for x in a[3:]))


# ---- chain manifest ----
def sha16(data): return hashlib.sha256(data).hexdigest()[:16]


def tool_version(exe):
    r = subprocess.run([exe, '--version'], capture_output=True, text=True, errors='replace')
    t = (r.stdout.strip() or r.stderr.strip())
    return t.splitlines()[0] if t else 'missing'


def chain_manifest(xlate, jobs_dir, opts):
    xdir = os.path.dirname(os.path.abspath(xlate)); src = pathlib.Path(HERE).parents[2]   # .../src
    m = {'xlate_source': subprocess.run([xlate, '--source-sha'], capture_output=True, text=True).stdout.strip()}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll', 'Spv.Generator.dll'):
        p = os.path.join(xdir, dll); m[dll] = sha16(open(p, 'rb').read()) if os.path.exists(p) else 'missing'
    ryu = src / 'dm' / 'third_party' / 'ryujinx' / 'src'
    h = hashlib.sha256()
    for f in sorted(ryu.rglob('*.cs')): h.update(f.relative_to(ryu).as_posix().encode() + b'\0' + f.read_bytes() + b'\0')
    m['ryujinx_src'] = h.hexdigest()[:16]
    m['spirv-opt'], m['spirv-val'] = tool_version(OPT), tool_version(VAL)
    for f in ('dmsh_vk_doom.py', 'dmsh_vk.py', 'pack_game_shaders.py'): m[f] = sha16(open(os.path.join(HERE, f), 'rb').read())
    jt = src.parent / 'tools' / 'doom_shader_jobs.py'
    m['doom_shader_jobs.py'] = sha16(jt.read_bytes()) if jt.exists() else 'missing'
    for f in ('jobs.tsv', 'shader_facts.tsv', 'glsl_bindings.json'): m[f] = sha16(open(os.path.join(jobs_dir, f), 'rb').read())
    m['options'] = opts
    m['chain'] = hashlib.sha256(json.dumps(m, sort_keys=True).encode()).hexdigest()[:12]
    return m


def main():
    jobs_dir, out = sys.argv[1:3]; want = set(sys.argv[3:])
    xlate = os.environ.get('XLATE') or sys.exit('[red] XLATE=<dmsh_xlate executable> is required')
    NS['FACTS'] = {k: json.loads(v) for k, v in (l.rstrip('\n').split('\t', 1) for l in open(os.path.join(jobs_dir, 'shader_facts.tsv'))
                                                 if l.strip() and not l.startswith('#'))}
    meta = {}
    for ln in open(os.path.join(jobs_dir, 'jobs.tsv')):
        c = ln.rstrip('\n').split('\t')
        if c[0] != 'key': meta[c[0]] = {'stage': c[1], 'sources': c[5].split(',')}
    NS['stage_of'] = {k: v['stage'] for k, v in meta.items()}
    gbind = json.load(open(os.path.join(jobs_dir, 'glsl_bindings.json')))
    keys = sorted(want) if want else sorted(meta)
    NJ = int(os.environ.get('J', '8'))
    os.makedirs(out, exist_ok=True)
    metadir = os.environ.get('META')
    if metadir: os.makedirs(metadir, exist_ok=True)
    work = tempfile.mkdtemp(prefix='dmsh_vk_doom_', dir=os.environ.get('WORK'))
    bundle = os.path.join(work, 'bundle'); os.makedirs(bundle)
    link = os.environ.get('DOOM_LINK', '1') != '0'; c1_guard = os.environ.get('DOOM_C1_GUARD', '1') != '0'
    nxt, progs = partners(meta)
    jobs, c1s = {}, {}
    need = set(keys) | ({p for k in keys for p in nxt.get(k, ())} if link else set())
    for key in sorted(need):
        rec = open(os.path.join(jobs_dir, 'rec', key + '.rec'), 'rb').read()
        jobs[key], c1s[key] = NS['job_for'](key, meta[key]['sources'][0], rec, work)
    if link:
        for key in keys:
            if nxt.get(key): jobs[key]['next'] = [{'key': p, 'code': jobs[p]['code'], 'c1': jobs[p]['c1']} for p in nxt[key]]
    chunks = [keys[i::NJ] for i in range(NJ)]

    def xl(ch):
        jp = os.path.join(work, f'job_{ch[0]}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([xlate, jp, work], capture_output=True)
    with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
    xerr = {}
    for r in xr:   # dmsh_xlate prints "【红】<key>：<Exception>：<message>"; the console code page may mangle the CJK marks
        for ln in r.stderr.decode('utf-8', 'replace').splitlines():
            m = re.search(r'([0-9a-f]{16})\W{0,3}(\w+(?:Exception|Error)\W{0,3}.*)', ln)
            if m: xerr[m.group(1)] = re.sub(r'[^\x20-\x7e]+', ': ', m.group(2))[:200]
    use_opt = opt_has_trim()
    trim_stats = collections.Counter(); spvs = {}

    def one(key):
        if key in xerr or not os.path.exists(os.path.join(work, key + '.spv')): return key, 'translate failed', xerr.get(key, 'no spv'), None
        stage = jobs[key]['stage']
        spv = NS['rewrite'](open(os.path.join(work, key + '.spv'), 'rb').read(), stage)
        f1, f2 = os.path.join(work, key + '.r.spv'), os.path.join(work, key + '.t.spv')
        open(f1, 'wb').write(spv)
        if use_opt:
            r = subprocess.run([OPT, '--trim-capabilities', f1, '-o', f2], capture_output=True, text=True, errors='replace')
            if r.returncode: return key, 'spirv-opt failed', r.stderr.strip()[:200], None
        else:
            t, dropped = py_trim(spv); open(f2, 'wb').write(t)
            for c in dropped: trim_stats[c] += 1
        if INLINE_CS and stage == 'cs' and NS['helper_has_subgroup'](open(f2, 'rb').read()):
            f3 = os.path.join(work, key + '.i.spv')
            r = subprocess.run([OPT, '--merge-return', '--inline-entry-points-exhaustive', '--eliminate-dead-functions', f2, '-o', f3], capture_output=True, text=True, errors='replace')
            if r.returncode: return key, 'spirv-opt inline failed', r.stderr.strip()[:200], None
            os.replace(f3, f2)
        spv = NS['add_format_caps'](open(f2, 'rb').read()); open(f2, 'wb').write(spv)
        r = subprocess.run([VAL, '--target-env', 'vulkan1.1', f2], capture_output=True, text=True, errors='replace')
        if r.returncode: return key, 'spirv-val failed', (r.stderr.strip() or r.stdout.strip() or '?').splitlines()[0][:200], None
        res = json.load(open(os.path.join(work, key + '.res.json'), encoding='utf-8'))
        try: vk = NS['export'](key, stage, c1s[key], spv, res)
        except (ValueError, KeyError) as e: return key, 'export failed', re.sub(r'[^\x20-\x7e]+', ' ', str(e))[:200], None
        dyn = c1_dynamic(spv, vk)
        if dyn and c1_guard: vk['frozen_job']['c1'] = (c1s[key] + bytes(65536 - len(c1s[key]))).hex()
        open(os.path.join(bundle, key + '.spv'), 'wb').write(spv); spvs[key] = spv
        json.dump(vk, open(os.path.join(bundle, key + '.vk.json'), 'w', encoding='utf-8'), ensure_ascii=False)
        try: k2, data = pack_game_shaders.pack(pathlib.Path(os.path.join(bundle, key + '.vk.json')))
        except (ValueError, KeyError) as e: return key, 'pack failed', str(e)[:200], None
        open(os.path.join(out, k2 + '.xgs'), 'wb').write(data)
        if metadir:
            for ext in ('.vk.json', '.spv'): shutil.copyfile(os.path.join(bundle, key + ext), os.path.join(metadir, key + ext))
            shutil.copyfile(os.path.join(work, key + '.res.json'), os.path.join(metadir, key + '.res.json'))
        info = {'caps': vk['capabilities'], 'notes': res.get('notes', []), 'linked': res.get('linked'), 'c1_dynamic': dyn, 'slot_check': check_slots(vk, gbind.get(key, {})) if key in gbind else ['no GLSL bindings']}
        return key, 'ok', '', info

    with cf.ThreadPoolExecutor(NJ) as ex: res = list(ex.map(one, keys))
    if os.environ.get('DMSH_VK_KEEP'): print('work dir kept:', work)
    else: shutil.rmtree(work, ignore_errors=True)
    st = collections.Counter(r[1] for r in res)
    lines = [f'stages {len(res)}: ok {st["ok"]}' + ''.join(f', {k} {v}' for k, v in st.items() if k != 'ok'),
             'capability trim: ' + ('spirv-opt --trim-capabilities' if use_opt else f'py_trim (spirv-opt lacks --trim-capabilities); dropped {dict(trim_stats)}')]
    for lvl in [k for k in st if k != 'ok']:
        for e, n in collections.Counter(re.sub(r'\b[0-9a-f]{16}\b', 'KEY', r[2]) for r in res if r[1] == lvl).most_common(12): lines.append(f'  [{lvl}] {n:5d}  {e}')
    by_stage = collections.Counter((meta[r[0]]['stage'], r[1]) for r in res)
    lines.append('by stage: ' + ', '.join(f'{s}/{k} {v}' for (s, k), v in sorted(by_stage.items())))
    ok = [r for r in res if r[1] == 'ok']
    lines.append('capabilities in bundles: ' + ', '.join(f'{c}:{n}' for c, n in sorted(collections.Counter(c for r in ok for c in r[3]['caps']).items())))
    sc = collections.Counter(x for r in ok for x in r[3]['slot_check'])
    lines.append(f'slot check vs GLSL bindings: {sum(not r[3]["slot_check"] for r in ok)} clean, {sum(bool(r[3]["slot_check"]) for r in ok)} with mismatches')
    for e, n in sc.most_common(12): lines.append(f'  {n:5d}  {e}')
    for label, tr in (('queries answered by default', False), ('translator messages', True)):
        nt = collections.Counter(re.sub(r'[0-9A-F]{3,}|\d+', 'N', x) for r in ok for x in r[3]['notes'] if x.startswith('translator: ') == tr)
        lines.append(f'notes, {label}: {sum(any(x.startswith("translator: ") == tr for x in r[3]["notes"]) for r in ok)} stages')
        for e, n in nt.most_common(10): lines.append(f'  {n:5d}  {e}')
    lines.append(f'stage linking: {"on" if link else "off"}; linked stages {sum(r[3]["linked"] is not None for r in ok)}')
    lines.append(f'c1 guard: {sum(r[3]["c1_dynamic"] > 0 for r in ok)} stages index c1 dynamically' + (' (c1 padded to 64 KiB)' if c1_guard else ' (not padded)'))
    ic, ib = iface_check(progs, spvs)
    lines.append('interface check (pixel inputs vs the stage before): ' + ', '.join(f'{k} {v}' for k, v in ic.items()))
    for b in ib[:12]: lines.append('  ' + b)
    man = chain_manifest(xlate, jobs_dir, {'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1',
                                           'link': link, 'c1_guard': c1_guard, 'inline_cs': INLINE_CS, 'trim': 'spirv-opt' if use_opt else 'py_trim'})
    json.dump(man, open(os.path.join(out, '_chain.json'), 'w'), indent=1, sort_keys=True)
    lines.append(f'chain {man["chain"]} (translator source {man["xlate_source"]}, ryujinx {man["ryujinx_src"]})')
    print('\n'.join(lines))
    json.dump([{'key': r[0], 'stage': meta[r[0]]['stage'], 'st': r[1], 'err': r[2], **(r[3] or {})} for r in res],
              open(os.path.join(out, '_vk_build.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    open(os.path.join(out, '_vk_build.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    sys.exit(0 if st['ok'] == len(res) else 1)


if __name__ == '__main__':
    main()
