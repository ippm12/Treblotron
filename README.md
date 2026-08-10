# DartLens
An application for playing darts with automatic vision scoring

## Building

Configurations are named presets. Pick one:

```bash
cmake --preset app-sim          && cmake --build build                  # dev: clickable dartboard
cmake --preset app-local-jetson && cmake --build build-app-local-jetson # standalone Jetson
cmake --preset app-network      && cmake --build build-app-network      # Raspberry Pi client
cmake --preset server-directml  && cmake --build build-server-directml  # inference server (GPU)
cmake --preset server-cpu       && cmake --build build-server-cpu       # inference server (CPU)
cmake --preset server-tensorrt  && cmake --build build-server-tensorrt  # inference server (CUDA)
```

Two independent options drive everything:

| option | values | meaning |
|---|---|---|
| `DARTLENS_VISION_SOURCE` | `sim` `hailo` `local` `network` | where the game gets dart events |
| `DARTLENS_INFER_BACKEND` | `none` `tensorrt` `directml` `cpu` | what executes a forward pass |

The old `DARTLENS_USE_SIM` / `_HAILO` / `_TENSORRT` booleans still work and map
onto the pair above. They only apply when `DARTLENS_VISION_SOURCE` is still at
its default, and are cleared from the cache once honoured — otherwise a build
directory that once had `DARTLENS_USE_HAILO=ON` would keep demanding HailoRT
forever, including after the accelerator has been removed from the machine.

### Building on the Raspberry Pi

The Pi runs the game and the cameras and streams frames out; it needs no
accelerator and no models.

```bash
git submodule update --init --recursive     # first time only
cmake --preset app-network
cmake --build build-app-network -j4
```

Then point it at the inference machine and run:

```bash
DARTLENS_SERVER=192.168.1.50:9876 ./build-app-network/bin/Dart_Lens
```

If your CMake predates presets (3.21+), the same thing longhand:

```bash
cmake -S . -B build-app-network -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DDARTLENS_BUILD_APP=ON -DDARTLENS_BUILD_SERVER=OFF \
      -DDARTLENS_VISION_SOURCE=network -DDARTLENS_INFER_BACKEND=none
cmake --build build-app-network -j4
```

**If a build complains about HailoRT after you've removed the hat**, it is
reading a stale `DARTLENS_USE_HAILO=ON` out of an old build directory. The
current CMake detects that and clears it, but the quickest cure is a fresh
build directory — `rm -rf build && cmake --preset app-network`.

Expect the first build to take a while: OpenCV is a submodule and gets compiled
from source. The Pi client builds a trimmed set
(`core,imgproc,features2d,flann,calib3d,imgcodecs,videoio` — no `dnn`, since
nothing on the Pi runs a model).

### Inference backends

| backend | runs on | notes |
|---|---|---|
| `tensorrt` | NVIDIA CUDA (Jetson) | Compiles an engine on first run and caches it. |
| `directml` | any DirectX 12 GPU, Windows | AMD, Intel and NVIDIA alike. Fetches ONNX Runtime at configure time. |
| `cpu` | anything | OpenCV DNN. No extra dependency; the fallback that always works. |
| `none` | — | No models compiled in (sim and Pi-client builds). |

Measured end-to-end, one full detection cycle (segmentation + autoregressive
detector + both hand-filter stages) on a Ryzen 7 9800X3D / Radeon RX 7900 XT:

| backend | per cycle | |
|---|---|---|
| `directml` | **19 ms** | ~52 Hz |
| `cpu` | 248 ms | ~4 Hz |

Both produce the same detections — the decode diagnostics agree exactly, and
raw model outputs agree to ~3e-4.

**Verifying you're actually on the GPU.** A backend that can't reach its
accelerator falls back to CPU rather than failing, so the startup line names
where the work really landed:

```
selftest: running on DirectML (ONNX Runtime)          <- GPU
selftest: running on ONNX Runtime (CPU — DirectML unavailable)   <- fell back
```

The game shows the same on the loading screen's final frame.

### Where DirectML comes from

`server-directml` downloads ONNX Runtime (~12 MB) into `external_libs/onnxruntime`
on first configure. **`DirectML.dll` is not fetched by default** — Windows ships
one in `System32` and the loader finds it. The startup log names the copy in use:

```
ort backend: DirectML loaded from C:\WINDOWS\SYSTEM32\DirectML.dll
```

Using the Windows copy is free of any redistribution obligation, but its version
tracks the OS. On Windows 11 and recent Windows 10 it is new enough; on an older
Windows 10 it may be too old for the pinned ONNX Runtime, in which case the
backend reports `ONNX Runtime (CPU — DirectML unavailable)` and runs ~13x slower.
If that happens, ship the redistributable instead:

```bash
cmake --preset server-directml -DDARTLENS_FETCH_DIRECTML=ON
```

That fetches the `Microsoft.AI.DirectML` package (a ~200 MB one-time download for
one 18 MB DLL — hence not the default) into `external_libs/directml`, licence and
third-party notices included. `-DDARTLENS_DIRECTML_DLL=<path>` points at a copy
you already have.

## Remote inference

The detection models are too large for a Raspberry Pi. `DARTLENS_VISION_SOURCE=network`
splits the system in two: the Pi keeps the cameras and the UI, a bigger machine
runs the models, and dart events come back over TCP.

Both halves link the same `detect` module, so detection is not duplicated — a
dart scored remotely is scored by exactly the code that would have scored it
locally. The client also ships its wire calibration on connect, so the server
fits the homography with the same routine rather than keeping its own copy.

```bash
# on the inference machine
./Dart_Lens_Server --port 9876

# on the Pi
DARTLENS_SERVER=192.168.1.50:9876 ./Dart_Lens
```

The client streams continuously and the server always scores the newest set it
has. A reader thread drains the socket into a **one-slot, newest-wins mailbox**
and re-credits the client the instant a set comes off the wire, while inference
runs on the other thread and takes whatever is freshest when it becomes free.
Anything overwritten before being scored is simply dropped.

Two properties fall out of that:

```
cycle     = max(transfer, server work)    not  transfer + server work
staleness = one transfer                  not  one server cycle
```

Throughput tracks `max()` — measured against a client with a simulated transfer
delay:

| simulated transfer | measured | if it serialised |
|---|---|---|
| 0 ms | 27.3 Hz | 27.8 Hz |
| 20 ms | 28.1 Hz | 17.9 Hz |
| 40 ms | 24.0 Hz | 13.2 Hz |
| 60 ms | 16.1 Hz | 10.4 Hz |

The staleness bound is the reason for the reader thread rather than just
crediting early. With a single credit queued and a single thread, a cycle that
runs long leaves the client unable to refresh the set already waiting, so what
gets scored next was captured when the slow cycle *started* — staleness bounded
by the worst cycle time. Draining continuously means a long cycle just discards
more stale sets and picks up a fresh one at the end.

The three cameras are JPEG-decoded concurrently (`cv::parallel_for_`), and
decoded straight to RGB via `IMREAD_COLOR_RGB` rather than to BGR plus a
full-frame conversion.

### Where a cycle goes

The server logs a per-stage breakdown every 30 cycles, because "is it fast
enough" is a question about where the time actually goes:

```
cycle 30: 27.3 ms total = 3.7 decode + 23.6 detect+reply (36.6 Hz), 0 newer set(s) skipped
```

Measured on a Radeon RX 7900 XT against a client paced at the camera rate:

| stage | time |
|---|---|
| JPEG decode, 3 cameras concurrent | 3.7 ms |
| segmentation + AR detector + hand filter + reply | 23.6 ms |
| **total** | **27.3 ms → 36.6 Hz** |

That clears the 30 Hz the cameras run at, with headroom — the sensors are the
cap, not the server. Send-to-detection latency is 30 ms median, 32 ms max.

Concurrent decode is worth ~6 ms/cycle; before it, a cycle was ~36 ms and
send-to-detection 46 ms median / 71 ms max.

A client that streams flat-out rather than at the camera rate costs the server
about 5 ms/cycle in receiving and discarding sets it will never score, which is
why the client only sends when a camera has actually produced a new frame.

Neither end ever blocks while holding work to do: the client polls rather than
parking in a read, so detections are never left sitting in the socket while it
waits for the next capture.

**The Pi runs no models at all** — segmentation, the autoregressive detector and
both hand-detection stages all execute server-side. The client's only jobs are
capture, forward, and act on the dart events that come back.

### Frames on the wire

Transport is plain TCP, one connection, carrying both control messages and
video: a 12-byte little-endian header (`magic`/`type`/`flags`/`payloadBytes`)
in front of each typed message. No RTP, no codec, no container.

Video is discrete JPEG stills rather than a compressed stream. The UVC cameras
already deliver MJPEG, so the client **forwards the sensor's own bytes
untouched** — no decode, no re-encode, and no second generation of compression
loss between the sensor and the model. Nothing is decoded on the Pi unless a
preview screen (calibration, vision_debug) actually asks for pixels, and that
decode happens on the screen's own thread.

If a driver ignores `CAP_PROP_CONVERT_RGB` and insists on decoding, the client
falls back to encoding at quality 75 — the same behaviour it had before, just
costing CPU. The log says which path is live:

```
Camera 0 passthrough active — forwarding the sensor's own JPEG, no decode/re-encode
```

Measured payload for a 1280x720 dartboard scene with grain: ~55 KB/frame at q75,
~91 KB at q85, so roughly 165-270 KB per 3-camera cycle. At 30 Hz that is
41-67 Mbit/s — unremarkable for Wi-Fi 5/6 and trivial over Ethernet. Credit-based
flow control means a slower link simply yields fewer cycles per second rather
than a growing backlog of stale frames.

### One client at a time

The server serves a single board. That isn't a shortcut — the detector holds one
stateful pipeline (confirmed darts, the conditioning mask, the Detecting/Removing
machine), so two clients sharing it would corrupt each other's board.

A client arriving mid-session is told so explicitly and keeps retrying:

```
turned away 192.168.1.9:41022 — busy with 192.168.1.8:53114
```

A client that stops responding — power cut, Wi-Fi drop — never sends a TCP FIN,
so the server drops the session after `--read-timeout` (20 s by default) rather
than waiting on a socket that will never speak again.

Board state survives a reconnect. For `--grace` milliseconds (60 s by default)
after a session ends, the same machine coming back resumes with its darts and
turn state intact, so a brief network blip mid-turn doesn't cost the player
throws they already made:

```
client 192.168.1.8:53170 connected (resuming, board state kept)
192.168.1.8 did not return within 60 s — clearing the board
```

Identity is the client's IP, since the port changes on every reconnect. Two
different boards behind one NAT would look like the same machine to this — not a
concern for a single-board setup, but worth knowing before adding a second.

Useful server flags:

- `--selftest` — load every model, run the full pipeline on synthetic input,
  report ms/cycle. The quickest way to check a re-exported ONNX still matches
  the code's expectations.
- `--replay <dir>` — score saved `{uuid}_camN.png` triples (what the game's
  capture hotkey writes to `./captures`) with no client and no cameras.
- `--no-hand-filter`, `--confirm <n>`, `--clear-confirm <n>` — tuning knobs for
  a backend slower than the ~30 Hz the defaults assume.
- `--read-timeout <ms>`, `--grace <ms>` — how long a silent client is tolerated,
  and how long its board state is held for a reconnect.

With `server-directml` on a Radeon RX 7900 XT the server sustains ~52 Hz, well
clear of the 30 Hz camera rate, so the credit window stays at one frame and
detection latency is bounded by capture and JPEG transport rather than
inference.

## Layout

| module | role |
|---|---|
| `detect/` | models, inference backends, the shared detection pipeline |
| `net/` | socket shim + client/server wire protocol |
| `server/` | headless inference server |
| `vision/` | cameras and the vision-source implementations |
| `frame/` `game_lib/` `games/` `players/` `dart/` `debug/` | the game itself |

## Credits

### Libraries

- **[SDL 3](https://www.libsdl.org/)** — Windowing, rendering, and input. zlib license.
- **[SDL_ttf](https://github.com/libsdl-org/SDL_ttf)** — TrueType font rendering. zlib license.
- **[SDL_image](https://github.com/libsdl-org/SDL_image)** — Image loading (PNG, JPG). zlib license.
- **[spdlog](https://github.com/gabime/spdlog)** — Fast C++ logging. MIT license.
- **[Flecs](https://github.com/SanderMertens/flecs)** — Entity Component System. MIT license.
- **[OpenCV](https://opencv.org/)** — Computer vision. Apache License 2.0.

Fetched at configure time, only for `DARTLENS_INFER_BACKEND=directml`:

- **[ONNX Runtime](https://github.com/microsoft/onnxruntime)** — Model execution.
  MIT license. © Microsoft Corporation. The notice is kept alongside the binaries
  as `external_libs/onnxruntime/LICENSE-onnxruntime.txt`.
- **[DirectML](https://aka.ms/DirectML)** — GPU execution provider. **Proprietary**
  — Microsoft Software License Terms, not open source. By default we do not
  redistribute it at all: the build uses the copy Windows already ships, which is
  covered by the user's Windows licence. With `DARTLENS_FETCH_DIRECTML=ON` the
  redistributable is fetched instead; its terms permit shipping it inside an
  application you develop for Windows/Xbox, and the licence plus third-party
  notices are kept next to the DLL in `external_libs/directml/`.

### Assets

- **[Roboto](https://fonts.google.com/specimen/Roboto)** — Font by Google. Apache License 2.0.
- **[Input Prompts](https://kenney.nl/assets/input-prompts)** (v1.4.1) — Keyboard, mouse, and controller icons by [Kenney](https://www.kenney.nl). CC0 1.0.
