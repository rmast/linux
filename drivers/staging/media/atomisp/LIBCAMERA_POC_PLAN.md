# Atomisp Libcamera PoC Plan (Minimal Track)

## Goal

Address the MUST item in TODO about libcamera support by defining a stable
kernel media-topology contract and validating it with a small PoC test matrix.

This plan intentionally does not depend on MT9M114 custom controls added in
commit 17881d1ef45ad3a053307b226a7571682b32680f.

## Scope

In scope:
- Stable media graph behavior for atomisp + mt9m114
- Stable pad-format behavior across pipeline entities
- PoC-level proof that libcamera can discover and use the pipeline

Out of scope:
- Final removal of all legacy atomisp private ABI
- Image-quality tuning for difficult backlit daylight scenes
- Full upstream acceptance criteria

## Contract v1 (Kernel-Side)

The following behavior is the contract that libcamera can rely on.

1. Topology contract
- Exactly one sensor path is enabled for active capture at a time.
- Data flow is sensor source pad -> CSI2 sink/source -> atomisp subdev sink/source -> video node sink.
- Link enable/disable changes are deterministic when input changes.
- Reprobe/remove cycles do not leave stale notifier links.

2. Pad/format contract
- Sink format negotiation is accepted at sensor side and propagated forward.
- Effective active format at each next stage is queryable after set_fmt.
- Width/height padding rules are stable and documented by behavior.
- Attempting unsupported codes/sizes fails with stable errno semantics.

3. Controls contract (PoC baseline)
- Libcamera PoC should rely on standard V4L2 controls first:
  - V4L2_CID_EXPOSURE_AUTO
  - V4L2_CID_EXPOSURE
  - V4L2_CID_ANALOGUE_GAIN
  - V4L2_CID_VBLANK
  - V4L2_CID_HBLANK
  - V4L2_CID_PIXEL_RATE
  - V4L2_CID_LINK_FREQ (when present)
- MT9M114 custom controls are explicitly non-required for PoC success.

4. Lifecycle contract
- Stream start/stop, module unload, and reprobe are race-safe for async notifier paths.
- No stale entity/link state remains after teardown.

## Minimal Deliverables

1. Contract note in tree (this file).
2. Optional kernel adjustments only where contract is still ambiguous.
3. Test evidence bundle:
- media-ctl topology dumps before and after stream.
- v4l2-ctl format/control traces.
- libcamera PoC run logs.

## Test Matrix (PoC)

Pass criteria for each test: no kernel warning/oops, deterministic graph state,
expected stream behavior.

1. Graph discovery
- media-ctl -p
- Verify expected entity chain and exactly one active input path.

2. Format negotiation
- Set 1280x720 and 1280x960.
- Confirm negotiated formats on each relevant pad.

Note: VGA/640x480 is intentionally not a PoC requirement for this minimal
track. On current atomisp + mt9m114 integration, VGA-sized requests can run
into a known padding/summing mismatch, which is a separate sizing-contract
problem rather than a prerequisite for libcamera topology proof.

3. Stream lifecycle
- Start/stop stream 20x in a loop.
- Unload/reload + stream again.

4. Control baseline
- Read and set standard exposure controls via subdev/video where applicable.
- Confirm no hard dependency on MT9M114 custom controls.

5. Libcamera PoC smoke
- Run a basic libcamera capture/preview command.
- Confirm stream starts, frames arrive, and stop is clean.

6. Regression sanity
- Keep existing gst-launch path working as fallback sanity check.

## Effort Split

Estimated user effort for this minimal track: 4-6 hours.

User tasks:
- Run the test matrix on hardware
- Share logs for failed cases
- Confirm acceptance of contract wording

Assistant tasks:
- Propose/prepare targeted kernel deltas when a matrix case fails
- Refine contract wording to remove ambiguity
- Prepare a compact status summary against TODO MUST item

## Exit Signal For This MUST Item

For staging TODO purposes, this track is considered successful when:
- The topology and pad-format contract above is validated by logs
- A libcamera PoC can capture using the defined contract
- Remaining work is clearly outside PoC scope (for example, final quality tuning)
