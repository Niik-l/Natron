# Potrace 1.16 - vendored for TraceToMask
#
# Peter Selinger's Potrace bitmap tracing library, GPL-2.0-or-later.
# Source: https://potrace.sourceforge.net/download/1.16/potrace-1.16.tar.gz
# Only the library sources (src/Makefile.am: libsources) are vendored - the
# command-line front end and file-format backends are not used. Natron is
# GPL-2.0, so the licence is compatible. Keep this copy in sync with upstream
# only deliberately; local fixes should be marked with "NATRON:" comments.

set(potrace_HEADERS
        auxiliary.h
        bitmap.h
        bitops.h
        curve.h
        decompose.h
        lists.h
        potracelib.h
        progress.h
        trace.h
)
set(potrace_SOURCES
        curve.c
        decompose.c
        potracelib.c
        trace.c
)
add_library(potrace STATIC ${potrace_HEADERS} ${potrace_SOURCES})
target_include_directories(potrace PUBLIC .)
target_compile_definitions(potrace PRIVATE VERSION="1.16")
if(MSVC)
    # The sources are plain C89 with POSIX-ish helpers guarded off; nothing to
    # add on MSVC beyond keeping the word_t bitmap helpers quiet.
    target_compile_definitions(potrace PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(potrace PROPERTIES POSITION_INDEPENDENT_CODE ON)
