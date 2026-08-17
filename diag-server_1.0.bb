SUMMARY = "RDK diagnostic execution service (diag-server)"
DESCRIPTION = "Lightweight WRP diagnostic service for RDK CPE devices. \
Registers with Parodus over raw nanomsg (no libparodus/wrp-c/cimplog \
dependency), executes a catalog-defined diagnostic command per plane \
(triage/management/control/config-apply), and returns command output \
in a WRP response. Also serves a local-only endpoint for Dispatch Core \
and a small local control protocol (DESCRIBE/HEALTH/PUSH/CHANGED) \
alongside the frozen EXEC wire contract."
HOMEPAGE = "https://github.com/<org>/<repo>"

# TODO: no LICENSE/SPDX identifier exists anywhere in the source tree
# (checked diag-server-nn.c, README.md, REQUIREMENTS.md -- none declare
# one). LICENSE = "CLOSED" is used here as the safe, explicit default so
# this recipe fails loudly rather than silently mis-declaring a license
# if left as-is -- do NOT ship this recipe without setting a real
# LICENSE/LIC_FILES_CHKSUM once the actual license for this codebase is
# known. If it's meant to be OpenPDMv2/Apache-2.0 (common for RDK
# components), add a LICENSE file to the source tree first, then point
# LIC_FILES_CHKSUM at it.
LICENSE = "CLOSED"
# LIC_FILES_CHKSUM = "file://LICENSE;md5=<checksum>"

# TODO: point this at wherever this source tree actually lives once it's
# in real version control -- this repo currently has no remote/tag/commit
# of its own (it's been developed in-place, see README.md's own history).
# ${PV} intentionally left unpinned to a real revision until one exists;
# do not release this recipe with SRCREV left at a floating branch head.
SRC_URI = "git://github.com/<org>/<repo>;protocol=https;branch=main \
           file://diag-server.service \
          "
SRCREV = "${AUTOREV}"
S = "${WORKDIR}/git/external/diag-server"

PV = "1.0"
PR = "r0"

# Build-time dependencies -- matches CMakeLists.txt's target_link_libraries
# exactly (nanomsg, msgpackc, cjson, pthread). pthread comes from libc on
# a glibc/musl target, no separate recipe dependency needed.
DEPENDS = "nanomsg msgpack-c cjson"

# Runtime: nanomsg/msgpack-c/cjson are all shared libraries diag-server
# dynamically links against -- normally auto-detected via shlibs by
# bitbake and don't need to be listed here explicitly, but are named for
# clarity/documentation. Remove if your shlibs provider names differ.
RDEPENDS:${PN} = "nanomsg msgpack-c cjson"

inherit cmake systemd

# ── Build options (CMake cache options this recipe exposes) ─────────────
# CMakeLists.txt defines two option()s, both #ifndef-guarded through to
# diag-server-nn.c's own compiled-in defaults (REGISTER_WITH_PARODUS=1,
# PUSH_REQUIRE_LOCAL_ONLY=0) -- building this recipe with neither
# PACKAGECONFIG feature touched reproduces those defaults exactly.
#
#   register-with-parodus       -> DIAG_SERVER_REGISTER_WITH_PARODUS
#     ON  (default): diag-server sends its WRP type-9 registration to
#         Parodus at startup and is reachable on the public pair.
#     OFF: registration is suppressed; diag-server is reachable only via
#         its local-only endpoint (see docs/24_diag_server_merge_plan.md
#         §15 B.4 part 2 -- "the actual point of no return" -- for why a
#         project might want this once Dispatch Core owns the public
#         registration and ACL-gates traffic ahead of diag-server).
#
#   push-require-local-only     -> DIAG_SERVER_PUSH_REQUIRE_LOCAL_ONLY
#     OFF (default): a catalog PUSH is accepted on either transport, with
#         NO ACL check on who sends it -- see diag-server-nn.c's own
#         PUSH_REQUIRE_LOCAL_ONLY comment for the exposure this default
#         carries; it matches this codebase's *current* source default,
#         not a security recommendation.
#     ON: PUSH is rejected outright unless received on the local-only
#         endpoint, closing the public-path catalog-rewrite exposure
#         above (this was the original, more conservative default before
#         2026-08-16's change to the source -- see README.md/docs/24 for
#         the full history of why it was loosened).
#
# Neither PACKAGECONFIG feature pulls in an extra DEPENDS -- both just
# flip a CMake bool that becomes a -D compile definition, no new library.
PACKAGECONFIG ??= "register-with-parodus"
PACKAGECONFIG[register-with-parodus] = "-DDIAG_SERVER_REGISTER_WITH_PARODUS=ON,-DDIAG_SERVER_REGISTER_WITH_PARODUS=OFF,,"
PACKAGECONFIG[push-require-local-only] = "-DDIAG_SERVER_PUSH_REQUIRE_LOCAL_ONLY=ON,-DDIAG_SERVER_PUSH_REQUIRE_LOCAL_ONLY=OFF,,"

# Standard CMake build-type knob -- not a diag-server-specific option,
# but worth surfacing since nothing else in this recipe sets it and an
# unset CMAKE_BUILD_TYPE means an unoptimized, non-stripped default
# build. RelWithDebInfo is the usual OE default for target recipes
# (optimized, but keeps debug info split out via dbg-package); override
# per-image with a bbappend if a given product wants plain Release.
EXTRA_OECMAKE += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"

# ── Known, pre-existing build gap -- not introduced by this recipe ──────
# CMakeLists.txt's target_link_options() references
# ${CMAKE_CURRENT_SOURCE_DIR}/glibc_version.map via
# -Wl,--version-script=..., but no glibc_version.map file exists anywhere
# in this source tree as of this recipe being written. The link step
# WILL FAIL with this recipe as written until that file is added to the
# source tree (or the target_link_options() call is removed from
# CMakeLists.txt, if the version-script was never actually needed for an
# executable target -- version scripts are far more commonly used for
# shared libraries' export symbol control than for a plain executable
# like diag-server). Flagged here rather than silently worked around by
# fabricating a version-script's contents, since its correct contents
# depend on which symbols (if any) this executable is meant to export
# dynamically -- a decision for whoever owns this build, not something
# to guess at in a packaging recipe.

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/diag-server.service ${D}${systemd_system_unitdir}/diag-server.service

    # Default per-plane catalog directory (CATALOG_DIR in diag-server-nn.c,
    # overridable per-deployment via argv[1] / the service file's
    # ExecStart=). Only the triage plane's catalog ships in this source
    # tree today (diag-triage-catalog.json) -- see
    # diag-server-toolsets-and-usage.md for what's in it and why
    # management/control/config-apply have no file yet.
    install -d ${D}${sysconfdir}/diag-server
    install -m 0644 ${S}/diag-triage-catalog.json ${D}${sysconfdir}/diag-server/diag-triage-catalog.json
}

SYSTEMD_SERVICE:${PN} = "diag-server.service"
# AUTO_ENABLE left at its OE default (enable) -- override per-image with
# SYSTEMD_AUTO_ENABLE:${PN} = "disable" if a given product wants this
# started manually/by another orchestrator instead.

FILES:${PN} += "${systemd_system_unitdir}/diag-server.service \
                ${sysconfdir}/diag-server \
               "
CONFFILES:${PN} += "${sysconfdir}/diag-server/diag-triage-catalog.json"
