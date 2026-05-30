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

Important:

- If the version string does not show the latest commit, you are not testing the newest bundle.
- For color-preview fixes, use v8 or newer.

## Reset qcam Persisted State

qcam can remember the previously used stream settings and may reopen with an unsuitable size/aspect profile.

Before retesting, clear qcam settings once:

```bash
rm -f ~/.config/libcamera.org/qcam.conf
```

Then start qcam again with an explicit stream request.

## Visual Test A: qcam (preferred)

If `qcam` is available in your runtime:

```bash
qcam --camera "\\_SB_.I2C4.CAM0"
```

If camera ID selection is not needed:

```bash
qcam
```

Recommended explicit stream for this platform:

```bash
qcam --camera "\\_SB_.I2C4.CAM0" --stream "width=1248,height=928"
```

Expected:

- qcam window opens
- live preview stream is visible
- no immediate crash or repeated pipeline restart loop

Note:

- On older PoC builds, qcam may pick RGB565 and show wrong colors (channel swap/tint).
- Prefer the latest PoC build (v8 or newer), where AtomISP skips RGB565 advertisement for preview stability.

## Visual Test B: GStreamer libcamerasrc

If `qcam` is unavailable, use gstreamer with libcamera source:

```bash
gst-launch-1.0 libcamerasrc ! videoconvert ! autovideosink
```

If you get `no element "libcamerasrc"`, your current libcamera build was created with gstreamer support disabled. In that case use qcam/cam for validation, or rebuild libcamera with gstreamer enabled.

Optional fixed size request:

```bash
gst-launch-1.0 libcamerasrc ! video/x-raw,width=640,height=480 ! videoconvert ! autovideosink
```

Expected:

- live preview window opens
- moving image updates continuously
- process exits cleanly on Ctrl+C

For color-stable preview, force NV12 explicitly:

```bash
gst-launch-1.0 libcamerasrc ! video/x-raw,format=NV12,width=1248,height=928,pixel-aspect-ratio=1/1 ! videoconvert ! autovideosink
```

If the image looks letterboxed/pillarboxed, this is often display scaling behavior in the sink.
The AtomISP path may expose non-exact 4:3 sizes (for example 600x440 or 1248x928) due hardware margins/cropping.

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
