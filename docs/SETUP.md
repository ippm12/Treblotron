# Setting up Dartmatic

Three ways to run it. Pick one — you do not need the others.

| | what you need | best for |
|---|---|---|
| **[Single PC](#single-pc)** | a Windows PC with a DX12 GPU, three USB cameras | simplest setup; start here |
| **[Pi + server](#raspberry-pi--inference-server)** | a Raspberry Pi at the board, a Windows PC anywhere on the network | keeping a small quiet box by the dartboard |
| **[Demo](#demo)** | nothing | trying the games without a dartboard |

Whichever you choose, **[calibration](#calibrating-the-board)** is the part that
actually matters, and it is the same for all of them.

---

## Where your data lives

Settings, calibration and captures are stored per user, not next to the program:

- **Windows** — `%LOCALAPPDATA%\Dartmatic\`
- **Linux** — `~/.local/share/dartmatic/`

```
config/wire_calibration.txt   the 120 points you clicked; the valuable one
config/vision.txt             detection thresholds
config/server.txt             inference server address
logs/                         one file per subsystem
captures/                     saved frames plus their transforms
```

Uninstalling leaves this directory alone. If you are moving to a new machine,
copying `config/` across saves you redoing the calibration — provided the
cameras have not moved.

To keep everything next to the program instead — a USB stick, or several
independent setups on one machine — put an empty file named `portable.txt`
beside the executable.

---

## Single PC

Cameras and inference on the same Windows machine. No network, no second
device.

1. Run `Dartmatic-0.1.0-AMD64.exe`.
2. Plug in three USB cameras and point them at the board.
3. Launch Dartmatic. It will tell you how many cameras it found.
4. Calibrate — see below.

Inference runs on your GPU through DirectML, which works on AMD, Intel and
NVIDIA alike. To confirm you are actually on the GPU, check the log:

```
DartDetector ready on DirectML (ONNX Runtime)                  <- GPU
DartDetector ready on ONNX Runtime (CPU - DirectML unavailable) <- fell back
```

A CPU fallback still works but runs roughly thirteen times slower. The usual
cause is a Windows 10 build whose bundled DirectML predates what this release
needs; installing current graphics drivers is the fix.

---

## Raspberry Pi + inference server

The Pi runs the game and the cameras. A Windows PC runs the models. Use this
when you want a small, quiet machine at the board.

### On the Windows PC

1. Run `Dartmatic-Server-0.1.0-AMD64.exe`.
2. Start **Dartmatic Server** from the Start Menu. It listens on port 9876.
3. Note the PC's LAN address — `ipconfig`, the IPv4 address on your Wi-Fi or
   Ethernet adapter.

The installer adds a Windows Firewall rule so the Pi can reach it. If you use
a different firewall, allow inbound TCP 9876.

> If the PC is on a VPN such as Tailscale, use the **LAN** address, not the VPN
> one, unless the Pi is on the same VPN. Otherwise the Pi cannot route to it.

### On the Pi

Download `dartmatic_0.1.0_arm64.deb` from the
[Releases page](https://github.com/ippm12/Dartmatic/releases) and install it:

```bash
sudo apt install ./dartmatic_0.1.0_arm64.deb
```

`apt` pulls in what it needs — SDL3, OpenCV and the rest are recorded as
dependencies of the package. Nothing is compiled, and you do not need the
source.

Then launch **Dartmatic** from the Games menu, or from a terminal:

```bash
dartmatic
```

The client carries no models at all: they live on the other end of the socket,
which is why the package is small and why the Pi needs no GPU.

<details>
<summary>Building the package yourself</summary>

Only needed if you are cutting a release, or running an architecture the
release does not cover. It has to be built **on** an arm64 machine — there is
no cross-compilation setup in this repo.

```bash
sudo apt install -y build-essential cmake ninja-build git dpkg-dev \
                    libsdl3-dev libgl1-mesa-dev

git clone --recurse-submodules https://github.com/ippm12/Dartmatic.git dartmatic
cd dartmatic
cmake --preset app-network
cmake --build build-app-network -j4

cd build-app-network && cpack
```

`dpkg-dev` is needed only for that last step: `cpack` calls `dpkg-shlibdeps` to
work out the dependency list, rather than hard-coding one that would go stale.
The first build takes a while, since OpenCV is a submodule compiled from
source.

You get both `dartmatic_0.1.0_arm64.deb` and a `.tar.gz` of the same tree. To
run straight out of the build directory without installing anything:

```bash
./build-app-network/bin/Dartmatic
```

</details>

Set the server address from inside the app: **Vision** card on the main menu,
or **F1** at any time. It is saved, so you only do this once. `DARTMATIC_SERVER=host:port`
still works as a starting value for scripted installs.

### Checking the link

A coloured dot sits in the corner of every screen:

| | meaning |
|---|---|
| green | connected and keeping up |
| amber | connected but slow, or nothing scored for two seconds |
| red | no server configured, unreachable, or it went away |

When it is red a banner names the problem. **F1** opens Vision settings over
whatever is on screen, including mid-leg, so a dropped server can be fixed
without abandoning a game.

---

## Demo

`Dartmatic-Demo-0.1.0-AMD64.zip` — unzip and run. A clickable dartboard stands
in for the cameras, so every game is playable with no hardware. Nothing to
install and nothing to calibrate.

---

## Calibrating the board

This is the one step that cannot be skipped, and the one that determines how
accurate scoring is. Dartmatic cannot score anything until it knows where the
board is in each camera's view.

You click **40 points per camera**: the 20 outer-triple-ring wire
intersections, then the 20 outer-double-ring ones, going clockwise from the
wire between 20 and 1. Three cameras, so 120 clicks. It takes a few minutes and
only has to be done once — as long as the cameras do not move afterwards.

1. Main menu → **Calibration**.
2. Pick a camera. The live preview shows what it sees.
3. Click the intersection the panel on the right asks for, then the next, until
   all 40 are placed.
4. Repeat for the other two cameras.
5. Press **S** on the camera list to save.

### Which point is it asking for?

A *wire intersection* is where one of the 20 radial wires crosses a ring. Each
ring has two such crossings per wire — an inner one and an outer one — and
Dartmatic always wants the **outer** one, the corner furthest from the bull:

- **Outer triple** — where the wire leaves the triple ring on the side facing
  the double ring, not the side facing the bull.
- **Outer double** — where the wire meets the outside of the double ring, at
  the very edge of the scoring area.

Wires are named by the two beds they divide, so "the wire between 13 and 6" is
the single wire with 13 on one side and 6 on the other. The guide panel names
the ring and the two beds, and marks the exact corner on a diagram of the
board, with the wire in question highlighted and the points you have already
placed shown in green.

**Left click** places a point, **right click** undoes the last one, and **C**
clears the camera and starts it over.

Tips:

- **Zoom in.** A point placed two pixels off is two pixels of error in every
  score afterwards.
- **Get all three cameras seeing the whole board.** A camera that cannot see
  part of the board contributes nothing for darts landing there.
- **If you bump a camera, recalibrate that one.** Its old points now describe a
  view it no longer has, which is worse than having no calibration.

---

## Tuning detection

Defaults are reasonable; adjust only if you see a specific problem. **F1**, or
the Vision card.

| setting | default | raise it if |
|---|---|---|
| Detections to confirm | 3 | phantom darts appear |
| Hold before counting | 300 ms | one throw is counted twice |
| Board clear delay | 1000 ms | turns end before you have finished throwing |
| Hand detect delay | 100 ms | reaching for darts is not noticed |
| Save every dart | off | you are collecting training data |

On a Pi + server setup these live on the Pi and are sent to the server, so
adjust them where you are standing.

---

## When something is wrong

**Darts are not registering.** Check the calibration first — the Vision Debug
screen shows what the model sees. Then check the link indicator if you are on a
Pi.

**One throw scores twice.** Raise *Hold before counting*. A dart still in
flight can briefly look like it landed.

**Turns end early.** Raise *Board clear delay*.

**Scores land in the wrong segment.** Recalibrate the camera that sees that
part of the board best.

**Nothing starts on Windows.** Check `%LOCALAPPDATA%\Dartmatic\logs\`. The
first line of `dartmatic_0_Main.log` names the version and the data directory.

**Server says "busy".** It serves one board at a time. Another client is
connected, or a previous session has not timed out yet — it clears within
twenty seconds.
