# Libcamera Real-Life Visual Test (T100TA)

Last updated: 2026-05-30

This guide validates a real GUI/video preview path using software that depends on libcamera.

## Goal

Show a live camera image (not just enumerate or single-frame capture) on ASUS T100TA using the PoC libcamera build.

## Preconditions

- Use the latest PoC bundle from this workstream (at least v7 with frame-start fallback).
- Camera discovery already works (`cam --list` shows one camera).
- Run from a graphical session on the T100TA.

## Environment

```bash
export LD_LIBRARY_PATH=/tmp/libcamera-poc/usr/local/lib64
export LIBCAMERA_IPA_MODULE_PATH=/tmp/libcamera-poc/usr/local/lib64/libcamera/ipa
export LIBCAMERA_IPA_CONFIG_PATH=/tmp/libcamera-poc/usr/local/share/libcamera/ipa
export LIBCAMERA_LOG_LEVELS=SimplePipeline:INFO,Camera:INFO
```

## Check Loaded Version

```bash
/tmp/libcamera-poc/usr/local/bin/cam --list | head -n 1
```

Expected: version string includes the latest PoC commit from this branch.

## Visual Test A: qcam (preferred)

If `qcam` is available in your runtime:

```bash
qcam --camera "\\_SB_.I2C4.CAM0"
```

If camera ID selection is not needed:

```bash
qcam
```

Expected:

- qcam window opens
- live preview stream is visible
- no immediate crash or repeated pipeline restart loop

## Visual Test B: GStreamer libcamerasrc

If `qcam` is unavailable, use gstreamer with libcamera source:

```bash
gst-launch-1.0 libcamerasrc ! videoconvert ! autovideosink
```

Optional fixed size request:

```bash
gst-launch-1.0 libcamerasrc ! video/x-raw,width=640,height=480 ! videoconvert ! autovideosink
```

Expected:

- live preview window opens
- moving image updates continuously
- process exits cleanly on Ctrl+C

## Optional Smoke Capture (already validated path)

```bash
/tmp/libcamera-poc/usr/local/bin/cam -c1 -C1 -s role=viewfinder,width=640,height=480,pixelformat=NV12
```

Expected:

- one frame captured
- no hard failure in pipeline start

## Evidence To Save

Store outputs for later report attachment:

```bash
mkdir -p /tmp/libcamera-real-life
/tmp/libcamera-poc/usr/local/bin/cam --list > /tmp/libcamera-real-life/cam_list.txt 2>&1
/tmp/libcamera-poc/usr/local/bin/cam -c1 -C1 -s role=viewfinder,width=640,height=480,pixelformat=NV12 > /tmp/libcamera-real-life/cam_capture.txt 2>&1
```

For visual tests, also note:

- which tool was used (`qcam` or `gst-launch-1.0 libcamerasrc`)
- whether preview was stable for at least 30 seconds
- any visible artifacts (exposure, color, frame drops)
- any stderr warnings/errors
