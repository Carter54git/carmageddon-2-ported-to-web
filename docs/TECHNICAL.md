# Technical notes

Runs the original `carma2_HW.exe` in the browser. The exe's code is executed by
an x86 interpreter plus an x86→WebAssembly JIT; the operating system and drivers
it talks to are reimplemented in C.

| file | what |
|------|------|
| `x86cpu.c/h` | i386 + x87 interpreter, lazy flags, single-step for the JIT |
| `jit.c` | x86 → wasm translator (regions of basic blocks, inlined direct calls, x87 stack cached in locals, region chaining) |
| `emu_core.c` | guest heap, trap table for imported APIs, guest calls, PE loader, DOS→host paths |
| `win32.c` | KERNEL32/USER32/GDI32/ADVAPI32/WINMM subset, smackw32/IFORCE2 stubs |
| `dx.c` | DirectInput (keyboard), DirectSound (mixed in C, `emu_audio_mix`) |
| `glide.c` | Glide 2.x for `3dfx_win.bdd`: software rasterizer (native) or WebGL2 (browser) |
| `web/` | browser host (`main.c`), WebGL/JIT/IDBFS glue, `build.sh` |
| `native/` | headless native host for testing (`build.sh`) |

Build for the browser (emscripten 3.1.50):

    OUT=dist EMCC=emcc sh src/web/build.sh      # or simply ./build.sh

Output `carma2hy.js` + `carma2hy.wasm`; the page loads the game data from the
`carma2data.js` / `carma2web.data.NNN` package made by `tools/make_data.py` (files under `/carma2`).
URL flags: `?nojit` (interpreter only), `?noraster` (skip drawing, for profiling).

Page: `web/index.html` (copy next to the build output). More URL flags: `?wide=0` (4:3),
`?scale=N`, `?msaa=N`, `?touch=1|0` (force touch controls on/off), `?nojitcache`,
`?name=Player` (default network name), `?room=NAME` (network room), `?relay=ws://host:port/net`,
`?stats=0`, `?netlog`, `?mcilog`, `?gldbg`.

Features of the browser host:

* **Widescreen** (`glide.c`, `web/main.c`): the GL target is `vw` x 480 Glide pixels with `vw`
  following the window shape. Race frames stretch the 3D scene over the full width while the race
  cameras' aspect is widened to match (Hor+, `widen_cameras`: gCamera_list 0x74a950,
  gRearview_camera 0x758e60, br_camera::aspect +0x10). HUD pieces (2D, drawn after the 3D scene
  with z = 0) keep their size and stick to the left/right edge or the centre (`hud_anchor`).
  Menus (frames with a big LFB image or no 3D) are 4:3 in the centre.
* **Touch controls** in `index.html`, shown only on touch screens; a tap on the picture is a
  mouse click (`emu_js_pointer`).
* **Gamepad** (standard mapping) polled every frame.
* **JIT fast start**: entry addresses of compiled regions are kept in localStorage and compiled
  ahead on the next start, 6 ms per frame (`jit_prewarm_add/step`).
* **CD music** from `music/TrackNN.ogg|mp3|wav` (MCI emulation in `win32.c`).
* **Network games**: the game's WSOCK32/IPX datagrams (`win32.c`) are carried over a WebSocket
  to a relay (`/net?room=..&node=..`, frames `[node 6 bytes][payload]`, node ff..ff =
  broadcast). Relay implementations: `server/relay.js` (Node.js), `server/relay.py` (Python 3) and
  `server/serve.ps1` (Windows, no dependencies; also serves the files to the LAN).
