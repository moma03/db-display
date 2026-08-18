#!/usr/bin/env bash
#
# start.sh -- run the driver/renderer pair as ONE program, so the
# LED_Panel_Controller hub can launch it like any other display program.
#
#   ./start.sh --eva 8002549 --led-rows=64 --led-cols=128 ... [--demo]
#
# The hub runs exactly one command per program and knows nothing about how it
# is implemented, so the split has to look like a single process from outside.
# Three things about how the hub launches and stops programs shape this script
# (led_controller/process_manager.py):
#
#   1. subprocess.Popen(shlex.split(command), cwd=_REPO_ROOT)
#      No shell. Arguments must be plain tokens -- no pipes, no globs, no
#      variable expansion. And cwd is the CONTROLLER's repo root, not ours, so
#      every path here resolves against $HERE rather than $PWD.
#
#   2. terminate() sends SIGTERM to the DIRECT CHILD ONLY, waits
#      process_terminate_timeout (5 s), then SIGKILL -- again only the direct
#      child. Nothing kills the process group. So this script MUST forward the
#      signal to both children and reap them well inside that window. If it
#      does not, the SIGKILL lands on the wrapper alone and leaves the driver
#      orphaned still holding /dev/mem and the GPIO, which breaks whatever the
#      hub launches next.
#
#   3. poll() watches the exit code, so this script must exit when the program
#      should be considered finished -- including when either child dies on its
#      own.
#
# Argument routing: --led-* go to the driver (they are rpi-rgb-led-matrix's own
# flags, and the hub renders {matrix_options} into exactly these), everything
# else that we recognise goes to the renderer.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER="$HERE/build/led_driver"
RENDERER="$HERE/build/led_renderer"
DRIVER_CFG="$HERE/config/driver.json"
RENDER_CFG="$HERE/config/renderer.json"
LAYOUT="$HERE/config/layout.json"

# Piping this into head/less must not kill it before the trap can clean up.
trap '' PIPE

# --- argument routing ------------------------------------------------------
LED_ARGS=()
REN_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --led-*)          LED_ARGS+=("$1"); shift ;;
    --eva)            REN_ARGS+=(--eva "${2:-}"); shift 2 ;;
    --eva=*)          REN_ARGS+=(--eva "${1#*=}"); shift ;;
    --demo)           REN_ARGS+=(--demo); shift ;;
    --layout)         LAYOUT="${2:-$LAYOUT}"; shift 2 ;;
    --driver-config)  DRIVER_CFG="${2:-$DRIVER_CFG}"; shift 2 ;;
    --renderer-config) RENDER_CFG="${2:-$RENDER_CFG}"; shift 2 ;;
    -h|--help)
      sed -n '3,8p' "$0" | sed 's|^# \{0,1\}||'
      exit 0 ;;
    *)
      # Unknown flags are reported but not fatal: the hub may grow new
      # placeholders, and a display program that refuses to start is worse
      # than one that ignores an argument it does not understand.
      echo "start.sh: ignoring unrecognised argument '$1'" >&2
      shift ;;
  esac
done

# Live data needs renderer.json; fall back to demo rather than not starting.
if [ ! -f "$RENDER_CFG" ]; then
  if [ -f "$HERE/config/renderer.json.example" ]; then
    echo "start.sh: $RENDER_CFG missing -- falling back to renderer.json.example" >&2
    echo "start.sh: copy it and fill in the database credentials for live data" >&2
    RENDER_CFG="$HERE/config/renderer.json.example"
    case " ${REN_ARGS[*]:-} " in *" --demo "*) ;; *) REN_ARGS+=(--demo) ;; esac
  else
    echo "start.sh: no renderer config found" >&2
    exit 1
  fi
fi

for f in "$DRIVER" "$RENDERER" "$DRIVER_CFG" "$LAYOUT"; do
  [ -e "$f" ] || { echo "start.sh: missing $f (did you build?)" >&2; exit 1; }
done

# --- shutdown --------------------------------------------------------------
DRIVER_PID=""
RENDERER_PID=""
SHUTTING_DOWN=0

shutdown() {
  [ "$SHUTTING_DOWN" -eq 1 ] && return
  SHUTTING_DOWN=1
  trap '' TERM INT HUP

  # Renderer first: it is the one that can be interrupted harmlessly. Taking
  # the driver down first would leave the panel showing a frozen frame for as
  # long as the renderer took to notice.
  [ -n "$RENDERER_PID" ] && kill -TERM "$RENDERER_PID" 2>/dev/null
  [ -n "$DRIVER_PID" ]   && kill -TERM "$DRIVER_PID"   2>/dev/null

  # The hub gives us 5 s before it SIGKILLs this wrapper, so be done in 3.
  for _ in $(seq 30); do
    local alive=0
    [ -n "$RENDERER_PID" ] && kill -0 "$RENDERER_PID" 2>/dev/null && alive=1
    [ -n "$DRIVER_PID" ]   && kill -0 "$DRIVER_PID"   2>/dev/null && alive=1
    [ "$alive" -eq 0 ] && break
    sleep 0.1
  done

  # Anything still up gets killed here rather than being orphaned when the hub
  # SIGKILLs us. An orphaned driver keeps the GPIO and blocks the next program.
  [ -n "$RENDERER_PID" ] && kill -KILL "$RENDERER_PID" 2>/dev/null
  [ -n "$DRIVER_PID" ]   && kill -KILL "$DRIVER_PID"   2>/dev/null

  rm -f /dev/shm/led-frames
  exit "${EXIT_CODE:-0}"
}
trap shutdown TERM INT HUP

# --- driver ----------------------------------------------------------------
"$DRIVER" --config "$DRIVER_CFG" ${LED_ARGS[@]+"${LED_ARGS[@]}"} &
DRIVER_PID=$!

# The driver creates the shared-memory segment; the renderer needs it to exist.
# Wait for it rather than relying on a fixed sleep.
for _ in $(seq 100); do
  [ -e /dev/shm/led-frames ] && break
  kill -0 "$DRIVER_PID" 2>/dev/null || break
  sleep 0.1
done

if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
  wait "$DRIVER_PID"
  EXIT_CODE=$?
  echo "start.sh: driver exited during startup (code $EXIT_CODE)" >&2
  rm -f /dev/shm/led-frames
  exit "${EXIT_CODE:-1}"
fi

# --- renderer --------------------------------------------------------------
"$RENDERER" --config "$RENDER_CFG" --layout "$LAYOUT" \
            ${REN_ARGS[@]+"${REN_ARGS[@]}"} &
RENDERER_PID=$!

# --- supervise -------------------------------------------------------------
# Exit as soon as EITHER child does, so the hub sees the program end instead of
# a wrapper babysitting a half-dead pair. `wait -n` also returns when a trapped
# signal arrives, which is how SIGTERM from the hub gets here.
wait -n
EXIT_CODE=$?
shutdown
