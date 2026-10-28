# DOOM (2016, Switch update v262144): adding a "Dynamic Resolution" entry to Settings → Video

Research only; nothing under the game data was modified. Binaries used:
`work/exefs-base/main.raw` (v0, has a full export symbol table in `main.symbols.json`) and
`work/exefs-update/main.raw` (v262144, what we run; build id `2847991952b3d7abd228624fcd3b0779`, stripped).
Both raw images are flat (file offset == image address). Update addresses below are update image offsets;
where only base has names, the base address is given as "base 0x...".
Note: the extracted romfs is at `work/romfs` (+ `work/romfs-list.tsv`, `work/romfs-manifest.json`), next to the two .nsz dumps it came from.

## TL;DR

* **The Switch Video menu is not data-driven.** There is no menu/settings decl type. The Video screen is the C++
  class `idMenuWidget_Shell_VideoSettings` + `idMenuDataSource_VideoSettings`; `Initialize()` creates exactly
  5 hardcoded buttons (FOV, Brightness/Gamma, Motion Blur, Chromatic Aberration, Colorblind Mode), each with a
  hardcoded `#str_` label and a datasource field index. The values go through `idPlayerProfileShell` getters/
  setters that write fixed cvars. The SWF (`generated/swf/menu_shell/menu_shell.bswf`) supplies only the visuals
  (list `options` with sprites `item0..item13`).
* **No option binds a cvar by name from data**, so a romfs overlay cannot add a menu option. You can only do it by
  patching code.
* **Cheapest real in-menu option:** repurpose the **Chromatic Aberration** toggle as **Dynamic Resolution**
  (`rs_enable`). This takes 6 instruction words (3 adrp+ldr pairs that redirect a GOT slot from `r_chromaticAberration` to
  `rs_enable`) plus 2 in-place rodata string patches for the label and help text. All of these fit the existing
  `CUTS` table in `dm/tools/patch_nso.py` (DOOM build-id key). The profile then also saves the setting.
* A real 6th entry, or a resolution-scale list, needs new native code (new widget + datasource field +
  profile setter). That is not practical as data. The closest workable option is to hijack the Colorblind list with a
  trampoline, but this has bad tradeoffs (see §3.3). For the scale, keep using `+set rs_forceFractionX/Y`.

## 1. How the options menus are defined

### 1.1 Data side: decls, SWF, strings
* Decl types in `gameresources_003.pindex` (parsed with `tools/doom_index.py`): the only menu-ish types are
  `shellMenu` (58 entries, all `generated/decls/shellmenu/multiplayer/profile/challenges/...`, e.g.
  `{ edit = { displayName = "#str_menu_mp_challenges_mode_doublehelix"; } }`) and `devMenuOption`
  (dev menu). **No** settings or option decl type exists, and no decl names a cvar for a menu.
* SWF: `generated/swf/menu_shell/menu_shell.bswf` (type `file`, patch 2 = `gameresources_002.patch`, zlib raw
  deflate, 5 188 995 bytes unpacked). Its symbol names include `videoSettings`, `advancedVideoSettings`, `audioSettings`,
  `controllerSettings`, `gameSettings`, `mouseKeyboardSettings`, `_center_videoMenu`, `options`, `item10..item13`,
  `option_0..option_10`, `colorOptions_On/Off`. These are only sprite and timeline names. The option list content is set by code.
* Localisation: `strings/<lang>.lang` are `binaryFile` entries (`generated/binaryfile/strings/english.bfile`,
  patch 3, flags 0x10, 2 258 444 bytes) and are **encrypted** (high-entropy data; the known idCrypt
  "swapTeam" key scheme did not verify, so the scheme is unknown). `default.cfg` / `xb_*.cfg` binaryFiles are encrypted too: 28 540 bytes
  vs. 28 469 bytes for the loose plaintext `romfs/base/default.cfg`, which is consistent with a salt+IV+AES-CBC+HMAC wrapper.
  Flag 0x10 alone only means "deflate". Other 0x10 `file` entries (the bswf) inflate fine.

### 1.2 Code side (base symbols, confirmed in update by xrefs)
* `idMenuWidget_Shell_VideoSettings::Initialize` (base 0x1291ba8; update ≈0xffcca8):
  * creates an `idMenuWidget_List` with sprite path `"options"`, `SetupControls(14)` (14 visible slots),
  * then 5 buttons, each `SetLabel("#str_menu_system_field_<x>_label")`, a help string, and `SetDataSource(ds, field)`:
    `fov`, `gamma`, `motion_blur`, `chromatic_aberration`, `colorblind_mode`.
    Update xrefs: fov label adrp at 0xffce60, CA label at **0xffd23c** (`0x35d6d52`), CA help at **0xffd280**
    (`0x39cd272`).
* `idMenuDataSource_VideoSettings::LoadData` (base 0x128fa48) calls only
  `idPlayerProfileShell::GetFieldOfView / GetMotionBlur / GetGamma / GetChromaticAberration / GetColorblindMode`.
  Popups: `ShowMotionBlurSelection`, `ShowChromaticAberrationSelection`, `ShowColorblindModeSelection`.
* The profile accessors are tiny cvar wrappers (base):
  `SetChromaticAberration` = `cvar(r_chromaticAberration)->SetInteger(v > 0, true)`, `GetChromaticAberration` =
  `return r_chromaticAberration.int`, `SetMotionBlur` maps 0..3 → `SetFloat(...)` on a shutter-speed cvar, etc.
  Update copies (found via the GOT slot of `r_chromaticAberration`, `0x4895850`):
  ```
  0x10e7c50 GetChromaticAberration: adrp x8,#0x4895000 ; ldr x8,[x8,#0x850] ; ldr w0,[x8,#0x30] ; ret
  0x10e7c60 SetChromaticAberration: adrp x0,#0x4895000 ; ldr x0,[x0,#0x850] ; cmp w1,#0 ; cset w1,gt ; mov w2,#1 ; b 0x1206a0 (idCVar::SetInteger)
  0x10e7c78..7c: 2x udf padding ; 0x10e7c80 = GetColorblindMode ([0x4895858] = r_colorBlindMode)
  ```
  Callers in update: Get at 0xffaf0c, Set at 0xffb970 and 0xffbde8 (all inside the VideoSettings datasource).
* Profile persistence: one profile-serialise function (update ≈0x10e0e00..0x10e1200) binds cvars by **name +
  cvar pointer** through `0x1107cc0(this, ser, "name", idCVar*)`. The list is `m_sensitivity, s_volume_*, r_motionblur,
  r_hdrShutterSpeed, r_chromaticAberration, r_colorBlindMode, g_fov, net_voice, net_voiceVolume, r_gamma`, followed by bool flags.
  The CA entry is `0x10e0fc4 adrp x3,#0x4895000 ; 0x10e0fcc ldr x3,[x3,#0x850] ; "r_chromaticAberration" ; bl 0x1107cc0`.
* `rs_enable` cvar: registered at update 0x1c2fea0 (GOT slot **`0x48d53b0`**, default `"4"`, flags 0x12,
  description: "Enable dynamic resolution scaling, 0 - off, 1 - horz only, 2 - vert only, 3 - both"). Readers at
  0x1c26ec4 / 0x1c27898 test it with `cbz`, so they only check for nonzero. What value 4 means is not documented (guess:
  a Switch-specific mode). `r_chromaticAberration` default `"1"`, `resource_loadLooseAssets` default `"0"`,
  `com_loadlooseLanguages` default `"0"`.

### 1.3 PC options hidden or removed?
* No `#str_` keys for resolution, resolution scaling, dynamic resolution, vsync or AA exist in the update binary
  (`#str_menu_video_*` only covers hdrbloom, dof, lensflare, lensdirt, sharpening, fov, colorblind, rendering mode,
  low/medium/high/ultra and similar). There is no code for an Advanced Video screen:
  `idMenuWidget_Shell_AdvancedVideoSettings` has only RTTI stubs (CastTo, CreateInstance, IsTypeOf at base
  0x1bb95e8..0x1bb97c0) and no Initialize or datasource.
* Remnants: the SWF sprite `advancedVideoSettings`, and the strings `advResolutionScale`,
  `advChromaticAberrationAmountIndex`, `advDirectionalOcclusionQualityIndex`, `seenAdvancedSettingsWarning`,
  `GDM_CONFIRM_ADVANCED_CHANGES`, `#str_dlg_advanced_settings_warning`, plus base `idCommonLocal::LoadVideoSettingsPresetsFile`.
  None of the `adv*` names has a code xref (probably reflection or typeinfo member names; that is a guess). **Conclusion:** the PC Advanced/Resolution UI was
  compiled out. Nothing hidden can be switched back on.

## 2. Loose files vs .resources, override search paths

* All decls, swf, strings and cfgs are inside `gameresources.resources` plus the `.patch`, `_002.patch` and `_003.patch` containers,
  indexed by `gameresources*.(p)index` (entry: type, name, name2, offset, size, zsize, flags, patch#; patch 0 =
  `.resources`, 1 = `.patch`, N = `_00N.patch`). Every container has a `.verify` file. The only loose data are
  `base/*.cfg`, the soundbanks, videos, motd and live_tiles, plus `virtualtextures/*`.
* Config exec strings: `resourceExec default.cfg -s`, `resourceExec default_sp/mp/coop.cfg -s`,
  `resourceExec package.cfg -s`, `verifiedExec candidate.cfg -s`, `verifiedExec disclayout.cfg -s` ("execute a
  configuration file stored in the resource file"). So `default.cfg` is run **from the archive** (encrypted
  binaryFile), not from the loose copy (likely, not traced). User config: `zionConfigNX.cfg` (read at
  update 0x1ec2fd4, written at 0x1ed4b58, saved in save storage, guarded by `configVersion 7`).
* FS cvars: `fs_basepath`, `fs_savepath`, `fs_installpath`, `fs_cachepath`, `fs_generatedPath`, `fs_sourcepath`,
  `fs_readOnly`, `fs_debug`, `fs_copyfiles`, etc. There is **no `fs_game`/mod path**. The only loose-override hooks are
  `resource_loadLooseAssets` (default 0, "check for loose assets when satisfying file io from a resource file")
  and `com_loadlooseLanguages` (default 0, "loads the language files loosly"), with path builders
  `"%s/_loose/%s"`, `"%s/_fast/%s"`, `"%s/_combo/%s"` (update 0x1b083c0). Whether these work in a production build,
  and which exact path they expect, is unverified (guess: dev-only paths). Our host overlay (`hle_fs.c`, `DM_ROMFS_OVERLAY`)
  can replace a whole container or a loose file at the FS level. Replacing one entry inside an archive would mean rebuilding
  a `.patch` and `.pindex` (and maybe `.verify`). For menus this does not matter anyway, because the menu is code.

## 3. Plan

### 3.1 Recommended: repurpose "Chromatic Aberration" → "Dynamic Resolution" (rs_enable)
Add to `CUTS['2847991952b3d7abd228624fcd3b0779']` in `dm/tools/patch_nso.py`. The old bytes below were read from
`work/exefs-update/main.raw` and the new bytes were assembled and checked with capstone:
```python
(0x10e7c50, '68bd01d0', '68bf01d0', 'idPlayerProfileShell::GetChromaticAberration -> rs_enable: adrp x8,#0x4895000 -> #0x48d5000'),
(0x10e7c54, '082944f9', '08d941f9', '... ldr x8,[x8,#0x850] -> [x8,#0x3b0] (GOT rs_enable)'),
(0x10e7c60, '60bd01d0', '60bf01d0', 'idPlayerProfileShell::SetChromaticAberration -> rs_enable: adrp x0'),
(0x10e7c64, '002844f9', '00d841f9', '... ldr x0,[x0,#0x850] -> [x0,#0x3b0]; still SetInteger(v>0, force)'),
(0x10e0fc4, 'a3bd01b0', 'a3bf01b0', 'profile serialise: key "r_chromaticAberration" now binds rs_enable (adrp x3)'),
(0x10e0fcc, '632844f9', '63d841f9', '... ldr x3,[x3,#0x850] -> [x3,#0x3b0]'),
```
Label and help (in-place rodata, each referenced by exactly one adrp+add: 0xffd23c and 0xffd280). The new text must be
padded with NUL bytes to the original length:
```python
(0x35d6d52, b'#str_menu_system_field_chromatic_aberration_label\0'.hex(), b'Dynamic Resolution'.ljust(50, b'\0').hex(), 'Video menu label'),
(0x39cd272, b'#str_menu_system_field_chromatic_aberration_help\0'.hex(),  b'Lower the 3D resolution when the GPU is busy'.ljust(49, b'\0').hex(), 'Video menu help'),
```
Host side, in `dm/host/hle_args.c`: once the profile owns `rs_enable`, the default `+set rs_enable 0` is overwritten when the
profile loads, so the menu decides. Drop that default, or keep it only for the env override. Optionally add `+set r_chromaticAberration 0`, because CA is no
longer loaded from the profile and stays at its default `1`.

Risks and things to verify:
* ON writes `rs_enable 1`, which the description calls "horz only", not the shipped default `4`. Test `+set rs_enable 1` vs `4`
  first. Writing 4 needs one more instruction (`lsl w1,w1,#2`). It fits in the setter plus the 0x10e7c78 padding (7 words).
  But then the getter returns 4, and the 2-entry popup may index out of range. A getter that clamps needs space that is not available
  in place, so it needs a small trampoline in the rewriter patch area.
* Existing saves: the saved `r_chromaticAberration` value (usually 1) loads into `rs_enable`, so DRS starts ON for
  current users until they toggle it.
* The literal label assumes that idLocalization passes text without a `#str_` prefix through unchanged (id Tech
  convention; a guess for Tech 6). If it shows blank or the raw key instead, reuse an existing key, or ship a loose language file
  via `com_loadlooseLanguages 1` (unverified).
* Other code still reads `r_chromaticAberration` directly (renderer 0x1c8a028, 0x10d9118 name list). It keeps
  working, with CA fixed at its cvar value.
* rodata strings could also be reached through relocated data pointers. Only adrp+add code xrefs were checked, because the
  update raw image has unrelocated (zero) GOT/data. Low risk, but confirm with a log or in-game.
* The confirm or "restart required" flow of the Video datasource is untouched. `rs_enable` is read every frame.

### 3.2 Resolution scale (rs_forceFractionX/Y)
No float/list option exists that can be redirected in place. Motion Blur (4 steps → `SetFloat` on a shutter cvar,
`SetMotionBlur` base 0xc9e414) and Colorblind (4 entries → `r_colorBlindMode`) are the only multi-value fields.
Hijacking one means a trampoline in the rewriter patch area that maps index → float and calls
`idCVar::SetFloat` twice (X and Y). The popup entry labels (`#str_menu_video_colorblind_*` / motion blur steps) would also
need string patches, and you lose that accessibility or effect option. **Recommendation:** keep the scale on
`DM_DOOM_RES` → `+set rs_forceFractionX/Y` (already in `hle_args.c`), or seed it into `zionConfigNX.cfg` if the cvar
is archived (not checked).

### 3.3 Alternatives if code patching is rejected
* Command line `+set` (already in use) or an app-side toggle. Startup config: lines in `zionConfigNX.cfg` in the save mount
  (written back by the game; only archived cvars persist; `configVersion` must match).
* `default.cfg` overrides via romfs overlay would not take effect unless the archive copy is replaced (it is run with
  `resourceExec` from the encrypted binaryFile; likely).

## Tools and scripts used
`tools/doom_index.py` (index parser), small scratch Python scripts (string dump, adrp/add and GOT xref scanner, capstone
disassembly with base symbols). Key strings: `0x37b9481 rs_enable`, `0x384f65e r_chromaticAberration`,
`0x35d6d52/0x39cd272` CA label and help, `0x39e6e0a zionConfigNX.cfg`, `0x35ab64a resource_loadLooseAssets`,
`0x35ce59c resourceExec default.cfg -s`.
