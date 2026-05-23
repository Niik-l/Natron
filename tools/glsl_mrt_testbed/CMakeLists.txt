# Phase 3 GLSL/MRT feasibility testbed.
# Standalone executable — not linked into Natron. Opt-in via:
#   cmake -DBUILD_GLSL_TESTBED=ON ..
#   mingw32-make glsl_mrt_testbed
# See main.cpp for what it does and how to interpret output.

cmake_minimum_required(VERSION 3.16)

# Qt6 OpenGL functions exposure
if(NATRON_QT6)
    find_package(Qt6 COMPONENTS Core Gui OpenGL REQUIRED)
    set(QT_LIBS Qt6::Core Qt6::Gui Qt6::OpenGL)
else()
    find_package(Qt5 COMPONENTS Core Gui OpenGL REQUIRED)
    set(QT_LIBS Qt5::Core Qt5::Gui Qt5::OpenGL)
endif()

# OIIO for EXR output (already a Natron dependency, so available).
find_package(OpenImageIO REQUIRED)

add_executable(glsl_mrt_testbed main.cpp)

# Force console subsystem on Windows so the standard `int main(...)` entry
# point is used (otherwise the linker looks for wWinMain when -municode is in
# play). We're a CLI test app, not a GUI app — we want stdout/stderr visible
# in the user's MSYS2 terminal.
if(WIN32)
    set_target_properties(glsl_mrt_testbed PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(glsl_mrt_testbed PRIVATE -mconsole)
endif()

target_link_libraries(glsl_mrt_testbed
    PRIVATE
        ${QT_LIBS}
        OpenImageIO::OpenImageIO
)

# C++17 to keep parity with Natron core build settings.
set_target_properties(glsl_mrt_testbed PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Place the binary directly under the testbed build dir, alongside its outputs.
set_target_properties(glsl_mrt_testbed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/glsl_mrt_testbed"
)
