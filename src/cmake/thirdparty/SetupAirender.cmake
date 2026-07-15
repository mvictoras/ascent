###############################################################################
# Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
# Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
# other details. No copyright assignment is required to contribute to Ascent.
###############################################################################

###############################################################################
# Setup airender
#
# Requires:
#   - AIRENDER_DIR pointing at an airender install prefix that contains
#     lib/cmake/airender/airenderConfig.cmake.
###############################################################################

if(NOT AIRENDER_DIR)
    message(FATAL_ERROR "airender support requires AIRENDER_DIR to be set")
endif()

message(STATUS "Looking for airender in AIRENDER_DIR = ${AIRENDER_DIR}")

set(_airender_cmake_dir "")
foreach(_candidate
        "${AIRENDER_DIR}/lib/cmake/airender"
        "${AIRENDER_DIR}/lib64/cmake/airender")
    if(EXISTS "${_candidate}/airenderConfig.cmake")
        set(_airender_cmake_dir "${_candidate}")
        break()
    endif()
endforeach()

if(NOT _airender_cmake_dir)
    message(FATAL_ERROR
        "Could not find airenderConfig.cmake under AIRENDER_DIR=${AIRENDER_DIR}")
endif()

find_package(airender REQUIRED
    NO_DEFAULT_PATH
    PATHS "${_airender_cmake_dir}")

if(NOT TARGET airender::airender)
    message(FATAL_ERROR "airender::airender target not defined after find_package(airender)")
endif()

set(AIRENDER_FOUND TRUE)
message(STATUS "  airender: found at ${_airender_cmake_dir}")
message(STATUS "  airender FSR1 support: ${AIRENDER_HAS_FSR1}")
message(STATUS "  airender DLSS support: ${AIRENDER_HAS_DLSS}")

###############################################################################
# GLAD (EGL + OpenGL loader) for the GPU upscaler's headless EGL context.
# GLAD ships as plain static libs (libglad.a + libglad_egl.a) + headers, not a
# CMake package, so wrap it in an interface target. The header root and lib root
# may differ (glad-cli emits headers under gen/include/ but libs under lib/), so
# probe a few common layouts under GLAD_DIR for each.
###############################################################################
if(NOT GLAD_DIR)
    message(FATAL_ERROR "airender GPU upscaler requires GLAD_DIR (EGL+GL loader)")
endif()

find_path(GLAD_INCLUDE_ROOT
    NAMES glad/gl.h
    PATHS "${GLAD_DIR}/include" "${GLAD_DIR}/gen/include"
    NO_DEFAULT_PATH)
find_library(GLAD_GL_LIB  NAMES glad
    PATHS "${GLAD_DIR}/lib" "${GLAD_DIR}/gen/lib" NO_DEFAULT_PATH)
find_library(GLAD_EGL_LIB NAMES glad_egl
    PATHS "${GLAD_DIR}/lib" "${GLAD_DIR}/gen/lib" NO_DEFAULT_PATH)
if(NOT GLAD_INCLUDE_ROOT OR NOT GLAD_GL_LIB OR NOT GLAD_EGL_LIB)
    message(FATAL_ERROR
        "Could not locate GLAD under GLAD_DIR=${GLAD_DIR} "
        "(need glad/gl.h + libglad + libglad_egl)")
endif()

# Exposed as plain path variables (not an exported target) and linked PRIVATE
# into the ascent libs, so GLAD stays baked into libascent{,_mpi}.so and does
# not leak an unresolvable "ascent_glad" name into Ascent's exported link
# interface for downstream consumers.
set(ASCENT_GLAD_INCLUDE_DIR "${GLAD_INCLUDE_ROOT}")
set(ASCENT_GLAD_LIBRARIES   "${GLAD_GL_LIB};${GLAD_EGL_LIB};dl")

message(STATUS "  GLAD: ${GLAD_GL_LIB} + ${GLAD_EGL_LIB} (include ${GLAD_INCLUDE_ROOT})")
