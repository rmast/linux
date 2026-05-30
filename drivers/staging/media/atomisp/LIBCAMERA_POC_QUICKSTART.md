# Quick Start: Atomisp Libcamera PoC - Minimal Track

This directory now contains everything needed to validate the kernel contract for libcamera support.

## Files

1. **[LIBCAMERA_POC_PLAN.md](LIBCAMERA_POC_PLAN.md)**  
   The contract and strategy document. Read this first to understand what you're testing and why.

2. **[LIBCAMERA_POC_TEST_SCRIPT.sh](LIBCAMERA_POC_TEST_SCRIPT.sh)**  
   Executable bash script with all test matrix commands. Copy-pasteble or run as a whole.

3. **[LIBCAMERA_POC_TEST_LOG_TEMPLATE.md](LIBCAMERA_POC_TEST_LOG_TEMPLATE.md)**  
   Fill this out as you run tests. Provides structure for what to capture and how to report results.

## Your Quick Path (4-6 hours)

### Hour 0-0.5: Prep
1. Read [LIBCAMERA_POC_PLAN.md](LIBCAMERA_POC_PLAN.md) sections: Goal, Scope, Contract v1, Test Matrix.
2. Confirm contract wording makes sense for your hardware. Raise questions if not.

### Hour 0.5-3: Run Tests on Hardware
1. Install required tools:
   ```bash
   sudo apt-get install v4l-utils libmediainfo0 gstreamer1.0-tools libcamera-tools
   ```

2. Run the test script:
   ```bash
   bash drivers/staging/media/atomisp/LIBCAMERA_POC_TEST_SCRIPT.sh | tee my_test_run.log
   ```

3. Fill in [LIBCAMERA_POC_TEST_LOG_TEMPLATE.md](LIBCAMERA_POC_TEST_LOG_TEMPLATE.md) as you go.  
   (The script logs are already in `/tmp/atomisp_poc_test_*/`)

4. Capture kernel logs:
   ```bash
   dmesg > dmesg_final.txt
   ```

### Hour 3-6: Iterate on Failures
- If tests fail, share logs + the filled template.
- I'll propose minimal kernel fixes if needed.
- Re-run tests to confirm fixes.
- Repeat until all critical tests pass.

## Exit Criteria

Success = the contract is validated and you have working proof:
- [ ] Graph discovery shows expected entity chain (Test 1)
- [ ] Format negotiation works at two resolutions (Tests 2-3)
- [ ] Stream lifecycle is stable (Test 4)
- [ ] Standard V4L2 controls are accessible (Test 6)
- [ ] Libcamera can discover/capture (Test 8, if available)
- [ ] No kernel warnings/oops (dmesg clean)

## Next Steps After Success

Once the contract is validated:
1. A minimal libcamera handler PoC can be written (separate effort).
2. Results can be presented as proof toward the TODO MUST item.
3. Any remaining work is scoped for after staging exit (tuning, cleanup).

---

**Questions?** Review the plan document or share logs from failed tests.
