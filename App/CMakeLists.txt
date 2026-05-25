# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <https://natrongithub.github.io/>,
# (C) 2018-2023 The Natron developers
# (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
#
# Natron is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Natron is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****

set(Natron_SOURCES NatronApp_main.cpp)
if(WIN32)
    list(APPEND Natron_SOURCES ../Natron.rc)
endif()
# Optional local-only crash handler. Untracked file (kept out of git via
# .git/info/exclude). Auto-disabled if the file isn't present, so this is
# safe to commit and have a no-op for everyone else.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/DevCrashHandler.cpp")
    list(APPEND Natron_SOURCES DevCrashHandler.cpp)
endif()
add_executable(Natron ${Natron_SOURCES})
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/DevCrashHandler.cpp")
    target_compile_definitions(Natron PRIVATE NATRON_DEV_CRASH_LOG)
    if(WIN32)
        target_link_libraries(Natron PRIVATE dbghelp psapi)
    endif()
endif()

# Copy bundled PyPlugs into the runtime plugin search path. Natron's runtime
# loader scans <CMAKE_BINARY_DIR>/Plugins/PyPlugs/ (relative to the executable
# parent), NOT Gui/Resources/PyPlugs/ where the source files live. Without
# this step, the built-in PyPlugs (AngleBlur, DropShadow, EdgeBlur, Fill,
# Glow, LightWrap, PIKColor, SplitAndJoin, ZMask, ZRemap) are silently absent
# in dev builds — and any community PyPlug that calls `createNode("fr.inria.ZRemap")`
# crashes with `'NoneType' object has no attribute 'setScriptName'` without
# mentioning ZRemap or PyPlugs anywhere in the error.
file(GLOB BUILTIN_PYPLUG_FILES
    "${CMAKE_SOURCE_DIR}/Gui/Resources/PyPlugs/*.py"
    "${CMAKE_SOURCE_DIR}/Gui/Resources/PyPlugs/*.png"
    "${CMAKE_SOURCE_DIR}/Gui/Resources/PyPlugs/*.svg"
    "${CMAKE_SOURCE_DIR}/Gui/Resources/PyPlugs/*.xml")
add_custom_command(TARGET Natron POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Plugins/PyPlugs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BUILTIN_PYPLUG_FILES} "${CMAKE_BINARY_DIR}/Plugins/PyPlugs/"
    COMMENT "Copying built-in PyPlugs to ${CMAKE_BINARY_DIR}/Plugins/PyPlugs/")
if(APPLE)
    set_target_properties(Natron PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
        MACOSX_BUNDLE_INFO_STRING "${PROJECT_NAME} Copyright (c) 2023 The Natron Developers"
        MACOSX_BUNDLE_ICON_FILE Icon.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_NAME}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2023 The Natron Developers"
    )
endif()
if(NATRON_CYCLES)
    # Cycles static libraries
    set(CYCLES_LIBS
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_scene.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_session.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_integrator.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_bvh.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_subd.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_graph.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_device.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_kernel.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libcycles_util.a
        ${NATRON_CYCLES_BUILD_DIR}/lib/libextern_sky.a
    )
    find_library(EMBREE_LIB embree4 HINTS /c/msys64/mingw64/lib)
    find_library(OIDN_LIB OpenImageDenoise HINTS /c/msys64/mingw64/lib)
    find_library(TBB_LIB tbb12 HINTS /c/msys64/mingw64/lib)
    find_library(OSD_GPU_LIB osdGPU HINTS /c/msys64/mingw64/lib)
    find_library(OSD_CPU_LIB osdCPU HINTS /c/msys64/mingw64/lib)
    find_library(PUGIXML_LIB pugixml HINTS /c/msys64/mingw64/lib)
    find_library(ZSTD_LIB zstd HINTS /c/msys64/mingw64/lib)
    find_library(OIIO_LIB OpenImageIO HINTS /c/msys64/mingw64/lib)
    find_library(OIIO_UTIL_LIB OpenImageIO_Util HINTS /c/msys64/mingw64/lib)
    find_library(OCIO_LIB OpenColorIO HINTS /c/msys64/mingw64/lib)
    find_library(OPENEXR_LIB OpenEXR HINTS /c/msys64/mingw64/lib)
    find_library(OPENEXRCORE_LIB OpenEXRCore HINTS /c/msys64/mingw64/lib)
    find_library(IEX_LIB Iex HINTS /c/msys64/mingw64/lib)
    find_library(ILMTHREAD_LIB IlmThread HINTS /c/msys64/mingw64/lib)
    find_library(IMATH_LIB Imath HINTS /c/msys64/mingw64/lib)
    find_library(OPENPGL_LIB openpgl HINTS /c/msys64/mingw64/lib)
    find_library(PNG_LIB png HINTS /c/msys64/mingw64/lib)
    find_library(JPEG_LIB jpeg HINTS /c/msys64/mingw64/lib)
    find_library(TIFF_LIB tiff HINTS /c/msys64/mingw64/lib)
    find_library(OPENVDB_LIB openvdb HINTS /c/msys64/mingw64/lib)

    # NatronEngine references Cycles. Add Cycles libs to NatronEngine's
    # INTERFACE so they appear after NatronEngine on the link line.
    target_link_libraries(NatronEngine
        INTERFACE
            ${CYCLES_LIBS}
            ${CYCLES_LIBS}
            ${EMBREE_LIB}
            ${OIDN_LIB}
            ${TBB_LIB}
            ${OSD_GPU_LIB}
            ${OSD_CPU_LIB}
            ${PUGIXML_LIB}
            ${ZSTD_LIB}
            ${OIIO_LIB}
            ${OIIO_UTIL_LIB}
            ${OCIO_LIB}
            ${OPENEXR_LIB}
            ${OPENEXRCORE_LIB}
            ${IEX_LIB}
            ${ILMTHREAD_LIB}
            ${IMATH_LIB}
            ${OPENPGL_LIB}
            ${PNG_LIB}
            ${JPEG_LIB}
            ${TIFF_LIB}
            ${OPENVDB_LIB}
    )
    target_link_libraries(Natron
        PRIVATE
            NatronGui
            Python3::Python
    )
else()
    target_link_libraries(Natron
        PRIVATE
            NatronGui
            Python3::Python
    )
endif()
target_include_directories(Natron
    PRIVATE
        ..
        ../Gui
        ../Engine
        ../Global
)

install(TARGETS Natron DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ../Gui/Resources/Applications/fr.natron.Natron.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES ../Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES ../Gui/Resources/Mime/x-natron.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mime")
install(FILES ../Gui/Resources/Images/natronIcon256_linux.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
install(FILES ../Gui/Resources/Images/natronProjectIcon_linux.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
