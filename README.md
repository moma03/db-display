# db-display — renderer / driver split

A Deutsche Bahn style departure board on a HUB75 LED matrix, split into two
processes that share nothing but a block of memory.

```
┌──────────────────┐   RGB888 frames    ┌──────────────────┐
│   led_renderer   │  ───────────────▶  │    led_driver    │  ──▶ HUB75 GPIO
│                  │   /dev/shm/        │                  │
│ • DB fetch       │   led-frames       │ • gamma          │
│ • layout.json    │   (seqlock,        │ • temporal dither│
│ • drawing        │    3 slots)        │ • SwapOnVSync    │
│                  │                    │                  │
│ may block, may   │                    │ SCHED_FIFO, no   │
│ stutter, may die │                    │ alloc, no syscall│
└──────────────────┘                    └──────────────────┘
   unprivileged                            root → drops to daemon
```

The renderer may block on the network, stall on a database query, or crash and
restart. None of it reaches the panel: the driver never waits on the renderer,
and when no new frame arrives it keeps showing the last good one.

---

## Contents

- [Build](#build)
- [Run](#run)
- [Architecture](#architecture)
- [Shared memory format](#shared-memory-format)
- [Where gamma lives](#where-gamma-lives)
- [Temporal dithering](#temporal-dithering)
- [Config schema](#config-schema)
- [Running under the hub](#running-under-the-hub)
- [Host setup](#host-setup)
- [Measured behaviour](#measured-behaviour)
- [Debugging](#debugging)

---

## Build

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j3
```

Targets:

| target | what | needs |
|---|---|---|
| `led_driver` | panel driver | Linux + Pi (librgbmatrix) |
| `led_renderer` | departure board | portable; libpq for live data |
| `led_shmgen` | synthetic frame generator | portable |
| `led_test_common` | host tests, no panel | portable |

The renderer links only `graphics.cc` and `bdf-font.cc` out of
rpi-rgb-led-matrix — both are GPIO-free — so it builds and runs on a laptop.
Only `led_driver` requires a Pi.

```bash
./build/led_test_common          # shm, gamma and dither maths
```

---

## Run

The **driver creates** the shared-memory segment, because it is the process
that knows the authoritative panel geometry. The renderer attaches to it.

```bash
sudo ./build/led_driver --config config/driver.json          # terminal 1
./build/led_renderer --config config/renderer.json           # terminal 2
```

Each runs independently:

```bash
# Driver alone, with synthetic frames — no renderer, no database, no network
sudo ./build/led_driver --config config/driver.json &
./build/led_shmgen --pattern bars
./build/led_shmgen --pattern gradient    # dither A/B
./build/led_shmgen --pattern refresh     # tearing / dropped frames
./build/led_shmgen --pattern shadow      # black-clamp / low-end
./build/led_shmgen --pattern checker     # pixel mapping / chain order

# Renderer alone, no panel and no shm — writes a PPM
./build/led_renderer --layout config/layout.json --once /tmp/frame.ppm

# Renderer with sample data instead of a database
./build/led_renderer --config config/renderer.json --demo
```

`--led-*` flags are passed through to rpi-rgb-led-matrix and override
`driver.json`, so the LED_Panel_Controller hub keeps working unchanged:

```bash
sudo ./build/led_driver --led-pwm-bits=5 --led-brightness=60 --dump-config
```

The driver reads the **effective** values back after flag parsing, so an
overridden `--led-pwm-bits` also retargets the gamma LUT and the dither.

---

## Architecture

Two interfaces carry the whole extensibility story.

**`IRenderBackend`** ([src/renderer/render_backend.h](src/renderer/render_backend.h))
— what a Qt/QML or headless-browser backend implements later. The renderer's
main loop hands over a raw RGB888 buffer and knows nothing else.

```cpp
virtual void RenderFrame(uint8_t *rgb, int64_t t_ns) = 0;
```

**`IOutputBackend`** ([src/driver/output_backend.h](src/driver/output_backend.h))
— where an Art-Net / Colorlight receiver-card backend drops in. The hot loop
never sees a `FrameCanvas` or a GPIO pin.

```cpp
virtual void PresentAndWait(const uint8_t *rgb888, uint32_t dither_phase) = 0;
virtual void WaitIdle() = 0;
```

`PresentAndWait` blocking is what paces the entire driver — there is no sleep
anywhere in the loop. For the GPIO backend that is `SwapOnVSync`; for Art-Net
it would be a send plus a frame-interval wait. That is why replacing the
backend cannot change the loop.

The hot loop itself ([src/driver/hot_loop.cpp](src/driver/hot_loop.cpp)) is
~45 lines: one acquire load, an optional quantise pass, one swap. No
allocation, no logging, no syscalls.

---

## Shared memory format

`/dev/shm/led-frames`, created by the driver:

```
┌────────────────┬──────────┬──────────┬──────────┐
│ FrameShmHeader │  slot 0  │  slot 1  │  slot 2  │
│   256 bytes    │  w*h*3   │  w*h*3   │  w*h*3   │   (each 64-byte aligned)
└────────────────┴──────────┴──────────┴──────────┘
```

Header ([src/common/frame_shm.h](src/common/frame_shm.h)), fields grouped onto
separate cache lines by writer so the driver's stores never invalidate the line
the renderer publishes from:

| offset | field | writer |
|---|---|---|
| 0 | `magic` (`0x4C454421`), `version`, `width`, `height`, `bytes_per_pixel`, `slot_count`, `slot_stride`, `header_bytes` | fixed at creation |
| 64 | `std::atomic<uint64_t> sequence`, `std::atomic<uint32_t> ready_slot` | renderer |
| 128 | `std::atomic<uint32_t> reading_slot` | driver |
| 192 | `render_ns`, `frames_published`, `frames_presented` | diagnostics |

**Publication is a seqlock.** The renderer writes a complete frame into a slot,
then release-stores an incremented `sequence`. The driver acquire-loads
`sequence` and only then reads `ready_slot`; the release/acquire pair
guarantees every pixel write is visible. No mutexes, no blocking, no priority
inversion.

**Why three slots and not two.** The driver quantises straight out of the
mapped slot without copying it, so a slot stays live for a whole driver pass.
With two slots the renderer's next frame necessarily targets the slot the
driver is mid-pass on, and you get a tear. The alternative — a strict seqlock
where the driver memcpys 98 KB and re-validates — costs more than a third slot.
`reading_slot` makes it airtight rather than merely improbable, and
`slot_count` lives in the header so neither binary bakes it in.

The driver **validates magic, version and dimensions at startup** and refuses
to run on a mismatch rather than rendering garbage.

---

## Where gamma lives

**Gamma is applied exactly once, in the driver**, by
[src/common/gamma.h](src/common/gamma.h). The library's own CIE1931 correction
is switched **off**:

```cpp
matrix_->set_luminance_correct(false);   // rgbmatrix_backend.cpp
matrix_->SetBrightness(100);
```

Turning it back on applies the curve twice and crushes the midtones.

**Why own it rather than let the library do it.** With luminance correction on,
the library maps 8-bit → an 11-bit CIE value and displays only the top
`pwm_bits`, discarding the rest. To command a specific output level we would
have to search a reverse table for a byte whose CIE image lands in the right
bucket — carrying a duplicate of the library's formula that silently desyncs if
it ever changes, and re-deriving it whenever brightness changes.

With it off, `DirectMapColor` reduces to `c << 3`, so

```
level L  ⟺  byte = L << (8 - pwm_bits)
```

is exact and onto for `pwm_bits ≤ 8`. Every level is addressable, the
arithmetic is integer-exact, and it is testable on a laptop with no panel
(`led_test_common` checks the round trip for every `pwm_bits` in 1..8).

Above 8 bits the 8-bit `Canvas` API physically cannot address every level; the
driver refuses to start rather than quietly producing wrong colours.

**Brightness is folded into our LUT**, and the library is pinned at 100. That
applies brightness *before* quantisation rather than after — the correct order,
and the reason a dimmed panel keeps its shadow detail. `--led-brightness` from
the hub is honoured this way.

The LUT scales into **level space** (`max_level << shift`), not the full 11-bit
range. Scaling to 2047 puts full white at 15.99 against a ceiling of 15, so the
brightest pixels could never average correctly.

---

## Temporal dithering

At `pwm_bits = 4` the panel has 16 levels per channel, and the library
truncates rather than rounds. **Input bytes 0..78 all render as pure black** —
31% of the input range, gone. At 5 bits it is 0..53.

The dither spends that residual over time instead of dropping it. For an 11-bit
target `t`, `q = t >> shift` is the level below and `r = t & mask` the
residual; showing `q+1` on a fraction `r/2^shift` of frames time-averages to
exactly `t`. Which frame steps up is decided by a 4×4 Bayer matrix indexed by
`(x, y, phase)` — spatial as well as temporal, so neighbouring pixels of the
same colour step on different frames. A naive per-pixel threshold makes the
whole display breathe instead.

Verified by `led_test_common`: averaged over the phase cycle and a 4×4 block,
the output reproduces the target to within **0.031 of a level** (bound 0.125),
and **71 of the 76** otherwise-black inputs produce light.

`dither.phases` must divide 16, or some pixels stay permanently biased; the
driver validates this.

**The alternation rate is the driver's swap rate, not the panel refresh rate.**
rpi-rgb-led-matrix rescans one `FrameCanvas` until `SwapOnVSync` replaces it, so
content can only change once per swap. `swap_every_n_vsync` is passed to
`SwapOnVSync` as its framerate fraction, so a reduced dither rate stays exactly
vsync-locked instead of drifting against a sleep of our own.

Toggle with `dither.enabled` for a clean A/B. With it off and no new frame, the
driver touches nothing at all.

---

## Config schema

Three files. **JSON, not YAML**: the repo already vendors nlohmann/json and
parses JSON in several places, so YAML would mean adding yaml-cpp to a Pi build
for no functional gain. `//` comments are enabled, which removes the usual
reason to prefer YAML for hand-edited config.

### `config/driver.json` — startup only, restart to change

| key | default | notes |
|---|---|---|
| `shm.path` / `shm.slot_count` | `/led-frames` / 3 | |
| `matrix.rows/cols/chain_length/parallel` | 64 / 128 / 2 / 2 | → 256×128 |
| `matrix.pwm_bits` | 4 | 1..8 |
| `matrix.pwm_dither_bits` | 1 | **not** the temporal dither — see below |
| `matrix.gpio_slowdown` | 2 | |
| `matrix.limit_refresh_rate_hz` | 190 | |
| `matrix.show_refresh_rate` | false | leave off in production |
| `gamma.mode` | `cie1931` | `cie1931` \| `power` \| `linear` |
| `gamma.brightness_percent` | 100 | applied before quantisation |
| `dither.enabled` / `phases` / `swap_every_n_vsync` | true / 4 / 1 | |
| `realtime.sched_fifo_priority` | 80 | below the library's 99 |
| `realtime.cpu_affinity` | -1 | **do not set to 3** — see below |
| `diagnostics.stats_interval_s` | 0 | 0 = off |

> **`matrix.pwm_dither_bits` is not the temporal dithering**, and it is not a
> no-op at low bit depth. It shifts the library's bitplane timing table down by
> that many stops (`if (b >= dither_bits) timing *= 2`), halving every plane's
> duration and roughly doubling the achievable refresh rate per step, at the
> cost of light output. At `pwm_bits=4` it is the difference between 125.7 Hz
> and the 190 Hz cap. Range 0..2.

> **`realtime.cpu_affinity` defaults to -1 (unpinned) on purpose.** The library
> already pins its own GPIO thread to core 3 at `SCHED_FIFO` 99. Pinning the
> driver's quantise pass to the same core puts it in competition with the
> bit-banger. With `isolcpus=3` set, unpinned already means cores 0–2. The
> driver warns if you set it to 3.

### `config/renderer.json` — startup only

Non-secret settings: `shm.path`, `layout` (relative paths resolve against the
config file), `target_fps`, `wait_for_shm_s`, and `database.eva` /
`database.refresh_seconds`.

**No credentials.** If this file contains `database.host`, `.port`, `.dbname`,
`.user` or `.password`, the renderer refuses to start and tells you which
`DB_PG_*` key to move it to. A secret is only as protected as the file it sits
in, and nothing treats a `config.json` as sensitive.

### `config/.env` — credentials

```sh
cp config/.env.example config/.env
chmod 600 config/.env
```

```sh
DB_PG_HOST=10.0.10.132
DB_PG_PORT=5442
DB_PG_DBNAME=timetable_stops_db
DB_PG_USER=...
DB_PG_PASSWORD="quote it if it contains a space or #"
```

Key names match `timetable-fetcher/.env`, so one file can serve both. A `.env`
is gitignored by default almost everywhere, backup and deploy tooling treats it
as sensitive, and anyone who sees one knows to protect it — none of which is
true of a random config file.

Searched in order, unless `env_file` (in renderer.json) or `--env` names one:

1. `$LED_ENV_FILE`
2. `<dir of renderer.json>/.env`
3. `<dir of renderer.json>/../.env` (repo root)

A missing file is not an error — the values may come from the real environment
instead, e.g. systemd's `EnvironmentFile=`. **Real environment variables win
over the file**, so `DB_PG_HOST=... ./led_renderer` works for a one-off. An
`env_file` that is named explicitly but absent *is* an error.

Values may be bare, `'single'` (literal) or `"double"` quoted (with `\n`, `\t`,
`\"`, `\\`). An unquoted value ends at a ` #` comment, so quote anything
containing `#`. The password is never logged.

### `config/layout.json` — **hot reloadable**

Save the file and the renderer picks it up within ~300 ms. No restart, no
recompile. A file that fails to parse or validate is **rejected** and the
running layout continues, with the reason logged — a half-finished edit cannot
blank the panel. Reloads are applied at a frame boundary so they cannot tear,
and an unchanged file is ignored by fingerprint so saving without editing does
not reset every scroll position.

```jsonc
{
  "canvas":      { "width": 256, "height": 128, "background": "#000078" },
  "fonts":       { "big": "…/clR6x12.bdf", "small": "…/5x8.bdf" },
  "line_colors": { "S": "#FFFF00", "ICE": "#FFFFFF", "default": "#FF3C3C" },
  "widgets": [ … ]
}
```

Widget types: `station_name`, `clock`, `separator`, `hero`, `departure_list`,
`ticker`. Every widget has `type`, `id`, `rect: [x,y,w,h]`, `font`, `color`;
each type adds its own keys (`format`/`align` for the clock, `colors`/`scroll`/
`scrollbar` for the list, and so on). All rects are validated against the
canvas at load time.

**Vertical auto-flow.** Two widgets are variable height, so a layout that
reserved worst-case space for both would waste most of the panel most of the
time:

| | minimum | maximum |
|---|---|---|
| hero | 18 px (solo departure) | 46 px (two-part wing group + reason line) |
| ticker | 0 px (no disruption message) | 10 px |

So the widget between them can have neither a fixed top nor a fixed bottom:

- **`flow_after: "<id>"`** — this widget's **top** follows that widget's actual
  rendered bottom.
- **`flow_before: "<id>"`** — this widget's **bottom** follows that widget's
  top, or reaches the bottom of the canvas when that widget is not currently
  shown, so its space is reclaimed rather than left blank.

With both set, `rect` supplies only x and width. That is the honest reading:
the vertical extent genuinely depends on the data, and the layout file says so
instead of the drawing code deciding silently. Remove either flow to pin that
edge to the rect.

The renderer logs the resolved geometry whenever it changes, so a layout can be
checked without counting pixels:

```
layout resolved -- hero y18..64 (46 px, 2 parts), list y67..118 (51 px), ticker shown
layout resolved -- hero y18..36 (18 px, 1 part), list y39..128 (89 px), ticker hidden (space reclaimed)
```

`--demo-minimal` renders the layout's minimum content (one solo departure, no
ticker), which is the case that proves optional widgets give their space back:

```bash
./build/led_renderer --layout config/layout.json --demo-minimal --once /tmp/min.ppm
```

**What is deliberately not in the layout file:** wing grouping, replacement-bus
detection, cancellation styling and the delay-code table. Those are behaviour,
not layout, and live in C++.

---

## Running under the hub

`start.sh` makes the driver/renderer pair look like a single program, so
LED_Panel_Controller can launch it like any other. Add to the hub's
`config/config.yaml`:

```yaml
  trainboard:
    name: Train Departure Board
    command: /home/pi/led-refactor/start.sh --eva {subprogram} {matrix_options}
    subprograms:
      "8005708": { name: Steinheim (Westf) }
      "8000297": { name: Paderborn Hbf }
      "8002549": { name: Hamburg Hbf }
```

`{matrix_options}` renders to rpi-rgb-led-matrix's own `--led-*` flags
(`MatrixConfig.as_cli_args`), which `start.sh` routes to the driver; `--eva`
goes to the renderer. Everything else it does not recognise is logged and
ignored rather than being fatal — a display program that refuses to start is
worse than one that ignores an argument.

Three properties of the hub shape the script
(`led_controller/process_manager.py`), and each is a way to get this wrong:

- **`Popen(shlex.split(cmd), cwd=_REPO_ROOT)`** — no shell, and the cwd is the
  *controller's* repo root, not this one. Every path in `start.sh` resolves
  against its own location.
- **`terminate()` signals the direct child only**, then `SIGKILL`s the direct
  child after `process_terminate_timeout` (5 s). Nothing kills the process
  group. So `start.sh` forwards `SIGTERM` to both children itself and reaps
  them in ~3.3 s worst case. Without that, the `SIGKILL` lands on the wrapper
  and leaves the driver orphaned holding `/dev/mem` and the GPIO — which breaks
  whatever the hub launches next.
- **`poll()` watches the exit code**, so `start.sh` exits as soon as either
  child does, propagating that child's code.

Verified against stubs, without touching the panel:

| case | result |
|---|---|
| hub-style launch, foreign cwd | args routed correctly, both children start |
| `SIGTERM` to the wrapper only | both children reaped, exit 0, shm cleaned |
| child ignores `SIGTERM` | `SIGKILL` fallback, done in 3.3 s (hub allows 5) |
| driver crashes at runtime | wrapper exits 3, renderer cleaned up |
| driver fails at startup | exits 1 in 150 ms, no hang |

> **Set `show_refresh_rate: false` in the hub's `matrix:` block.** It is
> currently `true`, so `{matrix_options}` renders `--led-show-refresh` and the
> library `printf()`s from its RT thread on every refresh. The driver warns
> when it sees this.

For live departures, `config/renderer.json` and `config/.env` must both exist;
without the former `start.sh` falls back to `renderer.json.example` in demo
mode rather than failing to start, and without DB credentials the renderer
warns and keeps going.

---

## Host setup

Everything below was verified on the target (Pi 3B+, `MoRaspiLEDPanel`).

### Kernel command line

`/boot/firmware/cmdline.txt`, single line:

```
isolcpus=3
```

Already set on the current host. It keeps the *scheduler* off core 3; it does
**not** move interrupts (see below).

### CPU governor

```bash
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance | sudo tee "$c"
done
```

The library sets this for core 3 itself, but only core 3. All four are already
`performance` on the current host.

### `rtprio` / memlock for `SCHED_FIFO`

The driver applies `SCHED_FIFO` and `mlockall` to itself **before** the matrix
library drops privileges to `daemon` — both need capabilities that would
otherwise be gone by then. The systemd unit grants them:

```ini
AmbientCapabilities=CAP_SYS_NICE CAP_IPC_LOCK
LimitRTPRIO=99
LimitMEMLOCK=infinity
```

Running by hand instead of via systemd, either use `sudo` or add to
`/etc/security/limits.conf`:

```
@daemon  -  rtprio  99
@daemon  -  memlock unlimited
```

The driver logs `realtime sched=… affinity=… mlock=…` at startup. If it says
`FAILED` it keeps running at normal priority rather than refusing to start.

### Wi-Fi power management

Not applicable on the current host — it is on **eth0**, and `wlan0` is not up.
If you move to Wi-Fi, power save adds latency spikes:

```bash
sudo iw dev wlan0 set power_save off
```

### What you cannot fix on a Pi 3B+

Two things worth knowing before chasing the remaining flicker:

1. **Interrupts cannot be moved off the isolated core.** Every IRQ on this
   board reports affinity `0-3`, and every write to `smp_affinity` returns
   `EIO` — the BCM2836 ARMCTRL irqchip has no `set_affinity` callback, so
   `irqaffinity=0-2` at boot will not work either. Measured on core 3:
   ~5,600 timer IRQ/s and ~2,010 USB FIQ/s, about 7,600/s total. A FIQ
   preempts `SCHED_FIFO` 99 unconditionally, and on a 3B+ **Ethernet is behind
   USB**, so network traffic *is* the interrupt load.

2. **`nohz_full` is not compiled in.** `CONFIG_NO_HZ_FULL is not set` in the
   stock Raspberry Pi OS kernel, so `nohz_full=3` needs a custom kernel build.

Both are structurally fixed by the planned Pi 5 move: a GIC-400 with real
per-IRQ affinity, native PCIe Ethernet (no USB FIQ), and RP1-PIO offloading the
pixel clocking so the CPU stops bit-banging.

### Thermal

`vcgencmd get_throttled` returned `0x80008` on the current host — soft
temperature limit **active**, at 60.7 °C. That matters more than it looks,
because the library's delay loop is cycle-counted for 1.4 GHz with no clock
reading:

```c
static void busy_wait_nanos_rpi_3(long nanos) {
  for (uint32_t i = (nanos - 15) * 100 / 73; i != 0; --i) { asm(""); }
}
```

When the soft limit drops the ARM clock to 1.2 GHz, every bit-plane on-time
stretches ~17% — a brightness and colour shift that oscillates as the
temperature crosses 60 °C. **A heatsink or fan is the cheapest fix available
for the remaining flicker.**

### Do not run with `show_refresh_rate` in production

The library `printf()`s the refresh rate from inside its `SCHED_FIFO` 99 GPIO
thread on **every refresh** ([led-matrix.cc:231]). That is the stdio lock ~190
times a second on the one thread that must not be delayed, plus a periodic
socket write to journald. The driver warns if you enable it.

---

## Measured behaviour

Target: Pi 3B+, 256×128, `pwm_bits=4`, `limit_refresh=190`, moving gradient at
30 fps, 12 s per configuration.

| configuration | median | p1 | min | driver CPU |
|---|---|---|---|---|
| `pwm_dither_bits=0`, temporal off | 125.5 Hz | 120.3 | 92.0 | 83% |
| `pwm_dither_bits=1`, temporal off | **190.0 Hz** | 190.0 | 133.9 | 110% |
| `pwm_dither_bits=1`, temporal **on** | **190.0 Hz** | **190.0** | 126.3 | 148% |
| `pwm_dither_bits=2`, temporal on | 190.0 Hz | 190.0 | 122.3 | 149% |

Temporal dithering is **free in refresh terms** — 190.0 Hz at the 1st
percentile, matching the pre-split binary — and costs about 38 percentage
points of CPU on a non-isolated core.

### Dither alternation rate

With the panel at 190 Hz the driver reports **~95 swaps/s**, not 190. The
quantise pass costs roughly 6 ms for 32768 pixels (~180 ns/pixel), which is
longer than one 5.26 ms refresh period, so `SwapOnVSync` lands on every second
vsync. The panel refresh itself is unaffected — that stayed at 190.0 Hz — but
the dither alternates at ~95 Hz and a full 4-phase cycle completes in ~42 ms.

That is comfortably above flicker fusion and was not visible in testing, and it
is the reason the dither is spatial as well as temporal: with a 4×4 Bayer
matrix the *instantaneous* spatial average is already close to correct, so the
temporal component only refines it. A purely temporal alternation at 95 Hz
would be far more marginal.

If a slow shimmer ever does show on large flat areas, `dither.phases: 2` halves
the cycle time. The deeper fix is the per-pixel `SetPixel` path, which does a
read-modify-write across four bit planes at a 2 KB stride; precomputing one
`FrameCanvas` per dither phase and cycling them would make the hot path a
pointer swap, at the cost of one canvas (~720 KB) per phase.

The `min` column is the interrupt stalls described above, and is not something
this codebase can fix; the pre-split binary showed the same excursions (worst
case 50.5 Hz over ~1.4 M samples).

### Picking `limit_refresh_rate_hz` (tuned against the real panel)

Brightness is `(on-time per frame) x (frames per second)`, and the limiter only
stops flicker while it is actually *engaging* — i.e. while it is padding most
frames to a constant period. If the cap is set near or above what the hardware
can sustain, nothing gets padded, the frame period wanders, and that reads as
**brightness pulsing** rather than as a rate change.

The rule that came out of tuning this panel (2 chains x 2 panels, `slowdown=2`),
judged visually at each step:

> **cap ~= 0.79 x the UNCAPPED median**

| pwm / dither | uncapped | cap | ratio | result |
|---|---|---|---|---|
| 5 / 1 | 216 Hz | **170** | 0.79 | rock solid |
| 4 / 1 | 239 Hz | 190 | 0.79 | stable (old default) |
| 6 / 2 | 295 Hz | 220 | 0.75 | stable, but dim |
| 6 / 1 | 197 Hz | 180 | 0.91 | **pulses** |
| 6 / 2 | 295 Hz | 280 | 0.95 | **flickers** |

Measure the uncapped median with `--led-limit-refresh=0` and `--led-show-refresh`.

At the chosen setting the rate is **median 170.0, p1 170.0, min 169.9** — the
headroom is wide enough that even interrupt-stalled frames still fit inside the
period, so the limiter absorbs all the jitter. Every other configuration tested
had a minimum between 56 and 89 Hz.

Two findings worth keeping:

- **`slowdown=1` ghosts on this hardware.** It raises the ceiling (347 Hz at
  pwm 6 / dither 2) but produces visible ghosting around high-contrast text.
  `slowdown=2` is the floor for these chains.
- **Temporal dithering costs ~16% of the refresh rate**, which is enough to push
  a config from the stable side of the ratio to the pulsing side. At
  `pwm_bits=5` the black clamp is 53 rather than 78, so it has little left to
  recover and is off by default. It earns its cost at `pwm_bits=4`, where it
  also fixes the scrollbar-track artefact described in the config.

### Old vs new, measured

Hannover Hbf (EVA 8000152), 50 s per run, identical matrix flags
(`pwm_bits=4 pwm_dither_bits=1 gpio_slowdown=2 limit=190`). "Stutters" counts
refreshes below 180 Hz out of ~10,640.

| | median | p1 | stutters | CPU |
|---|---|---|---|---|
| old, single process | 190.0 Hz | 190.0 | 11–13 | **106%** |
| new, dither off | 190.0 Hz | 190.0 | 5–22 | **117%** |
| new, dither on | 190.0 Hz | 190.0 | 21 | **157%** |

**There is no performance gain.** Refresh rate is identical — all three sit on
the 190 Hz limiter at both median and 1st percentile. The stutter counts
overlap across repetitions (the new system measured 22, then 7, then 5), so
that column is noise: those excursions come from interrupt load on the isolated
core, which neither architecture touches (see "What you cannot fix on a Pi 3B+"
above).

The split *costs* about 11 percentage points of CPU, and temporal dithering a
further ~40. That is the honest trade, and it is the expected one: the driver
now reads a frame from shared memory and quantises it, where the old code drew
straight into the canvas. It is strictly more work.

Worth noting separately: the **renderer costs only ~6% of one core** — the
entire dashboard, including the database fetch, layout and all the scrolling
text, for a large station. Essentially all the CPU is the driver's per-pixel
quantise pass.

So what the rewrite buys is not speed:

- **Temporal dithering** — picture quality, not throughput. It recovers input
  bytes 0..78, which 4-bit PWM otherwise crushes to black: 31% of the range.
  This is the only change visible on the panel, and it is what the extra 40
  points of CPU pay for. Turn it off with `dither.enabled` and the split's
  overhead is the remaining ~11 points.
- **Crash isolation** — not theoretical. A renderer segfault (there was one,
  on the first live frame before any data had arrived) blanks the old display;
  here the driver keeps showing the last good frame and the renderer restarts
  under it.
- **The seams** — declarative hot-reloadable layout, a swappable render
  backend, and the Art-Net output interface.

If none of those matter to a given deployment, the old single process was
cheaper and just as smooth.

### A note on what the split did and did not fix

The split was motivated by flicker attributed to the renderer stalling the GPIO
loop. Measurement on the running system did not support that: the GPIO thread
was *already* `SCHED_FIFO` 99 pinned to isolated core 3, with the render thread
confined to cores 0–2, so the two never competed for CPU. The remaining flicker
is interrupt load and thermal clock changes, both listed above.

The split is still worth having — swappable render backend, declarative
hot-reloadable layout, the Art-Net seam, and the dithering that recovers 31% of
the input range — but it should be understood as enabling future work, not as
the flicker fix.

---

## Debugging

```bash
# What config is actually in force, after --led-* overrides
sudo ./build/led_driver --dump-config --led-pwm-bits=5

# Swap rate and renderer frame rate, on a low-priority thread
#   set diagnostics.stats_interval_s in driver.json, then:
journalctl -u led-driver -f

# Is the segment there, and is anyone publishing?
ls -l /dev/shm/led-frames
```

| symptom | likely cause |
|---|---|
| driver exits with `bad magic` / dimension mismatch | stale segment from a different geometry — `rm /dev/shm/led-frames` and restart the driver |
| renderer: `could not attach after 30s` | driver is not running; it creates the segment |
| panel frozen on one frame | renderer died — check `journalctl -u led-renderer`; this is the intended failure mode |
| whole panel breathes / pulses | `dither.phases` not dividing 16, or a per-pixel threshold instead of the Bayer matrix |
| banding in gradients | `dither.enabled` false, or gamma applied twice (check `set_luminance_correct` is still off) |
| refresh ~125 Hz instead of 190 | `matrix.pwm_dither_bits` is 0 |
| everything below mid-grey is black | expected at `pwm_bits=4` with dithering off — that is the 0..78 clamp |
| `realtime sched=FAILED` at startup | missing `CAP_SYS_NICE` / `LimitRTPRIO` in the unit |
| layout edits do nothing | check the renderer log — a rejected reload says why and keeps the old layout |
| swap rate is half the refresh rate | expected: the quantise pass is longer than one refresh period, see [Dither alternation rate](#dither-alternation-rate) |

To validate the driver with no renderer, no database and no network at all, use
`led_shmgen` (see [Run](#run)). If the panel looks wrong with `shmgen` running,
the problem is the driver or the wiring, not the dashboard.
