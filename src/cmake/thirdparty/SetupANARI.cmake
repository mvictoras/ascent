###############################################################################
# Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
# Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
# other details. No copyright assignment is required to contribute to Ascent.
###############################################################################

###############################################################################
# Setup ANARI
#
# Requires:
#   - ANARI_DIR pointing at an ANARI-SDK install prefix (contains
#     lib/cmake/anari-*/anariConfig.cmake  OR  lib64/cmake/anari-*/anariConfig.cmake)
#   - Viskores already configured with Viskores_ENABLE_ANARI=ON so that the
#     viskores::anari imported target exists (SetupViskores.cmake runs before this).
###############################################################################

if(NOT ANARI_DIR)
    message(FATAL_ERROR "ANARI support requires ANARI_DIR to be set")
endif()

message(STATUS "Looking for ANARI in ANARI_DIR = ${ANARI_DIR}")

set(_anari_search_globs
    "${ANARI_DIR}/lib/cmake/anari-*"
    "${ANARI_DIR}/lib64/cmake/anari-*")

set(_anari_config_dir "")
foreach(_glob ${_anari_search_globs})
    file(GLOB _matches "${_glob}")
    foreach(_m ${_matches})
        if(EXISTS "${_m}/anariConfig.cmake")
            set(_anari_config_dir "${_m}")
            break()
        endif()
    endforeach()
    if(_anari_config_dir)
        break()
    endif()
endforeach()

if(NOT _anari_config_dir)
    message(FATAL_ERROR
        "Could not find anariConfig.cmake under ANARI_DIR=${ANARI_DIR}. "
        "Searched: ${_anari_search_globs}")
endif()

find_package(anari REQUIRED
    NO_DEFAULT_PATH
    PATHS "${_anari_config_dir}")

if(NOT TARGET viskores::anari)
    message(FATAL_ERROR
        "viskores::anari target not defined. Rebuild Viskores with "
        "Viskores_ENABLE_ANARI=ON (SetupViskores.cmake must run before SetupANARI.cmake).")
endif()

set(ANARI_FOUND TRUE)
message(STATUS "  ANARI: found at ${_anari_config_dir}")
