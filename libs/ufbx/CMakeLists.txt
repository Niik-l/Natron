# Vendored ufbx v0.23.1 — single source file FBX loader.
# https://github.com/ufbx/ufbx
# License: MIT or Public Domain (unlicense.org) — see LICENSE.

add_library(ufbx STATIC ufbx.c)
target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(ufbx PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)
# Suppress warnings from third-party code.
if(MSVC)
    target_compile_options(ufbx PRIVATE /w)
else()
    target_compile_options(ufbx PRIVATE -w)
endif()
