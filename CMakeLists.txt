# tugz: the library, the gzip, zip, and unzip programs, and their tests
#   $ cmake -B build && cmake --build build && ctest --test-dir build
# Each program and test is a unity build of one entry file, which a
# compiler may also be given directly (see README.md). Included through
# FetchContent or add_subdirectory, only the library is built, as the
# target tugz::tugz.
cmake_minimum_required(VERSION 3.21...4.4)

# The release, TUGZ_VERSION in src/base.c, which the programs report
file(STRINGS src/base.c tugz_version
     REGEX "^#define TUGZ_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
string(REGEX REPLACE ".*\"(.*)\"" "\\1" tugz_version "${tugz_version}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/base.c)

# Release by default at the top level, set before project(), which on
# the MSVC ABI would set Debug
get_property(tugz_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND
   NOT tugz_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(tugz
    VERSION ${tugz_version}
    DESCRIPTION "Streaming DEFLATE, zlib, and gzip library"
    HOMEPAGE_URL "https://github.com/skeeto/tugz"
    LANGUAGES C)

if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "tugz needs GCC or Clang (with the GNU-style driver): "
        "it uses their builtins and attributes")
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    set(tugz_tests_default ON)
else()
    set(tugz_tests_default OFF)
endif()
option(TUGZ_BUILD_LIBRARY "Build the tugz library" ON)
option(TUGZ_BUILD_GZIP "Build the gzip program" ${PROJECT_IS_TOP_LEVEL})
option(TUGZ_BUILD_ZIP "Build the zip program" ${PROJECT_IS_TOP_LEVEL})
option(TUGZ_BUILD_UNZIP "Build the unzip program" ${PROJECT_IS_TOP_LEVEL})
option(TUGZ_BUILD_TESTS "Build the tests" ${tugz_tests_default})
option(TUGZ_BUILD_FUZZ "Build the libFuzzer harnesses (LLVM clang)" OFF)
option(TUGZ_BUILD_BENCH "Build the benchmark (zlib, libdeflate)" OFF)
option(TUGZ_INSTALL "Install the library, header, and package" ${PROJECT_IS_TOP_LEVEL})
option(TUGZ_WARNINGS "Compile tugz with warnings" ${PROJECT_IS_TOP_LEVEL})
set(TUGZ_LIBMEMORY AUTO CACHE STRING
    "Link Windows programs with w64devkit's libmemory (AUTO, ON, OFF)")
set_property(CACHE TUGZ_LIBMEMORY PROPERTY STRINGS AUTO ON OFF)

include(CheckCSourceCompiles)
include(CMakePushCheckState)

set(tugz_warnings -Wall -Wextra -Wconversion -Wno-sign-conversion)

# Settings shared by every tugz target. They compile with the compiler's
# default C standard, as direct builds do (C11 with C23 attributes, which
# a consumer's CMAKE_C_STANDARD of 11 would reject), and at -O2 rather
# than Release's -O3.
function(tugz_common tgt)
    set_property(TARGET ${tgt} PROPERTY C_STANDARD)  # unset
    set_property(TARGET ${tgt} PROPERTY C_EXTENSIONS)
    target_compile_options(${tgt} PRIVATE
        $<$<NOT:$<CONFIG:Debug,RelWithDebInfo,MinSizeRel>>:-O2>)
    if(TUGZ_WARNINGS)
        target_compile_options(${tgt} PRIVATE ${tugz_warnings})
    endif()
endfunction()

# ---- Library ---------------------------------------------------------

include(GNUInstallDirs)

if(TUGZ_BUILD_LIBRARY)
    add_library(tugz platform/libtugz.c)
    add_library(tugz::tugz ALIAS tugz)
    tugz_common(tugz)
    # The source root holds no header but tugz.h
    target_include_directories(tugz PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set_target_properties(tugz PROPERTIES PUBLIC_HEADER tugz.h)
    if(BUILD_SHARED_LIBS)
        set_target_properties(tugz PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()
endif()

# ---- Programs --------------------------------------------------------

# Windows programs link no C runtime (see README.md)
set(tugz_libmemory OFF)
if(WIN32 AND (TUGZ_BUILD_GZIP OR TUGZ_BUILD_ZIP OR TUGZ_BUILD_UNZIP OR
              TUGZ_BUILD_TESTS))
    if(NOT TUGZ_LIBMEMORY MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "TUGZ_LIBMEMORY must be AUTO, ON, or OFF")
    endif()
    if(NOT TUGZ_LIBMEMORY STREQUAL "OFF")
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_LIBRARIES memory)
        set(CMAKE_REQUIRED_QUIET ON)
        check_c_source_compiles("int main(void) { return 0; }"
                                TUGZ_HAVE_LIBMEMORY)
        cmake_pop_check_state()
        if(TUGZ_HAVE_LIBMEMORY)
            set(tugz_libmemory ON)
        elseif(TUGZ_LIBMEMORY STREQUAL "ON")
            message(FATAL_ERROR
                "TUGZ_LIBMEMORY is ON, but this toolchain has no libmemory "
                "(w64devkit's -lmemory)")
        else()
            message(STATUS
                "tugz: no libmemory, so Windows programs import memcpy and "
                "memset from the C runtime's DLL (msvcrt or UCRT)")
        endif()
    endif()
endif()

# CRT-free Windows linking, stripped unless NOSTRIP
function(tugz_crtfree tgt)
    cmake_parse_arguments(PARSE_ARGV 1 arg NOSTRIP "" "")
    target_compile_options(${tgt} PRIVATE
        -fno-builtin -fno-asynchronous-unwind-tables)
    target_link_options(${tgt} PRIVATE -nostartfiles -Wl,--gc-sections)
    if(NOT arg_NOSTRIP)
        target_link_options(${tgt} PRIVATE -s)
    endif()
    if(tugz_libmemory)
        target_link_libraries(${tgt} PRIVATE memory)
    endif()
    target_link_libraries(${tgt} PRIVATE shell32 kernel32)
endfunction()

# A program from its platform entry file, platform/NAME_{posix,windows}.c
function(tugz_program tgt name output)
    if(WIN32)
        add_executable(${tgt} platform/${name}_windows.c)
    else()
        add_executable(${tgt} platform/${name}_posix.c)
    endif()
    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${output})
    tugz_common(${tgt})
    if(WIN32)
        tugz_crtfree(${tgt} ${ARGN})
    endif()
endfunction()

if(TUGZ_BUILD_GZIP)
    tugz_program(tugz_gzip gzip gzip)
endif()
if(TUGZ_BUILD_ZIP)
    tugz_program(tugz_zip zip zip)
endif()
if(TUGZ_BUILD_UNZIP)
    tugz_program(tugz_unzip unzip unzip)
endif()

# ---- Single-file sources ---------------------------------------------

# gzip.c, zip.c, unzip.c, tugz.c, and tugz.h in the build's amalgamation/
set(tugz_amalgamation_dir ${PROJECT_BINARY_DIR}/amalgamation)
set(tugz_amalgamation
    ${tugz_amalgamation_dir}/gzip.c
    ${tugz_amalgamation_dir}/zip.c
    ${tugz_amalgamation_dir}/unzip.c
    ${tugz_amalgamation_dir}/tugz.c
    ${tugz_amalgamation_dir}/tugz.h)
file(GLOB tugz_sources CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.c ${PROJECT_SOURCE_DIR}/platform/*.c)
add_custom_command(
    OUTPUT ${tugz_amalgamation}
    COMMAND ${CMAKE_COMMAND} -DTUGZ_ARTIFACT=all
            -DTUGZ_OUTPUT_DIR=${tugz_amalgamation_dir}
            -P ${PROJECT_SOURCE_DIR}/cmake/amalgamate.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/cmake/amalgamate.cmake
            ${PROJECT_SOURCE_DIR}/tugz.h ${tugz_sources}
    COMMENT "Generating gzip.c, zip.c, unzip.c, and tugz.c"
    VERBATIM)
add_custom_target(tugz_amalgamation DEPENDS ${tugz_amalgamation})

# ---- Tests, fuzzers, benchmark ---------------------------------------

if(TUGZ_BUILD_TESTS OR TUGZ_BUILD_FUZZ OR TUGZ_BUILD_BENCH)
    find_package(ZLIB)
    find_package(libdeflate CONFIG QUIET)
    if(TARGET libdeflate::libdeflate_shared)
        set(tugz_libdeflate libdeflate::libdeflate_shared)
    elseif(TARGET libdeflate::libdeflate_static)
        set(tugz_libdeflate libdeflate::libdeflate_static)
    else()
        find_library(TUGZ_LIBDEFLATE_LIBRARY deflate)
        find_path(TUGZ_LIBDEFLATE_INCLUDE_DIR libdeflate.h)
        if(TUGZ_LIBDEFLATE_LIBRARY AND TUGZ_LIBDEFLATE_INCLUDE_DIR)
            add_library(tugz_libdeflate UNKNOWN IMPORTED)
            set_target_properties(tugz_libdeflate PROPERTIES
                IMPORTED_LOCATION ${TUGZ_LIBDEFLATE_LIBRARY}
                INTERFACE_INCLUDE_DIRECTORIES ${TUGZ_LIBDEFLATE_INCLUDE_DIR})
            set(tugz_libdeflate tugz_libdeflate)
        endif()
    endif()
endif()

# Whether the reference libraries a target needs (ZLIB, LIBDEFLATE) were
# found, warning that it is skipped if not
function(tugz_have out what)
    set(missing "")
    foreach(lib IN LISTS ARGN)
        if(lib STREQUAL "ZLIB" AND NOT ZLIB_FOUND)
            list(APPEND missing zlib)
        elseif(lib STREQUAL "LIBDEFLATE" AND NOT tugz_libdeflate)
            list(APPEND missing libdeflate)
        endif()
    endforeach()
    if(missing)
        list(JOIN missing " and " missing)
        message(WARNING "tugz: skipping ${what}, which needs ${missing}")
        set(${out} OFF PARENT_SCOPE)
    else()
        set(${out} ON PARENT_SCOPE)
    endif()
endfunction()

function(tugz_reflibs tgt)
    foreach(lib IN LISTS ARGN)
        if(lib STREQUAL "ZLIB")
            target_link_libraries(${tgt} PRIVATE ZLIB::ZLIB)
        elseif(lib STREQUAL "LIBDEFLATE")
            target_link_libraries(${tgt} PRIVATE ${tugz_libdeflate})
        endif()
    endforeach()
endfunction()

if(TUGZ_BUILD_TESTS)
    enable_testing()

    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=address,undefined)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
    check_c_source_compiles("int main(void) { return 0; }"
                            TUGZ_HAVE_SANITIZERS)
    cmake_pop_check_state()
    option(TUGZ_SANITIZE "Build tests with ASan and UBSan"
           ${TUGZ_HAVE_SANITIZERS})

    # Debug and test builds: sanitized where supported
    function(tugz_sanitize tgt)
        target_compile_options(${tgt} PRIVATE -g3 -O1)
        if(TUGZ_SANITIZE)
            target_compile_options(${tgt} PRIVATE
                -fsanitize=address,undefined -fno-sanitize-recover=all)
            target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
        endif()
    endfunction()

    # A unit test program, run from the build directory
    function(tugz_test tgt test entry)
        tugz_have(ok "${test}" ${ARGN})
        if(ok)
            add_executable(${tgt} ${entry})
            set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${test})
            tugz_common(${tgt})
            tugz_sanitize(${tgt})
            tugz_reflibs(${tgt} ${ARGN})
            add_test(NAME ${test} COMMAND ${tgt})
            set_property(GLOBAL APPEND PROPERTY tugz_test_targets ${tgt})
        endif()
    endfunction()

    tugz_test(tugz_tests tests test/tests.c ZLIB LIBDEFLATE)
    tugz_test(tugz_tests_lib tests-lib test/libtests.c ZLIB)
    tugz_test(tugz_tests_zip tests-zip test/ziptests.c)
    tugz_test(tugz_tests_zipcli tests-zipcli test/zipclitests.c)
    tugz_test(tugz_tests_unzip tests-unzip test/unziptests.c)
    tugz_test(tugz_tests_unzipcli tests-unzipcli test/unzipclitests.c)

    # The programs again, for their end-to-end tests
    tugz_program(tugz_gzip_debug gzip gzip-debug NOSTRIP)
    tugz_sanitize(tugz_gzip_debug)
    tugz_program(tugz_zip_debug zip zip-debug NOSTRIP)
    tugz_sanitize(tugz_zip_debug)
    tugz_program(tugz_unzip_debug unzip unzip-debug NOSTRIP)
    tugz_sanitize(tugz_unzip_debug)
    set_property(GLOBAL APPEND PROPERTY tugz_test_targets
                 tugz_gzip_debug tugz_zip_debug tugz_unzip_debug)

    find_program(TUGZ_SH sh)
    if(EXISTS /usr/bin/gzip)
        set(TUGZ_REF_GZIP /usr/bin/gzip CACHE FILEPATH
            "Reference gzip for the gzip program's tests")
    else()
        find_program(TUGZ_REF_GZIP gzip
            DOC "Reference gzip for the gzip program's tests")
    endif()
    find_program(TUGZ_UNZIP unzip
        DOC "Info-ZIP's unzip, the reference for the unzip program's tests")
    find_program(TUGZ_ZIPINFO zipinfo)

    # Whether a program is Info-ZIP's, by its version information's first
    # line, rather than another of the same name, such as busybox's or
    # tugz's own (which mentions Info-ZIP's too)
    function(tugz_infozip out prog)
        set(${out} OFF PARENT_SCOPE)
        if(prog)
            execute_process(COMMAND ${prog} -v
                OUTPUT_VARIABLE version ERROR_QUIET)
            if(version MATCHES "^(UnZip|ZipInfo) [^\n]*Info-ZIP")
                set(${out} ON PARENT_SCOPE)
            endif()
        endif()
    endfunction()
    tugz_infozip(tugz_have_unzip "${TUGZ_UNZIP}")
    tugz_infozip(tugz_have_zipinfo "${TUGZ_ZIPINFO}")

    # gzip end to end, as installed
    if(TARGET tugz_gzip)
        set(gzip tugz_gzip)
    else()
        set(gzip tugz_gzip_debug)
    endif()
    add_test(NAME gzip-cli
        COMMAND ${TUGZ_SH} ${PROJECT_SOURCE_DIR}/test/cli.sh
                $<TARGET_FILE:${gzip}>)
    set_tests_properties(gzip-cli PROPERTIES
        ENVIRONMENT "REF=${TUGZ_REF_GZIP}")

    # zip end to end, sanitized, with the out-of-memory tests run by a
    # build without AddressSanitizer
    add_test(NAME zip-cli
        COMMAND ${TUGZ_SH} ${PROJECT_SOURCE_DIR}/test/zip.sh
                $<TARGET_FILE:tugz_zip_debug>)
    if(TARGET tugz_zip)
        set_tests_properties(zip-cli PROPERTIES
            ENVIRONMENT "ZIPOOM=$<TARGET_FILE:tugz_zip>")
    endif()
    if(NOT tugz_have_unzip OR NOT tugz_have_zipinfo)
        message(WARNING
            "tugz: zip-cli disabled: it needs Info-ZIP's unzip and zipinfo")
        set_tests_properties(zip-cli PROPERTIES DISABLED ON)
    endif()

    # unzip end to end, sanitized, compared with Info-ZIP's, and reading
    # archives from tugz's zip too, with the out-of-memory tests run by a
    # build without AddressSanitizer. On Windows, where its conventions
    # differ, unzip-windows tests it instead.
    if(NOT WIN32)
        add_test(NAME unzip-cli
            COMMAND ${TUGZ_SH} ${PROJECT_SOURCE_DIR}/test/unzip.sh
                    $<TARGET_FILE:tugz_unzip_debug>)
        set(tugz_unzip_env
            "REF=${TUGZ_UNZIP};TUGZ_ZIP=$<TARGET_FILE:tugz_zip_debug>")
        if(TARGET tugz_unzip)
            string(APPEND tugz_unzip_env ";UNZIPOOM=$<TARGET_FILE:tugz_unzip>")
        endif()
        set_tests_properties(unzip-cli PROPERTIES ENVIRONMENT
            "${tugz_unzip_env}")
        if(NOT tugz_have_unzip)
            message(WARNING
                "tugz: unzip-cli disabled: it needs Info-ZIP's unzip")
            set_tests_properties(unzip-cli PROPERTIES DISABLED ON)
        endif()
    endif()

    if(WIN32)
        if(TARGET tugz_zip)
            set(zip tugz_zip)
        else()
            set(zip tugz_zip_debug)
        endif()
        add_test(NAME zip-windows
            COMMAND ${TUGZ_SH} ${PROJECT_SOURCE_DIR}/test/zip_windows.sh
                    $<TARGET_FILE:${zip}>)
        if(TARGET tugz_unzip)
            set(unzip tugz_unzip)
        else()
            set(unzip tugz_unzip_debug)
        endif()
        add_test(NAME unzip-windows
            COMMAND ${TUGZ_SH} ${PROJECT_SOURCE_DIR}/test/unzip_windows.sh
                    $<TARGET_FILE:${unzip}>)
        set_tests_properties(unzip-windows PROPERTIES
            ENVIRONMENT "TUGZ_ZIP=$<TARGET_FILE:${zip}>")
    endif()

    # The single-file library, compiled alone, and embedded in a program
    # with TUGZ_API static
    add_library(tugz_amalgamation_lib STATIC ${tugz_amalgamation_dir}/tugz.c)
    add_dependencies(tugz_amalgamation_lib tugz_amalgamation)
    tugz_common(tugz_amalgamation_lib)
    add_executable(tugz_tests_amalg test/amalgtests.c)
    add_dependencies(tugz_tests_amalg tugz_amalgamation)
    set_target_properties(tugz_tests_amalg PROPERTIES
        OUTPUT_NAME tests-amalg)
    target_include_directories(tugz_tests_amalg PRIVATE
                               ${tugz_amalgamation_dir})
    tugz_common(tugz_tests_amalg)
    tugz_sanitize(tugz_tests_amalg)
    add_test(NAME amalgamation COMMAND tugz_tests_amalg)
    set_property(GLOBAL APPEND PROPERTY tugz_test_targets
                 tugz_amalgamation_lib tugz_tests_amalg)

    # Builds what the tests need and runs them
    if(PROJECT_IS_TOP_LEVEL)
        get_property(deps GLOBAL PROPERTY tugz_test_targets)
        foreach(tgt IN ITEMS tugz_gzip tugz_zip tugz_unzip)
            if(TARGET ${tgt})
                list(APPEND deps ${tgt})
            endif()
        endforeach()
        add_custom_target(check
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
            USES_TERMINAL)
        add_dependencies(check ${deps})
    endif()
endif()

if(TUGZ_BUILD_FUZZ)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
    check_c_source_compiles([[
        #include <stddef.h>
        #include <stdint.h>
        int LLVMFuzzerTestOneInput(uint8_t const *d, size_t n)
        {
            (void)d; (void)n; return 0;
        }]] TUGZ_HAVE_LIBFUZZER)
    cmake_pop_check_state()
    if(NOT TUGZ_HAVE_LIBFUZZER)
        message(FATAL_ERROR
            "TUGZ_BUILD_FUZZ needs a compiler with libFuzzer "
            "(-fsanitize=fuzzer), such as LLVM clang: "
            "-DCMAKE_C_COMPILER=/path/to/llvm/bin/clang")
    endif()

    function(tugz_fuzz tgt output entry)
        tugz_have(ok "${output}" ${ARGN})
        if(ok)
            add_executable(${tgt} ${entry})
            set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${output})
            tugz_common(${tgt})
            set(flags -g3 -O1 -Wno-unused-function
                -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
            target_compile_options(${tgt} PRIVATE ${flags})
            target_link_options(${tgt} PRIVATE
                -fsanitize=fuzzer,address,undefined)
            tugz_reflibs(${tgt} ${ARGN})
        endif()
    endfunction()

    tugz_fuzz(tugz_fuzz_inflate fuzz-inflate test/fuzz_inflate.c)
    tugz_fuzz(tugz_fuzz_roundtrip fuzz-roundtrip test/fuzz_roundtrip.c ZLIB)
    tugz_fuzz(tugz_fuzz_diff_inflate fuzz-diff-inflate
              test/fuzz_diff_inflate.c ZLIB)
    tugz_fuzz(tugz_fuzz_diff_deflate fuzz-diff-deflate
              test/fuzz_diff_deflate.c ZLIB LIBDEFLATE)
    tugz_fuzz(tugz_fuzz_zipread fuzz-zipread test/fuzz_zipread.c)
    tugz_fuzz(tugz_fuzz_zip fuzz-zip test/fuzz_zip.c)
    tugz_fuzz(tugz_fuzz_unzip fuzz-unzip test/fuzz_unzip.c ZLIB)

    # Seed corpora, in the source tree's fuzz/corpus/
    find_program(TUGZ_UV uv)
    add_custom_target(tugz_fuzz_seeds
        COMMAND ${TUGZ_UV} run --no-project python test/seeds.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        USES_TERMINAL
        VERBATIM)
endif()

if(TUGZ_BUILD_BENCH)
    tugz_have(ok bench ZLIB LIBDEFLATE)
    if(ok)
        add_executable(tugz_bench test/bench.c)
        set_target_properties(tugz_bench PROPERTIES OUTPUT_NAME bench)
        tugz_common(tugz_bench)
        target_compile_options(tugz_bench PRIVATE -O2 -Wno-unused-function)
        tugz_reflibs(tugz_bench ZLIB LIBDEFLATE)
    endif()
endif()

# ---- Install ---------------------------------------------------------

if(TUGZ_INSTALL)
    include(CMakePackageConfigHelpers)
    set(tugz_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/tugz)
    if(TARGET tugz)
        install(TARGETS tugz EXPORT tugzTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(EXPORT tugzTargets
            NAMESPACE tugz::
            DESTINATION ${tugz_cmakedir})
        configure_package_config_file(cmake/tugzConfig.cmake.in
            ${PROJECT_BINARY_DIR}/tugzConfig.cmake
            INSTALL_DESTINATION ${tugz_cmakedir})
        write_basic_package_version_file(
            ${PROJECT_BINARY_DIR}/tugzConfigVersion.cmake
            COMPATIBILITY SameMajorVersion)
        install(FILES
            ${PROJECT_BINARY_DIR}/tugzConfig.cmake
            ${PROJECT_BINARY_DIR}/tugzConfigVersion.cmake
            DESTINATION ${tugz_cmakedir})
    endif()
    # Installed as gzip, zip, and unzip, whenever built, as they may be the
    # system's
    foreach(tgt IN ITEMS tugz_gzip tugz_zip tugz_unzip)
        if(TARGET ${tgt})
            install(TARGETS ${tgt}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                COMPONENT programs)
        endif()
    endforeach()
endif()
