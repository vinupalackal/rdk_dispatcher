cmake_minimum_required(VERSION 3.10)
project(diag-server C)

set(CMAKE_C_STANDARD 11)

# Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md §2): the source
# filename here already matched diag-server-nn.c in the uploaded baseline --
# README.md and REQUIREMENTS.md described a diag-server.c/diag-server-nn.c
# mismatch that does not exist in this file. Left as-is; the stale
# documentation was corrected instead of this line.
add_executable(diag-server diag-server-nn.c)

target_link_options(diag-server PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/glibc_version.map
)

# Corrected 2026-08-14: diag-server-nn.c talks to Parodus over raw nanomsg
# sockets directly (see the file's own header comment) and never calls into
# wrp-c, libparodus, or cimplog -- those include paths were removed.
target_include_directories(diag-server PRIVATE
    ${CMAKE_INSTALL_PREFIX}/include
    /usr/include/cjson
)

target_link_directories(diag-server PRIVATE
    ${CMAKE_INSTALL_PREFIX}/lib
)

# Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md §2): wrp-c,
# libparodus, and cimplog were linked but never referenced by the code --
# likely carried over from a sibling project's CMakeLists (the previous
# comment here said "mirrors apparmor-simulator"). Removed; only the
# libraries diag-server-nn.c actually uses remain.
#
# nanomsg   -- transport (PUSH/PULL sockets to Parodus)
# msgpackc  -- WRP outer envelope + inner payload encoding
# cjson     -- catalog.json loading
# pthread   -- detached per-request worker threads
target_link_libraries(diag-server
    nanomsg
    msgpackc
    cjson
    pthread
)

install(TARGETS diag-server DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
