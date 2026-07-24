cmake_minimum_required(VERSION 3.16)
project(CaenDAQ LANGUAGES CXX)

# ---- Options ---------------------------------------------------------------
option(CAENDAQ_WITH_CAEN   "Build the real CAEN-backed digitizer (needs libCAENDigitizer)" OFF)
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
    find_path(CAEN_DGTZ_INCLUDE_DIR CAENDigitizer.h)
    find_library(CAEN_DGTZ_LIBRARY NAMES CAENDigitizer)
    if(NOT CAEN_DGTZ_INCLUDE_DIR OR NOT CAEN_DGTZ_LIBRARY)
        message(FATAL_ERROR "CAENDAQ_WITH_CAEN=ON but libCAENDigitizer was not found. "
                            "Set CAEN_DGTZ_INCLUDE_DIR / CAEN_DGTZ_LIBRARY.")
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
    target_include_directories(caendaq
        PUBLIC  ${CAEN_DGTZ_INCLUDE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/caen)
    target_link_libraries(caendaq PUBLIC ${CAEN_DGTZ_LIBRARY} PRIVATE ${JSONCPP_LINK})
    message(STATUS "CaenDAQ: CAEN backend enabled (${CAEN_DGTZ_LIBRARY})")
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
