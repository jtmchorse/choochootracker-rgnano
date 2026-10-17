Vendored from musl 1.2.1 (MIT, see COPYRIGHT): src/math exp, expf, exp2, exp2f,
pow, powf, their data tables, the __math_* error helpers, and src/internal/libm.h.
Only fp_arch.h is ours (stub plus the `hidden` macro, force-included). Linked into
the RG Nano executable only (Makefile.rgnano) so these definitions win over the
FunKey SDK libc.so, whose exp/expf/pow/powf return wrong values. Do not edit the
musl files.
