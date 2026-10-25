# Carmageddon II Web

**Carmageddon II: Carpocalypse Now (1998) running in a web browser** — single player, network games, gamepad and touch controls, widescreen, CD music.

▶ **Play it now in your browser: <https://dos.zone/carmageddon-2>**

This repository contains the source code of the browser runtime and the tools to host the game yourself. **It does not contain any game files.** To run your own copy you need the original game (see [Game files](#game-files)).

---

## How it works

The port runs the **original, unmodified `CARMA2_HW.EXE`** (the 3Dfx/hardware build of the game) inside the browser:

* an **i386 + x87 CPU interpreter** executes the game code;
* an **x86 → WebAssembly JIT** compiles hot code regions on the fly, so the game runs at full speed;
* the parts of **Windows** the game uses (KERNEL32, USER32, GDI32, ADVAPI32, WINMM, WSOCK32), **DirectInput** and **DirectSound** are reimplemented in C;
* **3Dfx Glide 2.x** (used by `3DFX_WIN.BDD`) is reimplemented on top of **WebGL 2**.

Everything is compiled to WebAssembly with Emscripten. The whole runtime is about 300 KB of wasm.

## Features

* The complete game: career, all cars, tracks and missions, saved games (stored in the browser).
* **Network game** for up to 16 players through a tiny WebSocket relay (the game's IPX traffic is tunneled over it). Rooms let separate groups play on one server.
* **Widescreen** (Hor+): the 3D view is widened to the window shape, the HUD sticks to the screen edges, menus stay 4:3.
* Higher internal resolution and MSAA (`?scale=`, `?msaa=`).
* **Gamepad** support (standard mapping) and **on-screen touch controls** on phones and tablets.
* **CD music** from `music/TrackNN.ogg|mp3|wav`.
* JIT "fast start": compiled code regions are remembered and precompiled on the next launch.

## Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Steer / accelerate / brake | Arrow keys | Stick / D-pad, RT gas, LT brake |
| Handbrake | Space | X |
| Repair | Backspace | Y |
| Change view | C | LB |
| Recover car | Insert | RB |
| Map | Tab | |
| Move minimap | Numpad 8/2/4/6 | |
| Menu select / back | Enter / Esc | A / B, Start |
| Fullscreen | Double-click or Alt+Enter | |

On touch screens on-screen buttons appear automatically (play in landscape). A tap on the picture acts as a mouse click.

## URL parameters

| Parameter | Effect |
|---|---|
| `?wide=0` | 4:3 picture instead of widescreen |
| `?scale=1..4` | Internal resolution multiplier (default 2) |
| `?msaa=0..8` | Anti-aliasing samples (default 4) |
| `?touch=1` / `?touch=0` | Force touch controls on / off |
| `?room=NAME` | Network game room |
| `?name=NAME` | Default network player name |
| `?relay=wss://host/net` | Use another relay server |
| `?stats=1` | Show FPS / emulator statistics |
| `?nojit` | Interpreter only (debugging) |

---

## Quick start: host your own copy

1. Download `carma2web-<version>.zip` from the [Releases](../../releases) page and unpack it.
2. Pack your game files into the site folder (Python 3 required):

   ```sh
   python3 tools/make_data.py "/path/to/Carmageddon 2" site --music "/path/to/music"
   ```

   On Windows you can simply drag the game folder onto `tools\make_data.cmd`.
   See **[GAME_FILES.md](docs/GAME_FILES.md)** for exactly which files are needed.
3. Start a local server and open the game:

   * Windows: run `server\serve.cmd` (no installation needed) — it opens the game in your browser and also lets other PCs on your LAN join network games;
   * any OS with Node.js: `node server/relay.js --static site` and open <http://localhost:8080/>;
   * any OS with Python: `python3 server/relay.py 8080 site`.

   Opening `index.html` directly from disk (`file://`) does not work — it has to be served over HTTP(S).

To put it on a real web site, upload the contents of `site/` to any static host. Network play additionally needs the relay server. Everything is described in **[docs/HOSTING.md](docs/HOSTING.md)** and **[server/README.md](server/README.md)**.

## Game files

You need your own copy of **Carmageddon II: Carpocalypse Now** for Windows (original CD with a full install, or the GOG release). The packer takes the installed game folder — the one with `CARMA2_HW.EXE`, `3DFX_WIN.BDD` and the `DATA` folder — and produces `carma2data.js` + `carma2web.data.000 … .009` (about 200 MB).

Full list of required files, the tested executable checksum and music instructions: **[docs/GAME_FILES.md](docs/GAME_FILES.md)**.

## Building from source

Requirements: [Emscripten](https://emscripten.org/) (tested with 3.1.50), `sh`, Python 3.

```sh
./build.sh                                   # -> dist/site/carma2hy.js, carma2hy.wasm, index.html ...
python3 tools/make_data.py "/path/to/Carmageddon 2" dist/site
node server/relay.js --static dist/site      # http://localhost:8080/
```

`sh tools/make_release.sh v1.0.0` packs `dist/` into the release zip (without game data).

A headless native build for testing on Linux/macOS: `sh src/native/build.sh && ./emu "/path/to/Carmageddon 2"`.

## Repository layout

```
src/            the runtime (C)
  x86cpu.c/h      i386 + x87 interpreter
  jit.c           x86 -> WebAssembly JIT
  emu_core.c      guest memory, PE loader, API trap table, file path mapping
  emu_main.c      start-up: loads CARMA2_HW.EXE and runs it
  win32.c         Win32 API subset, MCI CD audio, WSOCK32/IPX networking
  dx.c            DirectInput and DirectSound
  glide.c         3Dfx Glide 2.x -> WebGL 2 (software rasterizer in native builds)
  web/            browser host: main.c, JS libraries, index.html, build.sh
  native/         headless native host for testing
server/         network relay (Node.js / Python / PowerShell), Docker, nginx, systemd
tools/          make_data.py (game data packer), make_release.sh
docs/           hosting guide, game files, technical notes
```

More details on the internals: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Credits

* **Stainless Software** and **SCi** — the creators of Carmageddon II.
* The [rec2](https://github.com/dethrace-labs/rec2) project (Carmageddon 2 reverse engineering) and [DethRace](https://github.com/dethrace-labs/dethrace) — research this port started from.
* [dos.zone](https://dos.zone/carmageddon-2) — for hosting the playable version.

## License

The source code in this repository is released under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).

Carmageddon II and all its assets are the property of their respective copyright holders. This project is not affiliated with or endorsed by them. No game data is distributed here — please use your own legally obtained copy of the game.
