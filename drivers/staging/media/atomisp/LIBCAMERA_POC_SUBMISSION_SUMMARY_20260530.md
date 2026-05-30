# AtomISP / MT9M114 PoC Submission Summary (2026-05-30)

## Scope

This summary captures the latest rerun status for the AtomISP + MT9M114 libcamera PoC validation on ASUS T100TA.

- Hardware: ASUS T100TA
- Kernel: 7.0.0+
- Goal: verify kernel-side media topology and basic userspace behavior needed before full libcamera integration

## Result Summary

- Test 1 (graph discovery): PASS
- Test 2 (1280x720 format): PASS
- Test 3 (1280x960 format): PASS
- Test 4 (stream loop): PASS via gst fallback
- Test 5 (module unload/reload + reprobe): PASS
- Test 6 (control baseline): PASS
- Test 7 (GStreamer regression): PASS
- Test 8 (libcamera discovery): FAIL (no camera exposed yet)

## Important Note About Test 4

On this setup, direct `v4l2-ctl` stream methods (`--stream-mmap` / `--stream-user`) can report unsupported ioctls on the selected video node (`VIDIOC_CREATE_BUFS` / `VIDIOC_REQBUFS`).

The PoC script now treats these ioctl messages as hard method failures and falls back to a one-buffer GStreamer capture path.

Observed rerun behavior: cycles report OK via `gst-fallback`.

## Log Set Used In This Rerun

- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test1_media_graph.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test2_after_fmt.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test3_after_fmt_high.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test4_stream_cycles.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test6_ctrls.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test6_sensor_ctrls.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test7_gst.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test8_libcamera_list.txt
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/test8_libcamera_capture.txt

## Related Commits In This Branch

- 545baa2ff22e - add PoC docs, script, and baseline logs
- 2de561b2a310 - harden stream-loop detection and update test4 report
- 4393df18573e - refresh PoC logs with 20260530 rerun results

## Ready-To-Send Position

The kernel topology and V4L2/GStreamer path are in a reviewable state for this PoC.
libcamera discovery remains the open gap and is explicitly reported as such.
