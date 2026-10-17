/* ARM has no musl fp_arch.h; this stub also supplies musl's internal
   `hidden` visibility macro (from src/include/features.h). Force-included. */
#ifndef RGNANO_FP_ARCH_H
#define RGNANO_FP_ARCH_H
#ifndef hidden
#define hidden __attribute__((__visibility__("hidden")))
#endif
#endif
