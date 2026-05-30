# AtomISP / MT9M114 Libcamera PoC Master Checklist

Last updated: 2026-05-30

This is the single tracking file for current PoC status.
Use this as source of truth instead of VS Code todo indicators.

## Phase 1: Scope and Plan

- [x] Define PoC contract and scope
- [x] Document minimal track and test matrix
- [x] Keep published commits and local test-only delta separated

References:
- drivers/staging/media/atomisp/LIBCAMERA_POC_PLAN.md
- drivers/staging/media/atomisp/LIBCAMERA_POC_QUICKSTART.md

## Phase 2: Test Tooling

- [x] Provide executable test script
- [x] Provide structured log template
- [x] Add Fedora package alternatives in install instructions
- [x] Harden Test 4 stream-loop detection (no false OK on ioctl errors)
- [x] Add stream fallback path for nodes where v4l2-ctl stream ioctls are unsupported

References:
- drivers/staging/media/atomisp/LIBCAMERA_POC_TEST_SCRIPT.sh
- drivers/staging/media/atomisp/LIBCAMERA_POC_TEST_LOG_TEMPLATE.md

## Phase 3: Hardware Validation (ASUS T100TA)

- [x] Test 1: graph discovery
- [x] Test 2: 1280x720 format negotiation
- [x] Test 3: 1280x960 format negotiation
- [x] Test 4: stream lifecycle (PASS via gst fallback)
- [x] Test 5: module unload/reload and reprobe
- [x] Test 6: control baseline
- [x] Test 7: GStreamer regression
- [x] Test 8: libcamera discovery/capture

Current interpretation:
- Kernel topology and V4L2/GStreamer path: validated for PoC
- libcamera discovery and single-frame capture: validated for PoC
- frame-start events on Atom ISP return -EINVAL on this platform; simple pipeline now falls back and continues streaming

References:
- drivers/staging/media/atomisp/atomisp_poc_test_20260530_122049/
- drivers/staging/media/atomisp/LIBCAMERA_POC_SUBMISSION_SUMMARY_20260530.md

## Phase 4: Reporting and Submission Assets

- [x] Prepare maintainer-oriented mail drafts
- [x] Prepare compact submission summary document
- [x] Refresh logs with latest rerun set
- [ ] Send report set to linux-media (pending go/no-go)

References:
- drivers/staging/media/atomisp/LINUX_MEDIA_MAIL_DRAFT.md
- drivers/staging/media/atomisp/LIBCAMERA_POC_SUBMISSION_SUMMARY_20260530.md

## Phase 5: Next Actions

- [ ] Decide submission timing to linux-media
- [ ] Optionally perform one extra confirmation rerun before sending
- [x] Start implementation track for closing libcamera discovery gap
- [ ] Real-life visual preview test with libcamera-based software on T100TA (qcam and/or gstreamer libcamerasrc)

Reference:
- drivers/staging/media/atomisp/LIBCAMERA_REAL_LIFE_VISUAL_TEST.md

## AtomISP Staging TODO MUST Status Snapshot

Status against drivers/staging/media/atomisp/TODO MUST list:

- [ ] Remove/disable private IOCTLs
- [ ] Remove/disable custom v4l2-ctrls
- [ ] Remove unnecessary/unwanted module parameters
- [ ] Remove abuse of priv field in various v4l2 userspace API structs
- [x] PoC-level libcamera integration readiness for staging-exit planning (camera discovery + basic capture validated)

## Commit Trail (Current)

- 545baa2ff22e: add PoC docs, script, and baseline logs
- 2de561b2a310: harden stream-loop detection and update test4 report
- 4393df18573e: refresh PoC logs with 20260530 rerun results
- 14821fb298b6: add 20260530 PoC submission summary

## Usage Notes

- Update this file first when status changes.
- Keep checkboxes factual; avoid optimistic marking.
- If a test is pass-with-conditions, note the condition explicitly.
