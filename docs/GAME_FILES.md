# Game files

The web version runs the original game, so it needs the files of **Carmageddon II: Carpocalypse Now** for Windows. They are not included in this repository or in the release — use your own copy.

Suitable sources:

* the **original CD**, installed with the **full** install option, or
* a **digital release** (e.g. GOG) — the installed game folder.

## What to take

Take the **installed game folder** — the one that contains `CARMA2_HW.EXE`. The packer (`tools/make_data.py`) takes the whole folder; file and folder names may be in any letter case.

```
Carmageddon 2/
├── CARMA2_HW.EXE        required — the game itself (3Dfx / hardware build)
├── 3DFX_WIN.BDD         required — Glide renderer driver used by CARMA2_HW.EXE
├── DATA/                required — all game data, with all subfolders:
│   ├── 32X20X8/  64X48X8/          interface graphics
│   ├── ACTORS/  MODELS/  MATERIAL/  PIXELMAP/  SHADETAB/
│   ├── CARS/  DRONES/  NONCARS/  PEDS/
│   ├── COMMON/  INTRFACE/
│   ├── RACES/                      tracks (the biggest folder, ~120 MB)
│   ├── SOUND/
│   ├── REG/
│   └── OPTIONS.TXT, KEYMAP_*.TXT, KEYNAMES.TXT, NETDEFLT.TXT, ...
└── (other files)        optional — CARMA2.EXE, CARMA2_SW.EXE, *.BDD, SMACKW32.DLL,
                         IFORCE2.DLL, ReadMe.txt ... are packed too but not needed
```

The finished package is about **200 MB** (1 778 files).

### CD installs

If you installed from CD with a *minimal* / *typical* option, part of `DATA` stays on the CD (most of all `DATA/RACES`). In that case copy the `DATA` folder from the CD over the installed `DATA` folder (do not overwrite `OPTIONS.TXT` if you want to keep your settings), so the folder contains the complete data.

### Executable version

The runtime patches a few addresses inside the executable (widescreen cameras, HUD layout), so it is tested with one specific `CARMA2_HW.EXE`:

| File | Size | SHA-256 |
|---|---|---|
| `CARMA2_HW.EXE` | 2 674 688 bytes | `698c2b11865578b6ea2a77324fbc1840080841a8ae4041e9ca14ca5ff027bd6c` |
| `3DFX_WIN.BDD` | 69 632 bytes | `8ee8d5c1c359de9695b74f504faa6b522c7e01cdf2d88cea1c1cc389ec4e9301` |

`make_data.py` checks the checksum and stops if the executable is different. If you have a different executable (another patch level or a modified exe), you can try it anyway with `--force` — it may crash or show graphics glitches.

## Packing

```sh
python3 tools/make_data.py "/path/to/Carmageddon 2" site
```

On Windows: drag the game folder onto `tools\make_data.cmd`, or run

```bat
tools\make_data.cmd "C:\GOG Games\Carmageddon 2"
```

This writes into `site/`:

| File | What |
|---|---|
| `carma2data.js` | Loader with the file list |
| `carma2web.data.000` … `.009` | The game files in 20 MB parts (fit the file size limits of most hosts) |

Re-run the packer whenever you change the game files. Nothing else needs to be rebuilt.

## Music (optional)

The in-game music is CD audio — tracks **2 … 8** of the game CD (track 1 is data). Put them into `site/music/` named:

```
Track02.ogg  Track03.ogg  Track04.ogg  Track05.ogg  Track06.ogg  Track07.ogg  Track08.ogg
```

`.ogg`, `.mp3` and `.wav` work (OGG/MP3 at ~100–160 kbit/s is recommended — the browser downloads a track only when it starts playing).

* **From the CD:** rip tracks 2–8 with any CD ripper (e.g. Exact Audio Copy, fre:ac, `abcde`) to OGG or MP3 and name them as above.
* **From a digital release:** if your copy contains the soundtrack as files, copy them and rename them to `TrackNN` with the same numbers as on the CD.
* With the packer: `--music <folder>` copies all `TrackNN.ogg|mp3|wav` files from a folder into `site/music/`.

Without these files the game runs without music.

## Saved games and settings

Saved games, options and the network player name are stored in the player's browser (IndexedDB / localStorage), not on the server. Clearing the site data in the browser resets them.
