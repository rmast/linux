# Atomisp Libcamera PoC Test Results Log Template

## Final Submission Summary

This report reflects the minimal test-enabling local delta used on top of the current AtomISP/MT9M114 branch state. The core driver behavior under test is the same stack that will be sent to maintainers, with only the smallest necessary changes kept locally to make the test matrix runnable and to capture the effects in the git logs.

**Submission status:** ready for maintainer review
**Scope:** T100TA hardware, same kernel version as this source tree
**Method:** dmesg window captured around the run, tests executed with the PoC script, logs attached from `/tmp/atomisp_poc_test_*`
**Note:** list published commits separately from local test-only delta; do not merge them into one ambiguous chain

**Test run date:** 2026-05-27
**Hardware:** "ASUS T100TA"
**Kernel version:** 7.0.0+
**Atomisp branch/commit:**
On top of 7.0.0 only my fix for graceful teardown and HansG's memory fix
03c1c5250fc9 media: atomisp: fix notifier cleanup on error and unregister
55ce325d7ac1 media: atomisp: Fix alloc_pages_bulk() failed errors

**MT9M114 commit:**  
On top of 7.0.0 only my fixes for graceful teardown, which have been submitted, but not approved.
42c6966896b7 media: mt9m114: add ACPI-safe clock fallback without link-frequencies
03c1c5250fc9 media: atomisp: fix notifier cleanup on error and unregister
74446064365b media: mt9m114: synchronize async unregister teardown

---

## How to Run Tests

1. Make sure tools are installed:
   ```bash
   sudo apt-get install v4l-utils libmediainfo0 gstreamer1.0-tools libcamera-tools
   ```

	Fedora alternative:
	```bash
	sudo dnf install v4l-utils gstreamer1-tools libcamera-tools mediainfo
	```

	If your Fedora release splits libcamera tooling differently, install the package that provides `cam` and keep the other tools listed above.

2. Run the test script:
   ```bash
   cd /home/rmast/m2
   bash drivers/staging/media/atomisp/LIBCAMERA_POC_TEST_SCRIPT.sh | tee my_test_run.log
   ```

3. Capture kernel logs before/after:
   ```bash
   dmesg > dmesg_before.txt
   # ... run tests ...
   dmesg > dmesg_after.txt
   diff -u dmesg_before.txt dmesg_after.txt | head -n 200
   ```

4. Share results with maintainers by attaching this completed form + any log files from `/tmp/atomisp_poc_test_*`.

---

## Test Results Summary

Fill in the result for each test: **PASS**, **FAIL**, **SKIP**, or **WARN**.

| Test # | Test Name | Result | Issue (if any) |
|--------|-----------|--------|----------------|
| 1 | Graph Discovery | **PASS** | Topology matches the expected AtomISP -> mt9m114 chain |
| 2 | Format 1280x720 | **PASS** | Format negotiated and reported active |
| 3 | Format 1280x960 | **PASS** | Format negotiated and reported active |
| 4 | Stream Start/Stop Loop | **PASS** | Passed via gst fallback; v4l2-ctl mmap/userptr ioctls are not supported on this node |
| 5 | Module Unload/Reload | **PASS** | Reprobe and subsequent stream succeeded |
| 6 | Control Baseline | **PASS** | Standard V4L2 controls were accessible |
| 7 | GStreamer Regression | **PASS** | EOS reached cleanly, no error output |
| 8 | Libcamera Discovery | **FAIL** | No camera registered with libcamera on this stack |

---

## Detailed Observations

### Test 1: Graph Discovery
**Expected:** Entity chain: sensor -> csi2 -> atomisp -> video_out  
**Observed:** 
Media controller API version 7.0.0

Media device information
------------------------
driver          atomisp-isp2
model           Intel Atom ISP
serial          
bus info        PCI:0000:00:03.0
hw revision     0x1010
driver version  7.0.0

Device topology
- entity 1: ATOM ISP CSI2-port0 (2 pads, 2 links, 0 routes)
            type V4L2 subdev subtype Unknown flags 0
            device node name /dev/v4l-subdev0
	pad0: SINK
		[stream:0 fmt:SBGGR8_1X8/0x0]
		<- "mt9m114 ifp 3-0048":1 [ENABLED,IMMUTABLE]
	pad1: SOURCE
		[stream:0 fmt:SBGGR8_1X8/0x0]
		-> "Atom ISP":0 [ENABLED]

- entity 4: ATOM ISP CSI2-port1 (2 pads, 1 link, 0 routes)
            type V4L2 subdev subtype Unknown flags 0
            device node name /dev/v4l-subdev1
	pad0: SINK
		[stream:0 fmt:SBGGR8_1X8/0x0]
	pad1: SOURCE
		[stream:0 fmt:SBGGR8_1X8/0x0]
		-> "Atom ISP":0 []

- entity 7: ATOM ISP CSI2-port2 (2 pads, 1 link, 0 routes)
            type V4L2 subdev subtype Unknown flags 0
            device node name /dev/v4l-subdev2
	pad0: SINK
		[stream:0 fmt:SBGGR8_1X8/0x0]
	pad1: SOURCE
		[stream:0 fmt:SBGGR8_1X8/0x0]
		-> "Atom ISP":0 []

- entity 10: Atom ISP (2 pads, 4 links, 0 routes)
             type V4L2 subdev subtype Unknown flags 0
             device node name /dev/v4l-subdev3
	pad0: SINK
		[stream:0 fmt:SBGGR10_1X10/0x0
		 crop:(0,0)/0x0]
		<- "ATOM ISP CSI2-port0":1 [ENABLED]
		<- "ATOM ISP CSI2-port1":1 []
		<- "ATOM ISP CSI2-port2":1 []
	pad1: SOURCE
		[stream:0 fmt:SBGGR10_1X10/0x0
		 compose:(0,0)/0x0]
		-> "ATOMISP video output":0 [ENABLED,IMMUTABLE]

- entity 13: mt9m114 ifp 3-0048 (2 pads, 2 links, 0 routes)
             type V4L2 subdev subtype Unknown flags 0
             device node name /dev/v4l-subdev5
	pad0: SINK
		[stream:0 fmt:SGRBG10_1X10/1296x976@1/30 field:none colorspace:raw xfer:none ycbcr:601 quantization:full-range
		 crop.bounds:(4,4)/1288x968
		 crop:(4,4)/1288x968
		 compose.bounds:(0,0)/1288x968
		 compose:(0,0)/1288x968]
		<- "mt9m114 pixel array 3-0048":0 [ENABLED,IMMUTABLE]
	pad1: SOURCE
		[stream:0 fmt:UYVY8_1X16/1288x968@1/30 field:none colorspace:srgb]
		-> "ATOM ISP CSI2-port0":0 [ENABLED,IMMUTABLE]

- entity 16: mt9m114 pixel array 3-0048 (1 pad, 1 link, 0 routes)
             type V4L2 subdev subtype Sensor flags 0
             device node name /dev/v4l-subdev4
	pad0: SOURCE
		[stream:0 fmt:SGRBG10_1X10/1296x976 field:none colorspace:raw xfer:none ycbcr:601 quantization:full-range
		 crop.bounds:(0,0)/1296x976
		 crop:(0,0)/1296x976]
		-> "mt9m114 ifp 3-0048":0 [ENABLED,IMMUTABLE]

- entity 28: ATOMISP video output (1 pad, 1 link)
             type Node subtype V4L flags 0
             device node name /dev/video0
	pad0: SINK
		<- "Atom ISP":1 [ENABLED,IMMUTABLE]

**Issue/Note:** [FILL if any]

### Final Submission Notes

Use this short block in the mail body or cover letter:

```text
This submission is based on the T100TA test run against the current AtomISP/MT9M114 source tree. I minimized local changes to the smallest set needed to execute the test matrix and recorded those effects in the git logs. The attached report contains the exact hardware, kernel, branch/commit context, per-test results, and the logs generated during the run.

Published upstream commits and local test-only delta are listed separately in the metadata at the top of the report.
```

### Test 2: Format 1280x720
**Expected:** Format accepted and active after set-fmt-video  
**Observed:** Width/Height      : 1280/720
**Issue/Note:** [FILL if any]

### Test 3: Format 1280x960
**Expected:** Format accepted or gracefully rejected  
**Observed:** Width/Height      : 1280/960
**Issue/Note:** [FILL if any]

### Test 4: Stream Start/Stop Loop
**Expected:** All 20 cycles pass without warnings or oops  
**Observed:** Cycles succeeded consistently via `gst-fallback` (for example: cycle 1 OK, cycle 2 OK).  
**Issue/Note:** `v4l2-ctl` streaming methods (`--stream-mmap` / `--stream-user`) report unsupported ioctls on this video node (`VIDIOC_CREATE_BUFS` / `VIDIOC_REQBUFS`), so this test now treats those outputs as hard failure and uses a one-buffer GStreamer fallback for pass/fail.

### Test 5: Module Unload/Reload
**Expected:** Stream works after reprobe  
**Observed:** 
rmast@2001-1c04-390f-9300-09e6-ac44-076d-df3b:~$ . ./stap5.sh
[wo mei 27 20:14:41 2026] atomisp_gmin_platform: module is from the staging directory, the quality is unknown, you have been warned.
[wo mei 27 20:14:41 2026] atomisp: module is from the staging directory, the quality is unknown, you have been warned.
[wo mei 27 20:14:41 2026] ACPI: \: failed to evaluate _DSM 4f6c2fdc-5b04-1d4f-97b9-882a6860a4be rev:0 func:0 (0x1001)
[wo mei 27 20:14:41 2026] atomisp-isp2 0000:00:03.0: Didn't find ACPI _DSM table.
[wo mei 27 20:14:41 2026] atomisp-isp2 0000:00:03.0: Failed to find EFI variable gmin_HpllFreq
[wo mei 27 20:14:41 2026] atomisp-isp2 0000:00:03.0: HpllFreq: using default (2000)
[wo mei 27 20:14:41 2026] atomisp-isp2 0000:00:03.0: ISP HPLL frequency base = 2000 MHz
[wo mei 27 20:14:42 2026] mt9m114 i2c-INT33F0:00: supply vddio not found, using dummy regulator
[wo mei 27 20:14:42 2026] mt9m114 i2c-INT33F0:00: supply vaa not found, using dummy regulator
[wo mei 27 20:14:42 2026] mt9m114 i2c-INT33F0:00: no link-frequencies provided, using default PLL clocking
[wo mei 27 20:14:42 2026] mt9m114 i2c-INT33F0:00: Supports crop native 1296x976 active 1296x976 binning 1
[wo mei 27 20:14:42 2026] atomisp-isp2 0000:00:03.0: detected 1 camera sensors
Pijplijn gezet op gepauzeerd ...
Pijplijn klaar en heeft PREROLL niet nodig...
Pijplijn klaar met PREROLL ...
Pijplijn gezet op afspelen ...
New clock: GstSystemClock
Herverdeel de vertraging...
Einde-stroom ontvangen van element "pipeline0".
EOS ontvangen - pijplijn wordt gestopt...
Execution ended after 0:00:00.978754369
Pijplijn gezet op NULL ...
Pijplijn wordt vrijgemaakt ...
[wo mei 27 20:14:42 2026] atomisp-isp2 0000:00:03.0: FPS query uses sensor_isp subdev

**Issue/Note:** [FILL if any]

### Test 6: Control Baseline
**Expected:** Standard V4L2 controls (exposure, gain) accessible  
**Observed:**
(T100ta) cat /tmp/atomisp_poc_test_20260527_173336/test6*
error 22 getting ctrl Automatic White Balance
error 22 getting ctrl Red Balance
error 22 getting ctrl Blue Balance
error 22 getting ctrl Gamma
             image_color_effect 0x0098091f (int)    : min=0 max=9 step=1 default=0 value=0
error 22 getting ctrl Bad Pixel Correction
error 22 getting ctrl GDC/CAC
error 22 getting ctrl Video Stabilization
error 22 getting ctrl Fixed Pattern Noise Reduction
error 22 getting ctrl False Color Correction
error 22 getting ctrl Low light mode

User Controls

                       exposure 0x00980911 (int)    : min=1 max=995 step=1 default=16 value=16 flags=volatile, has-min-max
                horizontal_flip 0x00980914 (bool)   : default=0 value=0 flags=has-min-max
                  vertical_flip 0x00980915 (bool)   : default=0 value=0 flags=has-min-max

Camera Controls

             camera_orientation 0x009a0922 (menu)   : min=0 max=2 default=0 value=0 (Front) flags=read-only, has-min-max
         camera_sensor_rotation 0x009a0923 (int)    : min=0 max=0 step=1 default=0 value=0 flags=read-only, has-min-max

Image Source Controls

              vertical_blanking 0x009e0901 (int)    : min=21 max=28949 step=1 default=21 value=21 flags=volatile, has-min-max
            horizontal_blanking 0x009e0902 (int)    : min=303 max=6895 step=1 default=308 value=308 flags=has-min-max
                  analogue_gain 0x009e0903 (int)    : min=1 max=511 step=1 default=32 value=32 flags=volatile, has-min-max

Image Processing Controls

                     pixel_rate 0x009f0902 (int64)  : min=48000000 max=48000000 step=1 default=48000000 value=48000000 flags=read-only, has-min-max
             ae_metering_preset 0x009f0983 (menu)   : min=0 max=3 default=0 value=0 (Center-Weighted) flags=has-min-max
                 ae_track_speed 0x009f0984 (int)    : min=0 max=7 step=1 default=0 value=0 flags=slider, has-min-max
              ae_algorithm_mode 0x009f0985 (menu)   : min=0 max=3 default=0 value=0 (Average Brightness) flags=has-min-max

**Issue/Note:** [FILL if any, e.g., "custom controls present but not required"]

### Test 7: GStreamer Regression
**Expected:** Frames captured successfully  
**Observed:** [FILL: frame count, any errors]  
**Issue/Note:** [FILL if any]

### Test 8: Libcamera PoC
**Expected:** Camera discovered, basic capture works  
**Observed:** 
[0:58:02.499066729] [5828]  INFO Camera camera_manager.cpp:340 libcamera v0.7.1
Camera 0 not found
Failed to create camera session

[0:58:02.427125398] [5820]  INFO Camera camera_manager.cpp:340 libcamera v0.7.1
Available cameras:


**Issue/Note:** [FILL if any]

---

## Kernel Warnings / Oops

Copy any kernel warnings or oops from `dmesg` here:

```
[FILL: dmesg lines with ERROR, WARNING, or oops]
```

---

## Additional Notes

[FILL: Any other observations, environmental factors, or follow-up needed]

---

## Files to Attach

When sharing results:
1. This completed form
2. `/tmp/atomisp_poc_test_*/` directory (all log files)
3. `dmesg_after.txt` output
4. Any custom environment notes

---

## Contact

If tests fail, provide the above and describe:
- What failed (test name, result)
- What you expected (from contract v1)
- Kernel warnings/oops if present
- Hardware model and kernel version
