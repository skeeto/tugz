# Single-file amalgamations of the tugz sources, without a configure step:
#   $ cmake -P cmake/amalgamate.cmake
#   $ cmake -DTUGZ_ARTIFACT=tugz -DTUGZ_OUTPUT_DIR=dist -P cmake/amalgamate.cmake
# TUGZ_ARTIFACT is gzip, zip, tugz, or all (the default), and
# TUGZ_OUTPUT_DIR the directory written (default: the current one).
#   gzip.c, zip.c  the CRT-free Windows programs, with their build commands
#   tugz.c         the library with tugz.h inlined, and a copy of tugz.h
# The inputs are the quoted .c includes of platform/gzip_windows.c,
# platform/zip_windows.c, and platform/libtugz.c, in order, then the
# entry file itself, so the unity files stay the only lists of sources.
# Between files goes a blank line, and local includes and "// $ cc"
# build lines are dropped. Around the library core, tugz.c saves and
# restores (push_macro, pop_macro) each name the core defines as a macro
# or a type, the type names becoming tugz__ ones. Names are sorted by
# byte, as "LC_ALL=C sort -u" sorts, so output is the same in any locale.
cmake_minimum_required(VERSION 3.21...4.4)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED TUGZ_ARTIFACT)
    set(TUGZ_ARTIFACT all)
endif()
if(NOT DEFINED TUGZ_OUTPUT_DIR)
    set(TUGZ_OUTPUT_DIR .)
endif()
get_filename_component(outdir "${TUGZ_OUTPUT_DIR}" ABSOLUTE)
file(MAKE_DIRECTORY "${outdir}")

# Characters that list operations would interpret stand in as control
# characters, which no source contains, until output is written.
string(ASCII 1 esc_backslash)
string(ASCII 2 esc_semicolon)
string(ASCII 3 esc_lbracket)
string(ASCII 4 esc_rbracket)

# Sets out to a file's lines as a list, escaped.
function(tugz_lines out path)
    file(READ "${path}" text)
    string(REPLACE "\\" "${esc_backslash}" text "${text}")
    string(REPLACE ";" "${esc_semicolon}" text "${text}")
    string(REPLACE "[" "${esc_lbracket}" text "${text}")
    string(REPLACE "]" "${esc_rbracket}" text "${text}")
    string(REGEX REPLACE "\n$" "" text "${text}")
    string(REPLACE "\n" ";" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Sets out to an entry file's quoted .c includes, then the file itself.
function(tugz_inputs out entry)
    get_filename_component(dir "${root}/${entry}" DIRECTORY)
    tugz_lines(lines "${root}/${entry}")
    set(files "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^#include \"([^\"]*\\.c)\"")
            get_filename_component(path "${dir}/${CMAKE_MATCH_1}" ABSOLUTE)
            list(APPEND files "${path}")
        endif()
    endforeach()
    list(APPEND files "${root}/${entry}")
    set(${out} "${files}" PARENT_SCOPE)
endfunction()

# Appends files to var, a blank line between them, without local
# includes or build lines.
function(tugz_concat var)
    set(text "${${var}}")
    set(first TRUE)
    foreach(path IN LISTS ARGN)
        if(NOT first)
            string(APPEND text "\n")
        endif()
        set(first FALSE)
        tugz_lines(lines "${path}")
        foreach(line IN LISTS lines)
            if(NOT line MATCHES "^#include \"" AND
               NOT line MATCHES "^// +\\$ cc")
                string(APPEND text "${line}\n")
            endif()
        endforeach()
    endforeach()
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# Sets out to the version in a line of a file matching a pattern, whose
# first group is the version.
function(tugz_version out path pattern)
    file(STRINGS "${path}" lines REGEX "${pattern}")
    if(NOT lines MATCHES "${pattern}")
        message(FATAL_ERROR "no version in ${path}")
    endif()
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Writes text, unescaped, to a file in the output directory, through a
# temporary file renamed over it.
function(tugz_write name text)
    string(REPLACE "${esc_backslash}" "\\" text "${text}")
    string(REPLACE "${esc_semicolon}" ";" text "${text}")
    string(REPLACE "${esc_lbracket}" "[" text "${text}")
    string(REPLACE "${esc_rbracket}" "]" text "${text}")
    file(WRITE "${outdir}/${name}.tmp" "${text}")
    file(RENAME "${outdir}/${name}.tmp" "${outdir}/${name}")
endfunction()

function(tugz_gzip)
    tugz_version(v "${root}/src/cli.c" "gzip \\(tugz\\) ([0-9.]*)")
    tugz_inputs(files platform/gzip_windows.c)
    set(text
        "// tugz ${v}: tiny unity gzip, a drop-in gzip for Windows\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory\n"
        "// Copies named gunzip.exe or zcat.exe decompress by default.\n"
        "\n")
    string(CONCAT text ${text})
    tugz_concat(text ${files})
    tugz_write(gzip.c "${text}")
endfunction()

function(tugz_zip)
    tugz_version(v "${root}/src/zipcli.c" "tugz zip ([0-9][0-9.]*)")
    tugz_inputs(files platform/zip_windows.c)
    set(text
        "// tugz zip ${v}: an Info-ZIP compatible zip for Windows\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory\n"
        "\n")
    string(CONCAT text ${text})
    tugz_concat(text ${files})
    tugz_write(zip.c "${text}")
endfunction()

function(tugz_library)
    tugz_version(v "${root}/src/cli.c" "gzip \\(tugz\\) ([0-9.]*)")
    tugz_inputs(files platform/libtugz.c)

    # Names the core defines as macros, and as types: "typedef ... name;"
    # and "} name;" (lines are escaped, so the semicolon is a stand-in)
    set(macros "")
    set(types "")
    set(name "([A-Za-z_][A-Za-z_0-9]*)${esc_semicolon}$")
    foreach(path IN LISTS files)
        tugz_lines(lines "${path}")
        foreach(line IN LISTS lines)
            if(line MATCHES "^ *# *define ([A-Za-z_0-9]*)")
                list(APPEND macros "${CMAKE_MATCH_1}")
            endif()
            if(line MATCHES "^typedef .*[ *]${name}")
                list(APPEND types "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^} ${name}")
                list(APPEND types "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()
    foreach(list IN ITEMS macros types)
        list(REMOVE_ITEM ${list} "")
        list(REMOVE_DUPLICATES ${list})
        list(SORT ${list} COMPARE STRING CASE SENSITIVE)
    endforeach()

    file(READ "${root}/tugz.h" header)
    set(text
        "// tugz ${v}: streaming DEFLATE, zlib, and gzip library\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -c -O2 tugz.c\n"
        "// The interface documentation follows.\n"
        "\n")
    string(CONCAT text ${text})
    string(APPEND text "${header}\n")
    foreach(n IN LISTS macros types)
        string(APPEND text "#pragma push_macro(\"${n}\")\n#undef ${n}\n")
    endforeach()
    foreach(n IN LISTS types)
        string(APPEND text "#define ${n} tugz__${n}\n")
    endforeach()
    string(APPEND text "\n")
    tugz_concat(text ${files})
    string(APPEND text "\n")
    foreach(n IN LISTS macros types)
        string(APPEND text "#pragma pop_macro(\"${n}\")\n")
    endforeach()
    tugz_write(tugz.c "${text}")

    # The header beside it, unless this is the source tree itself
    file(REAL_PATH "${root}/tugz.h" src)
    if(EXISTS "${outdir}/tugz.h")
        file(REAL_PATH "${outdir}/tugz.h" dst)
    else()
        set(dst "${outdir}/tugz.h")
    endif()
    if(NOT src STREQUAL dst)
        file(COPY_FILE "${src}" "${outdir}/tugz.h.tmp")
        file(RENAME "${outdir}/tugz.h.tmp" "${outdir}/tugz.h")
    endif()
endfunction()

if(TUGZ_ARTIFACT STREQUAL "gzip")
    tugz_gzip()
elseif(TUGZ_ARTIFACT STREQUAL "zip")
    tugz_zip()
elseif(TUGZ_ARTIFACT STREQUAL "tugz")
    tugz_library()
elseif(TUGZ_ARTIFACT STREQUAL "all")
    tugz_gzip()
    tugz_zip()
    tugz_library()
else()
    message(FATAL_ERROR "TUGZ_ARTIFACT must be gzip, zip, tugz, or all")
endif()
