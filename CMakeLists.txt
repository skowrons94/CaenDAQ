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

    # An explicit root wins outright. This needs NO_DEFAULT_PATH: a plain HINTS
    # entry is searched AFTER CMAKE_PREFIX_PATH, so inside an activated conda
    # env the env prefix would be preferred over the /usr the caller asked for.
    if(_caen_hints)
        find_path(CAEN_DGTZ_INCLUDE_DIR
            NAMES CAENDigitizer.h
            HINTS ${_caen_hints}
            PATH_SUFFIXES include include/CAEN
            NO_DEFAULT_PATH)
        find_library(CAEN_DGTZ_LIBRARY
            NAMES CAENDigitizer
            HINTS ${_caen_hints}
            PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
            NO_DEFAULT_PATH)
    endif()

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

    # Locating the file on disk is NOT the same as the compiler being able to
    # include it: a sysroot, a pruned -I, or a header needing something else can
    # all break it. Compile a one-liner with the exact flag the build will use,
    # so this fails here with a readable message instead of deep inside
    # class_caen_dgtz.cc.
    if(CAEN_DGTZ_INCLUDE_DIR)
        include(CheckCXXSourceCompiles)
        # Exactly the flag the build will use — a probe with different flags
        # can succeed while the real compile fails.
        if(MSVC)
            set(CMAKE_REQUIRED_FLAGS "/I\"${CAEN_DGTZ_INCLUDE_DIR}\"")
        else()
            set(CMAKE_REQUIRED_FLAGS "-idirafter${CAEN_DGTZ_INCLUDE_DIR}")
        endif()
        check_cxx_source_compiles(
            "#include \"CAENDigitizer.h\"\nint main() { return 0; }"
            CAENDAQ_CAEN_HEADER_USABLE)
        unset(CMAKE_REQUIRED_FLAGS)
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
    elseif(NOT CAENDAQ_CAEN_HEADER_USABLE)
        # Found, but the compiler could not actually include it.
        list(APPEND _caen_missing
             "CAENDigitizer.h is present in ${CAEN_DGTZ_INCLUDE_DIR} but the "
             "compiler cannot include it (sysroot? see CMakeError.log)")
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

        # CMake treats every directory in CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES as
        # "the linker already searches this", and acts on that in two ways: an
        # absolute library path there is rewritten to a bare -lname, and any
        # matching -L is pruned from the link line. For CAEN in /usr/lib both
        # fire at once, which is why the link failed with
        #     ld: cannot find -lCAENDigitizer: No such file or directory
        # while -L/usr/lib never appeared on the command line. The assumption is
        # false for the conda toolchain: its ld searches its own sysroot, not
        # /usr/lib.
        #
        # Dropping the directory from that list makes CMake stop eliding it — it
        # then emits the -L (and an -Wl,-rpath, which fixes the runtime lookup
        # too). Scoped to the one directory the CAEN library lives in, and done
        # here because link information is computed from the directory-scope
        # value in effect when the targets are created.
        # NOTE: CMake keeps TWO such lists and consults both. /usr/lib arrives
        # via the PLATFORM one on Linux, so cleaning only the CXX one is not
        # enough — that was a real miss, and the link kept failing.
        get_filename_component(_caen_libdir "${CAEN_DGTZ_LIBRARY}" DIRECTORY)
        if(_caen_libdir)
            foreach(_caen_implicit_var
                    CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES
                    CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES
                    CMAKE_C_IMPLICIT_LINK_DIRECTORIES)
                list(REMOVE_ITEM ${_caen_implicit_var} "${_caen_libdir}")
            endforeach()
        endif()
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

    # Getting the CAEN headers onto the command line is fiddly, and the two
    # obvious approaches are both wrong:
    #
    #  * target_include_directories(PUBLIC /usr/include) produces NO flag at
    #    all. CMake prunes standard system directories from the -I list for
    #    GNU-like compilers, whether or not they appear in
    #    CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES. Normally harmless, because the
    #    compiler searches /usr/include by itself — but x86_64-conda-linux-gnu-c++
    #    builds against its own sysroot and does not, so the build fails with
    #        class_caen_dgtz.h:33: fatal error: CAENDigitizer.h: No such file
    #    on a header CMake just reported as found.
    #
    #  * Forcing -I/usr/include is WORSE. It puts the host's /usr/include ahead
    #    of the sysroot, mixing host glibc headers into the sysroot ones
    #    (/usr/include/bits/stdlib-float.h pulled into the sysroot's stdlib.h);
    #    every translation unit then dies on __BEGIN_NAMESPACE_STD, div_t,
    #    strtol... CMake's pruning exists precisely to prevent that.
    #
    # -idirafter is the tool for the job: searched only after all -I paths AND
    # all standard system directories are exhausted. stdlib.h still resolves
    # from the sysroot; CAENDigitizer.h, which nothing else provides, falls
    # through to /usr/include. It also counts as a system directory, so
    # third-party headers do not spray warnings.
    #
    # Being searched last also keeps the vendored CAENDigitizerType.h ahead of
    # any installed copy, so DataFrame.h and the real CAENDigitizer.h agree on
    # one set of type/enum definitions.
    #
    # Joined spelling (-idirafter<dir>, no space) so CMake cannot split or
    # reorder the option away from its argument.
    function(caendaq_add_include_dir target dir)
        if(NOT dir)
            return()
        endif()
        if(MSVC)
            target_include_directories(${target} PUBLIC "${dir}")
        else()
            target_compile_options(${target} PUBLIC "-idirafter${dir}")
        endif()
    endfunction()

    caendaq_add_include_dir(caendaq "${CAEN_DGTZ_INCLUDE_DIR}")
    caendaq_add_include_dir(caendaq "${JSONCPP_INCLUDE_DIR}")

    # The linker side has the mirror-image problem of the include side. Given an
    # absolute library path that sits in a standard link directory, CMake may
    # rewrite it to a plain -lCAENDigitizer, on the assumption that the linker
    # searches /usr/lib itself. The conda toolchain's ld does not, so the link
    # dies with
    #     ld: cannot find -lCAENDigitizer: No such file or directory
    # even though /usr/lib/libCAENDigitizer.so is right there.
    #
    # Rather than depend on which form CMake emits, make both work: keep the
    # absolute path AND put its directory on the search path so a rewritten -l
    # still resolves.
    #
    # The -L is passed as a raw item to target_link_libraries(), which matters
    # for two independent reasons:
    #
    #  * Not target_link_directories(): a LINK_DIRECTORIES entry is subject to
    #    the same implicit-directory pruning as the library path itself, so it
    #    gets silently deleted — which is exactly what kept happening. A raw
    #    item is passed through untouched, whichever implicit-dir list names
    #    the directory.
    #
    #  * Not target_link_options() either: those are emitted with the linker
    #    flags, ahead of the environment's own -L entries. That would put
    #    /usr/lib in front of the conda prefix and let -lpthread resolve to the
    #    host glibc instead of the sysroot one — the same host/sysroot mixing
    #    that -idirafter avoids on the include side. As a link-libraries item it
    #    lands after the environment's flags, so conda-provided libraries keep
    #    precedence and only libraries nothing else provides fall through here.
    if(_caen_libdir AND NOT MSVC)
        target_link_libraries(caendaq PUBLIC "-L${_caen_libdir}")
    endif()
    target_link_libraries(caendaq PUBLIC ${CAEN_DGTZ_LIBRARY} PRIVATE ${JSONCPP_LINK})

    message(STATUS "CaenDAQ: CAEN backend ENABLED (hardware acquisition available)")
    message(STATUS "  header : ${CAEN_DGTZ_INCLUDE_DIR}/CAENDigitizer.h")
    message(STATUS "  library: ${CAEN_DGTZ_LIBRARY}")
    message(STATUS "  linkdir: -L${_caen_libdir} (so a rewritten -lCAENDigitizer resolves)")
    # Echo the actual flag: CMake silently prunes system dirs from the -I list,
    # so "found it" and "the compiler will see it" are different claims.
    message(STATUS "  flag   : -idirafter${CAEN_DGTZ_INCLUDE_DIR}")

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

    # Asserts the Graphite push stays time-bounded when the Carbon server is
    # unreachable. Capped well below the kernel SYN timeout it guards against,
    # so a regression to blocking sockets fails here rather than on the DAQ.
    add_executable(graphite_test tests/graphite_test.cpp)
    target_link_libraries(graphite_test PRIVATE caendaq)
    add_test(NAME graphite_test COMMAND graphite_test)
    set_tests_properties(graphite_test PROPERTIES TIMEOUT 30)

    # Asserts the stats pacing an operator actually feels: the first evaluation
    # arrives early, and shortening the interval from the UI applies at once.
    add_executable(stats_interval_test tests/stats_interval_test.cpp)
    target_link_libraries(stats_interval_test PRIVATE caendaq)
    add_test(NAME stats_interval_test COMMAND stats_interval_test)
    set_tests_properties(stats_interval_test PROPERTIES TIMEOUT 60)
endif()
