# D2R-derived records in the base tree (from d2r-mobile)

DOOM Mobile is built on d2r-mobile, a native port of Diablo II: Resurrected. Its tree still carries some records of
facts about *that* game, written by the d2r-mobile authors; apart from `nvn.h` they serve only the D2R build. This page is
d2r-mobile's own description of them, kept so their nature and the credit for the offline rewrites stay on record.
The DOOM-derived information is listed in [doom/DERIVED.md](doom/DERIVED.md).

## In the repository

These are records of facts about Diablo II: Resurrected, not game content: d2r-mobile needs them to produce a working package from the player's own D2R files.

| Where | Size | What it is | Why it has to be here |
|---|---|---|---|
| `dm/gfx/nvn/shader_map.tsv` | 1.33 MB | The identifier and checksum of every original shader variant, and the correspondences between them. No shader code | Shader translation uses it to tell which shader is which |
| `dm/gfx/nvn/shader_facts.tsv` | 1.13 MB | The interface description of every shader: which texture slots it uses, of what type, and how many outputs | Translation needs these facts; they would otherwise have to be looked up in the PC release's files, and most people only have the Switch release |
| `dm/gfx/nvn/pso_manifest.tsv` | 177 KB | The list of render pipeline state combinations the game uses | Pipelines are built ahead of time at launch, so the first use does not stutter |
| `dm/gfx/nvn/names.txt`, `nvn_slots.tsv` | 16 KB, 24 KB | The 523 function names of the console graphics API, and where the game's main program keeps them | The host takes over these functions by name |
| `dm/gfx/nvn/include/nvn.h` | 89 KB | Declarations of the console graphics API. Third-party work under the MIT license; the author is named in the file header | The graphics layer implements against it |
| `dm/ios/tools/touchrectv1_patch.json` | 28 KB | A description of changes to the original controller interface layout, carrying widget names and values from that layout | The touch interface is derived from the original layout |
| `dm/data/casc_overlay/borrowed_paths.tsv` | 2 KB | The path names of 6 original textures | Our new panels reuse these original textures |
| `dm/verify/logicself/*.json`, `dm/verify/touchcases/tests/fixture/`, `dm/verify/shotgate_pages/` | about 200 KB in total | Verification baselines: per-tick state checksums recorded while running the original, a few bytes of input commands, and comparison specs for interface screenshots | They prove the rewritten program behaves like the original |
| `main+0x…` addresses in the source (about 800 of them, across 59 files) and their descriptions | — | The locations of functions and data structures inside the game's main program, with descriptions of what they do | The host and the touch layer hook into the game at these locations; valid for version 3.3.94012 only |
| The rewrite rules in `dm/tools/patch_nso.py` | — | Each rule reads "original instruction word → replacement instruction word". The original word is there to confirm the version and the location | This is what the rewriter does |
| A small number of the game's internal names in the source | — | For example the internal names of about 70 monsters the Kill Board recognizes, and the keys of items and interface widgets that feature code looks up | Features find their targets by these names |

### About the 7 rewrites for offline play

Seven of the rewrite rules in `dm/tools/patch_nso.py` exist to let the game run without a network connection (the original checks online at startup). **These 7 are taken from an existing community offline fix** (LFS Mod Offline Fix); they are not our original work. We have no reliable record of where it was first published or who wrote it — if you know, please open an issue and we will add the credit. This project is fully offline and has nothing to do with online play.
