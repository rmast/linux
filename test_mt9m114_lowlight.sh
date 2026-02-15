#!/bin/bash
# MT9M114 Low-Light Patch - Quick Test Script
# For Asus T100 (Bay Trail) - Linux 6.19-rc7

set -e

SENSOR_DEV="/dev/v4l-subdev2"  # Adjust if different on your system
VIDEO_DEV="/dev/video0"

echo "==================================="
echo "MT9M114 Low-Light Patch Test Script"
echo "==================================="
echo ""

# Check if devices exist
if [ ! -e "$SENSOR_DEV" ]; then
    echo "ERROR: Sensor device $SENSOR_DEV not found!"
    echo "Try: ls /dev/v4l-subdev* to find the correct device"
    exit 1
fi

echo "[1/6] Checking current sensor controls..."
v4l2-ctl -d "$SENSOR_DEV" --list-ctrls | grep -E "exposure|vertical_blanking|ae_"

echo ""
echo "[2/6] Testing VBLANK adjustment..."
# Save original VBLANK
ORIGINAL_VBLANK=$(v4l2-ctl -d "$SENSOR_DEV" --get-ctrl=vertical_blanking | awk '{print $2}')
echo "Original VBLANK: $ORIGINAL_VBLANK"

# Set extended VBLANK for long exposure
echo "Setting VBLANK to 5000 (allows ~5s exposures)..."
v4l2-ctl -d "$SENSOR_DEV" --set-ctrl=vertical_blanking=5000

# Check new exposure maximum
MAX_EXPOSURE=$(v4l2-ctl -d "$SENSOR_DEV" --list-ctrls | grep "exposure 0x" | sed -n 's/.*max=\([0-9]*\).*/\1/p')
echo "New max exposure: $MAX_EXPOSURE lines"

if [ "$MAX_EXPOSURE" -gt 4000 ]; then
    echo "✓ VBLANK adjustment working correctly"
else
    echo "✗ VBLANK adjustment may have issues (max exposure still low)"
fi

echo ""
echo "[3/6] Testing long exposure with VTS adjustment..."
# Request a long exposure (should trigger VTS auto-adjustment)
v4l2-ctl -d "$SENSOR_DEV" --set-ctrl=exposure=3000
echo "Set exposure to 3000 lines"

# Check dmesg for VTS adjustment message
echo "Checking kernel logs for VTS adjustment..."
dmesg | tail -20 | grep -i "Long exposure\|VTS adjustment" || echo "  (No debug message - check if debug logging is enabled)"

echo ""
echo "[4/6] Testing custom AE metering weights..."
# Create a center-weighted pattern for low-light
# This is a 5x5 grid (25 values), 0x00-0x0F each
# Pattern: center = 15, edges = 2
WEIGHTS="0x02,0x04,0x08,0x04,0x02,0x04,0x08,0x0c,0x08,0x04,0x08,0x0c,0x0f,0x0c,0x08,0x04,0x08,0x0c,0x08,0x04,0x02,0x04,0x08,0x04,0x02"

# Check if the control exists
if v4l2-ctl -d "$SENSOR_DEV" --list-ctrls | grep -q "ae_metering"; then
    echo "Setting center-weighted AE metering pattern..."
    # Note: This requires a custom v4l2-ctl that supports array controls
    # Or use a C program to set via VIDIOC_S_EXT_CTRLS
    echo "  (Use custom tool or C program to set array control)"
    echo "  Control ID: V4L2_CID_MT9M114_AE_METERING_WEIGHTS"
else
    echo "✗ AE metering weights control not found - patch may not be applied"
fi

echo ""
echo "[5/6] Testing AE tracking speed (low-light mode)..."
if v4l2-ctl -d "$SENSOR_DEV" --list-ctrls | grep -q "ae_track_speed"; then
    echo "Setting AE speed to low-light mode (0x03)..."
    v4l2-ctl -d "$SENSOR_DEV" --set-ctrl=ae_track_speed=3
    echo "✓ AE speed control set"
else
    echo "✗ AE tracking speed control not found - patch may not be applied"
fi

echo ""
echo "[6/6] Capturing test frames..."
if [ -e "$VIDEO_DEV" ]; then
    echo "Capturing 10 frames to /tmp/mt9m114_test_*.jpg..."
    
    # Set media pipeline (adjust for your system)
    # media-ctl --set-v4l2 '"mt9m114 2-0048":0[fmt:UYVY8_1X16/1280x960]' || true
    
    gst-launch-1.0 -q v4l2src device="$VIDEO_DEV" num-buffers=10 ! \
        videoconvert ! jpegenc ! multifilesink location=/tmp/mt9m114_test_%02d.jpg 2>&1 || \
        echo "  (GStreamer failed - check video pipeline configuration)"
    
    if [ -f "/tmp/mt9m114_test_00.jpg" ]; then
        echo "✓ Frames captured"
        echo "  Check exposure times with: exiftool /tmp/mt9m114_test_*.jpg | grep ExposureTime"
    fi
else
    echo "Video device $VIDEO_DEV not found - skipping capture test"
fi

echo ""
echo "==================================="
echo "Test Summary"
echo "==================================="
echo ""
echo "Next steps:"
echo "1. Check dmesg for kernel messages: dmesg | grep mt9m114"
echo "2. Test in actual low-light conditions"
echo "3. Compare image quality before/after patch"
echo ""
echo "Low-light test scenario:"
echo "  - Set VBLANK high: v4l2-ctl -d $SENSOR_DEV --set-ctrl=vertical_blanking=10000"
echo "  - Enable low-light AE: v4l2-ctl -d $SENSOR_DEV --set-ctrl=ae_track_speed=3"
echo "  - Let AE converge (wait 2-3 seconds)"
echo "  - Capture image and check metadata"
echo ""
echo "Expected improvements:"
echo "  - Longer exposure times (>100ms) in dark scenes"
echo "  - Lower ISO/gain values (less noise)"
echo "  - Better face exposure in backlit situations"
echo ""

# Restore original VBLANK
echo "Restoring original VBLANK..."
v4l2-ctl -d "$SENSOR_DEV" --set-ctrl=vertical_blanking="$ORIGINAL_VBLANK"

echo "Test complete!"
