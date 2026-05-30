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
- Stream loop test: WARN (automation did not yield a clean pass counter)
- Module unload/reload and reprobe: PASS
- Control baseline: PASS
- GStreamer regression: PASS
- libcamera discovery: FAIL (no camera registered / Camera 0 not found)

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
- Stream loop automation: WARN (counter logic did not produce a clean pass number)
- libcamera discovery/capture: FAIL (no camera exposed to libcamera yet)

I attached:
1) completed report form,
2) full per-test logs,
3) dmesg delta from the same run.

The report explicitly separates published upstream commits from local test-only changes.

I am holding final submission until one more internal positive run, but I can submit this set as-is if that is preferred.

Thanks,
Rik
```

## Optional Follow-Up Reply (if asked "why no libcamera camera yet?")

```text
Current results show the kernel topology and V4L2/GStreamer path are mostly healthy, but libcamera still does not discover an operational camera on this stack. So this is not presented as full libcamera support yet; it is a status report showing what is validated and where the remaining gap is.
```
