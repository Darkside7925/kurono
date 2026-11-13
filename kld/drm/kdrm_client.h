// in-container drm client: the translator's first half (d0.4 architecture B).
// driver-agnostic: everything here uses the upstream drm_client API and dumb
// buffers, so cirrus today and i915 later take the same path. (satoru)
//
// end (satoru)

#pragma once

struct drm_device;

// attach a client to a registered device and commit the preferred mode with
// one dumb framebuffer. returns 0 or a negative errno. (satoru)
int kdrm_client_setup(struct drm_device *dev);

// d1 verification only: draw the deterministic pattern, flush it, hold it for
// a screendump, then run 120 moving-block frames. (satoru)
int kdrm_client_test_pattern(void);

// d2: attach the display bridge, report the mode list and start serving the
// kernel's damage frames. returns 0 when the bridge owns the client, negative
// when the boot flag is off (caller falls back to the d1 pattern). (satoru)
int kdrm_client_serve_enable(void);

// end (satoru)
