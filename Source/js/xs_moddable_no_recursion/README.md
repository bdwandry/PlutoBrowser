# XS (Moddable) 9.5.0 — vendored, STOCK

This directory holds the **XS JavaScript engine** (Moddable Open Source,
release 9.5.0), vendored **byte-identical** from the official release
tarball `https://github.com/Moddable-OpenSource/moddable/archive/refs/tags/9.5.0.tar.gz`.

## What lives here

Only the pieces the PlutoBrowser builds, straight from the tarball:

- `xs/sources/` — the XS engine sources (44 `.c` files + headers, plus
  `xsmc.c`) and `xs/tools/xst.h`-style platform glue. The build compiles
  them **per-file**, exactly like upstream's own `xst` tool
  (`xs/makefiles/lin/xst.mk`): `-DINCLUDE_XSPLATFORM
  -DXSPLATFORM="xs_platform.h"` plus xst's source list, minus the tools
  (xst.c, fdlibm, yaml, text*).
- `xs/includes/`, `xs/platforms/` — the public API headers and the
  platform layer that routes to the host's platform header.

**Nothing in this tree has been modified or will ever be modified.**
All PlutoBrowser-specific adaptation lives OUTSIDE:

- `Source/html/xs_platform.h` — the `XSPLATFORM` header (libc env,
  mxUseDefault* switches, mxMachinePlatform, no threads, mxMetering,
  mx32bitID, footprint switches).
- `Source/html/jsbridge_xs.c` — the engine bridge: defines the four
  host-provided platform functions (`fxAbort` containment via
  `fxExitToHost`, `fxCStackLimit`, `fxQueuePromiseJobs`, machine
  platform no-ops) and implements the browser's DOM/globals surface.

## Engine selection

`about:config`-style Settings row "Javascript Engine" cycles
muJS → Duktape → QuickJS → XS (Moddable); storage `jsEngine=3` selects
XS and the router (`jsbridge.c`) maps it to `js_engine_xs`. Pages run
exclusively on the chosen engine — no cross-engine binaries, no shared
engine state.

## Updating the engine

Re-download the release tarball and re-extract `xs/` here. Because the
engine is stock and every hook is external, an update is a drop-in.
