# Vendored cgltf v1.15 — stb-style glTF 2.0 loader and writer.
# https://github.com/jkuhlmann/cgltf
# License: MIT — see LICENSE.

set(cgltf_SOURCES cgltf.c)
add_library(cgltf STATIC ${cgltf_SOURCES})
target_include_directories(cgltf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(cgltf PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)
# Suppress warnings from third-party code.
if(MSVC)
    target_compile_options(cgltf PRIVATE /w)
else()
    target_compile_options(cgltf PRIVATE -w)
endif()
