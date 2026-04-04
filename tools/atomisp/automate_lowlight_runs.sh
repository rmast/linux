#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARSER="$SCRIPT_DIR/parse_mt9m114_atomisp_logs.py"
PLOTTER="$SCRIPT_DIR/plot_gain_vs_s3a.py"

OUT_DIR="lowlight_runs_$(date +%Y%m%d_%H%M%S)"
RUN_SPEC="tl_on:90,lamp_low:90"
MT_INTERVAL_MS=500
S3A_INTERVAL_MS=500
MATCH_DELTA_MS=400
MAKE_PLOT=1
WARMUP_SECONDS=0
REPEAT_COUNT=1
FPS_VALUES=""
VIDEO_DEV="/dev/video0"
PA_SUBDEV="/dev/v4l-subdev4"
AE_SUBDEV="/dev/v4l-subdev5"
AE_CTRL_NAME="auto_exposure"
PIXRATE=48000000
ALLOW_UNFEASIBLE_FPS=0
PREFLIGHT_ONLY=0
AE_TOGGLE_ON_FPS_SWITCH=1
AE_AUTO_VALUE=1
AE_PREARM_BEFORE_PROMPT=1
AE_PREARM_SETTLE_SECONDS=2
VBLANK_VERIFY_RETRIES=1
VBLANK_RETRY_SETTLE_SECONDS=1

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  -o, --out-dir DIR           Output directory (default: $OUT_DIR)
  -r, --runs SPEC             Comma-separated label:seconds list
                              (default: $RUN_SPEC)
  --mt-interval-ms N          mt9m114 stream status interval (default: $MT_INTERVAL_MS)
  --s3a-interval-ms N         atomisp s3a status interval (default: $S3A_INTERVAL_MS)
  --match-delta-ms N          Join threshold for parser (default: $MATCH_DELTA_MS)
  --warmup-seconds N          Optional warmup period before each capture (default: $WARMUP_SECONDS)
  --repeat N                  Repeat each run condition N times (default: $REPEAT_COUNT)
  --fps-values LIST           Optional comma-separated fixed FPS setpoints per run
                              Example: 30,15,8,4,2
  --video-dev DEV             Video device used for FPS set/get (default: $VIDEO_DEV)
  --pa-subdev DEV             Pixel-array subdev for VBLANK fallback (default: $PA_SUBDEV)
  --ae-subdev DEV             Sensor/AE subdev for auto exposure control (default: $AE_SUBDEV)
  --ae-ctrl-name NAME         AE control name on AE subdev (default: $AE_CTRL_NAME)
  --pixrate N                 Pixel rate used for VBLANK FPS math (default: $PIXRATE)
  --allow-unfeasible-fps      Do not skip FPS targets below theoretical minimum
  --no-ae-toggle              Do not force $AE_CTRL_NAME value switches around FPS changes
  --ae-auto-value N           Value written to exposure_auto when AE is enabled (default: $AE_AUTO_VALUE)
  --no-ae-prearm              Do not set AE=0 before the light-condition prompt
  --ae-prearm-settle-seconds N
                              Wait time after pre-arming AE before prompt (default: $AE_PREARM_SETTLE_SECONDS)
  --vblank-verify-retries N   Retry AE/VBLANK sequence N times if readback mismatches (default: $VBLANK_VERIFY_RETRIES)
  --vblank-retry-settle-seconds N
                              Wait time before VBLANK retry after mismatch (default: $VBLANK_RETRY_SETTLE_SECONDS)
  --preflight-only            Print feasible FPS range/matrix and exit
  --no-plot                   Skip plotting step
  -h, --help                  Show this help

Example:
  $0 -o runs_evening -r tl_on:120,lamp_low:120 --mt-interval-ms 400 --s3a-interval-ms 400
EOF
}

extract_kv_field() {
  local line="$1"
  local key="$2"

  awk -v key="$key" '{
    for (i = 1; i <= NF; i++) {
      n = split($i, a, "=")
      if (n == 2 && a[1] == key) {
        print a[2]
        exit
      }
    }
  }' <<< "$line"
}

get_recent_valid_status_line() {
  local line=""
  local fl=""
  local ll=""

  while IFS= read -r line; do
    fl="$(extract_kv_field "$line" frame_length_live)"
    ll="$(extract_kv_field "$line" line_length)"
    if [[ -n "$fl" && -n "$ll" && "$fl" -gt 0 && "$ll" -gt 0 ]]; then
      echo "$line"
      return 0
    fi
  done < <(sudo dmesg | tac | grep 'mt9m114 stream status:' | head -n 40)

  return 1
}

set_exposure_auto_mode() {
  local value="$1"
  local readback=""
  local mode_label=""

  if [[ "$value" == "0" ]]; then
    mode_label="Auto Mode"
  elif [[ "$value" == "1" ]]; then
    mode_label="Manual Mode"
  fi

  if ! command -v v4l2-ctl >/dev/null 2>&1; then
    echo "  warning: v4l2-ctl not found, cannot set $AE_CTRL_NAME=$value"
    return 1
  fi

  if ! v4l2-ctl --device "$AE_SUBDEV" --set-ctrl "${AE_CTRL_NAME}=$value" >/dev/null 2>&1; then
    echo "  warning: failed to set ${AE_CTRL_NAME}=$value on $AE_SUBDEV"
    return 1
  fi

  readback="$(v4l2-ctl --device "$AE_SUBDEV" --get-ctrl "$AE_CTRL_NAME" 2>/dev/null | awk -F': ' '{print $2}' | tr -d '\r')"
  if [[ -n "$readback" ]]; then
    if [[ -n "$mode_label" ]]; then
      echo "  ${AE_CTRL_NAME} set to $value ($mode_label), readback=$readback"
    else
      echo "  ${AE_CTRL_NAME} set to $value, readback=$readback"
    fi
  else
    if [[ -n "$mode_label" ]]; then
      echo "  ${AE_CTRL_NAME} set to $value ($mode_label)"
    else
      echo "  ${AE_CTRL_NAME} set to $value"
    fi
  fi

  return 0
}

apply_vblank_with_ae_sequence() {
  local target_vblank="$1"
  local max_retries="$2"
  local attempt=0
  local vb_readback=""

  while (( attempt <= max_retries )); do
    if (( attempt == 0 )); then
      echo "  applying $AE_CTRL_NAME/VBLANK sequence ($AE_CTRL_NAME=0 -> $AE_CTRL_NAME=$AE_AUTO_VALUE -> VBLANK)..."
    else
      echo "  retrying $AE_CTRL_NAME/VBLANK sequence ($attempt/$max_retries)..."
    fi

    # Keep the historical pre-step (value 0), then switch to the target mode
    # before writing VBLANK to reduce immediate auto-mode overwrite.
    set_exposure_auto_mode 0 || true
    set_exposure_auto_mode "$AE_AUTO_VALUE" || true
    if ! v4l2-ctl --device "$PA_SUBDEV" --set-ctrl "vertical_blanking=$target_vblank" >/dev/null 2>&1; then
      echo "  warning: failed to set vertical_blanking=$target_vblank on $PA_SUBDEV"
      return 1
    fi

    vb_readback="$(v4l2-ctl --device "$PA_SUBDEV" --get-ctrl vertical_blanking 2>/dev/null | awk -F': ' '{print $2}' | tr -d '\r')"
    if [[ -z "$vb_readback" ]]; then
      echo "  warning: could not read vertical_blanking back from $PA_SUBDEV"
      return 1
    fi

    echo "  vertical_blanking target=$target_vblank readback=$vb_readback"
    if [[ "$vb_readback" == "$target_vblank" ]]; then
      return 0
    fi
    if [[ "$target_vblank" -gt 100 && "$vb_readback" -le 21 ]]; then
      echo "  warning: VBLANK snapped to minimum while target is high; active timing owner likely overrides this control"
    fi

    attempt=$((attempt + 1))
    if (( attempt <= max_retries )) && [[ "$VBLANK_RETRY_SETTLE_SECONDS" -gt 0 ]]; then
      echo "  waiting ${VBLANK_RETRY_SETTLE_SECONDS}s before retry..."
      sleep "$VBLANK_RETRY_SETTLE_SECONDS"
    fi
  done

  echo "  warning: vertical_blanking readback mismatch persisted after $max_retries retries"
  return 1
}

prepare_stream_and_ae_menu() {
  local ctrl_dump=""
  local status_line=""

  status_line="$(get_recent_valid_status_line || true)"
  if [[ -z "$status_line" ]]; then
    echo "  warning: no recent mt9m114 stream status found. Start stream first, then continue."
    echo "  press Enter when stream is running."
    read -r _
  fi

  echo "  setting $AE_CTRL_NAME=$AE_AUTO_VALUE before run selection..."
  set_exposure_auto_mode "$AE_AUTO_VALUE" || true

  ctrl_dump="$(v4l2-ctl --device "$AE_SUBDEV" --list-ctrls-menus 2>/dev/null | awk -v k="$AE_CTRL_NAME" '$0 ~ k {show=1} show {print; if (NF==0) exit}')"
  if [[ -n "$ctrl_dump" ]]; then
    echo "  $AE_CTRL_NAME menu on $AE_SUBDEV:"
    echo "$ctrl_dump" | sed 's/^/    /'
  else
    echo "  note: $AE_CTRL_NAME menu not found on $AE_SUBDEV"
  fi
}

apply_fps() {
  local fps="$1"
  local actual=""
  local last_line=""
  local frame_length_live=""
  local vblank=""
  local line_length=""
  local active_height=""
  local target_frame_length=""
  local target_vblank=""
  local vb_limits=""
  local vb_min=""
  local vb_max=""
  local vb_readback=""
  local fps_est=""
  local min_fps_theoretical=""

  if ! command -v v4l2-ctl >/dev/null 2>&1; then
    echo "  warning: v4l2-ctl not found, cannot force FPS=$fps"
    return 1
  fi

  if ! v4l2-ctl --device "$VIDEO_DEV" --set-parm "$fps" >/dev/null 2>&1; then
    echo "  warning: failed to set FPS=$fps on $VIDEO_DEV (device busy or unsupported), trying VBLANK fallback on $PA_SUBDEV"

    if ! command -v tac >/dev/null 2>&1; then
      echo "  warning: tac not found; cannot parse latest mt9m114 status for VBLANK fallback"
      return 1
    fi

    last_line="$(get_recent_valid_status_line || true)"
    if [[ -z "$last_line" ]]; then
      echo "  warning: no valid mt9m114 stream status line found for VBLANK fallback"
      return 1
    fi

    frame_length_live="$(extract_kv_field "$last_line" frame_length_live)"
    vblank="$(extract_kv_field "$last_line" vblank)"
    line_length="$(extract_kv_field "$last_line" line_length)"

    if [[ -z "$frame_length_live" || -z "$vblank" || -z "$line_length" ]]; then
      echo "  warning: could not parse frame_length/vblank/line_length for fallback"
      return 1
    fi
    if [[ "$frame_length_live" -le 0 || "$line_length" -le 0 ]]; then
      echo "  warning: invalid frame_length_live=$frame_length_live or line_length=$line_length for fallback"
      return 1
    fi

    active_height=$(( frame_length_live - vblank ))
    if [[ "$active_height" -le 0 ]]; then
      echo "  warning: computed active_height=$active_height invalid for fallback"
      return 1
    fi

    target_frame_length=$(( PIXRATE / (line_length * fps) ))
    if [[ "$target_frame_length" -lt $((active_height + 1)) ]]; then
      target_frame_length=$((active_height + 1))
    fi
    target_vblank=$(( target_frame_length - active_height ))

    vb_limits="$(v4l2-ctl --device "$PA_SUBDEV" --list-ctrls-menus 2>/dev/null | awk '/vertical_blanking/ {print; exit}')"
    vb_min="$(echo "$vb_limits" | sed -n 's/.*min=\([0-9]*\).*/\1/p')"
    vb_max="$(echo "$vb_limits" | sed -n 's/.*max=\([0-9]*\).*/\1/p')"
    if [[ -z "$vb_min" || -z "$vb_max" ]]; then
      echo "  warning: could not read vertical_blanking min/max from $PA_SUBDEV"
      return 1
    fi

    min_fps_theoretical=$(awk -v p="$PIXRATE" -v ll="$line_length" -v ah="$active_height" -v vbmax="$vb_max" 'BEGIN { if (ll > 0 && (ah + vbmax) > 0) printf "%.3f", p / (ll * (ah + vbmax)); else printf "nan" }')
    if awk -v req="$fps" -v minf="$min_fps_theoretical" 'BEGIN { exit !(req < minf) }'; then
      echo "  warning: requested FPS=$fps is below theoretical minimum ${min_fps_theoretical} for current mode (line_length=$line_length, active_height=$active_height, vblank_max=$vb_max)"
      if [[ "$ALLOW_UNFEASIBLE_FPS" -eq 0 ]]; then
        echo "  skipping this FPS target (use --allow-unfeasible-fps to force)"
        return 2
      fi
    fi

    if [[ "$target_vblank" -lt "$vb_min" ]]; then
      target_vblank="$vb_min"
    fi
    if [[ "$target_vblank" -gt "$vb_max" ]]; then
      target_vblank="$vb_max"
    fi

    if [[ "$AE_TOGGLE_ON_FPS_SWITCH" -eq 1 ]]; then
      if ! apply_vblank_with_ae_sequence "$target_vblank" "$VBLANK_VERIFY_RETRIES"; then
        return 1
      fi
    else
      if ! v4l2-ctl --device "$PA_SUBDEV" --set-ctrl "vertical_blanking=$target_vblank" >/dev/null 2>&1; then
        echo "  warning: failed to set vertical_blanking=$target_vblank on $PA_SUBDEV"
        return 1
      fi
    fi

    vb_readback="$(v4l2-ctl --device "$PA_SUBDEV" --get-ctrl vertical_blanking 2>/dev/null | awk -F': ' '{print $2}' | tr -d '\r')"
    if [[ -z "$vb_readback" ]]; then
      vb_readback="$target_vblank"
    fi
    target_frame_length=$(( active_height + vb_readback ))
    fps_est=$(awk -v p="$PIXRATE" -v ll="$line_length" -v fl="$target_frame_length" 'BEGIN { if (ll > 0 && fl > 0) printf "%.3f", p / (ll * fl); else printf "nan" }')

    echo "  fallback applied: active_height=$active_height line_length=$line_length target_vblank=$target_vblank readback_vblank=$vb_readback (range $vb_min..$vb_max), estimated_fps=$fps_est, theoretical_min_fps=$min_fps_theoretical"
    if awk -v t="$target_vblank" -v r="$vb_readback" 'BEGIN { d = t - r; if (d < 0) d = -d; exit !(d > 100) }'; then
      echo "  warning: readback_vblank differs strongly from target; AE/driver may be overriding timing in auto mode"
    fi

    return 0
  fi

  actual="$(v4l2-ctl --device "$VIDEO_DEV" --get-parm 2>/dev/null | awk '/Frames per second/ {print $NF}' | tr -d '\r')"
  if [[ -n "$actual" ]]; then
    echo "  FPS target=$fps, reported=$actual"
  else
    echo "  FPS target=$fps applied (no get-parm feedback)"
  fi

  if [[ "$AE_TOGGLE_ON_FPS_SWITCH" -eq 1 ]]; then
    echo "  set-parm succeeded; enforcing AE auto state after FPS switch..."
    set_exposure_auto_mode "$AE_AUTO_VALUE" || true
  fi

  return 0
}

print_fps_preflight() {
  local last_line=""
  local frame_length_live=""
  local vblank=""
  local line_length=""
  local active_height=""
  local vb_limits=""
  local vb_min=""
  local vb_max=""
  local min_fps_theoretical=""
  local max_fps_theoretical=""
  local mark=""

  if [[ -z "$FPS_VALUES" ]]; then
    return 0
  fi

  if ! command -v v4l2-ctl >/dev/null 2>&1; then
    echo "preflight: v4l2-ctl not found, cannot compute feasible FPS range"
    return 1
  fi

  if ! command -v tac >/dev/null 2>&1; then
    echo "preflight: tac not found, cannot parse latest mt9m114 status"
    return 1
  fi

  last_line="$(get_recent_valid_status_line || true)"
  if [[ -z "$last_line" ]]; then
    echo "preflight: no valid mt9m114 stream status line found; start preview/stream first"
    return 1
  fi

  frame_length_live="$(extract_kv_field "$last_line" frame_length_live)"
  vblank="$(extract_kv_field "$last_line" vblank)"
  line_length="$(extract_kv_field "$last_line" line_length)"

  if [[ -z "$frame_length_live" || -z "$vblank" || -z "$line_length" ]]; then
    echo "preflight: could not parse frame_length/vblank/line_length from latest status"
    return 1
  fi
  if [[ "$frame_length_live" -le 0 || "$line_length" -le 0 ]]; then
    echo "preflight: invalid timing values frame_length_live=$frame_length_live line_length=$line_length"
    return 1
  fi

  active_height=$(( frame_length_live - vblank ))
  if [[ "$active_height" -le 0 ]]; then
    echo "preflight: computed active_height=$active_height invalid"
    return 1
  fi

  vb_limits="$(v4l2-ctl --device "$PA_SUBDEV" --list-ctrls-menus 2>/dev/null | awk '/vertical_blanking/ {print; exit}')"
  vb_min="$(echo "$vb_limits" | sed -n 's/.*min=\([0-9]*\).*/\1/p')"
  vb_max="$(echo "$vb_limits" | sed -n 's/.*max=\([0-9]*\).*/\1/p')"
  if [[ -z "$vb_min" || -z "$vb_max" ]]; then
    echo "preflight: could not read vertical_blanking min/max from $PA_SUBDEV"
    return 1
  fi

  min_fps_theoretical=$(awk -v p="$PIXRATE" -v ll="$line_length" -v ah="$active_height" -v vbmax="$vb_max" 'BEGIN { if (ll > 0 && (ah + vbmax) > 0) printf "%.3f", p / (ll * (ah + vbmax)); else printf "nan" }')
  max_fps_theoretical=$(awk -v p="$PIXRATE" -v ll="$line_length" -v ah="$active_height" -v vbmin="$vb_min" 'BEGIN { if (ll > 0 && (ah + vbmin) > 0) printf "%.3f", p / (ll * (ah + vbmin)); else printf "nan" }')

  echo "preflight: timing basis line_length=$line_length active_height=$active_height vblank_range=$vb_min..$vb_max"
  echo "preflight: theoretical fps range ${min_fps_theoretical} .. ${max_fps_theoretical}"
  echo "preflight: requested fps feasibility"
  for fps in "${FPS_STEPS[@]}"; do
    if [[ "$fps" == "native" ]]; then
      echo "  native: uses current pipeline control"
      continue
    fi
    if awk -v req="$fps" -v minf="$min_fps_theoretical" -v maxf="$max_fps_theoretical" 'BEGIN { exit !(req >= minf && req <= maxf) }'; then
      mark="OK"
    else
      mark="OUT_OF_RANGE"
    fi
    echo "  $fps fps -> $mark"
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--out-dir)
      OUT_DIR="$2"; shift 2;;
    -r|--runs)
      RUN_SPEC="$2"; shift 2;;
    --mt-interval-ms)
      MT_INTERVAL_MS="$2"; shift 2;;
    --s3a-interval-ms)
      S3A_INTERVAL_MS="$2"; shift 2;;
    --match-delta-ms)
      MATCH_DELTA_MS="$2"; shift 2;;
    --warmup-seconds)
      WARMUP_SECONDS="$2"; shift 2;;
    --repeat)
      REPEAT_COUNT="$2"; shift 2;;
    --fps-values)
      FPS_VALUES="$2"; shift 2;;
    --video-dev)
      VIDEO_DEV="$2"; shift 2;;
    --pa-subdev)
      PA_SUBDEV="$2"; shift 2;;
    --ae-subdev)
      AE_SUBDEV="$2"; shift 2;;
    --ae-ctrl-name)
      AE_CTRL_NAME="$2"; shift 2;;
    --pixrate)
      PIXRATE="$2"; shift 2;;
    --allow-unfeasible-fps)
      ALLOW_UNFEASIBLE_FPS=1; shift;;
    --no-ae-toggle)
      AE_TOGGLE_ON_FPS_SWITCH=0; shift;;
    --ae-auto-value)
      AE_AUTO_VALUE="$2"; shift 2;;
    --no-ae-prearm)
      AE_PREARM_BEFORE_PROMPT=0; shift;;
    --ae-prearm-settle-seconds)
      AE_PREARM_SETTLE_SECONDS="$2"; shift 2;;
    --vblank-verify-retries)
      VBLANK_VERIFY_RETRIES="$2"; shift 2;;
    --vblank-retry-settle-seconds)
      VBLANK_RETRY_SETTLE_SECONDS="$2"; shift 2;;
    --preflight-only)
      PREFLIGHT_ONLY=1; shift;;
    --no-plot)
      MAKE_PLOT=0; shift;;
    -h|--help)
      usage; exit 0;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1;;
  esac
done

if [[ ! -f "$PARSER" ]]; then
  echo "Parser not found: $PARSER" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "[1/4] Configuring module logging intervals..."
if [[ -w /sys/module/atomisp/parameters/s3a_status_log_enable ]]; then
  echo 1 | sudo tee /sys/module/atomisp/parameters/s3a_status_log_enable >/dev/null
else
  echo "  warning: /sys/module/atomisp/parameters/s3a_status_log_enable not writable"
fi

if [[ -w /sys/module/atomisp/parameters/s3a_status_log_interval_ms ]]; then
  echo "$S3A_INTERVAL_MS" | sudo tee /sys/module/atomisp/parameters/s3a_status_log_interval_ms >/dev/null
else
  echo "  warning: /sys/module/atomisp/parameters/s3a_status_log_interval_ms not writable"
fi

if [[ -w /sys/module/mt9m114/parameters/stream_status_interval_ms ]]; then
  echo "$MT_INTERVAL_MS" | sudo tee /sys/module/mt9m114/parameters/stream_status_interval_ms >/dev/null
else
  echo "  warning: /sys/module/mt9m114/parameters/stream_status_interval_ms not writable"
fi

echo "[2/4] Starting scripted runs..."
IFS=',' read -r -a RUNS <<< "$RUN_SPEC"

if [[ -n "$FPS_VALUES" ]]; then
  IFS=',' read -r -a FPS_STEPS <<< "$FPS_VALUES"
else
  FPS_STEPS=("native")
fi

if [[ ${#FPS_STEPS[@]} -gt 0 && "${FPS_STEPS[0]}" != "native" ]]; then
  echo "  preparing stream and AE state for FPS-step runs..."
  prepare_stream_and_ae_menu
fi

print_fps_preflight || true
if [[ "$PREFLIGHT_ONLY" -eq 1 ]]; then
  exit 0
fi

for run in "${RUNS[@]}"; do
  label="${run%%:*}"
  seconds="${run##*:}"

  if [[ -z "$label" || -z "$seconds" || "$label" == "$seconds" ]]; then
    echo "Invalid run spec entry: $run (expected label:seconds)" >&2
    exit 1
  fi

  for fps in "${FPS_STEPS[@]}"; do
    for rep in $(seq 1 "$REPEAT_COUNT"); do
      if [[ "$fps" == "native" ]]; then
        run_dir="$OUT_DIR/$label/rep_${rep}"
      else
        run_dir="$OUT_DIR/$label/fps_${fps}/rep_${rep}"
      fi
      mkdir -p "$run_dir"

      echo
      if [[ "$fps" == "native" ]]; then
        echo "=== Run '$label' repeat ${rep}/${REPEAT_COUNT} for ${seconds}s ==="
      else
        echo "=== Run '$label' @ ${fps} FPS repeat ${rep}/${REPEAT_COUNT} for ${seconds}s ==="
      fi

      if [[ "$fps" != "native" && "$AE_TOGGLE_ON_FPS_SWITCH" -eq 1 && "$AE_PREARM_BEFORE_PROMPT" -eq 1 ]]; then
        echo "  pre-arming $AE_CTRL_NAME=0 during setup/wait phase..."
        set_exposure_auto_mode 0 || true
        if [[ "$AE_PREARM_SETTLE_SECONDS" -gt 0 ]]; then
          echo "  waiting ${AE_PREARM_SETTLE_SECONDS}s for AE to settle before start prompt..."
          sleep "$AE_PREARM_SETTLE_SECONDS"
        fi
      fi

      echo "Set your light condition now, then press Enter to start."
      read -r _

      if [[ "$fps" != "native" ]]; then
        set +e
        apply_fps "$fps"
        fps_rc=$?
        set -e
        if [[ $fps_rc -eq 2 ]]; then
          continue
        elif [[ $fps_rc -ne 0 ]]; then
          echo "  warning: FPS/VBLANK apply failed for fps=$fps, skipping this run repeat"
          continue
        fi
      fi

      if [[ "$WARMUP_SECONDS" -gt 0 ]]; then
        warmup_log="$run_dir/warmup.log"
        echo "  warmup capture (${WARMUP_SECONDS}s)..."
        set +e
        timeout "${WARMUP_SECONDS}s" bash -lc \
          "sudo dmesg --follow --since now | grep -E 'mt9m114 stream status|atomisp s3a status'" \
          > "$warmup_log"
        warmup_rc=$?
        set -e
        if [[ $warmup_rc -ne 0 && $warmup_rc -ne 124 ]]; then
          echo "Warmup capture failed for '$label' rep_${rep} (rc=$warmup_rc)" >&2
          exit 1
        fi
      fi

      raw_log="$run_dir/raw.log"
      joined_csv="$run_dir/joined.csv"
      mt_csv="$run_dir/mt.csv"
      s3a_csv="$run_dir/s3a.csv"
      plot_png="$run_dir/gain_vs_s3a.png"

      echo "  capturing dmesg lines..."
      set +e
      timeout "${seconds}s" bash -lc \
        "sudo dmesg --follow --since now | grep -E 'mt9m114 stream status|atomisp s3a status'" \
        > "$raw_log"
      capture_rc=$?
      set -e
      if [[ $capture_rc -ne 0 && $capture_rc -ne 124 ]]; then
        echo "Capture failed for '$label' rep_${rep} (rc=$capture_rc)" >&2
        exit 1
      fi

      echo "  parsing and joining..."
      python3 "$PARSER" \
        -i "$raw_log" \
        -o "$joined_csv" \
        --mt-out "$mt_csv" \
        --atomisp-out "$s3a_csv" \
        --max-delta-ms "$MATCH_DELTA_MS"

      if [[ $MAKE_PLOT -eq 1 ]]; then
        echo "  plotting..."
        set +e
        python3 "$PLOTTER" -i "$joined_csv" -o "$plot_png" --max-delta-ms "$MATCH_DELTA_MS"
        plot_rc=$?
        set -e
        if [[ $plot_rc -ne 0 ]]; then
          echo "  warning: plot step failed (likely missing matplotlib), CSV outputs are still available"
        fi
      fi
    done
  done

done

echo
cat <<EOF
[3/4] Runs completed.
Output root: $OUT_DIR

Per run you'll find:
- raw.log
- joined.csv
- mt.csv
- s3a.csv
- gain_vs_s3a.png (if plotting succeeded)

[4/4] Done.
EOF
