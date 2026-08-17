cmake_minimum_required(VERSION 3.10)
project(diag-server C)

set(CMAKE_C_STANDARD 11)

# Added 2026-08-16: the only two build-time-configurable behaviors this
# source currently has -- both are #ifndef-guarded in diag-server-nn.c
# (see REGISTER_WITH_PARODUS's and PUSH_REQUIRE_LOCAL_ONLY's own
# comments there), so an override here changes the compiled-in default
# without touching source. Defaults here match the source's own
# defaults exactly -- building with neither option touched reproduces
# today's behavior bit-for-bit. Exposed as CMake cache options (rather
# than raw target_compile_definitions with a fixed value) specifically
# so a Yocto recipe can surface them as PACKAGECONFIG entries -- see
# diag-server_1.0.bb.
option(DIAG_SERVER_REGISTER_WITH_PARODUS
       "Send the WRP type-9 registration to Parodus at startup" ON)
option(DIAG_SERVER_PUSH_REQUIRE_LOCAL_ONLY
       "Reject a catalog PUSH received via the public (Parodus) pair, accepting it only on the local-only endpoint" OFF)

# Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md §2): the source
# filename here already matched diag-server-nn.c in the uploaded baseline --
# README.md and REQUIREMENTS.md described a diag-server.c/diag-server-nn.c
# mismatch that does not exist in this file. Left as-is; the stale
# documentation was corrected instead of this line.
add_executable(diag-server diag-server-nn.c)

# Added 2026-08-16: translates the two cache options above into the
# #ifndef-guarded #defines diag-server-nn.c reads. Generator expression
# form (rather than a plain if/else block calling
# target_compile_definitions twice) so both options are handled
# identically in one place.
target_compile_definitions(diag-server PRIVATE
    REGISTER_WITH_PARODUS=$<BOOL:${DIAG_SERVER_REGISTER_WITH_PARODUS}>
    PUSH_REQUIRE_LOCAL_ONLY=$<BOOL:${DIAG_SERVER_PUSH_REQUIRE_LOCAL_ONLY}>
)

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
