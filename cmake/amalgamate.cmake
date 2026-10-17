# Single-file amalgamations of the tugz sources, without a configure step:
#   $ cmake -P cmake/amalgamate.cmake
#   $ cmake -DTUGZ_ARTIFACT=libtugz -DTUGZ_OUTPUT_DIR=dist -P cmake/amalgamate.cmake
# TUGZ_ARTIFACT is gzip, zip, unzip, tugz, libtugz, or all (the default),
# and TUGZ_OUTPUT_DIR the directory written (default: the current one).
#   gzip.c, zip.c, unzip.c  the CRT-free Windows programs, with their
#                           build commands
#   tugz.c                  the Windows GUI, tugz.exe, its icon inlined,
#                           with its build command
#   libtugz.c               the library with tugz.h inlined, and a copy
#                           of tugz.h
# The inputs are the quoted .c includes of platform/gzip_windows.c,
# platform/zip_windows.c, platform/unzip_windows.c, and
# platform/libtugz.c, in order, then the entry file itself, so the unity
# files stay the only lists of sources. platform/gui_windows.c's includes
# are inlined where they are, as it defines macros around some, and its
# leading comment, up to its first "//" line, gives way to the banner.
# Between files goes a blank line, and local includes and "// $ cc"
# build lines are dropped, but for src/base.c's include of tugz.h, which
# in the programs becomes the header's version lines (in libtugz.c, the
# whole header comes first). Each line of an .incbin of platform/tugz.ico
# (a literal offset, and length or none for the rest) becomes the bytes
# it names, so that tugz.c needs no other file. Around the library core,
# libtugz.c saves and restores (push_macro, pop_macro) each name the core
# defines as a macro or a type, the type names becoming tugz__ ones.
# Names are sorted by byte, as "LC_ALL=C sort -u" sorts, so output is the
# same in any locale.
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

# Sets out to a line, escaped, with an .incbin of platform/tugz.ico in a
# C string of top-level asm, as the lines of .byte that it names: any
# label before it on a line of its own, with the line's comment.
set(incbin_line "^ *\"[^\"]*\\.incbin ")
function(tugz_incbin out line)
    set(e "${esc_backslash}")
    set(pattern "^( *)\"([^\"]*)\\.incbin ${e}\"platform/tugz\\.ico${e}\", "
                "([0-9]+)(, ([0-9]+))?${e}n\"( *//.*)?$")
    string(CONCAT pattern ${pattern})
    if(NOT line MATCHES "${pattern}")
        message(FATAL_ERROR "not a literal .incbin of tugz.ico: ${line}")
    endif()
    set(indent "${CMAKE_MATCH_1}")
    string(STRIP "${CMAKE_MATCH_2}" label)
    set(offset "${CMAKE_MATCH_3}")
    set(len "${CMAKE_MATCH_5}")
    set(comment "${CMAKE_MATCH_6}")
    if(len STREQUAL "")
        file(READ "${root}/platform/tugz.ico" hex OFFSET ${offset} HEX)
    else()
        file(READ "${root}/platform/tugz.ico" hex OFFSET ${offset}
             LIMIT ${len} HEX)
        string(LENGTH "${hex}" got)
        math(EXPR want "2 * ${len}")
        if(NOT got EQUAL want)
            message(FATAL_ERROR "platform/tugz.ico is too short: ${line}")
        endif()
    endif()
    set(text "")
    if(NOT label STREQUAL "")
        string(APPEND text "${indent}\"${label}${e}n\"${comment}\n")
    elseif(NOT comment STREQUAL "")
        string(STRIP "${comment}" comment)
        string(APPEND text "${indent}${comment}\n")
    endif()
    # Twelve bytes to a row
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    string(REPEAT "0x..," 12 row)
    string(REGEX REPLACE "(${row})" "\\1;" hex "${hex}")
    foreach(row IN LISTS hex)
        if(NOT row STREQUAL "")
            string(REGEX REPLACE ",$" "" row "${row}")
            string(APPEND text "${indent}\"    .byte ${row}${e}n\"\n")
        endif()
    endforeach()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Appends files to var, a blank line between them, without local
# includes or build lines. The include of tugz.h becomes header_text.
function(tugz_concat var header_text)
    set(text "${${var}}")
    set(first TRUE)
    foreach(path IN LISTS ARGN)
        if(NOT first)
            string(APPEND text "\n")
        endif()
        set(first FALSE)
        tugz_lines(lines "${path}")
        foreach(line IN LISTS lines)
            if(line MATCHES "^#include \"[./]*tugz\\.h\"")
                string(APPEND text "${header_text}")
            elseif(line MATCHES "${incbin_line}")
                tugz_incbin(bytes "${line}")
                string(APPEND text "${bytes}")
            elseif(NOT line MATCHES "^#include \"" AND
                   NOT line MATCHES "^// +\\$ cc")
                string(APPEND text "${line}\n")
            endif()
        endforeach()
    endforeach()
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# The release, TUGZ_VERSION in tugz.h, and the lines that define it and
# its parts, which take the place of tugz.h in the programs (src/base.c
# includes it for them)
set(pattern "^#define TUGZ_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
file(STRINGS "${root}/tugz.h" v REGEX "${pattern}")
if(NOT v MATCHES "${pattern}")
    message(FATAL_ERROR "no TUGZ_VERSION in tugz.h")
endif()
set(v "${CMAKE_MATCH_1}")
file(STRINGS "${root}/tugz.h" version_lines
     REGEX "^#define TUGZ_VERSION(_[A-Z]+)? ")
list(JOIN version_lines "\n" version_text)
set(version_text "${version_text}\n")

# Writes text, unescaped, to a file in the output directory, through a
# temporary file renamed over it. Lines end in LF on every host, which
# file(WRITE) would not do on Windows, while file(CONFIGURE) substitutes
# @name@ references, so each @ is written as one.
function(tugz_write name text)
    string(REPLACE "${esc_backslash}" "\\" text "${text}")
    string(REPLACE "${esc_semicolon}" ";" text "${text}")
    string(REPLACE "${esc_lbracket}" "[" text "${text}")
    string(REPLACE "${esc_rbracket}" "]" text "${text}")
    string(REPLACE "@" "@at@" text "${text}")
    set(at "@")
    file(CONFIGURE OUTPUT "${outdir}/${name}.tmp" CONTENT "${text}"
         @ONLY NEWLINE_STYLE UNIX)
    file(RENAME "${outdir}/${name}.tmp" "${outdir}/${name}")
endfunction()

function(tugz_gzip)
    tugz_inputs(files platform/gzip_windows.c)
    set(text
        "// tugz ${v}: tiny unity gzip, a drop-in gzip for Windows\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory\n"
        "// Copies named gunzip.exe or zcat.exe decompress by default.\n"
        "\n")
    string(CONCAT text ${text})
    tugz_concat(text "${version_text}" ${files})
    tugz_write(gzip.c "${text}")
endfunction()

function(tugz_zip)
    tugz_inputs(files platform/zip_windows.c)
    set(text
        "// tugz zip ${v}: an Info-ZIP compatible zip for Windows\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory\n"
        "\n")
    string(CONCAT text ${text})
    tugz_concat(text "${version_text}" ${files})
    tugz_write(zip.c "${text}")
endfunction()

function(tugz_unzip)
    tugz_inputs(files platform/unzip_windows.c)
    set(text
        "// tugz unzip ${v}: an Info-ZIP compatible unzip for Windows\n"
        "// Single-file amalgamation of the tugz sources. Build:\n"
        "//   $ cc -O2 -nostartfiles -o unzip.exe unzip.c -lmemory\n"
        "// A copy named zipinfo.exe lists archives as Info-ZIP's zipinfo.\n"
        "\n")
    string(CONCAT text ${text})
    tugz_concat(text "${version_text}" ${files})
    tugz_write(unzip.c "${text}")
endfunction()

function(tugz_gui)
    set(entry "${root}/platform/gui_windows.c")
    get_filename_component(dir "${entry}" DIRECTORY)
    set(text
        "// tugz ${v}: a Windows GUI for zip archives, tugz.exe\n"
        "// Single-file amalgamation of the tugz sources. Build, with w64devkit:\n"
        "//   $ cc -O2 -fno-builtin -nostartfiles -mwindows -o tugz.exe tugz.c -lmemory -lcomctl32 -lole32\n"
        "// For XP (i686), as there, and to address more memory, add:\n"
        "//   -Wl,--large-address-aware\n"
        "//\n")
    string(CONCAT text ${text})
    tugz_lines(lines "${entry}")
    set(lead TRUE)     # in the entry's leading comment, before its "//"
    set(after FALSE)   # the line before was an inlined file
    foreach(line IN LISTS lines)
        if(lead)
            if(line STREQUAL "//")
                set(lead FALSE)
            elseif(NOT line MATCHES "^//")
                message(FATAL_ERROR "no \"//\" line in gui_windows.c's head")
            endif()
            continue()
        endif()
        if(line MATCHES "^#include \"([^\"]*\\.c)\"")
            get_filename_component(path "${dir}/${CMAKE_MATCH_1}" ABSOLUTE)
            if(NOT text MATCHES "\n\n$")
                string(APPEND text "\n")  # a blank line before each file
            endif()
            tugz_concat(text "${version_text}" "${path}")
            set(after TRUE)
            continue()
        elseif(after AND NOT line STREQUAL "")
            string(APPEND text "\n")
        endif()
        set(after FALSE)
        if(line MATCHES "${incbin_line}")
            tugz_incbin(bytes "${line}")
            string(APPEND text "${bytes}")
        elseif(NOT line MATCHES "^#include \"" AND
               NOT line MATCHES "^// +\\$ cc")
            string(APPEND text "${line}\n")
        endif()
    endforeach()
    tugz_write(tugz.c "${text}")
endfunction()

function(tugz_library)
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
        "//   $ cc -c -O2 libtugz.c\n"
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
    tugz_concat(text "" ${files})  # the header is above
    string(APPEND text "\n")
    foreach(n IN LISTS macros types)
        string(APPEND text "#pragma pop_macro(\"${n}\")\n")
    endforeach()
    tugz_write(libtugz.c "${text}")

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
elseif(TUGZ_ARTIFACT STREQUAL "unzip")
    tugz_unzip()
elseif(TUGZ_ARTIFACT STREQUAL "tugz")
    tugz_gui()
elseif(TUGZ_ARTIFACT STREQUAL "libtugz")
    tugz_library()
elseif(TUGZ_ARTIFACT STREQUAL "all")
    tugz_gzip()
    tugz_zip()
    tugz_unzip()
    tugz_gui()
    tugz_library()
else()
    message(FATAL_ERROR
        "TUGZ_ARTIFACT must be gzip, zip, unzip, tugz, libtugz, or all")
endif()
