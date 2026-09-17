/*
 * PlutoBrowser — Source/fenv.h (compatibility shim)
 * The Playdate ARM toolchain (newlib for cortex-m7) ships no <fenv.h>, but
 * the STOCK vendored QuickJS (Source/js/QuickJS/quickjs.c) opens it with
 * #include <fenv.h> — a vestigial include from the libbf era: the engine
 * uses ZERO fenv symbols (no fegetround/fesetround/FE_* anywhere in
 * Source/js/QuickJS). This intentionally near-empty header satisfies that
 * include so the untouched engine compiles on device. It also shadows the
 * host <fenv.h> for the QuickJS TU on the simulator, which is safe for the
 * same reason (nothing in the vendored code references fenv identifiers).
 *
 * Do NOT add fenv functionality here. The vendored engines under Source/js
 * are never modified; host code does not use fenv; if that ever changes,
 * revisit this shim (e.g. per-include-path placement) rather than editing
 * the engine.
 */
#ifndef PLUTO_COMPAT_FENV_H
#define PLUTO_COMPAT_FENV_H
/* empty by design — see comment above */
#endif /* PLUTO_COMPAT_FENV_H */
