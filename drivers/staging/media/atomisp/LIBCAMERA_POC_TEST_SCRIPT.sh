#!/bin/bash
#
# Atomisp Libcamera PoC Test Matrix - Minimal Track
# Copy-paste each test block or run this script with output redirected to a log file
#
# Usage: bash LIBCAMERA_POC_TEST_SCRIPT.sh | tee test_results.log
#

set -e

LOG_DIR="/tmp/atomisp_poc_test_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

echo "==============================================================================="
echo "Atomisp Libcamera PoC Test Matrix"
echo "Start time: $(date)"
echo "Log directory: $LOG_DIR"
echo "==============================================================================="
echo ""

# ----------------------------------------------------------------------------
# Auto-detect the atomisp media device node
# media-ctl defaults to /dev/media0, but atomisp may be enumerated at a
# different number (e.g. /dev/media1) if another media device is present.
# We scan all /dev/media* nodes and pick the one whose driver is atomisp-isp2.
# ----------------------------------------------------------------------------
MEDIA_DEV=""
for dev in /dev/media*; do
    [ -e "$dev" ] || continue
    if media-ctl -d "$dev" -p 2>/dev/null | grep -q "atomisp-isp2"; then
        MEDIA_DEV="$dev"
        break
    fi
done

if [ -z "$MEDIA_DEV" ]; then
    echo "WARNING: No atomisp media device found under /dev/media*."
    echo "  Check that the atomisp kernel module is loaded:"
    echo "    lsmod | grep atomisp"
    echo "  And that /dev/media* nodes exist:"
    echo "    ls -la /dev/media*"
    echo "  If nodes are missing entirely, the driver may not have probed."
    echo "  If nodes exist but are not accessible, try: sudo $0"
    echo ""
else
    echo "Detected atomisp media device: $MEDIA_DEV"
    echo ""
fi

# Resolve mt9m114-related subdev nodes from media topology output.
resolve_mt9m114_subdevs() {
    local media_dev="$1"
    [ -n "$media_dev" ] || return 0
    media-ctl -d "$media_dev" -p 2>/dev/null |
        awk '
            /^- entity [0-9]+: mt9m114 / { in_mt=1; next }
            /^- entity [0-9]+:/ { in_mt=0 }
            in_mt && /device node name / {
                print $NF
                in_mt=0
            }
        '
}

resolve_mt9m114_sensor_subdev() {
    local media_dev="$1"
    [ -n "$media_dev" ] || return 0
    media-ctl -d "$media_dev" -p 2>/dev/null |
        awk '
            /^- entity [0-9]+: mt9m114 / { in_mt=1; is_sensor=0; next }
            /^- entity [0-9]+:/ { in_mt=0; is_sensor=0 }
            in_mt && /subtype Sensor/ { is_sensor=1 }
            in_mt && is_sensor && /device node name / {
                print $NF
                exit
            }
        '
}

# ============================================================================
# TEST 1: Graph Discovery
# ============================================================================
echo "[TEST 1] Graph Discovery - media-ctl topology"
echo "---"
echo "Goal: Verify expected entity chain and exactly one active input path"
echo ""

if ! command -v media-ctl &> /dev/null; then
    echo "SKIP: media-ctl not found. Install libmediainfo0 or media-ctl package."
elif [ -z "$MEDIA_DEV" ]; then
    echo "SKIP: No atomisp media device found (see warning above)."
else
    echo "Running: media-ctl -d $MEDIA_DEV -p"
    media-ctl -d "$MEDIA_DEV" -p | tee "$LOG_DIR/test1_media_graph.txt"
    echo ""
    echo "Expected: entity chain sensor -> csi2 -> atomisp -> video_out"
    echo "Expected: exactly one link marked ENABLED"
    echo ""
fi

# ============================================================================
# TEST 2: Format Negotiation - Lower Resolution
# ============================================================================
echo "[TEST 2] Format Negotiation - 1280x720 (lower)"
echo "---"
echo "Goal: Set format and confirm negotiated formats on each pad"
echo ""

if ! command -v v4l2-ctl &> /dev/null; then
    echo "SKIP: v4l2-ctl not found. Install v4l-utils package."
else
    # Find the video device
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | head -n 1)
    if [ -z "$VIDEO_DEV" ]; then
        echo "SKIP: No /dev/video* found"
    else
        echo "Using video device: $VIDEO_DEV"
        echo ""
        
        echo "Query current format:"
        v4l2-ctl -d "$VIDEO_DEV" --get-fmt-video 2>&1 | tee "$LOG_DIR/test2_current_fmt.txt" || true
        echo ""
        
        echo "Setting format to 1280x720 YUYV:"
        v4l2-ctl -d "$VIDEO_DEV" --set-fmt-video=width=1280,height=720,pixelformat=YUYV 2>&1 | tee "$LOG_DIR/test2_set_fmt.txt" || true
        echo ""
        
        echo "Query format after set:"
        v4l2-ctl -d "$VIDEO_DEV" --get-fmt-video 2>&1 | tee "$LOG_DIR/test2_after_fmt.txt" || true
        echo ""
        
        # Try to query sensor subdev format (if available)
        if [ -n "$MEDIA_DEV" ]; then
            SENSOR_SD=$(resolve_mt9m114_subdevs "$MEDIA_DEV" | head -n 1)
            if [ -n "$SENSOR_SD" ]; then
                echo "Sensor subdev found: $SENSOR_SD"
                echo "Querying sensor format (via v4l2-ctl --all):"
                v4l2-ctl -d "$SENSOR_SD" --all 2>&1 | head -n 20 || true
                echo ""
            fi
        fi
    fi
fi

# ============================================================================
# TEST 3: Format Negotiation - Higher Resolution
# ============================================================================
echo "[TEST 3] Format Negotiation - Higher resolution (1280x960 max)"
echo "---"
echo "Goal: Confirm format negotiation at higher resolution"
echo ""

if ! command -v v4l2-ctl &> /dev/null; then
    echo "SKIP: v4l2-ctl not found"
else
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | head -n 1)
    if [ -z "$VIDEO_DEV" ]; then
        echo "SKIP: No /dev/video* found"
    else
        echo "Setting format to 1280x960 YUYV:"
        v4l2-ctl -d "$VIDEO_DEV" --set-fmt-video=width=1280,height=960,pixelformat=YUYV 2>&1 | tee "$LOG_DIR/test3_set_fmt_high.txt" || true
        echo ""
        
        echo "Query format after set:"
        v4l2-ctl -d "$VIDEO_DEV" --get-fmt-video 2>&1 | tee "$LOG_DIR/test3_after_fmt_high.txt" || true
        echo ""
    fi
fi

# ============================================================================
# TEST 4: Stream Lifecycle - Start/Stop Loop
# ============================================================================
echo "[TEST 4] Stream Lifecycle - Start/Stop 20x in a loop"
echo "---"
echo "Goal: Confirm stream start/stop is race-safe and deterministic"
echo ""

if ! command -v v4l2-ctl &> /dev/null; then
    echo "SKIP: v4l2-ctl not found"
else
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | head -n 1)
    if [ -z "$VIDEO_DEV" ]; then
        echo "SKIP: No /dev/video* found"
    else
        echo "Running 20 start/stop cycles (capture timeout 1s per iteration):"
        for i in {1..20}; do
            echo -n "  Cycle $i: "
            # Try a brief stream capture
            v4l2-ctl -d "$VIDEO_DEV" --stream-mmap --stream-count=1 --stream-to=/dev/null 2>&1 | grep -q "1 frames" && echo "OK" || echo "WARN/SKIP"
        done | tee "$LOG_DIR/test4_stream_cycles.txt"
        echo ""
    fi
fi

# ============================================================================
# TEST 5: Module Unload/Reload
# ============================================================================
echo "[TEST 5] Module Unload/Reload + Stream"
echo "---"
echo "Goal: Confirm reprobe is race-safe for async notifier paths"
echo ""

echo "CAUTION: This test requires root and will unload kernel modules."
echo "Skipping automated unload/reload (manual verification recommended)."
echo ""
echo "Manual steps (if desired):"
echo "  1. Ensure no active streams"
echo "  2. sudo modprobe -r mt9m114 atomisp_gmin_platform atomisp"
echo "  3. sudo modprobe atomisp"
echo "  4. sudo modprobe mt9m114    # aptina_pll and v4l2_cci load as deps"
echo "  5. Re-run graph discovery and stream test"
echo ""
echo "Notes:"
echo "  - modprobe -r is preferred over rmmod (dependency-aware remove)."
echo "  - Do not unload videodev/v4l2_async/videobuf2/mc (shared core stack)."
echo "  - Do not unload intel_skl_int3472_discrete or ipu_bridge unless debugging platform glue."
echo "  - atomisp_css2400 is not expected as a module in this tree/config."
echo ""

# ============================================================================
# TEST 6: Control Baseline - Standard V4L2 Controls
# ============================================================================
echo "[TEST 6] Control Baseline - Standard V4L2 controls"
echo "---"
echo "Goal: Confirm standard controls are accessible (no dependency on custom controls)"
echo ""

if ! command -v v4l2-ctl &> /dev/null; then
    echo "SKIP: v4l2-ctl not found"
else
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | head -n 1)
    if [ -z "$VIDEO_DEV" ]; then
        echo "SKIP: No /dev/video* found"
    else
        echo "Querying standard controls (video node):"
        echo "  - Exposure (should be present on sensor subdev via get_ctrl calls)"
        echo "  - Gain (should be present on sensor subdev)"
        echo ""
        
        # Try to list all controls
        echo "All available controls on $VIDEO_DEV:"
        v4l2-ctl -d "$VIDEO_DEV" --list-ctrls 2>&1 | head -n 30 | tee "$LOG_DIR/test6_ctrls.txt" || true
        echo ""
        
        # Try sensor subdev if available.
        # Device nodes are typically /dev/v4l-subdevN, not named after mt9m114.
        SENSOR_DEV=""
        if [ -n "$MEDIA_DEV" ]; then
            SENSOR_DEV=$(resolve_mt9m114_sensor_subdev "$MEDIA_DEV" | head -n 1)
        fi
        if [ -z "$SENSOR_DEV" ]; then
            for sd in /dev/v4l-subdev*; do
                [ -e "$sd" ] || continue
                if v4l2-ctl -d "$sd" --all 2>/dev/null | grep -Eqi "mt9m114|aptina"; then
                    SENSOR_DEV="$sd"
                    break
                fi
            done
        fi
        if [ -n "$SENSOR_DEV" ]; then
            echo "Sensor subdev controls on $SENSOR_DEV:"
            v4l2-ctl -d "$SENSOR_DEV" --list-ctrls 2>&1 | head -n 30 | tee "$LOG_DIR/test6_sensor_ctrls.txt" || true
            echo ""
        else
            echo "SKIP: Could not resolve mt9m114 sensor subdev under /dev/v4l-subdev*"
            echo ""
        fi
    fi
fi

# ============================================================================
# TEST 7: Regression Sanity - GStreamer Path
# ============================================================================
echo "[TEST 7] Regression Sanity - gst-launch existing pipeline"
echo "---"
echo "Goal: Keep existing gst-launch path working as fallback"
echo ""

if ! command -v gst-launch-1.0 &> /dev/null; then
    echo "SKIP: gst-launch-1.0 not found. Install gstreamer1.0-tools package."
else
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | head -n 1)
    if [ -z "$VIDEO_DEV" ]; then
        echo "SKIP: No /dev/video* found"
    else
        echo "Running: gst-launch-1.0 v4l2src device=$VIDEO_DEV ! videoconvert ! fakesink (3s timeout)"
        timeout 3 gst-launch-1.0 -e v4l2src device="$VIDEO_DEV" num-buffers=5 ! videoconvert ! fakesink 2>&1 | tee "$LOG_DIR/test7_gst.txt" || true
        echo ""
    fi
fi

# ============================================================================
# TEST 8: Libcamera Discovery (if libcamera available)
# ============================================================================
echo "[TEST 8] Libcamera PoC - Camera discovery"
echo "---"
echo "Goal: Run a basic libcamera smoke test if available"
echo ""

if ! command -v cam &> /dev/null; then
    echo "SKIP: libcamera 'cam' tool not found."
    echo "       Install libcamera or libcamera-tools package to test libcamera integration."
else
    echo "Listing cameras via libcamera:"
    cam --list 2>&1 | tee "$LOG_DIR/test8_libcamera_list.txt" || true
    echo ""
    
    echo "Attempting a basic capture (5 frames, 2s timeout):"
    timeout 2 cam -c 0 --capture=5 2>&1 | head -n 50 | tee "$LOG_DIR/test8_libcamera_capture.txt" || true
    echo ""
fi

# ============================================================================
# Summary
# ============================================================================
echo "==============================================================================="
echo "Test Matrix Complete"
echo "End time: $(date)"
echo "Log directory: $LOG_DIR"
echo ""
echo "Next steps:"
echo "  1. Review logs in $LOG_DIR for failures"
echo "  2. Check kernel logs: dmesg | tail -50"
echo "  3. Share failed test output with maintainers"
echo "==============================================================================="
