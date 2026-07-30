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

if(CAENDAQ_WITH_CAEN)
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

# ---- Optional real CAEN backend -------------------------------------------
if(CAENDAQ_WITH_CAEN)
    # CAEN's own installer drops CAENDigitizer.h into /usr/include and
    # libCAENDigitizer.so into /usr/lib, which a bare find_path() normally picks
    # up. It does NOT when the build runs under a prefix that reroots the search
    # — a conda env, a cross toolchain, or anything setting CMAKE_FIND_ROOT_PATH
    # / CMAKE_SYSROOT — because then /usr/include is looked for *inside* that
    # root. `pip install .` hits this whenever the interpreter lives in such an
    # environment. So: search normally, then retry with the rerooting bypassed.
    #
    # Override either way with an explicit root:
    #   cmake -DCAEN_DGTZ_ROOT=/opt/CAEN ...
    #   pip install . --config-settings=cmake.define.CAEN_DGTZ_ROOT=/opt/CAEN
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

    if(NOT CAEN_DGTZ_INCLUDE_DIR OR NOT CAEN_DGTZ_LIBRARY)
        # Say what was actually looked for — "not found" alone is useless when
        # the file is sitting in /usr/include and something rerooted the search.
        message(FATAL_ERROR
            "CAENDAQ_WITH_CAEN=ON but libCAENDigitizer was not found.\n"
            "  CAENDigitizer.h    : ${CAEN_DGTZ_INCLUDE_DIR}\n"
            "  libCAENDigitizer   : ${CAEN_DGTZ_LIBRARY}\n"
            "  searched hints     : ${_caen_hints}\n"
            "  searched paths     : ${_caen_paths} (suffixes include/, lib/, lib64/)\n"
            "  CMAKE_SYSROOT      : ${CMAKE_SYSROOT}\n"
            "  CMAKE_FIND_ROOT_PATH: ${CMAKE_FIND_ROOT_PATH}\n"
            "  CMAKE_PREFIX_PATH  : ${CMAKE_PREFIX_PATH}\n"
            "If the header is present (e.g. /usr/include/CAENDigitizer.h), point "
            "the build at it with -DCAEN_DGTZ_ROOT=/usr, or set "
            "CAEN_DGTZ_INCLUDE_DIR / CAEN_DGTZ_LIBRARY directly. Note that a "
            "stale CMakeCache.txt keeps a previous -NOTFOUND: delete the build "
            "directory after installing the CAEN libraries.")
    endif()

    # The vendored driver parses its JSON register files with jsoncpp.
    find_package(jsoncpp QUIET)
    if(jsoncpp_FOUND)
        set(JSONCPP_LINK jsoncpp_lib)
    else()
        find_path(JSONCPP_INCLUDE_DIR json/json.h PATH_SUFFIXES jsoncpp)
        find_library(JSONCPP_LIBRARY NAMES jsoncpp)
        if(NOT JSONCPP_INCLUDE_DIR OR NOT JSONCPP_LIBRARY)
            message(FATAL_ERROR "CAENDAQ_WITH_CAEN=ON needs jsoncpp (json/json.h). "
                                "Install libjsoncpp-dev / jsoncpp.")
        endif()
        target_include_directories(caendaq PRIVATE ${JSONCPP_INCLUDE_DIR})
        set(JSONCPP_LINK ${JSONCPP_LIBRARY})
    endif()

    target_compile_definitions(caendaq PUBLIC CAENDAQ_WITH_CAEN)
    # Appended AFTER vendor/rureader, so the vendored CAENDigitizerType.h keeps
    # precedence over the installed one. Deliberate: DataFrame.h and the real
    # CAENDigitizer.h then agree on one set of type/enum definitions. If the
    # installed CAENDigitizer release ever diverges from the vendored copy, this
    # is the line that decides which wins — see the version check below.
    target_include_directories(caendaq
        PUBLIC  ${CAEN_DGTZ_INCLUDE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/caen)
    target_link_libraries(caendaq PUBLIC ${CAEN_DGTZ_LIBRARY} PRIVATE ${JSONCPP_LINK})
    message(STATUS "CaenDAQ: CAEN backend enabled")
    message(STATUS "  header : ${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizer.h")
    message(STATUS "  library: ${CAEN_DGTZ_LIBRARY}")

    # The vendored driver is compiled against vendor/rureader/CAENDigitizerType.h
    # while linking the installed libCAENDigitizer. Warn if the installed release
    # ships its own type header, since a silent struct/enum mismatch there shows
    # up as corrupted readout rather than a compile error.
    if(EXISTS "${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizerType.h")
        message(STATUS "  note   : using vendored CAENDigitizerType.h, not "
                       "${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizerType.h")
    endif()
else()
    message(STATUS "CaenDAQ: mock backend only (configure with -DCAENDAQ_WITH_CAEN=ON for hardware)")
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
