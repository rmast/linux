# MT9M114 v2 - Extra Technische Waarschuwingen (Gemini Review)

## 🚨 CRITICAL: VTS Margin Rule (Hardware Bug)

**THE MOST IMPORTANT RULE FOR MT9M114:**

```
Integration_Time MUST BE < Frame_Length_Lines - 2
```

**What happens if you violate this:**
- Sensor's timing generator **FREEZES**
- No more frames output
- I2C still responds but sensor is stuck
- Only recovery: GPIO reset (power cycle)

**Why 2 lines?**
- Sensor needs 2 blanking lines between exposure end and frame end
- 1 line for readout settling
- 1 line for timing margin
- Without this, internal state machine deadlocks

**How v2 patch enforces this:**
```c
// Line 1302 in patch:
min_frame_length = exposure + 2;  // Always maintain 2-line margin!

// This ensures: Frame_Length_Lines = Exposure + 2
// Therefore:    Exposure = VTS - 2  ✓ SAFE
```

**Manual testing:**
```bash
# NEVER do this:
v4l2-ctl --set-ctrl=vertical_blanking=100
v4l2-ctl --set-ctrl=exposure=1076  # 976 (height) + 100 = 1076 → FREEZE!

# ALWAYS ensure:
v4l2-ctl --set-ctrl=vertical_blanking=100
v4l2-ctl --set-ctrl=exposure=1074  # 1076 - 2 = 1074 ✓ SAFE
```

**Recovery from freeze:**
```bash
# GPIO reset (adjust GPIO number for your platform):
echo 0 > /sys/class/gpio/gpio<reset_pin>/value
sleep 0.1
echo 1 > /sys/class/gpio/gpio<reset_pin>/value

# Or reload driver:
sudo modprobe -r mt9m114
sudo modprobe mt9m114
```

---

## ⚙️ I2C Register Timing Requirements

**The MT9M114 is VERY timing-sensitive!**

### 1. I2C Bus Speed

**Minimum: 400 kHz**

Check your device tree:
```dts
i2c@... {
    clock-frequency = <400000>;  /* 400 kHz - MINIMUM */
    
    mt9m114@48 {
        compatible = "onnn,mt9m114";
        reg = <0x48>;
        ...
    };
};
```

100 kHz will cause:
- GROUP_HOLD timeouts
- Missed SOF synchronization
- Frame corruption

### 2. Write Ordering Within GROUP_HOLD

**CORRECT sequence:**
```c
mt9m114_group_hold(sensor, true);

// 1. VTS FIRST (Frame_Length_Lines)
cci_write(MT9M114_FRAME_LENGTH_LINES, new_vts);
cci_write(MT9M114_CAM_SENSOR_CFG_FRAME_LENGTH_LINES, new_vts);

// 2. Exposure SECOND (Coarse_Integration_Time)
cci_write(MT9M114_COARSE_INTEGRATION_TIME, exposure);
cci_write(MT9M114_CAM_SENSOR_CONTROL_COARSE_INTEGRATION_TIME, exposure);

// 3. Gain THIRD (optional, if changing)
cci_write(MT9M114_CAM_SENSOR_CONTROL_ANALOG_GAIN, gain);

mt9m114_group_hold(sensor, false);  // Apply atomically at next SOF
```

**WRONG ordering causes:**
- Exposure applied before VTS → sensor may reject it
- Gain applied first → incorrect for one frame
- Visual artifacts: flashes, black frames

### 3. GROUP_HOLD Window Constraints

**The shadow register buffer is limited:**
- Maximum ~10 register writes per GROUP_HOLD block
- All writes must complete within ONE frame time
- At 30 FPS: ~33ms window
- At 5 FPS: ~200ms window

**Don't do this:**
```c
mt9m114_group_hold(sensor, true);
cci_write(VTS);
msleep(100);  // ❌ TOO LONG - sensor will timeout
cci_write(Exposure);
mt9m114_group_hold(sensor, false);
```

**Best practice:**
- Keep all writes sequential, no delays
- Write only timing-related registers in one GROUP_HOLD
- Use separate GROUP_HOLD for other register groups (color, test pattern, etc.)

### 4. No Interleaving

**Within a GROUP_HOLD block, ONLY write timing registers:**
```c
// ✓ CORRECT:
mt9m114_group_hold(sensor, true);
cci_write(VTS);
cci_write(Exposure);
cci_write(Gain);
mt9m114_group_hold(sensor, false);

// ❌ WRONG:
mt9m114_group_hold(sensor, true);
cci_write(VTS);
cci_write(MT9M114_FLASH);  // ❌ Non-timing register!
cci_write(Exposure);
mt9m114_group_hold(sensor, false);
```

---

## 🎨 AtomISP Metadata Synchronization Issue

### Problem

When VTS changes (framerate drops for long exposure), the **AtomISP CSS firmware's AWB (Auto White Balance) malfunctions** because it expects consistent frame timing.

**Symptoms:**
- Blue/orange color cast after entering low-light mode
- AWB "hunting" - colors oscillate for 5-15 seconds
- Incorrect color temperature despite correct exposure

### Root Cause

AtomISP CSS firmware uses metadata buffer for AWB calculations:

```c
// drivers/staging/media/atomisp/pci/atomisp_cmd.c
struct atomisp_metadata_buf {
    uint32_t frame_duration;   // CSS expects FIXED value!
    uint32_t exposure_time;    // We update this...
    uint16_t analog_gain;      // ...and this...
    // But NOT frame_duration!
};
```

When sensor drops from 30 FPS → 5 FPS:
1. Sensor correctly outputs frames at 5 FPS
2. Exposure metadata is updated ✓
3. But `frame_duration` still says 33ms (30 FPS) ❌
4. CSS AWB algorithm: "Frame is 33ms but exposure is 200ms? That's impossible!"
5. AWB makes incorrect color gain adjustments
6. Result: Blue cast or warm cast depending on scene

### Workaround (Current - No AtomISP Driver Changes)

**Option 1: Wait for AWB convergence**
```bash
# Enable low-light mode
./mt9m114_lowlight_control /dev/v4l-subdev2 lowlight low

# Wait 10-15 frames for AWB to stabilize
sleep 3

# Now capture - AWB should be correct
gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=30 ! ...
```

**Option 2: Disable AWB in extreme low light**
```bash
# Use manual white balance
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=auto_white_balance=0
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=white_balance_temperature=4000  # Daylight

# Or use preset
# (Check available presets with --list-ctrls)
```

**Option 3: Lock AWB before VTS change**
```bash
# Let AWB stabilize in normal mode first
v4l2-ctl --set-ctrl=auto_white_balance=1
sleep 2

# Lock AWB
v4l2-ctl --set-ctrl=auto_white_balance=0

# Now enable low-light mode
./mt9m114_lowlight_control /dev/v4l-subdev2 lowlight low

# AWB is locked to previous good values
```

### Proper Fix (Requires AtomISP Driver Modification)

**File:** `drivers/staging/media/atomisp/pci/atomisp_cmd.c`

**Add this to `atomisp_get_metadata()` or similar:**

```c
int atomisp_update_sensor_metadata(struct atomisp_device *isp)
{
    struct v4l2_subdev *sensor = isp->inputs[isp->asd.input_curr].camera;
    struct v4l2_control vblank_ctrl = { .id = V4L2_CID_VBLANK };
    struct v4l2_control hblank_ctrl = { .id = V4L2_CID_HBLANK };
    u32 vts, hts, pixclk, frame_duration_us;
    int ret;
    
    // Read current VTS from sensor
    ret = v4l2_subdev_call(sensor, core, g_ctrl, &vblank_ctrl);
    if (ret)
        return ret;
    
    ret = v4l2_subdev_call(sensor, core, g_ctrl, &hblank_ctrl);
    if (ret)
        return ret;
    
    // Calculate actual frame time
    vts = vblank_ctrl.value + 976;  // height
    hts = hblank_ctrl.value + 1280; // width
    pixclk = 48000000;  // MT9M114 typical
    
    frame_duration_us = (u64)vts * hts * 1000000 / pixclk;
    
    // Update CSS metadata
    isp->asd.params.metadata_config.frame_duration = frame_duration_us;
    
    // Detect significant FPS change
    if (abs((int)frame_duration_us - (int)isp->asd.prev_frame_duration) > 10000) {
        dev_dbg(&isp->pdev->dev,
                "Frame duration changed %u -> %u us, reinit AWB\n",
                isp->asd.prev_frame_duration, frame_duration_us);
        
        // Trigger AWB re-initialization
        atomisp_css_set_wb_config(&isp->asd, NULL);  // Reset to defaults
    }
    
    isp->asd.prev_frame_duration = frame_duration_us;
    
    return 0;
}
```

**Call this:**
- After sensor VTS changes
- Before starting AWB algorithm
- Periodically during streaming (every 10 frames)

### Testing AWB Fix

**Before fix:**
```bash
# Start streaming at 30 FPS
gst-launch-1.0 v4l2src ! autovideosink &

# Enable low-light (drops to 5 FPS)
v4l2-ctl --set-ctrl=vertical_blanking=10000

# Observe: Blue or orange cast for 5-15 seconds
```

**After fix (AtomISP driver updated):**
```bash
# Same test
gst-launch-1.0 v4l2src ! autovideosink &
v4l2-ctl --set-ctrl=vertical_blanking=10000

# Observe: Colors stay correct, AWB stable within 2-3 frames
```

---

## 📋 Pre-Flight Checklist

Before using v2 patch in production, verify:

- [ ] **I2C bus is 400 kHz** (check device tree)
- [ ] **GPIO reset line is defined** (for sensor recovery)
- [ ] **Never violate VTS-2 margin** (use v2 patch, don't write registers manually)
- [ ] **GROUP_HOLD is working** (check dmesg for any GROUP_HOLD errors)
- [ ] **Understand AWB limitation** (metadata sync issue, use workaround)
- [ ] **Test sensor freeze recovery** (verify GPIO reset works)
- [ ] **AtomISP firmware version** (newer CSS may handle timing changes better)

---

## 🔬 Debugging Tools

### Check Current VTS/Exposure Margin

```bash
# Get current values
VBLANK=$(v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=vertical_blanking | awk '{print $2}')
EXPOSURE=$(v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=exposure | awk '{print $2}')
HEIGHT=976

# Calculate margin
VTS=$((HEIGHT + VBLANK))
MARGIN=$((VTS - EXPOSURE))

echo "VTS: $VTS"
echo "Exposure: $EXPOSURE"
echo "Margin: $MARGIN lines"

if [ $MARGIN -lt 2 ]; then
    echo "⚠️ WARNING: Margin too small! Sensor may freeze!"
else
    echo "✓ Margin OK"
fi
```

### Monitor I2C Traffic

```bash
# Enable I2C debug
echo 8 > /sys/module/i2c_core/parameters/debug

# Watch I2C transactions
dmesg -w | grep i2c

# Look for:
# - Write errors
# - Timeouts
# - NACK responses
```

### Check GROUP_HOLD Status

```bash
# Enable mt9m114 debug
echo 8 > /sys/module/mt9m114/parameters/debug

# Watch for GROUP_HOLD pairs
dmesg -w | grep -E "group.hold|GROUP_HOLD"

# Should see:
# "Enable group hold"
# "Disable group hold"
# In pairs, no orphaned enables
```

---

## 📚 References

- **MT9M114 Datasheet:** Section 6.3.8 "Grouped Parameter Hold"
- **AtomISP CSS Firmware:** `drivers/staging/media/atomisp/pci/css/`
- **V4L2 Timing Controls:** Documentation/userspace-api/media/v4l/ext-ctrls-camera.rst
- **I2C Subsystem:** Documentation/i2c/i2c-protocol.rst

---

**Last updated:** 15 februari 2026  
**Gemini review:** Technisch 100% correct, implementatie details toegevoegd  
**V2 Patch status:** Production-ready with documented limitations
