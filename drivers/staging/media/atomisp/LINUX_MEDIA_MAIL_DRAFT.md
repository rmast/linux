# Linux-media Mail Drafts (AtomISP / MT9M114 PoC)

This file contains ready-to-send email drafts for linux-media.
Use the short version first if you want to keep review overhead minimal.

## Draft A: Short Maintainer-Style

Subject: [PATCH 0/0] atomisp/mt9m114: libcamera PoC validation report

```text
Hi linux-media,

I validated the current atomisp/mt9m114 stack against a minimal libcamera PoC contract on ASUS T100TA (kernel 7.0.0+).

Result summary:
- Graph discovery: PASS
- 1280x720 negotiation: PASS
- 1280x960 negotiation: PASS
- Stream loop test: PASS (with documented gst fallback on unsupported v4l2-ctl stream ioctls)
- Module unload/reload and reprobe: PASS
- Control baseline: PASS
- GStreamer regression: PASS
- libcamera discovery/capture: PASS (camera registered and single-frame capture completed)

Known caveat:
- On this platform, enabling frame-start events on Atom ISP returns -EINVAL; PoC uses a fallback path and capture proceeds.

Attached:
- completed PoC report form
- per-test logs from /tmp/atomisp_poc_test_*
- dmesg before/after delta

Published commits and local test-only delta are listed separately in the report metadata.

If preferred, I can rerun and post one more complete pass set before requesting detailed review.

Thanks,
Rik
```

## Draft B: Slightly More Context

Subject: [PATCH 0/0] AtomISP / MT9M114 libcamera PoC validation results

```text
Hi all,

I ran a PoC validation for atomisp/mt9m114 to check whether the kernel-side media contract needed for libcamera is in place.

Test environment:
- Hardware: ASUS T100TA
- Kernel: 7.0.0+
- Stack: current atomisp/mt9m114 branch, with minimal local test-enabling delta only

Summary:
- Media graph discovery and entity chain: PASS
- Format negotiation at 1280x720 and 1280x960: PASS
- Reprobe path (module unload/reload + stream after reprobe): PASS
- Baseline controls: PASS
- GStreamer pipeline behavior: PASS (clean EOS)
- Stream loop automation: PASS (gst fallback path on unsupported v4l2-ctl stream ioctls)
- libcamera discovery/capture: PASS (camera exposed and basic frame capture works)

Known caveat:
- Frame-start event enabling on Atom ISP returns -EINVAL on this platform; a fallback path is used and streaming continues.

I attached:
1) completed report form,
2) full per-test logs,
3) dmesg delta from the same run.

The report explicitly separates published upstream commits from local test-only changes.

The libcamera PoC gating item from the atomisp staging TODO now has a working proof-of-concept result on this hardware. Other staging TODO MUST items are tracked separately and are not claimed solved by this report.

Thanks,
Rik
```

## Optional Follow-Up Reply (if asked "why no libcamera camera yet?")

```text
Current results show a working PoC path for libcamera discovery and basic capture on the tested T100TA setup. This addresses the staging TODO requirement that PoC-level libcamera integration be ready before discussing staging exit. Other atomisp staging TODO MUST items remain open and are tracked separately.
```
