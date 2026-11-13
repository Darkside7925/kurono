# Intel GPU path

The Intel integrated GPU and its display outputs, from boot to the first frame. The files are in
`src/drivers/`: `intel_display_probe`, `intel_dp_aux` and `intel_dp_lt` (DisplayPort over
USB-C and AUX), `intel_modeset` and `intel_modeset_calc` (pipes and modes), `intel_gt` and
`intel_gt_calc` (the render engine and blits), `intel_accel` (the acceleration switch), and the
`*_hosttest.cpp` files, which run the same code on the host for tests.

## Boot order

1. `intel_display_probe` reads the display engine state the firmware left behind.
2. Stage 1, `IntelDpAux::Run`, reads DPCD and EDID over the DP AUX channel on the ports the
   firmware already powers. It logs `[igpu-aux]` lines.
3. Stage 2, `IntelModeset::Boot`, lights an external DP sink that stage 1 found on a USB-C port,
   on pipe B.
4. `IntelAccel::Boot` starts the blit engine when the display has been set up.

## Flags

| Flag | Default | What |
| --- | --- | --- |
| `kurono.igpu.aux=0` | stage 1 on | skip the AUX probe |
| `kurono.igpu.modeset=0` | stage 2 on | skip the external sink modeset |
| `kurono.igpu.tclegacy=1` | off | the legacy Type-C probe in stage 1 |
| `kurono.igpu.legacytc=1` | off | keep a legacy-mode Type-C port connected for stage 2, following the Linux 7.1.6 `intel_tc.c` connect and disconnect steps |
| `kurono.igpu.accel=1` | off | start the blit engine |
| `kurono.igpu.accel.wmcalc=1` | off | the watermark calculation variant (`IntelAccel::SetWmCalc`) |
| `kurono.igpu.zerocopy=1` | off | zero-copy present |
| `kurono.gfx.scissor=1` | off | partial redraw: only the damaged rectangles are copied to the front buffer |
| `kurono.gfx.scissorcheck=N` | 24 | with the scissor on, compare the scanned frame with the back buffer on N presents, and turn the scissor off on a mismatch |
| `kurono.gfx.oracle=1` | off | the damage oracle (below) |

A KUROMODE.OFF file in the KURONO partition root also skips stage 2.

## The damage oracle

`src/ui/gfx_oracle.cpp` checks partial redraws against full redraws. A scripted desktop plays
through the real input path: menus, a context menu, a window whose content changes, a drag over
it, the control center, a toast and a UI scale change. At checkpoints, once the scene is still,
the whole frame is rendered again with the default renderer. It is compared pixel for pixel with
the back buffer the partial frames left, and with what reached the screen.

The result is printed as `[oracle] summary:` with the checkpoint count and the mismatch counts,
front and back, and the visible subset. `kurono.gfx.oracle.soak=N` takes the number of seconds the
scenario runs. The oracle does not touch a display register.

## Results

- The blit batch ordering was wrong: a blit that read or overwrote an earlier blit's output in the
  same batch could run before that write landed. A `MI_FLUSH_DW` between them fixed it, and it
  drew wrong pixels on the real iGPU before.
- Stale column and window-shadow damage leaks are fixed.
- With the scissor, overlapping damage rectangles are merged, and the 60 percent sum rule is gone.
  A window drag moved 49 MB/s to the front buffer, down from 184 MB/s. The oracle reported zero
  mismatches for that scenario.

## Files

- `src/drivers/intel_display_probe.cpp`, `intel_dp_aux.cpp`, `intel_dp_lt.cpp`
- `src/drivers/intel_modeset.cpp`, `intel_modeset_calc.cpp`
- `src/drivers/intel_gt.cpp`, `intel_gt_calc.cpp`, `intel_accel.cpp`
- `src/ui/gfx_oracle.cpp` and `gfx_oracle.h` the damage oracle
- `src/kernel/kurono_kernel.cpp` the flags above, parsed at boot
