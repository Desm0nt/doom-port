# DOOM (Switch update v262144): MULTIPLAYER removed from the main menu

Build id `2847991952b3d7abd228624fcd3b0779` (update image offsets; base v0 names via `main.symbols.json`).

## What the main menu is
`idMenuScreen_Shell_Root` (base 0x11e6d38 Initialize / 0x11e7770 Refresh; update Initialize 0xf17a20,
Refresh 0xf18670; update code differs from base, fmatch does not match, found via string xrefs).
Update Initialize creates three `idMenuWidget_Button`s on the screen object:

| field  | label                                | help                               | widgetAction |
|--------|--------------------------------------|------------------------------------|--------------|
| +0x1c8 | `#str_menu_root_campaign_label` 0x361c568 | `#str_menu_root_campaign_help`  | 0x15 (campaign) |
| +0x1d0 | `#str_menu_root_multiplayer_label` 0x3713aeb | `#str_menu_root_multiplayer_help` | 0x1a (multiplayer) |
| +0x1d8 | `START DEMO` (debug/demo build only) | `Launch the Conan Demo`            | 0xd / 0x21   |

Action 0x1a in `idMenuManager_Shell::HandleAction` (base 0x1b84d74, case 0x1a at base 0x1b85188) goes to
`CheckPrivilegesAndLaunchMultiplayer` -> online privilege check. SnapMap (action 0x17) has **no root entry**
in this build (its label is used only by `idMenuManager_Shell::Activate` / LaunchSnapmaps), nothing to remove.

Refresh rebuilds the list (`[x19+0x1b8]`, `RemoveAllChildren` then):
```
f187c4 ldr x0,[x19,#0x1b8] ; f187c8 ldr x1,[x19,#0x1c8] ; f187cc bl AddChild   ; campaign
f187d0 ldr x0,[x19,#0x1b8] ; f187d4 ldr x1,[x19,#0x1d0] ; f187d8 bl AddChild   ; multiplayer (demo path also jumps to f187d8)
f187dc ...
```

## Cut (dm/tools/patch_nso.py, CUTS for this build id)
`0xf187d0: 60de40f9 (ldr x0,[x19,#0x1b8]) -> 03000014 (b #0xf187dc)` - the MP button is created but never
added to the list; Campaign stays; the demo branch (b 0xf187d8) is unaffected. Refresh then clamps the focus
index to the list size (0xf18838), so a 1-item list is safe. Settings etc. are not in this list (untouched).

Verified: regenerated `work/patched/main.patched.bin` (cuts 16 -> 17, `main.cuts.tsv` has the row);
diff vs previous patched bin = exactly 4 bytes at 0xf187d0. Not run on device yet.

## Undo
Delete (or comment out) the `0xf187d0` entry in CUTS and rerun
`DM_TARGET=android PYTHONUTF8=1 python dm/tools/patch_nso.py work/exefs-update/main work/patched main`.
Fallback idea if MP is still reachable some other way: retarget case 0x1a of the update's
`idMenuManager_Shell::HandleAction` jump table to its default case.
