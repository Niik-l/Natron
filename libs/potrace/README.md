Potrace 1.16 (vendored subset)
==============================

Peter Selinger's Potrace bitmap-tracing library, GPL-2.0-or-later.
Vendored from https://potrace.sourceforge.net/download/1.16/potrace-1.16.tar.gz
(file set = src/Makefile.am `libsources`: the library only, no CLI/backends).

Used by the TraceToMask node (Engine/Dev/Mask/parsers/PotraceTrace.*). The
wrapper converts a Natron float plane into potrace_bitmap_t (1 bit per pixel),
runs potrace_trace(), and converts potrace_path_t contours + holes into
VectorMask::VectorShape Beziers.

Natron is GPL-2.0, so the licence is compatible. Local modifications are
marked with "NATRON:" comments; otherwise keep this directory byte-identical
to upstream 1.16.
