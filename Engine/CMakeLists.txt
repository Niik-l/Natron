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

include(FindPkgConfig)

pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo fontconfig)

# Dev nodes dependencies
find_package(OpenImageIO QUIET)
if(OpenImageIO_FOUND)
    message(STATUS "OpenImageIO found — Deep nodes will have full EXR support")
else()
    message(STATUS "OpenImageIO not found — Deep nodes will have limited functionality")
endif()

file(GLOB NatronEngine_HEADERS *.h)
file(GLOB NatronEngine_SOURCES *.cpp)

# Dev nodes — custom node extensions
file(GLOB DevDeep_HEADERS Dev/Deep/*.h)
file(GLOB DevDeep_SOURCES Dev/Deep/*.cpp)
file(GLOB DevScene3D_HEADERS Dev/Scene3D/*.h)
file(GLOB DevScene3D_SOURCES Dev/Scene3D/*.cpp)
file(GLOB DevParticles_HEADERS Dev/Particles/*.h)
file(GLOB DevParticles_SOURCES Dev/Particles/*.cpp)
file(GLOB DevChannel_HEADERS Dev/Channel/*.h)
file(GLOB DevChannel_SOURCES Dev/Channel/*.cpp)
list(APPEND NatronEngine_HEADERS ${DevDeep_HEADERS} ${DevScene3D_HEADERS} ${DevParticles_HEADERS} ${DevChannel_HEADERS})
list(APPEND NatronEngine_SOURCES ${DevDeep_SOURCES} ${DevScene3D_SOURCES} ${DevParticles_SOURCES} ${DevChannel_SOURCES})

set(PYENGINE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Qt${QT_VERSION_MAJOR})
set(PYENGINE_INCLUDE_DIRS ${PYSIDE_INCLUDE_DIRS} ${PYSIDE_INCLUDE_DIRS}/QtCore)
list(APPEND PYENGINE_INCLUDE_DIRS ${QTCORE_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
list(TRANSFORM PYENGINE_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE PYENGINE_INCS)
set(PYENGINE_HEADER PySide${PYSIDE_VERSION_MAJOR}_Engine_Python.h)

set(shiboken_args
    "-std=c++17" "--enable-parent-ctor-heuristic" "--use-isnull-as-nb_nonzero"
    "--avoid-protected-hack" "--enable-pyside-extensions"
    "-I." "-I.." "-I../Global" "-I../libs/OpenFX/include" ${PYENGINE_INCS}
    "-T${PYSIDE_TYPESYSTEMS}" "--output-directory=${PYENGINE_OUT}"
    "${PYENGINE_HEADER}" "typesystem_engine.xml")

execute_process(COMMAND ${Python3_EXECUTABLE} ../tools/utils/sourceList.py typesystem_engine.xml ${PYENGINE_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PyEngine_SOURCES)
set_property(SOURCE ${PyEngine_SOURCES} PROPERTY SKIP_AUTOGEN ON)
add_custom_command(OUTPUT ${PyEngine_SOURCES}
    COMMAND ${CMAKE_COMMAND} -E env "LLVM_INSTALL_DIR=${NATRON_LLVM_INSTALL_DIR}" $<TARGET_FILE:${SHIBOKEN_EXE}> ${shiboken_args}
    COMMAND ${Python3_EXECUTABLE} ../tools/utils/sourceCleanup.py typesystem_engine.xml ${PYENGINE_OUT}
    DEPENDS ${SHIBOKEN_EXE} ${PYENGINE_HEADER} ${NatronEngine_HEADERS} ../tools/utils/sourceCleanup.py typesystem_engine.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX AND NOT APPLE)
    set(XDG_LIBS ${X11_LIBRARIES})
    if(Wayland_FOUND)
        set(XDG_LIBS Wayland::Client Wayland::Egl ${XDG_LIBS})
        set(XDG_DEFS __NATRON_WAYLAND__)
    endif()
endif()


list(APPEND NatronEngine_SOURCES
    ${NatronEngine_SOURCES}
    ${PyEngine_SOURCES}
    ../Global/glad_source.c
    ../Global/FStreamsSupport.cpp
    ../Global/ProcInfo.cpp
    ../Global/PythonUtils.cpp
    ../Global/StrUtils.cpp
    ../libs/SequenceParsing/SequenceParsing.cpp
)
add_library(NatronEngine STATIC ${NatronEngine_HEADERS} ${NatronEngine_SOURCES})

# Exclude non-Qt include paths from AUTOMOC to prevent moc from parsing
# Python, Boost, and other third-party headers it doesn't need.
set_target_properties(NatronEngine PROPERTIES AUTOMOC_PATH_PREFIX OFF)

target_link_libraries(NatronEngine
    PUBLIC
        HostSupport
        PkgConfig::Cairo
    PRIVATE
        ${SHIBOKEN_LIB}
        ${PYSIDE_LIB}
        Qt::Core
        Qt::Concurrent
        Qt::Network
        Python3::Module
        ${XDG_LIBS}
        ceres
        hoedown
        mv
        openMVG
        tess
        $<$<BOOL:${OpenImageIO_FOUND}>:OpenImageIO::OpenImageIO>
)

# Link Boost separately so we can mark its includes as SYSTEM
target_link_libraries(NatronEngine PUBLIC Boost::headers Boost::serialization)

if(WIN32)
    #System library is required on windows to map network share names from drive letters
    target_link_libraries(NatronEngine PRIVATE mpr)
endif()

target_include_directories(NatronEngine
    PUBLIC
        ../Global/glad$<IF:$<CONFIG:Debug>,Deb,Rel>/include
    PRIVATE
        .
        ..
        ../Global
        ../libs/SequenceParsing
        Dev/Deep
        Dev/Scene3D
        Dev/Particles
        Dev/Channel
        ${X11_INCLUDE_DIR}
)
# Non-Qt includes that AUTOMOC/moc doesn't need — mark as SYSTEM to keep
# them out of moc's include search path (moc only needs Qt headers).
target_include_directories(NatronEngine SYSTEM PRIVATE
        ${PYSIDE_INCLUDE_DIRS}/QtCore
        ${Python3_INCLUDE_DIRS}
        ${SHIBOKEN_INCLUDE_DIRS}
)
target_compile_definitions(NatronEngine
    PRIVATE
        $<$<BOOL:${BUILD_USER_NAME}>:NATRON_CUSTOM_BUILD_USER_TOKEN="${BUILD_USER_NAME}">
        NATRON_BUILD_NUMBER=0
        QT_NO_KEYWORDS
        ${XDG_DEFS}
        $<$<BOOL:${OpenImageIO_FOUND}>:NATRON_HAVE_OPENIMAGEIO>
)
