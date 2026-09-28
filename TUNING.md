# Panel tuning — post-mortem, pipeline, and how to re-run it

## Post-mortem

**Goal.** Split the dashboard into a renderer and a driver so that fetching and
drawing could no longer stall the GPIO loop, which was believed to cause
visible flicker.

**The premise was wrong.** Measured on the running system, the GPIO thread was
*already* `SCHED_FIFO` 99 pinned to isolated core 3, with the render thread
confined to cores 0–2. There was no scheduling contention to remove. The real
causes of the residual flicker are:

- ~7,600 interrupts/second landing on the "isolated" core (≈5,600 timer,
  ≈2,010 USB FIQ). On a Pi 3B+ these **cannot** be moved: every write to
  `smp_affinity` returns `EIO` (the BCM2836 irqchip has no `set_affinity`), and
  `CONFIG_NO_HZ_FULL` is not compiled into the stock kernel.
- The soft thermal limit active at 60.7 °C, against a delay loop that is
  cycle-counted for 1.4 GHz with no clock reading — a downclock stretches every
  bit-plane by ~17%.

**Result: the split produced no performance gain.** Identical refresh (190.0 Hz
median and p1 either way), stutter counts within noise, and ~11 pp more CPU.
What it did buy: a hot-reloadable declarative layout, a swappable render
backend, the Art-Net seam, crash isolation, and temporal dithering.

**Four bugs found, three of them mine.**

| bug | why it hid |
|---|---|
| Driver `SIGABRT` (exit 134) under the hub | `mlockall(MCL_FUTURE)` charges every later allocation against `RLIMIT_MEMLOCK`. Interactive ssh gives 125 MB, systemd gives 8 MB. Every hand-run test passed. |
| Renderer `SIGSEGV` on first live frame | `BuildScrollers()` indexes `list[0]`; in live mode `SetData()` is not called until the first DB fetch returns. Demo mode seeds data first, so it never showed. |
| Layout stopped being dynamic | Making rects authoritative pinned both edges of the list; a short hero and a hidden ticker left ~38 px blank. Only the worst-case demo data was ever rendered. |
| Scrollbar track flickered | `#3C3C3C` = grey 60 sits below the `pwm_bits=4` black clamp of 78, so it could only be shown by the dither alternating level 0/1 — visible on a flat area. |

**Tuning result.** Six configurations judged on the real panel:

```
pwm 5 / dither 1   uncapped 216   cap 170   ratio 0.79   WORKING  <- chosen
pwm 4 / dither 1   uncapped 239   cap 190   ratio 0.79   working (previous)
pwm 6 / dither 2   uncapped 295   cap 220   ratio 0.75   working, borderline dim
pwm 6 / dither 2 @ slowdown=1     cap 260   ratio 0.75   ghosting on text
pwm 6 / dither 1   uncapped 197   cap 180   ratio 0.91   PULSING
pwm 6 / dither 2   uncapped 295   cap 280   ratio 0.95   FLICKERING
```

Chosen config measures **median 170.0, p1 170.0, min 169.9 Hz**. Every other
configuration had a minimum between 56 and 89 Hz. It doubles colour depth to 32
levels at ~92% of the previous brightness.

**The rule.** Brightness = `(on-time per frame) x (frames per second)`, and the
limiter only suppresses flicker *while it is actually engaging*. Set the cap
near the achievable rate and nothing gets padded, the period wanders, and it is
seen as **brightness pulsing**, not as a rate change.

> **cap ≈ 0.79 × the UNCAPPED median**

Two hardware facts no arithmetic would have given: `slowdown=1` ghosts around
high-contrast text on these chains (so `slowdown=2` is the floor), and temporal
dithering costs ~16% of the refresh rate — enough on its own to push a config
from the stable side of the ratio into pulsing.

---

## Pipeline

```
 PostgreSQL ──► led_renderer ──► /dev/shm/led-frames ──► led_driver ──► HUB75
                                   header + 3 slots
                                   seqlock, lock-free
```

1. **led_renderer** (unprivileged, ~6% CPU, 30 fps) fetches departures, lays
   them out per `config/layout.json`, and draws RGB888 into a shm slot. It may
   block, stutter or crash; none of that reaches the panel.
2. **Publish** is a release-store of an incremented sequence number. Three
   slots, so the driver can quantise straight out of the mapped slot without
   copying and without ever racing the writer.
3. **led_driver** (root → drops to `daemon`, `SCHED_FIFO`) acquire-loads the
   sequence, applies gamma, quantises to the panel's PWM depth (optionally with
   temporal dither), and `SwapOnVSync()`s. If no new frame has arrived it
   re-shows the last good one.
4. **start.sh** wraps both as a single process so the LED_Panel_Controller hub
   can launch it like any other program, forwarding `SIGTERM` to both children.

**Gamma is applied exactly once**, in the driver, with the library's own CIE
correction switched off (`set_luminance_correct(false)`). See `src/common/gamma.h`.

---

## Re-tuning prompt

Paste this into a new session whenever the panel layout, the adapter, or the Pi
changes. It is self-contained.

```text
Re-tune the LED panel PWM and refresh settings for this repo (db-display).
The hardware changed, so the values in config/driver.json are stale.

Read TUNING.md first — especially the 0.79 cap rule and the pipeline section.

Procedure:

1. SSH to the Pi (root@10.0.10.50). Confirm the current panel geometry: rows,
   cols, chain_length, parallel. Ask me if it is not obvious.

2. Take the panel safely:
   - systemctl stop led-panel-controller     (do this FIRST — stopping it
                                              de-energises the relay)
   - pinctrl set 0 op dl                     (relay is ACTIVE LOW: lo = PSU ON)
   The PSU dislikes rapid on/off, so bring it up ONCE and leave it on for the
   whole session.

3. Numerically pre-screen, so my eyes are only spent on viable configs. For
   each candidate run the driver with --led-limit-refresh=0 (uncapped) and
   --led-show-refresh, feed it with led_shmgen, and record the median and p1 of
   the reported Hz. Sweep pwm_bits 4..7 x pwm_dither_bits 1..2 at the lowest
   gpio_slowdown that does not ghost.

4. For each promising config, compute the candidate cap as 0.79 x uncapped
   median, and the relative brightness as
       (on-time per frame) x cap,   on-time = sum of the displayed bit-plane
                                    durations x double_rows
   Bit-plane durations follow framebuffer.cc: start at pwm_lsb_nanoseconds and
   double at each plane b where b >= pwm_dither_bits.

5. Then show me each candidate ON THE PANEL, one at a time, with LIVE data
   (led_renderer --eva 8000152 for Hannover Hbf, or 8002549 for Hamburg Hbf).
   Run each as a persistent systemd-run unit so it survives between your tool
   calls. After each one, ask me with a question box offering: working /
   flickering / not working / other. Tell me what to look for each time —
   flicker, brightness pulsing, ghosting around high-contrast text, and whether
   flat grey areas (the scrollbar track) are stable.

6. Watch for these specific failure modes:
   - brightness PULSING  -> cap is too close to the ceiling; lower it
   - ghosting on text    -> gpio_slowdown too low; raise it
   - flat grey flickers  -> that grey is below the black clamp for this
                            pwm_bits; either raise pwm_bits or brighten it in
                            layout.json

7. When I confirm a winner, write it to BOTH:
   - config/driver.json
   - the matrix: block in /home/pi/LED_Panel_Controller/config/config.yaml
   The hub renders {matrix_options} into --led-* flags that OVERRIDE
   driver.json, so changing only the first has no effect when launched from
   Home Assistant. Back up config.yaml before editing it.

8. Verify once through the real hub launch path with start.sh, then restore:
   - stop any test units, kill stray led-refactor/build processes, rm
     /dev/shm/led-frames
   - pinctrl set 0 op dh                     (PSU OFF)
   - systemctl start led-panel-controller
   Tell me to re-select the program in Home Assistant — the controller comes
   back idle and cannot be resumed from the shell.

9. Update the tables in TUNING.md and README.md with the new measurements, and
   commit.

Report honestly. If a config is worse than what we had, say so.
```
