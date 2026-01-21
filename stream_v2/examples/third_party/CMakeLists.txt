# Build compression into the top-level build _deps directory to keep sources clean.
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/compression
    ${CMAKE_BINARY_DIR}/_deps/compression
    EXCLUDE_FROM_ALL)

add_library(tinycbor OBJECT
    tinycbor/src/cborencoder.c
    tinycbor/src/cborencoder_close_container_checked.c
    tinycbor/src/cborerrorstrings.c
    tinycbor/src/cborparser.c
    tinycbor/src/cborparser_dup_string.c
    tinycbor/src/cborpretty.c
    tinycbor/src/cborpretty_stdio.c
    tinycbor/src/cborvalidation.c
    )
