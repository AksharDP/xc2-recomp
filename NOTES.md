# XCX Recomp: feasibility notes

Static recompilation of *Xenoblade Chronicles X: Definitive Edition* (Switch) to Windows x86-64, reusing the
XCX Recomp pipeline (`..\..\totk-pc-recomp`). Everything under `build\` and `generated\` is derived from the game: keep it private.

## Inputs

| File | Title id | Notes |
|---|---|---|
| base `[v0].nsp` | `0100453019AA8000` | 14.4 GB |
| update `[v196608][UPD2.0.0].nsp` | `0100453019AA8800` | 117 MB; ships a full ExeFS, RomFS via BKTR |
| Exploration Support Package | `0100453019AA9001` | DLC, not needed for boot |

## What was done

1. ExeFS extracted with MacNativeGames' `extract_exefs.py`, retargeted to the XCX title ids
   (`tools\vendor\macnativegames\extract_exefs.py`) into `build\extracted\{v0,upd}\exefs`.
2. Modules surveyed. Same layout as XCX: `main`, `sdk`, `subsdk0` (`movie`), `rtld`.
   - NintendoSDK 19.3.2, NVN 19.3.2 (XCX 1.4.3 is SDK 20.5.22; 1.2.1 is older).
   - `main` (17.8 MB, text 0x1735050): Havok Physics 2023.2, NPLN online stack (gRPC/WebRTC/OpenSSL), 831 imports.
   - `sdk`, `subsdk0` and `rtld` are identical between base and update; only `main` changes.
3. Host boundary: 208 of the 212 `nn::*` symbols XCX's runtime replaces exist by name in XCX's modules (the other
   4 are prefixes in the grep, not real misses).
4. Lifted with XCX's `recomp\gen.py`, unchanged, no patch table:

   | Module | Functions | Jump tables | C |
   |---|---|---|---|
   | main | 102,644 | 2,005 | 423 MB |
   | subsdk0 | 14,152 | 334 | 61 MB |
   | sdk | 40,067 | 574 | 115 MB |

   0 unsupported instructions; 33 s.

## Reproduce

```powershell
uv venv --python 3.12 .venv
uv pip install --python .venv\Scripts\python.exe cryptography capstone lz4 numpy
.venv\Scripts\python -I tools\vendor\macnativegames\extract_exefs.py "<update.nsp>" --keys <prod.keys> --update --output build\extracted\upd\exefs
.venv\Scripts\python ..\..\totk-pc-recomp\recomp\gen.py --exefs build\extracted\upd\exefs --out generated\upd
```

## Road to first boot

- RomFS: extract base + BKTR-merge the update (XCX's patched hactool, in the XCX setup's `bin\`).
- Fork the XCX runtime and strip what is XCX-specific: version detection by `main` hash, patch page and
  resolution sites, `ov_zstd` entry points, RomFS index, title id and save paths.
- Build: compile `generated\upd` with the bundled llvm-mingw and link the runtime (`build_game.py --runtime-lib` path).
- Then iterate on whatever XCX calls that XCX never did: NPLN/online services (stub as offline), Havok threading,
  Monolith's renderer through the NVN front end, shader list (none exists yet; first-use translation only).
