cmake_minimum_required(VERSION 3.16)
project(CaenDAQ VERSION 0.1.0 LANGUAGES CXX)

# ---- Options ---------------------------------------------------------------
# Tri-state, and AUTO by default: a machine with libCAENDigitizer installed gets
# the hardware backend without being asked, a machine without one still builds
# (mock only, usable with TEST_FLAG=True). So a plain `pip install .` does the
# right thing everywhere and no caller has to probe for CAEN itself.
#   AUTO  use the real backend if libCAENDigitizer is found, else mock only
#   ON    require it; fail the configure if it is missing
#   OFF   never build it, even if it is installed
set(CAENDAQ_WITH_CAEN "AUTO" CACHE STRING
    "Real CAEN-backed digitizer: AUTO (if available), ON (require), OFF (never)")
set_property(CACHE CAENDAQ_WITH_CAEN PROPERTY STRINGS AUTO ON OFF)
option(CAENDAQ_BUILD_CLI   "Build the caendaq_cli standalone runner" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd

find_package(Threads REQUIRED)

# ---- Real CAEN backend: detection ------------------------------------------
# Must run BEFORE the source list: whether src/CaenDigitizer.cpp and the
# vendored driver are compiled at all depends on the outcome.
#
# CAEN's own installer drops CAENDigitizer.h into /usr/include and
# libCAENDigitizer.so into /usr/lib, which a bare find_path() normally picks up.
# It does NOT when the build runs under a prefix that reroots the search — a
# conda env, a cross toolchain, or anything setting CMAKE_FIND_ROOT_PATH /
# CMAKE_SYSROOT — because /usr/include is then looked for *inside* that root.
# `pip install .` hits this whenever the interpreter lives in such an
# environment. So: search normally, then retry with the rerooting bypassed.
#
# Override either way with an explicit root:
#   cmake -DCAEN_DGTZ_ROOT=/opt/CAEN ...
#   pip install . --config-settings=cmake.define.CAEN_DGTZ_ROOT=/opt/CAEN
string(TOUPPER "${CAENDAQ_WITH_CAEN}" _caen_mode)
if(_caen_mode STREQUAL "AUTO")
    set(_caen_wanted TRUE)
    set(_caen_required FALSE)
elseif(_caen_mode)          # ON / TRUE / 1 / YES
    set(_caen_wanted TRUE)
    set(_caen_required TRUE)
else()                      # OFF / FALSE / 0 / NO
    set(_caen_wanted FALSE)
    set(_caen_required FALSE)
endif()

set(CAENDAQ_CAEN_ENABLED FALSE)

if(_caen_wanted)
    set(CAEN_DGTZ_ROOT "" CACHE PATH
        "Root of a CAENDigitizer installation (expects include/ and lib/ below it)")

    set(_caen_hints "")
    foreach(_caen_p "${CAEN_DGTZ_ROOT}" "$ENV{CAEN_DGTZ_ROOT}" "$ENV{CAENDIGITIZER_ROOT}")
        if(_caen_p)
            list(APPEND _caen_hints "${_caen_p}")
        endif()
    endforeach()
    set(_caen_paths /usr /usr/local /opt/CAEN /opt/caen)

    find_path(CAEN_DGTZ_INCLUDE_DIR
        NAMES CAENDigitizer.h
        HINTS ${_caen_hints}
        PATHS ${_caen_paths}
        PATH_SUFFIXES include include/CAEN)
    find_library(CAEN_DGTZ_LIBRARY
        NAMES CAENDigitizer
        HINTS ${_caen_hints}
        PATHS ${_caen_paths}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu)

    # Second pass with the sysroot/find-root rerooting switched off, so a real
    # /usr/include on the build host is still visible from inside a venv.
    if(NOT CAEN_DGTZ_INCLUDE_DIR)
        find_path(CAEN_DGTZ_INCLUDE_DIR
            NAMES CAENDigitizer.h
            HINTS ${_caen_hints}
            PATHS ${_caen_paths}
            PATH_SUFFIXES include include/CAEN
            NO_CMAKE_FIND_ROOT_PATH)
    endif()
    if(NOT CAEN_DGTZ_LIBRARY)
        find_library(CAEN_DGTZ_LIBRARY
            NAMES CAENDigitizer
            HINTS ${_caen_hints}
            PATHS ${_caen_paths}
            PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
            NO_CMAKE_FIND_ROOT_PATH)
    endif()

    # The vendored driver parses its JSON register dumps with jsoncpp, so a CAEN
    # backend without jsoncpp cannot be built — treat it as part of the probe.
    find_package(jsoncpp QUIET)
    if(jsoncpp_FOUND)
        set(JSONCPP_LINK jsoncpp_lib)
    else()
        find_path(JSONCPP_INCLUDE_DIR json/json.h PATH_SUFFIXES jsoncpp)
        find_library(JSONCPP_LIBRARY NAMES jsoncpp)
        if(NOT JSONCPP_INCLUDE_DIR)
            find_path(JSONCPP_INCLUDE_DIR json/json.h
                      PATH_SUFFIXES jsoncpp NO_CMAKE_FIND_ROOT_PATH)
        endif()
        if(NOT JSONCPP_LIBRARY)
            find_library(JSONCPP_LIBRARY NAMES jsoncpp NO_CMAKE_FIND_ROOT_PATH)
        endif()
        if(JSONCPP_INCLUDE_DIR AND JSONCPP_LIBRARY)
            set(JSONCPP_LINK ${JSONCPP_LIBRARY})
        else()
            set(JSONCPP_LINK "")
        endif()
    endif()

    set(_caen_missing "")
    if(NOT CAEN_DGTZ_INCLUDE_DIR)
        list(APPEND _caen_missing "CAENDigitizer.h")
    endif()
    if(NOT CAEN_DGTZ_LIBRARY)
        list(APPEND _caen_missing "libCAENDigitizer")
    endif()
    if(NOT JSONCPP_LINK)
        list(APPEND _caen_missing "jsoncpp (json/json.h)")
    endif()

    if(_caen_missing)
        # Join first: message() concatenates its arguments, so a bare list would
        # print as "CAENDigitizer.hlibCAENDigitizer" with the separators eaten.
        string(JOIN ", " _caen_missing_str ${_caen_missing})
        string(JOIN ", " _caen_hints_str   ${_caen_hints})
        string(JOIN ", " _caen_paths_str   ${_caen_paths})
        # Say what was actually looked for — "not found" alone is useless when
        # the file is sitting in /usr/include and something rerooted the search.
        set(_caen_diag
            "  missing            : ${_caen_missing_str}\n"
            "  CAENDigitizer.h    : ${CAEN_DGTZ_INCLUDE_DIR}\n"
            "  libCAENDigitizer   : ${CAEN_DGTZ_LIBRARY}\n"
            "  searched hints     : ${_caen_hints_str}\n"
            "  searched paths     : ${_caen_paths_str} (suffixes include/, lib/, lib64/)\n"
            "  CMAKE_SYSROOT      : ${CMAKE_SYSROOT}\n"
            "  CMAKE_FIND_ROOT_PATH: ${CMAKE_FIND_ROOT_PATH}\n"
            "  CMAKE_PREFIX_PATH  : ${CMAKE_PREFIX_PATH}\n")
        if(_caen_required)
            message(FATAL_ERROR
                "CAENDAQ_WITH_CAEN=ON but the CAEN backend cannot be built.\n"
                ${_caen_diag}
                "If the header is present (e.g. /usr/include/CAENDigitizer.h), "
                "point the build at it with -DCAEN_DGTZ_ROOT=/usr, or set "
                "CAEN_DGTZ_INCLUDE_DIR / CAEN_DGTZ_LIBRARY directly. Note that a "
                "stale CMakeCache.txt keeps a previous -NOTFOUND: delete the "
                "build directory after installing the CAEN libraries. Use "
                "-DCAENDAQ_WITH_CAEN=AUTO to fall back to the mock instead of "
                "failing here.")
        else()
            message(STATUS "CaenDAQ: CAEN backend NOT available — mock backend only "
                           "(fully usable with TEST_FLAG=True)")
            message(STATUS "  missing: ${_caen_missing_str}")
            message(STATUS "  if CAEN *is* installed, point at it with "
                           "-DCAEN_DGTZ_ROOT=/usr (or -DCAENDAQ_WITH_CAEN=ON to see "
                           "the full diagnostic)")
        endif()
    else()
        set(CAENDAQ_CAEN_ENABLED TRUE)
    endif()
else()
    message(STATUS "CaenDAQ: CAEN backend disabled by CAENDAQ_WITH_CAEN=${CAENDAQ_WITH_CAEN}")
endif()

# ---- Core library ----------------------------------------------------------
# Portable, dependency-free core: acquisition pipeline + .caendat file writing
# + the parallel aggregate decoder (RUReader's format tables, vendored).
set(CAENDAQ_SOURCES
    src/RawFileWriter.cpp
    src/BoardRunner.cpp
    src/MockDigitizer.cpp
    src/AggregateDecoder.cpp
    src/HistogramStore.cpp
    src/DecodeStage.cpp
    src/GraphiteClient.cpp
    src/StatsCollector.cpp
    src/Daq.cpp
    vendor/rureader/DataFrame.cc
    vendor/rureader/DataFrameBuilderPHA.cc
    vendor/rureader/DataFrameBuilderPSD.cc
)

if(CAENDAQ_CAEN_ENABLED)
    # The adapter + the vendored, battle-tested CAEN driver.
    list(APPEND CAENDAQ_SOURCES
        src/CaenDigitizer.cpp
        vendor/caen/class_caen_dgtz.cc)
endif()

add_library(caendaq ${CAENDAQ_SOURCES})
target_include_directories(caendaq
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rureader)  # DataFrame.h, Utils.h, CAENDigitizerType.h
target_link_libraries(caendaq PUBLIC Threads::Threads)
# Position-independent so the static core can be linked into the Python module.
set_target_properties(caendaq PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(caendaq PRIVATE /W4 /permissive-)
else()
    target_compile_options(caendaq PRIVATE -Wall -Wextra)
endif()

# ---- Real CAEN backend: apply ----------------------------------------------
# Detection already happened above (CAENDAQ_CAEN_ENABLED); this only wires the
# results into the target.
if(CAENDAQ_CAEN_ENABLED)
    target_compile_definitions(caendaq PUBLIC CAENDAQ_WITH_CAEN)
    target_include_directories(caendaq
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/caen)

    # CMake DELETES any directory that is already in the compiler's implicit
    # include search path from the -I flags it generates. For CAEN that is the
    # normal case: the installer puts the headers straight into /usr/include, so
    # the target ends up with no flag for them at all. That is harmless only
    # while the compiler really does search /usr/include by itself — a conda
    # toolchain that builds against a sysroot does not, and then the build dies
    # on the header it just "found":
    #     class_caen_dgtz.h:33: fatal error: CAENDigitizer.h: No such file
    # Raw compile options are never pruned, so pass -I explicitly in that case.
    #
    # Appended AFTER vendor/rureader either way, so the vendored
    # CAENDigitizerType.h keeps precedence over the installed one: DataFrame.h
    # and the real CAENDigitizer.h then agree on one set of type/enum defs.
    function(caendaq_add_include_dir target dir)
        if(NOT dir)
            return()
        endif()
        list(FIND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "${dir}" _idx)
        if(_idx EQUAL -1 OR MSVC)
            target_include_directories(${target} PUBLIC "${dir}")
        else()
            # Implicit dir: a target_include_directories() entry would be pruned.
            target_compile_options(${target} PUBLIC "-I${dir}")
            message(STATUS "  note   : ${dir} is an implicit include dir — passing "
                           "-I explicitly so CMake cannot prune it")
        endif()
    endfunction()

    caendaq_add_include_dir(caendaq "${CAEN_DGTZ_INCLUDE_DIR}")
    caendaq_add_include_dir(caendaq "${JSONCPP_INCLUDE_DIR}")
    target_link_libraries(caendaq PUBLIC ${CAEN_DGTZ_LIBRARY} PRIVATE ${JSONCPP_LINK})

    message(STATUS "CaenDAQ: CAEN backend ENABLED (hardware acquisition available)")
    message(STATUS "  header : ${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizer.h")
    message(STATUS "  library: ${CAEN_DGTZ_LIBRARY}")

    # The vendored driver is compiled against vendor/rureader/CAENDigitizerType.h
    # while linking the installed libCAENDigitizer. Note it when the installed
    # release ships its own type header, since a silent struct/enum mismatch
    # there shows up as corrupted readout rather than a compile error.
    if(EXISTS "${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizerType.h")
        message(STATUS "  note   : using vendored CAENDigitizerType.h, not "
                       "${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizerType.h")
    endif()
endif()

# ---- CLI -------------------------------------------------------------------
if(CAENDAQ_BUILD_CLI)
    add_executable(caendaq_cli apps/caendaq_cli.cpp)
    target_link_libraries(caendaq_cli PRIVATE caendaq)
endif()

# ---- Python module ---------------------------------------------------------
option(CAENDAQ_BUILD_PYTHON "Build the pybind11 Python module" OFF)
if(CAENDAQ_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(caendaq_py python/bindings.cpp)
    set_target_properties(caendaq_py PROPERTIES OUTPUT_NAME caendaq)
    target_link_libraries(caendaq_py PRIVATE caendaq)
    # Exposed as caendaq.__version__ so runs record which build took the data.
    target_compile_definitions(caendaq_py PRIVATE
        CAENDAQ_VERSION="${PROJECT_VERSION}")
    # Install rule so `pip install .` (scikit-build-core) packages the module.
    install(TARGETS caendaq_py LIBRARY DESTINATION .)
    message(STATUS "CaenDAQ: Python module enabled")
endif()

# ---- Tests -----------------------------------------------------------------
option(CAENDAQ_BUILD_TESTS "Build unit tests" ON)
if(CAENDAQ_BUILD_TESTS)
    enable_testing()
    add_executable(decode_test tests/decode_test.cpp)
    target_link_libraries(decode_test PRIVATE caendaq)
    add_test(NAME decode_test COMMAND decode_test)
endif()
