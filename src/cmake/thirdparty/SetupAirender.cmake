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
