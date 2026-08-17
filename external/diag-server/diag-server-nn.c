/*
 * diag-server-nn.c — WRP diagnostic service for RDK CPE devices.
 *
 * Registers with parodus via raw nanomsg sockets (no libparodus dependency).
 * Receives WRP type-3 requests from the cloud OPS Gateway, executes the
 * requested tool from the catalog, and returns the result as a WRP response.
 *
 * Threaded: each request is dispatched to a detached pthread so the main
 * loop continuously drains the PULL socket for parodus keepalives.
 *
 * Usage: diag-server [catalog-dir]
 *   catalog-dir   directory containing per-plane catalog files
 *                 (default: /etc/diag-server). Each of
 *                 diag-triage-catalog.json, diag-management-catalog.json,
 *                 diag-control-catalog.json, diag-config-apply-catalog.json
 *                 is loaded from this directory if present; a plane with
 *                 no catalog file there is simply not served. Changed
 *                 2026-08-15 (docs/24_diag_server_merge_plan.md §15 B.5)
 *                 from a single catalog *file* path to a *directory* of
 *                 per-plane catalog files -- a deploy-facing CLI change.
 *
 * Payload format (msgpack):
 *   EXEC (default -- no "kind" field, or "kind":"EXEC"; unchanged since
 *   before this project's merge work):
 *     Request:  {"tool": "<name>", "command": "", "plane": ""}
 *               — command and plane may both be omitted. "plane" is
 *               optional (added 2026-08-15, §5 amendment/§15 B.5): if
 *               given, the tool is looked up only in that plane's catalog;
 *               if omitted, all loaded planes are searched, and a tool
 *               name declared in more than one plane's catalog is treated
 *               as unresolvable on this path (see catalog_lookup()).
 *     Response: {"tool": "<name>", "exit_code": N, "stdout": <bin>}
 *
 *   Four more message kinds, added 2026-08-15 (docs/24_diag_server_merge_plan.md
 *   §11.1/§15 B.3), all riding this same payload shape's outer WRP type-3
 *   envelope, disambiguated by a "kind" string field EXEC never carries:
 *     DESCRIBE  Request:  {"kind":"DESCRIBE", "plane": ""}  (plane optional)
 *               Response: one plane's {plane,version,tools:[{name,type,plane,timeout}]},
 *                         or (no plane given) an array of that shape per loaded plane.
 *     HEALTH    Request:  {"kind":"HEALTH"}
 *               Response: {"status":"ok"}
 *     PUSH      Request:  {"kind":"PUSH", "plane", "base_version", "target_version",
 *                          "diff": {"added":{},"removed":[],"modified":{}}}
 *               Response: {"status":"loaded","plane","version"} or
 *                         {"status":"rejected","plane","reason"}
 *     CHANGED   Unsolicited, sent by diag-server itself right after a
 *               successful PUSH promote: {"kind":"CHANGED","plane","version"}
 *   See handle_local_request() for the dispatch and each kind's own
 *   handler/decoder/builder for the full field semantics.
 *
 * Transport (docs/24_diag_server_merge_plan.md §15 B.4, both parts now
 * implemented -- part 1 added 2026-08-15, part 2 added 2026-08-16
 * after D.1/D.3 passed):
 *   diag-server binds two independent socket pairs:
 *     - the original public pair (CLIENT_URL/PARODUS_URL, via Parodus)
 *     - a local-only pair (DIAG_LOCAL_RECV_URL/DIAG_LOCAL_SEND_URL, for
 *       Dispatch Core, once Phase C exists)
 *   Both are serviced by the same message loop (see service_one_message())
 *   and every kind's handler dispatches identically regardless of which
 *   socket a request arrived on -- with one exception: PUSH is rejected
 *   outright if received via the public pair (see
 *   handle_push_request()'s transport-origin check). DESCRIBE/HEALTH/EXEC
 *   carry no such restriction; EXEC is instead gated per-tool by
 *   diag_acl_check() (§13.4, see below). The local pair is best-effort:
 *   if its directory isn't provisioned, diag-server logs a warning and
 *   falls back to serving only the public pair. Part 2 -- "the actual
 *   point of no return" -- is now flipped: REGISTER_WITH_PARODUS is 0,
 *   so diag-server no longer sends its WRP type-9 registration, and
 *   Parodus has no route to CLIENT_URL as a result. The public
 *   PULL/PUSH sockets themselves are still bound/connected (outbound
 *   traffic like capability_sync.updated is unaffected), only inbound
 *   *registration* is disabled -- see REGISTER_WITH_PARODUS's own
 *   comment for the full rationale and how to revert.
 *
 * ACL gate and capability-sync (added 2026-08-15, docs/24 §13.4, by
 * direct instruction -- see the code comments at diag_acl_check() and
 * diag_notify_capability_sync() for the full design/rationale):
 *   Every EXEC request now passes through diag_acl_check() immediately
 *   after decoding, before catalog lookup -- a thin wrapper around
 *   acl_policy_store_query(), the same ACL entry point every other
 *   toolset already goes through. A denial returns
 *   {"tool","exit_code":126,"stdout":"access denied"} without ever
 *   reaching catalog_lookup()/run_command(). NOTE: acl_policy_store_query()
 *   has no implementation anywhere in this codebase yet (its transport
 *   is still unresolved project-wide) -- this code will not link into a
 *   runnable binary until that's chosen elsewhere. Separately, a
 *   successful PUSH promote now also fires diag_notify_capability_sync(),
 *   a JSON-RPC 2.0 "capability_sync.updated" notification sent over the
 *   public g_push_sock (Parodus), alongside (not instead of) the
 *   existing local-only CHANGED notification.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/wait.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <msgpack.h>
#include <cJSON.h>

/* ── Constants ────────────────────────────────────────────────────────────── */

#define LOG_IDENT            "diag-server"
#define PARODUS_URL          "tcp://127.0.0.1:6666"
#define CLIENT_URL           "tcp://127.0.0.1:6669"
/* Added 2026-08-15 (§15 B.4): the new local-only endpoint, additive
 * alongside the existing public PARODUS_URL/CLIENT_URL pair -- see
 * main()'s "B.4 part 1" section and service_one_message(). Two distinct
 * ipc:// addresses, not one, deliberately mirroring the public pair's
 * own bind/connect split (diag-server binds one side to receive,
 * connects the other to send) rather than switching to nanomsg's
 * REQ/REP socket type -- this keeps every existing PUSH/PULL send/
 * receive call site's shape unchanged, matching §10.3's own text
 * ("reusing its existing nanomsg PUSH/PULL machinery"). NOTE:
 * reference-impl/diag_legacy_framing.c's illustrative DIAG_LOCAL_ENDPOINT
 * is a single address; Phase C's real implementation needs to speak
 * these same two addresses, not one -- flagged here so that work
 * doesn't silently diverge from what diag-server actually binds. */
#define DIAG_LOCAL_RECV_URL  "ipc:///run/dispatcher/diagnostics-in.sock"
#define DIAG_LOCAL_SEND_URL  "ipc:///run/dispatcher/diagnostics-out.sock"
/* Changed 2026-08-16 (§15 B.4 part 2, docs/24_diag_server_merge_plan.md
 * §12.2 D.4): "the actual point of no return," per the plan's own
 * words -- held at 0 (don't register) throughout this whole project
 * until now, since it was explicitly gated on D.1's and D.3's tests
 * passing (both did, 2026-08-16). Once diag-server stops sending its
 * WRP type-9 registration, Parodus has no route to CLIENT_URL, so the
 * local endpoint (part 1, above) becomes the only address anything can
 * actually reach diag-server through. Deliberately a single flag, not
 * a structural change: the public PULL/PUSH socket pair in main() is
 * still bound/connected exactly as before (g_push_sock is still what
 * diag_notify_capability_sync() sends over, and outbound traffic to
 * Parodus is unaffected by inbound registration) -- only the
 * registration *send* is gated. Flipping this back to 1 is the entire
 * revert, if this cutover is ever undone. */
#define REGISTER_WITH_PARODUS 0
#define SERVICE_NAME         "diag-server"
/* Changed 2026-08-15 (§15 B.5): a directory of per-plane catalog files,
 * not one catalog file path. Overridden by argv[1]. */
#define CATALOG_DIR          "/etc/diag-server"
#define MAX_OUTPUT_BYTES     (64 * 1024)
/* Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md §2, FR-013):
 * previously defined but never referenced anywhere -- no timeout was
 * actually enforced. Now the real fallback used when a catalog entry
 * doesn't declare its own "timeout"; see run_command() and the timeout
 * resolution in handle_request(). */
#define DEFAULT_TIMEOUT_SEC  30
#define RECV_TIMEOUT_MS      2000

/* WRP message type constants (from wrp-c/include/wrp-c.h) */
#define WRP_MSG_TYPE_REQ     3
#define WRP_MSG_TYPE_REG     9
#define WRP_MSG_TYPE_ALIVE   10

static const char *BLOCKED_CMDS[] = {
    "rm", "rmdir", "reboot", "shutdown", "halt", "poweroff",
    "factory_reset", "kill", "killall", "pkill", "dd",
    "mkfs", "fdisk", "mount", "umount", "iptables", "passwd",
    NULL
};

/* ── Global state ─────────────────────────────────────────────────────────── */

static int          g_push_sock = -1;
static int          g_pull_sock = -1;
/* Added 2026-08-15 (§15 B.4). g_local_enabled is only set true in
 * main() once *both* the local bind and connect below succeed --
 * best-effort, not fatal to startup if the local directory isn't
 * provisioned yet (e.g. Phase C's Dispatch Core side doesn't exist in
 * this deployment). See main()'s "B.4 part 1" section. */
static int          g_local_push_sock = -1;
static int          g_local_pull_sock = -1;
static int          g_local_enabled   = 0;
static volatile int g_running   = 1;
static const char  *catalog_dir_override = NULL;
static unsigned long g_missing_tool_reqs = 0;

/* Changed 2026-08-15, docs/24_diag_server_merge_plan.md §15 B.5: a single
 * global g_catalog/g_catalog_version pair is replaced by a small fixed
 * table, one entry per plane this project's plane model defines
 * (config-apply/management/control/triage). Each entry's "catalog"
 * stays NULL if that plane's file isn't present at this deployment --
 * that plane is then simply not served, not an error. Keeping catalogs
 * as separate cJSON objects (not merged into one) is deliberate: each
 * plane keeps its own _catalog_version (§15 B.1) and, once §15 B.2's
 * swap mechanism exists, can be updated independently of the others via
 * a plane-scoped PUSH. */
/* Changed 2026-08-15, docs/24_diag_server_merge_plan.md §15 B.2: two
 * fields added for the push/swap/persist pipeline. `loaded_path` is the
 * exact on-disk path this plane's catalog was (or would be) loaded
 * from -- recorded unconditionally by load_catalogs(), even for a
 * plane whose file is currently absent, so a future push that
 * bootstraps a not-yet-provisioned plane still has a real path to
 * persist to. `push_lock` serializes concurrent pushes to *this one*
 * plane (see catalog_apply_push()) -- a different lock, for a
 * different purpose, than g_catalog_mutex below (which protects
 * concurrent reads against a swap, across all planes). */
typedef struct {
    const char     *plane;              /* e.g. "triage" -- matches catalog "plane" field values */
    const char     *filename;           /* e.g. "diag-triage-catalog.json" */
    cJSON          *catalog;            /* NULL if not loaded (file absent or failed to parse) */
    long            version;            /* this plane's own _catalog_version; 0 if catalog is NULL */
    char            loaded_path[512];   /* full path this plane's catalog file lives at */
    pthread_mutex_t push_lock;          /* serializes concurrent pushes to this plane only */
} plane_catalog_t;

#define PLANE_COUNT 4
static plane_catalog_t g_planes[PLANE_COUNT] = {
    { "triage",       "diag-triage-catalog.json",       NULL, 0, {0}, PTHREAD_MUTEX_INITIALIZER },
    { "management",   "diag-management-catalog.json",   NULL, 0, {0}, PTHREAD_MUTEX_INITIALIZER },
    { "control",      "diag-control-catalog.json",      NULL, 0, {0}, PTHREAD_MUTEX_INITIALIZER },
    { "config-apply", "diag-config-apply-catalog.json", NULL, 0, {0}, PTHREAD_MUTEX_INITIALIZER },
};

/* Added 2026-08-15 (§15 B.2). Protects the *swap* moment only -- taken
 * right before catalog_lookup() and released right after a reader has
 * finished copying every field it needs out of the resolved entry
 * (never held across run_command(), which can block for up to a tool's
 * timeout). A promote takes the same mutex for the pointer/version
 * swap itself. See §15 B.2 step 6's design note for why this single
 * short critical section is sufficient without per-entry reference
 * counting. */
static pthread_mutex_t g_catalog_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Signal handler ───────────────────────────────────────────────────────── */

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ── Catalog ──────────────────────────────────────────────────────────────── */

/*
 * load_catalogs() — Changed 2026-08-15 (§15 B.5), superseding the prior
 * single-file load_catalog(). Loads every plane's catalog file found in
 * the configured directory into g_planes[]. A missing file for a given
 * plane is normal (not every deployment populates every plane) and is
 * logged at LOG_INFO, not a warning; a file that exists but fails to
 * parse is logged at LOG_ERR and that plane is left unserved (NULL),
 * exactly like "absent", rather than aborting startup entirely -- one
 * malformed plane catalog should not take every other plane down with
 * it. Returns the count of planes actually loaded (0 is a valid, if
 * unusual, outcome -- the process still starts and serves nothing until
 * a catalog is provisioned, same tolerance load_catalog() always had).
 */
static int load_catalogs(void)
{
    const char *dir = catalog_dir_override ? catalog_dir_override : CATALOG_DIR;
    int loaded = 0;

    for (int i = 0; i < PLANE_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, g_planes[i].filename);
        /* Added 2026-08-15 (§15 B.2): recorded regardless of whether this
         * plane's file exists yet -- catalog_apply_push() needs a real
         * target path to persist to even for a plane being provisioned
         * for the first time via a push. */
        snprintf(g_planes[i].loaded_path, sizeof(g_planes[i].loaded_path), "%s", path);

        FILE *f = fopen(path, "r");
        if (!f) {
            syslog(LOG_INFO, "plane '%s' catalog not present at %s -- not served",
                   g_planes[i].plane, path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *parsed = cJSON_Parse(buf);
        free(buf);
        if (!parsed) {
            syslog(LOG_ERR, "plane '%s' catalog at %s failed to parse -- not served",
                   g_planes[i].plane, path);
            continue;
        }

        g_planes[i].catalog = parsed;

        cJSON *ver = cJSON_GetObjectItem(parsed, "_catalog_version");
        if (ver && cJSON_IsNumber(ver)) {
            g_planes[i].version = (long)ver->valuedouble;
        } else {
            syslog(LOG_WARNING, "plane '%s' catalog at %s has no _catalog_version field; defaulting to 0",
                   g_planes[i].plane, path);
            g_planes[i].version = 0;
        }

        syslog(LOG_INFO, "plane '%s' catalog loaded from %s (version %ld)",
               g_planes[i].plane, path, g_planes[i].version);
        loaded++;
    }
    return loaded;
}

/*
 * detect_cross_plane_collisions() — Added 2026-08-15 (§15 B.5). One-time
 * startup pass, run after every plane's catalog has loaded, purely for
 * syslog visibility: if the same tool name is declared in more than one
 * loaded plane's catalog, log it loudly now rather than only discovering
 * it the first time a no-plane request for that name arrives. This does
 * not itself change lookup behavior -- catalog_lookup()'s no-plane path
 * (below) recomputes the same check fresh on every request, so a
 * collision introduced later by a per-plane catalog swap (§15 B.2, not
 * yet built) is still caught correctly even though this startup pass
 * only ever saw the catalogs as they were at process start.
 */
static void detect_cross_plane_collisions(void)
{
    for (int i = 0; i < PLANE_COUNT; i++) {
        if (!g_planes[i].catalog) continue;
        cJSON *tools_i = cJSON_GetObjectItem(g_planes[i].catalog, "tools");
        if (!tools_i) continue;

        for (cJSON *entry = tools_i->child; entry; entry = entry->next) {
            const char *name = entry->string;
            if (!name) continue;

            for (int j = i + 1; j < PLANE_COUNT; j++) {
                if (!g_planes[j].catalog) continue;
                cJSON *tools_j = cJSON_GetObjectItem(g_planes[j].catalog, "tools");
                if (tools_j && cJSON_GetObjectItem(tools_j, name)) {
                    syslog(LOG_ERR,
                           "tool '%s' declared in both plane '%s' and plane '%s' -- "
                           "a request naming '%s' without an explicit 'plane' will be "
                           "rejected as ambiguous; a request naming a plane explicitly "
                           "still resolves normally",
                           name, g_planes[i].plane, g_planes[j].plane, name);
                }
            }
        }
    }
}

/*
 * catalog_lookup() — Changed 2026-08-15 (§15 B.5) to take an optional
 * `plane` argument, sourced from the request's own optional "plane"
 * field (decode_request_payload()).
 *
 *   plane != NULL: look up `tool` only in that one plane's catalog.
 *     Not found there means not found, full stop -- this never falls
 *     through and searches other planes. A caller that explicitly names
 *     the wrong plane should see a clear miss, not a silent cross-plane
 *     resolution that would mask the mistake. An unrecognized plane
 *     name (not one of g_planes[]'s four) is likewise just a miss.
 *
 *   plane == NULL: every existing/legacy caller, until updated to send
 *     it. Searches all loaded catalogs. If the name is found in exactly
 *     one, that's the (unambiguous) answer, same as before this change.
 *     If found in more than one, this is the cross-plane collision case
 *     detect_cross_plane_collisions() already warned about at startup --
 *     rejected here too (LOG_ERR, returns NULL) rather than guessing via
 *     a silent priority order.
 */
static cJSON *catalog_lookup(const char *tool, const char *plane)
{
    if (!tool) return NULL;

    if (plane) {
        for (int i = 0; i < PLANE_COUNT; i++) {
            if (strcmp(g_planes[i].plane, plane) != 0) continue;
            if (!g_planes[i].catalog) return NULL;
            cJSON *tools = cJSON_GetObjectItem(g_planes[i].catalog, "tools");
            return tools ? cJSON_GetObjectItem(tools, tool) : NULL;
        }
        return NULL; /* plane name not recognized */
    }

    cJSON *found = NULL;
    int    found_count = 0;
    for (int i = 0; i < PLANE_COUNT; i++) {
        if (!g_planes[i].catalog) continue;
        cJSON *tools = cJSON_GetObjectItem(g_planes[i].catalog, "tools");
        cJSON *entry = tools ? cJSON_GetObjectItem(tools, tool) : NULL;
        if (entry) {
            found_count++;
            if (!found) found = entry;
        }
    }
    if (found_count > 1) {
        syslog(LOG_ERR, "tool '%s' ambiguous across %d planes and no 'plane' supplied -- rejecting",
               tool, found_count);
        return NULL;
    }
    return found;
}

/* ── Safety gate ──────────────────────────────────────────────────────────── */

static int is_blocked(const char *cmd)
{
    if (!cmd || !*cmd) return 1;
    char first[64] = {0};
    const char *sp = strchr(cmd, ' ');
    size_t len = sp ? (size_t)(sp - cmd) : strlen(cmd);
    if (len >= sizeof(first)) len = sizeof(first) - 1;
    strncpy(first, cmd, len);

    /* Fixed 2026-08-16 (D.1 Finding 2, docs/24_diag_server_merge_plan.md
     * §12.2): also match against the basename -- the part after the
     * last '/', if any -- not just the raw first token. Before this, a
     * full path to a blocked binary (e.g. "/bin/rm -rf /") was never
     * byte-for-byte equal to the bare name BLOCKED_CMDS actually lists
     * ("rm"), so it slipped past both validate_static_commands()'s
     * startup pass and is_command_safe()'s override check entirely. A
     * bare name is unaffected (no '/' means basename == first, same
     * check as before this fix). */
    const char *base = strrchr(first, '/');
    base = base ? base + 1 : first;

    for (int i = 0; BLOCKED_CMDS[i]; i++)
        if (strcmp(first, BLOCKED_CMDS[i]) == 0 || strcmp(base, BLOCKED_CMDS[i]) == 0)
            return 1;
    return 0;
}

/* ── Argument-vector tokenizer ────────────────────────────────────────────── */

/*
 * tokenize_argv() — Added 2026-08-14 (NFR-007: "Target `fork`/`execve`-
 * style invocation with an argument vector, not a shell string" -- the
 * code's own README.md improvement note, now implemented). Splits `cmd`
 * into a NULL-terminated argv array on whitespace, honoring
 * double-quoted segments as a single token (no escape-sequence support
 * -- no current catalog entry needs one). This -- not shell parsing --
 * is what turns a catalog/request `command` string into what execvp()
 * actually receives; there is no shell in the execution path anymore
 * (see run_command()).
 *
 * Security note: because there is no shell, characters like `;`, `&&`,
 * backticks, `$()`, and `|` are no longer operators -- they're inert
 * literal bytes inside whatever argv token they land in, passed as
 * plain arguments to argv[0]. A compound string like
 * "cat /proc/uptime; rm -rf /tmp/x" tokenizes to five literal argv
 * entries for `cat`, which fails to open files with those names rather
 * than running `rm` as a second command. This closes the shell-
 * metacharacter half of the caller-command-override injection path
 * flagged in docs/24_diag_server_merge_plan.md §2/REQUIREMENTS.md Risk
 * item 5. It does not add an ACL check (still-open Risk item 6); see
 * is_command_safe() below for the program-pinning check added
 * 2026-08-14 that closes the other half of Risk item 5 (a caller
 * redirecting a tool to an entirely different program).
 *
 * Caller must free the result with free_argv(). Returns NULL on
 * allocation failure or if `cmd` tokenizes to zero arguments.
 */
static char **tokenize_argv(const char *cmd)
{
    size_t cap = 8, argc = 0;
    char **argv = malloc(cap * sizeof(char *));
    if (!argv) return NULL;

    const char *p = cmd;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *start;
        size_t len;
        if (*p == '"') {
            p++;
            start = p;
            while (*p && *p != '"') p++;
            len = (size_t)(p - start);
            if (*p == '"') p++;
        } else {
            start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            len = (size_t)(p - start);
        }

        char *tok = malloc(len + 1);
        if (!tok) {
            for (size_t i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return NULL;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';

        if (argc + 1 >= cap) {
            cap *= 2;
            char **grown = realloc(argv, cap * sizeof(char *));
            if (!grown) {
                free(tok);
                for (size_t i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return NULL;
            }
            argv = grown;
        }
        argv[argc++] = tok;
    }

    if (argc == 0) { free(argv); return NULL; }
    argv[argc] = NULL;
    return argv;
}

static void free_argv(char **argv)
{
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

/*
 * is_command_safe() — Added 2026-08-14 (docs/24_diag_server_merge_plan.md
 * §2/§6, REQUIREMENTS.md Risk item 5, §9 open question 3). This is the
 * single, common enforcement point for whether a resolved command may
 * execute for a given tool -- called for a caller-supplied override,
 * from the one call site in handle_request(). It replaces the previous
 * direct call to is_blocked() there with two checks:
 *
 *   1. Blocklist (unchanged from before): the resolved command's first
 *      argv token must not be one of BLOCKED_CMDS.
 *   2. Program pinning (new): if the catalog declares its own
 *      "command" for this tool, the resolved command's first argv
 *      token (the program that will actually run) must be identical
 *      to the catalog command's first argv token. A caller-supplied
 *      override can still adjust arguments -- e.g. a different target
 *      address for ping_google_ip -- but can no longer redirect a
 *      tool to run a *different program* than the catalog names for
 *      it. Before this, a request like
 *      `{"tool": "device_uptime", "command": "cat /etc/shadow"}` (same
 *      program, different, unintended argument) or
 *      `{"tool": "device_uptime", "command": "ls /root"}` (a wholly
 *      different, non-blocklisted program) would both have executed
 *      under the pre-2026-08-14 baseline as long as the first token
 *      wasn't blocklisted -- this closes that gap for the *program*
 *      half of it; it deliberately does not restrict arguments to a
 *      declared parameter set (a stricter, larger feature the docs
 *      floated as an alternative to this "restrict" approach, distinct
 *      from the "remove the override entirely" alternative -- neither
 *      is fully what this implements, which is program-level pinning).
 *
 * A tool with no cached program token has nothing to pin against, so
 * only the blocklist check applies to it -- the same behavior as
 * before this fix for that edge case.
 *
 * `dynamic_type` — Added 2026-08-14, same day as a follow-up. A catalog
 * entry may now declare `"type": "dynamic"` (see diag-triage-catalog.json's
 * `adhoc_diagnostic` entry) to mark itself as intentionally
 * caller-command-driven, as opposed to the default `"type": "static"`
 * (a fixed catalog command, optionally argument-overridden but always
 * program-pinned). When `dynamic_type` is true, the program-pinning
 * check above is skipped entirely for this tool -- there is no
 * catalog-declared program to pin against, by design, not by omission
 * -- and only the blocklist check applies, exactly as it does for
 * every other command that reaches this function. The blocklist is
 * never skipped, for either type; only the program-pin is type-gated.
 *
 * `catalog_program_token` — Signature changed 2026-08-14 (init-time
 * validation design, see validate_static_commands() below). This used
 * to be the raw catalog `command` string, tokenized fresh via
 * tokenize_argv() on every single call -- meaning every request against
 * a tool with a caller override re-parsed the catalog's own,
 * never-changing default command. It's now the tool's program token
 * (argv[0] only) precomputed once at process startup by
 * validate_static_commands() and cached on the catalog entry as
 * `_program_token`; the caller (handle_request()) passes that cached
 * string straight through. is_command_safe() itself now only tokenizes
 * `resolved_cmd` -- the one piece of data that's genuinely
 * request-time-variable (a caller override's actual content).
 *
 * `dynamic_type == 0` (static) call site retired 2026-08-16 (§9 open
 * question 3, static-override removal, docs/24_diag_server_merge_plan.md
 * §14 item 7): handle_request() no longer calls this function for
 * static tools at all -- it now discards a static tool's override
 * before reaching here (see handle_request()'s override branch) and
 * always runs the catalog's own command instead. The single remaining
 * call site in handle_request() is therefore only ever reached with
 * `dynamic_type == 1`. This function's `dynamic_type == 0` /
 * program-pinning branch (the block, above, that pins argv[0] against
 * `catalog_program_token`) is left fully intact rather than deleted:
 * it's dead code from this call site only by construction, not
 * broken, and deleting it would (a) discard working, previously
 * verified logic for no functional benefit, and (b) foreclose reusing
 * it if a future call site ever needs program-pinned validation again
 * (e.g. if a later phase reintroduces a scoped, narrower override
 * mechanism for static tools). Kept as the minimal-diff, lowest-risk
 * choice consistent with this project's established pattern.
 */
static int is_command_safe(const char *catalog_program_token, const char *resolved_cmd,
                            int dynamic_type)
{
    if (is_blocked(resolved_cmd)) return 0;

    if (!dynamic_type && catalog_program_token && *catalog_program_token) {
        char **req_argv = tokenize_argv(resolved_cmd);
        int program_matches = req_argv
                               && strcmp(catalog_program_token, req_argv[0]) == 0;
        free_argv(req_argv);
        if (!program_matches) return 0;
    }
    return 1;
}

/*
 * validate_static_commands() — Added 2026-08-14, per an explicit design
 * request: run the catalog-side half of command safety validation once,
 * at process initialization, instead of repeating it on every request.
 *
 * ── Problem this solves ──────────────────────────────────────────────
 * Before this: for a tool invoked with no caller override, `cmd` became
 * a fresh strdup() of the catalog's own `command` string, and (in an
 * earlier revision of this file, now superseded) that string still went
 * through is_blocked() again on every single request -- checking a
 * value that cannot change for the lifetime of the process, since
 * load_catalog() runs exactly once at startup and there is no catalog
 * hot-reload path anywhere in this code. Every request against a tool
 * with a caller override also re-tokenized the catalog's own command
 * string from scratch via tokenize_argv() inside is_command_safe(),
 * purely to compare its argv[0] against the override's argv[0] -- again,
 * re-deriving a value that's constant for the process's whole lifetime.
 *
 * ── What runs here, once, at startup ─────────────────────────────────
 * Called from main() immediately after load_catalog() returns
 * successfully, before either transport socket is created and before
 * any request can possibly be received. For every tool entry in the
 * catalog that declares a non-empty `command`:
 *   1. Tokenize it with tokenize_argv(). If that fails (NULL -- an
 *      empty or whitespace-only command string), mark the tool skipped
 *      with reason "unparseable command".
 *   2. Otherwise check the *full command string* with is_blocked() (the
 *      same blocklist check used everywhere else in this file). If
 *      blocked, mark the tool skipped with reason "blocklisted static
 *      command".
 *   3. Otherwise cache argv[0] (only the program name/path, not the
 *      full command) on the entry as `_program_token`, for
 *      is_command_safe() to use later at zero re-tokenization cost.
 * A tool with no `command` field at all (the normal shape for a
 * `"type": "dynamic"` tool -- see `adhoc_diagnostic`) has nothing to
 * validate here and is left alone: neither skipped nor given a
 * `_program_token`, exactly as before this change.
 *
 * ── What "skipped" means at runtime ──────────────────────────────────
 * A tool marked `_skipped` is treated as wholesale unusable for the
 * rest of the process's lifetime -- handle_request() rejects *any*
 * request naming it, including one that supplies its own override
 * command, without executing anything and without repeating the
 * blocklist/tokenize work that already found the problem. This is a
 * deliberately blanket exclusion, not a narrower "only the default-
 * command path is blocked" rule: a catalog entry whose own declared
 * command is dangerous or malformed indicates a misconfigured catalog,
 * and the safer default is to take the whole entry out of service
 * rather than leave a path open where a well-chosen override could
 * still reach it. (If a narrower rule -- skip only blocks the
 * no-override path, still allow a validated override through -- turns
 * out to be what's wanted instead, that's a one-line change at the
 * handle_request() call site, not a rearchitecture of this function.)
 *
 * Every skip is logged individually at LOG_WARNING with the tool name,
 * its offending command, and the reason, so a misconfigured catalog is
 * visible in syslog at service start -- not discovered later, only
 * when something happens to invoke the broken tool. A one-line summary
 * (`static command validation: N checked, M skipped`) is logged after
 * the pass for at-a-glance visibility.
 *
 * ── What still happens at request time, and why ──────────────────────
 * A caller-supplied override's own content is, by definition, not known
 * until the request arrives -- it cannot be precomputed here. So
 * is_command_safe() still runs live for the override path, doing
 * exactly two things per request: is_blocked() on the override string
 * (mandatory; the override's content is new every time), and a single
 * strcmp() against the cached `_program_token` (cheap; no allocation,
 * no re-tokenization of the catalog side). The no-override path (a
 * request that just names a tool and takes its catalog default) no
 * longer calls is_command_safe() at all -- see handle_request() -- since
 * a non-skipped tool's own command was already proven safe here, once.
 *
 * ── Changed 2026-08-15 (§15 B.5) ──────────────────────────────────────
 * Runs once per loaded plane catalog in g_planes[] instead of once
 * against a single global g_catalog -- each plane's tools are validated
 * independently, and the summary/per-tool log lines now carry the plane
 * name so a misconfigured tool is traceable to the catalog file it came
 * from.
 *
 * ── Changed 2026-08-15 (§15 B.2) ──────────────────────────────────────
 * The actual per-tool validation loop is factored out into
 * validate_catalog_tools() below, so B.2's push pipeline can run the
 * identical logic against a *candidate* clone before promoting it,
 * without duplicating this function's rules in two places.
 */
static void validate_catalog_tools(cJSON *catalog, const char *plane_name,
                                    int *checked_out, int *skipped_out)
{
    int checked = 0, skipped = 0;
    cJSON *tools = catalog ? cJSON_GetObjectItem(catalog, "tools") : NULL;
    if (!tools) { if (checked_out) *checked_out = 0; if (skipped_out) *skipped_out = 0; return; }

    for (cJSON *entry = tools->child; entry; entry = entry->next) {
        cJSON *cc = cJSON_GetObjectItem(entry, "command");
        const char *cmd = cc ? cJSON_GetStringValue(cc) : NULL;
        if (!cmd || !*cmd) continue; /* nothing to validate for this tool */

        checked++;
        const char *reason = NULL;
        char **argv = tokenize_argv(cmd);
        if (!argv) {
            reason = "unparseable command";
        } else if (is_blocked(cmd)) {
            reason = "blocklisted static command";
        } else {
            cJSON_AddStringToObject(entry, "_program_token", argv[0]);
        }
        free_argv(argv);

        if (reason) {
            skipped++;
            cJSON_AddBoolToObject(entry, "_skipped", 1);
            cJSON_AddStringToObject(entry, "_skip_reason", reason);
            syslog(LOG_WARNING,
                   "catalog validation: plane '%s' tool '%s' marked skipped at init "
                   "(%s): command='%s'",
                   plane_name, entry->string ? entry->string : "?", reason, cmd);
        }
    }
    if (checked_out) *checked_out = checked;
    if (skipped_out) *skipped_out = skipped;
}

static void validate_static_commands(void)
{
    int checked_total = 0, skipped_total = 0;

    for (int p = 0; p < PLANE_COUNT; p++) {
        if (!g_planes[p].catalog) continue;
        int checked = 0, skipped = 0;
        validate_catalog_tools(g_planes[p].catalog, g_planes[p].plane, &checked, &skipped);
        syslog(LOG_INFO, "static command validation: plane '%s' — %d checked, %d skipped",
               g_planes[p].plane, checked, skipped);
        checked_total += checked;
        skipped_total += skipped;
    }
    syslog(LOG_INFO, "static command validation: %d checked, %d skipped (all planes)",
           checked_total, skipped_total);

    /* Added 2026-08-15 (§15 B.5): cross-plane name collisions are only
     * meaningful once every plane's own entries have been validated. */
    detect_cross_plane_collisions();
}

/* ── Push / swap / persist pipeline (§15 B.2, B.2.5) ─────────────────────── */

typedef enum {
    PUSH_OK = 0,
    PUSH_ERR_UNKNOWN_PLANE,
    PUSH_ERR_VERSION_MISMATCH,
    PUSH_ERR_VALIDATION_FAILED,
    PUSH_ERR_PERSIST_FAILED,
    /* Added 2026-08-15 (§15 B.4): PUSH received via the public,
     * Parodus-facing socket rather than the new local-only endpoint.
     * See handle_push_request()'s transport-origin check. */
    PUSH_ERR_FORBIDDEN_TRANSPORT,
} push_status_t;

typedef struct {
    push_status_t status;
    long          new_version;
    char          reason[256];
} push_outcome_t;

/*
 * catalog_apply_push() — Added 2026-08-15 (§15 B.2, extended by §15
 * B.2.5 for disk persistence). Implements F3's clone / apply-diff /
 * validate / persist / promote pipeline for exactly one plane, given
 * an already-parsed diff (a cJSON object shaped
 * `{added:{...}, removed:[...], modified:{...}}`, per §11.1's PUSH
 * design). Deliberately takes plain, already-parsed data rather than a
 * wire message -- it's callable and testable standalone, per the
 * project's own stated intent for this piece ("built and tested
 * standalone before anything can trigger it"). §15 B.3's PUSH handler
 * (not yet built) will be a thin parser in front of this function, not
 * a reimplementation of it.
 *
 * Ownership: does NOT take ownership of `diff` -- the caller parses it,
 * passes it, and frees it after this returns. Every tool object taken
 * from `diff` is deep-duplicated via cJSON_Duplicate() into the
 * candidate, so nothing in the live catalog or a future request ever
 * aliases the caller's `diff` tree.
 *
 * Concurrency: see plane_catalog_t's `push_lock` (serializes concurrent
 * pushes to *this* plane, held for the whole call) and g_catalog_mutex
 * (protects the swap moment against concurrent readers in
 * handle_request(), held only for the swap itself -- see its own
 * comment for why that short a critical section is sufficient).
 *
 * Failure policy (§15 B.2.5 point 4): a persistence failure rejects the
 * whole push -- the in-memory catalog is never promoted if the disk
 * write didn't succeed, so memory and disk can never diverge.
 */
static push_outcome_t catalog_apply_push(const char *plane_name, long base_version,
                                          long target_version, cJSON *diff)
{
    push_outcome_t out;
    memset(&out, 0, sizeof(out));

    int idx = -1;
    for (int i = 0; i < PLANE_COUNT; i++) {
        if (strcmp(g_planes[i].plane, plane_name) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        out.status = PUSH_ERR_UNKNOWN_PLANE;
        snprintf(out.reason, sizeof(out.reason), "unrecognized plane '%s'", plane_name);
        return out;
    }

    plane_catalog_t *pc = &g_planes[idx];

    /* Held for the whole operation -- see the function comment on why
     * this makes the base_version check below race-free without extra
     * bookkeeping: no other push to this same plane can run
     * concurrently while we hold it. */
    pthread_mutex_lock(&pc->push_lock);

    if (pc->version != base_version) {
        out.status = PUSH_ERR_VERSION_MISMATCH;
        snprintf(out.reason, sizeof(out.reason),
                 "base_version %ld does not match live version %ld", base_version, pc->version);
        pthread_mutex_unlock(&pc->push_lock);
        return out;
    }

    /* Step 1: clone -- or start fresh if this plane has no catalog yet.
     * A push can bootstrap a not-yet-provisioned plane this way:
     * base_version 0 matches the "unloaded" default (plane_catalog_t's
     * initial state), and the diff's "added" set becomes the plane's
     * entire initial tool list. Not the primary goal of this change,
     * but falls out of the design for free. */
    cJSON *candidate;
    if (pc->catalog) {
        candidate = cJSON_Duplicate(pc->catalog, 1);
    } else {
        candidate = cJSON_CreateObject();
        cJSON_AddItemToObject(candidate, "tools", cJSON_CreateObject());
    }
    cJSON *tools = cJSON_GetObjectItem(candidate, "tools");
    if (!tools) {
        tools = cJSON_CreateObject();
        cJSON_AddItemToObject(candidate, "tools", tools);
    }

    /* Step 2: apply the diff to the clone only -- removed first, so a
     * name present in both "removed" and "added"/"modified" in the same
     * diff ends up present (upsert-after-remove), not silently dropped.
     * "added"/"modified" are both treated as upsert (ReplaceItemInObject
     * deletes-then-adds) rather than assuming "added" can't already
     * exist -- safe either way, matches "whole-tool replacement" from
     * §11.1's design. */
    cJSON *removed = cJSON_GetObjectItem(diff, "removed");
    if (removed && cJSON_IsArray(removed)) {
        for (cJSON *name_item = removed->child; name_item; name_item = name_item->next) {
            if (cJSON_IsString(name_item) && name_item->valuestring)
                cJSON_DeleteItemFromObject(tools, name_item->valuestring);
        }
    }
    cJSON *added = cJSON_GetObjectItem(diff, "added");
    if (added) {
        for (cJSON *tool_item = added->child; tool_item; tool_item = tool_item->next) {
            if (!tool_item->string) continue;
            cJSON_ReplaceItemInObject(tools, tool_item->string, cJSON_Duplicate(tool_item, 1));
        }
    }
    cJSON *modified = cJSON_GetObjectItem(diff, "modified");
    if (modified) {
        for (cJSON *tool_item = modified->child; tool_item; tool_item = tool_item->next) {
            if (!tool_item->string) continue;
            cJSON_ReplaceItemInObject(tools, tool_item->string, cJSON_Duplicate(tool_item, 1));
        }
    }

    /* Step 3: validate the candidate in isolation -- the same per-tool
     * logic validate_static_commands() runs at startup, against the
     * candidate, not the live catalog. */
    int checked = 0, skipped = 0;
    validate_catalog_tools(candidate, plane_name, &checked, &skipped);

    /* Step 4: decide pass/fail -- per §11.1/§14 item 1, only tools the
     * diff actually touched (added/modified) are checked; a
     * pre-existing skip on an untouched tool doesn't block this push. */
    int diff_touched_skipped = 0;
    const char *first_bad_tool = NULL;
    if (added) {
        for (cJSON *tool_item = added->child; tool_item && !diff_touched_skipped; tool_item = tool_item->next) {
            cJSON *live_entry = cJSON_GetObjectItem(tools, tool_item->string);
            cJSON *sk = live_entry ? cJSON_GetObjectItem(live_entry, "_skipped") : NULL;
            if (sk && cJSON_IsBool(sk) && cJSON_IsTrue(sk)) { diff_touched_skipped = 1; first_bad_tool = tool_item->string; }
        }
    }
    if (modified && !diff_touched_skipped) {
        for (cJSON *tool_item = modified->child; tool_item && !diff_touched_skipped; tool_item = tool_item->next) {
            cJSON *live_entry = cJSON_GetObjectItem(tools, tool_item->string);
            cJSON *sk = live_entry ? cJSON_GetObjectItem(live_entry, "_skipped") : NULL;
            if (sk && cJSON_IsBool(sk) && cJSON_IsTrue(sk)) { diff_touched_skipped = 1; first_bad_tool = tool_item->string; }
        }
    }

    if (diff_touched_skipped) {
        out.status = PUSH_ERR_VALIDATION_FAILED;
        snprintf(out.reason, sizeof(out.reason),
                 "pushed tool '%s' failed validation (blocklisted or unparseable command)",
                 first_bad_tool ? first_bad_tool : "?");
        cJSON_Delete(candidate);
        pthread_mutex_unlock(&pc->push_lock);
        return out;
    }

    /* Candidate passed. Stamp its version to what's about to be
     * persisted/promoted before writing it out. */
    cJSON_DeleteItemFromObject(candidate, "_catalog_version");
    cJSON_AddNumberToObject(candidate, "_catalog_version", (double)target_version);

    /* §15 B.2.5: persist to disk BEFORE the in-memory promote below --
     * reject the whole push, not just skip persistence, on any failure
     * here, so memory and disk can never diverge (see the function
     * comment's "Failure policy"). */
    char *serialized = cJSON_PrintUnformatted(candidate);
    if (!serialized) {
        out.status = PUSH_ERR_PERSIST_FAILED;
        snprintf(out.reason, sizeof(out.reason), "failed to serialize candidate catalog");
        cJSON_Delete(candidate);
        pthread_mutex_unlock(&pc->push_lock);
        return out;
    }

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", pc->loaded_path);
    int persist_ok = 0;
    int persist_errno = 0;
    FILE *wf = fopen(tmp_path, "w");
    if (wf) {
        size_t wlen = strlen(serialized);
        size_t written = fwrite(serialized, 1, wlen, wf);
        int flush_rc = fflush(wf);
        int fsync_rc = fsync(fileno(wf));
        int close_rc = fclose(wf);
        if (written == wlen && flush_rc == 0 && fsync_rc == 0 && close_rc == 0) {
            if (rename(tmp_path, pc->loaded_path) == 0) {
                persist_ok = 1;
                /* Recommended per §15 B.2.5 point 3: best-effort fsync
                 * of the containing directory too, since some
                 * filesystems don't guarantee a rename survives a power
                 * loss without it -- relevant on a CPE device where an
                 * unclean reboot mid-push is a real scenario. Failure
                 * here doesn't undo persist_ok -- the rename itself
                 * already succeeded; this is defense in depth, not the
                 * primary durability guarantee. */
                char dir_path[600];
                snprintf(dir_path, sizeof(dir_path), "%s", pc->loaded_path);
                char *slash = strrchr(dir_path, '/');
                if (slash) {
                    *slash = '\0';
                    int dfd = open(dir_path, O_RDONLY);
                    if (dfd >= 0) { fsync(dfd); close(dfd); }
                }
            } else {
                persist_errno = errno;
            }
        } else {
            persist_errno = errno;
        }
    } else {
        persist_errno = errno;
    }
    free(serialized);

    if (!persist_ok) {
        out.status = PUSH_ERR_PERSIST_FAILED;
        snprintf(out.reason, sizeof(out.reason), "failed to persist catalog to disk: %s",
                 strerror(persist_errno));
        unlink(tmp_path);
        cJSON_Delete(candidate);
        pthread_mutex_unlock(&pc->push_lock);
        syslog(LOG_ERR, "plane '%s' push rejected: %s", plane_name, out.reason);
        return out;
    }

    /* Steps 5/6 (concurrency model, above validate_static_commands()):
     * promote under g_catalog_mutex -- held only for the pointer/version
     * swap itself -- then free the old object once released. Safe to
     * free immediately: no reader can be mid-extraction from the old
     * object once this thread has acquired and released g_catalog_mutex,
     * since every reader's own extraction happens entirely inside the
     * same mutex's critical section (see handle_request()). */
    cJSON *old = pc->catalog;
    pthread_mutex_lock(&g_catalog_mutex);
    pc->catalog = candidate;
    pc->version = target_version;
    pthread_mutex_unlock(&g_catalog_mutex);
    if (old) cJSON_Delete(old);

    syslog(LOG_INFO, "plane '%s' catalog pushed: version %ld -> %ld (%d checked, %d skipped)",
           plane_name, base_version, target_version, checked, skipped);

    out.status = PUSH_OK;
    out.new_version = target_version;
    pthread_mutex_unlock(&pc->push_lock);
    return out;
}

/*
 * apply_count_lines_matching() — Added 2026-08-14, alongside the
 * NFR-007 shell-removal fix. Replaces `buf` (length *total_io) with the
 * decimal count of newline-delimited lines containing `needle`,
 * formatted identically to `grep <needle> | wc -l`'s output (the count
 * followed by a newline). Exists to preserve diag-triage-catalog.json's
 * "active_connections" tool's exact existing output shape, whose
 * original command (`/bin/netstat -n 2>/dev/null | grep ESTABLISHED |
 * wc -l`) relied on a two-stage shell pipeline that no longer exists
 * once execution goes through execvp() with no shell -- see the
 * "count_lines_matching" catalog field in handle_request() and the
 * updated catalog entry.
 */
static void apply_count_lines_matching(char *buf, size_t *total_io, const char *needle)
{
    unsigned long count = 0;
    char *line = buf;
    char *nl;
    while (line && *line) {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (*line && strstr(line, needle)) count++;
        if (nl) { *nl = '\n'; line = nl + 1; } else { break; }
    }
    int n = snprintf(buf, MAX_OUTPUT_BYTES, "%lu\n", count);
    *total_io = (n > 0) ? (size_t)n : 0;
}

/* ── Command runner ───────────────────────────────────────────────────────── */

/*
 * run_command() — Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md
 * §2, FR-013 and NFR-007). Executes `cmd` via execvp() with a tokenized
 * argument vector -- no shell is invoked anywhere in this path. This
 * closes NFR-007 ("Move from `popen` to safer `fork/execve` command
 * execution model") in full: `fork()`+`pipe()` were already introduced
 * for FR-013's timeout enforcement (giving the child a real, killable
 * PID); this change removes the `/bin/sh -c` shell layer that FR-013's
 * fix still went through, replacing it with tokenize_argv()+execvp().
 * See tokenize_argv()'s comment for what this does and doesn't close
 * from a security standpoint.
 *
 * `suppress_stderr` and `count_lines_matching` replace the two
 * shell-only behaviors ("2>/dev/null" and "| grep X | wc -l") that
 * diag-triage-catalog.json's "active_connections" entry previously
 * expressed inline in its shell command string; see
 * apply_count_lines_matching() and the updated catalog entry.
 *
 * Enforces timeout_sec as a hard wall-clock ceiling: a command still
 * running once the deadline passes is killed with SIGKILL rather than
 * left to block its worker thread indefinitely.
 *
 * Also kills the child if MAX_OUTPUT_BYTES is reached, not just on
 * timeout, so a command that keeps writing past the cap into a full,
 * undrained pipe can never hang this function.
 */
static char *run_command(const char *cmd, int timeout_sec, int suppress_stderr,
                          const char *count_lines_matching, int *exit_code)
{
    char *buf = malloc(MAX_OUTPUT_BYTES + 1);
    if (!buf) { *exit_code = 1; return strdup("out of memory"); }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        *exit_code = 1;
        snprintf(buf, MAX_OUTPUT_BYTES, "pipe() failed: %s", strerror(errno));
        return buf;
    }

    pid_t pid = fork();
    if (pid < 0) {
        *exit_code = 1;
        snprintf(buf, MAX_OUTPUT_BYTES, "fork() failed: %s", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return buf;
    }

    if (pid == 0) {
        /* Child: stdout -> pipe write end. stderr is left inherited
         * (matching the pre-2026-08-14 popen(cmd, "r")/execl behavior,
         * which never captured stderr either) unless suppress_stderr
         * asks for it to be silenced -- the replacement for a catalog
         * command's former inline "2>/dev/null". */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (suppress_stderr) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        }
        char **argv = tokenize_argv(cmd);
        if (!argv) _exit(127);
        execvp(argv[0], argv);
        _exit(127); /* execvp only returns on failure */
    }

    /* Parent */
    close(pipefd[1]);

    struct timespec deadline, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    size_t total = 0;
    int timed_out = 0, cap_hit = 0;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain_ms = (long)(deadline.tv_sec - now.tv_sec) * 1000
                        + (deadline.tv_nsec / 1000000 - now.tv_nsec / 1000000);
        if (remain_ms <= 0) { timed_out = 1; break; }

        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int rc = poll(&pfd, 1, (int)remain_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break; /* treat poll() failure as end of stream */
        }
        if (rc == 0) continue; /* poll's own timeout -- loop re-checks deadline */

        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(pipefd[0], buf + total, MAX_OUTPUT_BYTES - total);
            if (n <= 0) break; /* EOF (child exited/closed stdout) or read error */
            total += (size_t)n;
            if (total >= MAX_OUTPUT_BYTES) { cap_hit = 1; break; }
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);

    if (timed_out || cap_hit) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0); /* SIGKILL can't be caught/blocked -- returns promptly */
        if (timed_out) {
            free(buf);
            char *msg = malloc(64);
            if (msg) snprintf(msg, 64, "command timed out after %ds", timeout_sec);
            *exit_code = 124; /* matches GNU coreutils' timeout(1) convention */
            return msg ? msg : strdup("command timed out");
        }
        /* Byte cap hit: keep whatever output was captured -- same
         * truncation behavior as before, just guaranteed non-hanging now. */
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
        return buf;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    /* Added 2026-08-14: replaces the former shell pipeline's final
     * "| grep X | wc -l" stage. Only applied on clean completion --
     * not on the timeout/cap-hit paths above, which already return
     * their own error text. Note this changes *exit_code's semantics
     * for a tool using this field: it now reflects the underlying
     * command's own exit status (e.g. netstat's), whereas the original
     * shell pipeline's exit code was always wc -l's (~always 0),
     * regardless of the underlying command's outcome. The response
     * *shape* -- {tool, exit_code, stdout} -- is unchanged; only this
     * one field's value semantics differ for tools using this filter. */
    if (count_lines_matching && *count_lines_matching)
        apply_count_lines_matching(buf, &total, count_lines_matching);

    return buf;
}

/* ── WRP msgpack helpers ──────────────────────────────────────────────────── */

/*
 * pack_str_kv() — pack a msgpack string key + string value pair.
 */
static void pack_str_kv(msgpack_packer *pk, const char *key, const char *val)
{
    size_t kl = strlen(key), vl = strlen(val);
    msgpack_pack_str(pk, kl);  msgpack_pack_str_body(pk, key, kl);
    msgpack_pack_str(pk, vl);  msgpack_pack_str_body(pk, val, vl);
}

/*
 * build_registration() — Build a WRP type-9 SVC_REGISTRATION message.
 *
 * The WRP msgpack map format (from wrp-c):
 *   { "msg_type": 9, "service_name": "diag-server", "url": CLIENT_URL }
 */
static void *build_registration(size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_map(&pk, 3);
    msgpack_pack_str(&pk, 8);  msgpack_pack_str_body(&pk, "msg_type", 8);
    msgpack_pack_int(&pk, WRP_MSG_TYPE_REG);
    pack_str_kv(&pk, "service_name", SERVICE_NAME);
    pack_str_kv(&pk, "url", CLIENT_URL);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/*
 * Decode a WRP type-3 request payload (inner msgpack map).
 * Extracts "tool", "command", and "plane" fields into caller-owned
 * strings. "command" and "plane" are both optional -- *cmd_out/*plane_out
 * are left NULL if the caller didn't send them, exactly as "command"
 * already worked before "plane" existed.
 *
 * Added 2026-08-15 (§5 amendment, §15 B.5): "plane" is a new, purely
 * additive optional key. This loop already only extracts keys it
 * recognizes by name and silently ignores anything else, so this
 * change is backward-compatible in both directions: a caller that never
 * sends "plane" is unaffected, and a not-yet-upgraded binary of this
 * function encountering a "plane" key from a newer caller would have
 * already ignored it before this change existed, same as it ignores
 * any other unrecognized key today.
 */
static void decode_request_payload(const uint8_t *payload, size_t len,
                                   char **tool_out, char **cmd_out,
                                   char **plane_out)
{
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    if (msgpack_unpack_next(&result, (const char *)payload, len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {

        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            const char *k  = kv->key.via.str.ptr;
            uint32_t    kl = kv->key.via.str.size;
            if (kl == 4 && memcmp(k, "tool", 4) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *tool_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            else if (kl == 7 && memcmp(k, "command", 7) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *cmd_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            else if (kl == 5 && memcmp(k, "plane", 5) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *plane_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
        }
    }
    msgpack_unpacked_destroy(&result);
}

/*
 * build_response_payload() — Build the inner msgpack response payload.
 *   { "tool": <str>, "exit_code": <int>, "stdout": <bin> }
 */
static void *build_response_payload(const char *tool, int exit_code,
                                    const char *output, size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    const char *tv  = tool   ? tool   : "unknown";
    const char *ov  = output ? output : "";
    size_t      tl  = strlen(tv);
    size_t      ol  = output ? strlen(output) : 0;

    msgpack_pack_map(&pk, 3);
    msgpack_pack_str(&pk, 4);  msgpack_pack_str_body(&pk, "tool", 4);
    msgpack_pack_str(&pk, tl); msgpack_pack_str_body(&pk, tv, tl);
    msgpack_pack_str(&pk, 9);  msgpack_pack_str_body(&pk, "exit_code", 9);
    msgpack_pack_int(&pk, exit_code);
    msgpack_pack_str(&pk, 6);  msgpack_pack_str_body(&pk, "stdout", 6);
    msgpack_pack_bin(&pk, ol);
    if (ol > 0) msgpack_pack_bin_body(&pk, ov, ol);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/*
 * build_wrp_response() — Build a full WRP type-3 response message.
 *
 * WRP map fields: msg_type, source, dest, transaction_uuid,
 *                 content_type, payload
 */
static void *build_wrp_response(const char *src, const char *dst,
                                 const char *uuid,
                                 const void *payload, size_t payload_len,
                                 size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    const char *ct = "application/msgpack";
    size_t sl = strlen(src), dl = strlen(dst), ul = strlen(uuid);
    size_t ctl = strlen(ct);

    msgpack_pack_map(&pk, 6);
    msgpack_pack_str(&pk, 8);  msgpack_pack_str_body(&pk, "msg_type", 8);
    msgpack_pack_int(&pk, WRP_MSG_TYPE_REQ);
    pack_str_kv(&pk, "source", src);   (void)sl;
    pack_str_kv(&pk, "dest",   dst);   (void)dl;
    pack_str_kv(&pk, "transaction_uuid", uuid); (void)ul;
    pack_str_kv(&pk, "content_type", ct); (void)ctl;
    msgpack_pack_str(&pk, 7);  msgpack_pack_str_body(&pk, "payload", 7);
    msgpack_pack_bin(&pk, payload_len);
    if (payload_len > 0) msgpack_pack_bin_body(&pk, payload, payload_len);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/*
 * build_wrp_json_notification() — Added 2026-08-15 (§13.4). A near-copy
 * of build_wrp_response() above, deliberately kept as a separate
 * function rather than adding a content_type parameter to the shared
 * one: this is the only caller in the file whose payload is JSON text,
 * not a msgpack-encoded map (capability-sync's outbound notification,
 * §13.4), and touching build_wrp_response()'s signature would ripple
 * into all five of its existing call sites for no benefit. Same WRP
 * envelope shape and msg_type (3, matching commands, per §13.4's own
 * text), only content_type differs.
 */
static void *build_wrp_json_notification(const char *src, const char *dst,
                                          const char *uuid,
                                          const void *json_payload, size_t json_len,
                                          size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    const char *ct = "application/json";

    msgpack_pack_map(&pk, 6);
    msgpack_pack_str(&pk, 8);  msgpack_pack_str_body(&pk, "msg_type", 8);
    msgpack_pack_int(&pk, WRP_MSG_TYPE_REQ);
    pack_str_kv(&pk, "source", src);
    pack_str_kv(&pk, "dest",   dst);
    pack_str_kv(&pk, "transaction_uuid", uuid);
    pack_str_kv(&pk, "content_type", ct);
    msgpack_pack_str(&pk, 7);  msgpack_pack_str_body(&pk, "payload", 7);
    msgpack_pack_bin(&pk, json_len);
    if (json_len > 0) msgpack_pack_bin_body(&pk, json_payload, json_len);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/* ── WRP request decoder ──────────────────────────────────────────────────── */

typedef struct {
    int    msg_type;
    char  *source;
    char  *dest;
    char  *transaction_uuid;
    void  *payload;
    size_t payload_len;
    /* Added 2026-08-15 (§15 B.4): which socket to reply on, and whether
     * this request arrived via the new local-only endpoint. Set by
     * service_one_message() after decode_wrp() succeeds, not by
     * decode_wrp() itself -- decode_wrp() stays transport-agnostic,
     * since these two fields are metadata about *where the bytes came
     * from*, not anything present in the WRP message itself. Consumed
     * by every response-send call site (in place of the old hardcoded
     * g_push_sock) and by handle_push_request()'s transport-origin
     * check. */
    int    reply_sock;
    int    from_local;
} wrp_req_t;

static void wrp_req_free(wrp_req_t *r)
{
    free(r->source); free(r->dest); free(r->transaction_uuid); free(r->payload);
}

static int decode_wrp(const void *buf, size_t len, wrp_req_t *out)
{
    memset(out, 0, sizeof(*out));
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    int ok = 0;
    if (msgpack_unpack_next(&result, buf, len, NULL) == MSGPACK_UNPACK_SUCCESS
            && result.data.type == MSGPACK_OBJECT_MAP) {
        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            const char *k  = kv->key.via.str.ptr;
            uint32_t    kl = kv->key.via.str.size;
            if (kl == 8 && memcmp(k, "msg_type", 8) == 0
                    && kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
                out->msg_type = (int)kv->val.via.u64;
            else if (kl == 6 && memcmp(k, "source", 6) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                out->source = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            else if (kl == 4 && memcmp(k, "dest", 4) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                out->dest = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            else if (kl == 16 && memcmp(k, "transaction_uuid", 16) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                out->transaction_uuid = strndup(kv->val.via.str.ptr,
                                                kv->val.via.str.size);
            else if (kl == 7 && memcmp(k, "payload", 7) == 0
                    && (kv->val.type == MSGPACK_OBJECT_BIN
                        || kv->val.type == MSGPACK_OBJECT_STR)) {
                size_t pl = (kv->val.type == MSGPACK_OBJECT_BIN)
                            ? kv->val.via.bin.size : kv->val.via.str.size;
                const char *pd = (kv->val.type == MSGPACK_OBJECT_BIN)
                                 ? kv->val.via.bin.ptr : kv->val.via.str.ptr;
                out->payload = malloc(pl);
                if (out->payload) { memcpy(out->payload, pd, pl); out->payload_len = pl; }
            }
        }
        ok = 1;
    }
    msgpack_unpacked_destroy(&result);
    return ok;
}

/* ── ACL gate (§13.4, added 2026-08-15, by direct instruction) ───────────────
 *
 * `diag_acl_check()` is a thin wrapper, not a new interface: it calls
 * the exact same `acl_policy_store_query()` every other toolset's ACL
 * check already goes through, declared identically to
 * `reference-impl/dispatcher_command_path.c` (line 45) -- `caller_identity_t`
 * and the extern declaration below are a byte-for-byte mirror of that
 * file's own definitions, not a new, parallel shape invented for
 * diag-server. There is no shared header connecting diag-server's build
 * to reference-impl/ today, so this is duplicated rather than #included
 * -- flagged here so a future shared header doesn't silently leave two
 * declarations to keep in sync by hand.
 *
 * `acl_policy_store_query()` itself has no implementation anywhere in
 * this codebase -- it's declared `extern` in dispatcher_command_path.c
 * with the ACL Policy Store's actual transport still unresolved (§14
 * item 4), and diag-server inherits that exact same open dependency
 * here rather than getting a second one to solve. This code will not
 * link into a runnable binary until that transport is chosen and a real
 * implementation exists somewhere in the build -- a pre-existing,
 * project-wide gap, not something new introduced by this call site.
 */
typedef struct {
    const char *identity;   /* e.g. "cloud:skillset-mapper" or a device-local client id */
    const char **groups;    /* ACL Policy Store group names granted to this identity */
    int group_count;
} caller_identity_t;

extern bool acl_policy_store_query(const caller_identity_t *caller,
                                    const char *toolset, const char *method);

static bool diag_acl_check(const caller_identity_t *caller, const char *tool)
{
    return acl_policy_store_query(caller, "diagnostics", tool);
}

/* ── Handle one incoming WRP REQ ─────────────────────────────────────────── */

static void handle_request(wrp_req_t *req)
{
    /* req_plane: the caller-supplied, optional "plane" field (§15 B.5),
     * distinct from plane_dup below (the catalog entry's own "plane"
     * metadata, used for logging only). */
    char *tool = NULL, *cmd = NULL, *req_plane = NULL;

    syslog(LOG_INFO, "recv uuid=%s src=%s bytes=%zu",
           req->transaction_uuid ? req->transaction_uuid : "?",
           req->source ? req->source : "?",
           req->payload_len);

    if (req->payload && req->payload_len > 0)
        decode_request_payload(req->payload, req->payload_len, &tool, &cmd, &req_plane);

    if (!tool) {
        g_missing_tool_reqs++;
        syslog(LOG_WARNING, "missing 'tool' field (count=%lu)", g_missing_tool_reqs);
        free(cmd);
        free(req_plane);
        wrp_req_free(req);
        free(req);
        return;
    }

    /* Added 2026-08-15 (§13.4): ACL gate, inserted here per direct
     * instruction -- immediately after decode_request_payload() (so a
     * real tool name exists to check) and before any catalog lookup or
     * execution, matching §13.4's own text exactly. This is a guard
     * clause added at the top of handle_request(), not a change to the
     * catalog-lookup/skip-check/execution logic itself (the "core
     * logic" §13 explicitly leaves untouched) -- same pattern already
     * used for handle_push_request()'s transport-origin check in §15
     * B.4. On deny, builds the standard access-denied response
     * directly and returns without ever reaching catalog_lookup() --
     * the "denied caller's request never reaches execution" property
     * from §10.1 holds here too, just enforced in-process.
     *
     * caller_identity is best-effort from what a WRP request actually
     * carries today: just req->source, no ACL groups. This is a known,
     * documented gap, not a considered design -- A1 (the real
     * caller-identity/SAT token format) is still open (§12 Phase E.3),
     * so there is no real group/permission data to populate here yet.
     * caller_identity_t's *shape* is final; what diag-server populates
     * it with waits on A1, same as every other caller of
     * acl_policy_store_query(). */
    caller_identity_t caller = {
        .identity = req->source ? req->source : "unknown",
        .groups = NULL,
        .group_count = 0,
    };
    if (!diag_acl_check(&caller, tool)) {
        syslog(LOG_WARNING, "ACL denied: tool='%s' src=%s", tool, req->source ? req->source : "?");
        size_t dpl = 0;
        void *dpayload = build_response_payload(tool, 126, "access denied", &dpl);
        const char *dresp_src = req->dest   ? req->dest   : "dns:" SERVICE_NAME;
        const char *dresp_dst = req->source ? req->source : "unknown";
        const char *duuid     = req->transaction_uuid ? req->transaction_uuid : "unknown";
        size_t dwrp_len = 0;
        void *dwrp = build_wrp_response(dresp_src, dresp_dst, duuid, dpayload, dpl, &dwrp_len);
        free(dpayload);
        if (dwrp && dwrp_len > 0) {
            int drc = nn_send(req->reply_sock, dwrp, dwrp_len, 0);
            if (drc < 0)
                syslog(LOG_ERR, "ACL-deny nn_send failed: %s (uuid=%s)", nn_strerror(nn_errno()), duuid);
        }
        free(dwrp);
        free(tool);
        free(cmd);
        free(req_plane);
        wrp_req_free(req);
        free(req);
        return;
    }

    /* Added 2026-08-15 (§15 B.2): held from catalog_lookup() through the
     * last field read off `entry` below -- never across run_command(),
     * which can block for up to the tool's timeout. This is the entire
     * "danger window" a concurrent promote (catalog_apply_push()) needs
     * to be excluded from; see g_catalog_mutex's own comment for why
     * this short a critical section is sufficient without per-entry
     * reference counting. */
    pthread_mutex_lock(&g_catalog_mutex);

    cJSON *entry = catalog_lookup(tool, req_plane);
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    int suppress_stderr = 0;
    char *count_lines_matching = NULL;
    int is_dynamic = 0;
    char *plane_dup = NULL;
    int is_skipped = 0;
    const char *skip_reason = NULL;

    /* Added 2026-08-14 (init-time static command validation design):
     * check the precomputed skip flag before doing anything else with
     * this entry -- validate_static_commands() already determined at
     * process startup that this tool's own catalog command is
     * dangerous or malformed, so there's nothing further to check or
     * execute here. This is a wholesale rejection of the tool, not
     * just of the "use the catalog default" path -- see
     * validate_static_commands()'s comment for why a caller-supplied
     * override doesn't get a separate chance for a skipped tool. */
    if (entry) {
        cJSON *sk = cJSON_GetObjectItem(entry, "_skipped");
        is_skipped = (sk && cJSON_IsBool(sk) && cJSON_IsTrue(sk));
        if (is_skipped) {
            cJSON *sr = cJSON_GetObjectItem(entry, "_skip_reason");
            skip_reason = sr ? cJSON_GetStringValue(sr) : "unknown";
        }
    }

    if (!entry) {
        syslog(LOG_WARNING, "tool '%s' not in catalog", tool);
        /* Fixed 2026-08-16 (D.1 Finding 1, docs/24 §12.2): a tool name
         * catalog_lookup() can't resolve -- whether it plainly doesn't
         * exist, or is ambiguous across planes (§15 B.5) -- previously
         * left `cmd` untouched here, so a caller-supplied override for
         * an unresolvable tool reached run_command() below completely
         * unchecked: is_command_safe()/is_blocked() only ever ran inside
         * the `else` branch, which requires a resolved entry. Clearing
         * `cmd` unconditionally here closes that path the same way the
         * `is_skipped` branch already clears it for a known-but-skipped
         * tool -- see that branch's comment for the identical rationale.
         * No behavior change for any tool that *does* resolve. */
        free(cmd); cmd = NULL;
    } else if (is_skipped) {
        syslog(LOG_WARNING, "tool '%s' skipped (marked at init: %s) -- rejecting request",
               tool, skip_reason);
        /* CRITICAL: a skipped tool must reject *any* request against it,
         * including one carrying its own caller-supplied override --
         * clear cmd unconditionally so the "if (cmd && *cmd)" execution
         * gate below cannot be satisfied by whatever the caller sent.
         * Skipping the safety check above (by construction, since we
         * never reach it for a skipped tool) must never mean skipping
         * the *rejection* too. */
        free(cmd); cmd = NULL;
    } else {
        cJSON *cc = cJSON_GetObjectItem(entry, "command");
        const char *catalog_cmd = cc ? cJSON_GetStringValue(cc) : NULL;
        cJSON *pt = cJSON_GetObjectItem(entry, "_program_token");
        const char *catalog_program_token = pt ? cJSON_GetStringValue(pt) : NULL;

        /* Added 2026-08-14, follow-up to is_command_safe(): a catalog
         * entry may declare "type": "dynamic" to mark itself as
         * intentionally caller-command-driven (see the "adhoc_diagnostic"
         * entry in diag-triage-catalog.json), as opposed to the default
         * "type": "static". Unrecognized/absent "type" defaults to
         * static -- the same behavior every existing tool had before
         * this field existed. */
        cJSON *ty = cJSON_GetObjectItem(entry, "type");
        const char *type_s = ty ? cJSON_GetStringValue(ty) : NULL;
        is_dynamic = (type_s && strcmp(type_s, "dynamic") == 0);

        /* Added 2026-08-14, alongside "type": pure categorization
         * metadata for the future toolset-manifest conversion
         * (docs/24_diag_server_merge_plan.md §8 step 2) -- one of this
         * project's four planes (config-apply/management/control/
         * triage). diag-server does not branch on this; it's logged
         * for observability only. */
        cJSON *pl = cJSON_GetObjectItem(entry, "plane");
        const char *plane_s = pl ? cJSON_GetStringValue(pl) : NULL;
        if (plane_s) plane_dup = strdup(plane_s);

        /* Added 2026-08-14 (init-time static command validation design):
         * capture whether the caller supplied their own command BEFORE
         * substituting the catalog default -- this is what lets the
         * two paths below diverge. */
        int is_override = (cmd && *cmd);

        /* Corrected 2026-08-16 (§9 open question 3, design resolved
         * 2026-08-15 in docs/24_diag_server_merge_plan.md §14 item 7):
         * static-type tools now drop a caller-supplied override
         * entirely, unconditionally -- the catalog's own command
         * always runs for a static tool, whether or not the caller
         * sent one. This closes the residual "same program, different
         * arguments" gap that program-pinning alone left open:
         * is_command_safe()'s pin check only ever compared argv[0], so
         * a caller could keep the pinned program name but swap in
         * different arguments/flags and still pass. Discarding the
         * override outright for static tools -- rather than
         * validating it -- removes that gap completely, at the cost of
         * a static tool never being able to honor a per-call override
         * (which was never the intent for "static" in the first
         * place; that's what "dynamic" is for). Dynamic tools are
         * completely unaffected: they still take the is_override /
         * is_command_safe() branch below unchanged, since caller-
         * supplied commands are their entire purpose. */
        if (!is_dynamic) {
            if (is_override) {
                syslog(LOG_INFO, "override ignored for static tool '%s': "
                       "static tools always run the catalog command", tool);
            }
            free(cmd); cmd = NULL;
            if (catalog_cmd) cmd = strdup(catalog_cmd);
            /* No further validation here, same reasoning as before this
             * change: validate_static_commands() already proved the
             * catalog's own command string safe once, at startup. */
        } else if (!is_override) {
            free(cmd); cmd = NULL;
            if (catalog_cmd) cmd = strdup(catalog_cmd);
            /* No further validation here: this tool wasn't skipped, so
             * validate_static_commands() already proved this exact
             * command string safe once, at startup. Re-running
             * is_blocked()/tokenize_argv() on it here on every request
             * would just re-derive a value that cannot have changed --
             * that redundant work is exactly what this design change
             * removes from the runtime path. */
        } else {
            /* Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md
             * §2/§6, REQUIREMENTS.md Risk item 5, §9 open question 3),
             * signature updated same day (init-time validation design):
             * is_command_safe() now only runs for the override path --
             * the one case whose content genuinely cannot be known
             * ahead of time. It still does a live is_blocked() check
             * (mandatory) plus a strcmp() against the cached
             * `_program_token` (cheap -- no re-tokenizing the catalog
             * side per request). Reached only when is_dynamic is true
             * (static overrides are discarded above before reaching
             * here), so the program-pin skip inside is_command_safe()
             * for dynamic tools is the only live path left -- the
             * static/program-pinned branch of is_command_safe() is now
             * unreachable from this call site but is left intact (see
             * is_command_safe()'s own comment header for why it's kept
             * rather than deleted). */
            if (!is_command_safe(catalog_program_token, cmd, is_dynamic)) {
                syslog(LOG_WARNING, "unsafe command rejected for tool '%s' (type=%s): '%s'",
                       tool, is_dynamic ? "dynamic" : "static", cmd);
                free(cmd); cmd = NULL;
            }
        }
        /* Corrected 2026-08-14 (docs/24_diag_server_merge_plan.md §2,
         * FR-013): resolve the effective timeout from the catalog entry's
         * own "timeout" field; fall back to DEFAULT_TIMEOUT_SEC (30s) if
         * the field is absent, non-numeric, or non-positive. */
        cJSON *tc = cJSON_GetObjectItem(entry, "timeout");
        if (tc && cJSON_IsNumber(tc) && tc->valueint > 0)
            timeout_sec = tc->valueint;

        /* Corrected 2026-08-14 (NFR-007): command execution no longer
         * goes through a shell (see run_command()/tokenize_argv()), so
         * the two shell-only behaviors a catalog command string used to
         * express inline -- "2>/dev/null" and "| grep X | wc -l" -- are
         * now explicit, optional catalog fields applied natively.
         * README.md §1.4 and docs/24_diag_server_merge_plan.md §2/§6
         * have the full writeup. */
        cJSON *ss = cJSON_GetObjectItem(entry, "suppress_stderr");
        suppress_stderr = (ss && cJSON_IsBool(ss) && cJSON_IsTrue(ss)) ? 1 : 0;
        cJSON *clm = cJSON_GetObjectItem(entry, "count_lines_matching");
        const char *clm_s = clm ? cJSON_GetStringValue(clm) : NULL;
        if (clm_s) count_lines_matching = strdup(clm_s);
    }

    /* Added 2026-08-15 (§15 B.2): end of the danger window -- every field
     * this function still needs (cmd, timeout_sec, suppress_stderr,
     * count_lines_matching, is_dynamic, plane_dup, is_skipped,
     * skip_reason) is now a caller-owned local, not a pointer into the
     * catalog tree. Safe to release before run_command()'s potentially
     * long-running, blocking work. */
    pthread_mutex_unlock(&g_catalog_mutex);

    int exit_code = 1;
    char *output = NULL;

    if (cmd && *cmd) {
        /* Added 2026-08-14: type/plane included purely for observability
         * (correlating a run with its catalog categorization in syslog);
         * neither changes execution behavior here. */
        syslog(LOG_INFO, "exec tool=%s type=%s plane=%s cmd='%s' timeout=%ds",
               tool, is_dynamic ? "dynamic" : "static",
               plane_dup ? plane_dup : "unset", cmd, timeout_sec);
        output = run_command(cmd, timeout_sec, suppress_stderr, count_lines_matching, &exit_code);
        syslog(LOG_INFO, "done tool=%s exit=%d", tool, exit_code);
    } else if (is_skipped) {
        char msg[128];
        snprintf(msg, sizeof(msg), "tool skipped at init: %s",
                 skip_reason ? skip_reason : "unknown");
        output = strdup(msg);
    } else {
        output = strdup(entry ? "command blocked or missing" : "tool not in catalog");
    }
    free(count_lines_matching);
    free(plane_dup);

    /* Build and send response */
    size_t pl = 0;
    void *payload = build_response_payload(tool, exit_code, output ? output : "", &pl);

    const char *resp_src = req->dest   ? req->dest   : "dns:" SERVICE_NAME;
    const char *resp_dst = req->source ? req->source : "unknown";
    const char *uuid     = req->transaction_uuid ? req->transaction_uuid : "unknown";

    size_t wrp_len = 0;
    void *wrp = build_wrp_response(resp_src, resp_dst, uuid, payload, pl, &wrp_len);
    free(payload);

    if (wrp && wrp_len > 0) {
        /* Changed 2026-08-15 (§15 B.4): reply on whichever socket this
         * request arrived on (req->reply_sock), not the hardcoded public
         * g_push_sock -- a request received via the new local endpoint
         * must be answered back over the local endpoint, not routed to
         * Parodus. */
        int rc = nn_send(req->reply_sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "nn_send failed: %s (uuid=%s)", nn_strerror(nn_errno()), uuid);
        else
            syslog(LOG_INFO, "sent response uuid=%s tool=%s exit=%d bytes=%zu",
                   uuid, tool ? tool : "?", exit_code, wrp_len);
    }
    free(wrp);
    free(output);
    free(tool);
    free(cmd);
    free(req_plane);
    wrp_req_free(req);
    free(req);
}

/* ── Local protocol: DESCRIBE / HEALTH / PUSH / CHANGED (§15 B.3) ───────────
 *
 * Added 2026-08-15. §11.1 specified four new message kinds riding the
 * same local, Dispatch-Core-only channel EXEC already uses (§10.3) --
 * this section is the concrete realization of that: every local-protocol
 * message is still a WRP type-3 payload (same outer envelope EXEC
 * already speaks), disambiguated by an optional "kind" string field in
 * the inner map. EXEC itself carries no "kind" at all -- point 1 below
 * and decode_request_payload()'s own comment both say EXEC is
 * "unchanged" by this addition, and that's true at the byte level: a
 * request with no "kind" key is indistinguishable from what diag-server
 * has always accepted. A request WITH "kind":"EXEC" is treated
 * identically to one with no "kind" key at all -- both routes reach
 * handle_request() unmodified.
 *
 * This lands entirely on the *existing* g_pull_sock/g_push_sock pair --
 * it does not require §15 B.4's new local-only bind to exist first.
 * B.4 changes *which address* diag-server is reachable at; it does not
 * change how a message already received gets dispatched. Building this
 * now, ahead of B.4, matches the same "build and test standalone before
 * the next thing needs it" approach B.2 used for the swap pipeline.
 */

/*
 * peek_message_kind() — Reads only the optional "kind" string field
 * from an inner payload map, ignoring every other key. Returns NULL
 * (caller treats this as EXEC) if absent, unparseable, or the map
 * itself is malformed -- mirrors decode_request_payload()'s existing
 * "ignore what you don't recognize" tolerance.
 */
static char *peek_message_kind(const uint8_t *payload, size_t len)
{
    char *kind = NULL;
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    if (msgpack_unpack_next(&result, (const char *)payload, len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {
        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            if (kv->key.via.str.size == 4 && memcmp(kv->key.via.str.ptr, "kind", 4) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR) {
                kind = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
                break;
            }
        }
    }
    msgpack_unpacked_destroy(&result);
    return kind;
}

/*
 * msgpack_obj_to_cjson() — Added 2026-08-15 for PUSH's "diff" field.
 * The wire is msgpack (matching everything else this file speaks); the
 * live catalog and catalog_apply_push()'s candidate manipulation are
 * cJSON (matching the catalog file format). This is the one place those
 * two representations actually need to cross -- a small, general,
 * recursive converter, not a diff-specific parser, so it doesn't need
 * to know anything about what "added"/"removed"/"modified" mean.
 */
static cJSON *msgpack_obj_to_cjson(const msgpack_object *obj)
{
    if (!obj) return cJSON_CreateNull();
    switch (obj->type) {
    case MSGPACK_OBJECT_NIL:
        return cJSON_CreateNull();
    case MSGPACK_OBJECT_BOOLEAN:
        return obj->via.boolean ? cJSON_CreateTrue() : cJSON_CreateFalse();
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        return cJSON_CreateNumber((double)obj->via.u64);
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        return cJSON_CreateNumber((double)obj->via.i64);
    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
        return cJSON_CreateNumber(obj->via.f64);
    case MSGPACK_OBJECT_STR: {
        char *s = strndup(obj->via.str.ptr, obj->via.str.size);
        cJSON *n = cJSON_CreateString(s ? s : "");
        free(s);
        return n;
    }
    case MSGPACK_OBJECT_BIN: {
        char *s = strndup(obj->via.bin.ptr, obj->via.bin.size);
        cJSON *n = cJSON_CreateString(s ? s : "");
        free(s);
        return n;
    }
    case MSGPACK_OBJECT_ARRAY: {
        cJSON *arr = cJSON_CreateArray();
        for (uint32_t i = 0; i < obj->via.array.size; i++)
            cJSON_AddItemToArray(arr, msgpack_obj_to_cjson(&obj->via.array.ptr[i]));
        return arr;
    }
    case MSGPACK_OBJECT_MAP: {
        cJSON *o = cJSON_CreateObject();
        for (uint32_t i = 0; i < obj->via.map.size; i++) {
            msgpack_object_kv *kv = &obj->via.map.ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            char *key = strndup(kv->key.via.str.ptr, kv->key.via.str.size);
            if (key) {
                cJSON_AddItemToObject(o, key, msgpack_obj_to_cjson(&kv->val));
                free(key);
            }
        }
        return o;
    }
    default:
        return cJSON_CreateNull();
    }
}

/* ── DESCRIBE ─────────────────────────────────────────────────────────────── */

static void decode_describe_request(const uint8_t *payload, size_t len, char **plane_out)
{
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    if (msgpack_unpack_next(&result, (const char *)payload, len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {
        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            if (kv->key.via.str.size == 5 && memcmp(kv->key.via.str.ptr, "plane", 5) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *plane_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
        }
    }
    msgpack_unpacked_destroy(&result);
}

/* Packs one plane's {plane, version, tools:[{name,type,plane,timeout}]}.
 * Caller must hold g_catalog_mutex -- reads g_planes[idx].catalog/version
 * directly, same discipline handle_request() uses for its own reads. */
static void pack_one_plane_describe(msgpack_packer *pk, int idx)
{
    cJSON *tools = g_planes[idx].catalog ? cJSON_GetObjectItem(g_planes[idx].catalog, "tools") : NULL;
    int count = 0;
    for (cJSON *e = tools ? tools->child : NULL; e; e = e->next) count++;

    msgpack_pack_map(pk, 3);
    pack_str_kv(pk, "plane", g_planes[idx].plane);
    msgpack_pack_str(pk, 7); msgpack_pack_str_body(pk, "version", 7);
    msgpack_pack_int(pk, (int)g_planes[idx].version);
    msgpack_pack_str(pk, 5); msgpack_pack_str_body(pk, "tools", 5);
    msgpack_pack_array(pk, count);
    for (cJSON *e = tools ? tools->child : NULL; e; e = e->next) {
        cJSON *ty = cJSON_GetObjectItem(e, "type");
        cJSON *pl = cJSON_GetObjectItem(e, "plane");
        cJSON *tc = cJSON_GetObjectItem(e, "timeout");
        const char *name    = e->string ? e->string : "";
        const char *type_s  = (ty && cJSON_GetStringValue(ty)) ? cJSON_GetStringValue(ty) : "static";
        const char *plane_s = (pl && cJSON_GetStringValue(pl)) ? cJSON_GetStringValue(pl) : "";
        int timeout_v = (tc && cJSON_IsNumber(tc)) ? tc->valueint : 0;

        msgpack_pack_map(pk, 4);
        pack_str_kv(pk, "name", name);
        pack_str_kv(pk, "type", type_s);
        pack_str_kv(pk, "plane", plane_s);
        msgpack_pack_str(pk, 7); msgpack_pack_str_body(pk, "timeout", 7);
        msgpack_pack_int(pk, timeout_v);
    }
}

/*
 * build_describe_response_payload() — with `plane` non-NULL, one plane's
 * object; with `plane` NULL, an array of every *loaded* plane's object
 * (an unloaded plane is simply omitted, matching B.5's "absent means
 * unserved, not an error" treatment throughout). Locks g_catalog_mutex
 * for the whole read -- DESCRIBE only ever answers from whichever
 * catalog is currently marked live, never a candidate mid-validation,
 * by construction (only catalog_apply_push()'s swap step, itself
 * mutex-guarded, ever changes what "live" points to).
 */
static void *build_describe_response_payload(const char *plane, size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    pthread_mutex_lock(&g_catalog_mutex);

    if (plane) {
        int idx = -1;
        for (int i = 0; i < PLANE_COUNT; i++)
            if (strcmp(g_planes[i].plane, plane) == 0) { idx = i; break; }
        if (idx >= 0 && g_planes[idx].catalog) {
            pack_one_plane_describe(&pk, idx);
        } else {
            /* Unknown or unloaded plane: an empty map, not an error --
             * matches DESCRIBE's read-only, side-effect-free nature. */
            msgpack_pack_map(&pk, 0);
        }
    } else {
        int loaded_count = 0;
        for (int i = 0; i < PLANE_COUNT; i++) if (g_planes[i].catalog) loaded_count++;
        msgpack_pack_array(&pk, loaded_count);
        for (int i = 0; i < PLANE_COUNT; i++)
            if (g_planes[i].catalog) pack_one_plane_describe(&pk, i);
    }

    pthread_mutex_unlock(&g_catalog_mutex);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

static void handle_describe_request(wrp_req_t *req)
{
    char *plane = NULL;
    if (req->payload && req->payload_len > 0)
        decode_describe_request(req->payload, req->payload_len, &plane);

    size_t pl = 0;
    void *payload = build_describe_response_payload(plane, &pl);

    const char *resp_src = req->dest   ? req->dest   : "dns:" SERVICE_NAME;
    const char *resp_dst = req->source ? req->source : "unknown";
    const char *uuid     = req->transaction_uuid ? req->transaction_uuid : "unknown";

    size_t wrp_len = 0;
    void *wrp = build_wrp_response(resp_src, resp_dst, uuid, payload, pl, &wrp_len);
    free(payload);
    if (wrp && wrp_len > 0) {
        /* Changed 2026-08-15 (§15 B.4): reply on req->reply_sock -- see
         * handle_request()'s identical change for the full rationale. */
        int rc = nn_send(req->reply_sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "DESCRIBE nn_send failed: %s (uuid=%s)", nn_strerror(nn_errno()), uuid);
        else
            syslog(LOG_INFO, "DESCRIBE served plane=%s uuid=%s bytes=%zu", plane ? plane : "(all)", uuid, wrp_len);
    }
    free(wrp);
    free(plane);
    wrp_req_free(req);
    free(req);
}

/* ── HEALTH ───────────────────────────────────────────────────────────────── */

/*
 * Deliberately does not touch g_catalog_mutex or any g_planes[] field --
 * per §15 B.3 point 3, HEALTH answers "is the process alive," not
 * anything about catalog state, so it stays cheap and side-effect-free
 * regardless of catalog size or how many planes are loaded.
 */
static void *build_health_response_payload(size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_map(&pk, 1);
    pack_str_kv(&pk, "status", "ok");

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

static void handle_health_request(wrp_req_t *req)
{
    size_t pl = 0;
    void *payload = build_health_response_payload(&pl);

    const char *resp_src = req->dest   ? req->dest   : "dns:" SERVICE_NAME;
    const char *resp_dst = req->source ? req->source : "unknown";
    const char *uuid     = req->transaction_uuid ? req->transaction_uuid : "unknown";

    size_t wrp_len = 0;
    void *wrp = build_wrp_response(resp_src, resp_dst, uuid, payload, pl, &wrp_len);
    free(payload);
    if (wrp && wrp_len > 0) {
        /* Changed 2026-08-15 (§15 B.4): reply on req->reply_sock -- see
         * handle_request()'s identical change for the full rationale. */
        int rc = nn_send(req->reply_sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "HEALTH nn_send failed: %s (uuid=%s)", nn_strerror(nn_errno()), uuid);
    }
    free(wrp);
    wrp_req_free(req);
    free(req);
}

/* ── PUSH + CHANGED ───────────────────────────────────────────────────────── */

/*
 * decode_push_request() — Reads {plane, base_version, target_version,
 * diff} from the inner payload. `diff` (a nested map) is converted to
 * cJSON via msgpack_obj_to_cjson() *inside* this function, while the
 * msgpack_unpacked zone that owns the underlying string/map memory is
 * still alive -- the converted cJSON tree owns independent copies of
 * everything it needs (every string is strndup()'d), so it remains
 * valid after msgpack_unpacked_destroy() below. Returns 1 only if at
 * least `plane` was present; base_version/target_version default to 0
 * and diff defaults to NULL if absent (catalog_apply_push() and its
 * caller handle those the same way an absent/empty diff naturally would
 * -- a no-op diff against whatever base_version was supplied).
 */
static int decode_push_request(const uint8_t *payload, size_t len,
                                char **plane_out, long *base_version_out,
                                long *target_version_out, cJSON **diff_out)
{
    int ok = 0;
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    if (msgpack_unpack_next(&result, (const char *)payload, len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {
        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            const char *k  = kv->key.via.str.ptr;
            uint32_t    kl = kv->key.via.str.size;

            if (kl == 5 && memcmp(k, "plane", 5) == 0 && kv->val.type == MSGPACK_OBJECT_STR) {
                *plane_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            } else if (kl == 12 && memcmp(k, "base_version", 12) == 0) {
                if (kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER) *base_version_out = (long)kv->val.via.u64;
                else if (kv->val.type == MSGPACK_OBJECT_NEGATIVE_INTEGER) *base_version_out = (long)kv->val.via.i64;
            } else if (kl == 14 && memcmp(k, "target_version", 14) == 0) {
                if (kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER) *target_version_out = (long)kv->val.via.u64;
                else if (kv->val.type == MSGPACK_OBJECT_NEGATIVE_INTEGER) *target_version_out = (long)kv->val.via.i64;
            } else if (kl == 4 && memcmp(k, "diff", 4) == 0 && kv->val.type == MSGPACK_OBJECT_MAP) {
                *diff_out = msgpack_obj_to_cjson(&kv->val);
            }
        }
        if (*plane_out) ok = 1;
    }
    msgpack_unpacked_destroy(&result);
    return ok;
}

static void *build_push_response_payload(const push_outcome_t *outcome, const char *plane, size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    if (outcome->status == PUSH_OK) {
        msgpack_pack_map(&pk, 3);
        pack_str_kv(&pk, "status", "loaded");
        pack_str_kv(&pk, "plane", plane ? plane : "");
        msgpack_pack_str(&pk, 7); msgpack_pack_str_body(&pk, "version", 7);
        msgpack_pack_int(&pk, (int)outcome->new_version);
    } else {
        msgpack_pack_map(&pk, 3);
        pack_str_kv(&pk, "status", "rejected");
        pack_str_kv(&pk, "plane", plane ? plane : "");
        pack_str_kv(&pk, "reason", outcome->reason);
    }

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/*
 * build_changed_notification_payload() — §15 B.3 point 5 / §11.1 F5.
 * Unsolicited, sent right after a successful PUSH-triggered promote, so
 * the receiver knows *which* plane changed without re-DESCRIBEing
 * everything. Rides the same WRP type-3 payload convention as every
 * other local-protocol message here, disambiguated by
 * "kind":"CHANGED" -- there is no dedicated WRP message type for this
 * (unlike registration's real WRP_MSG_TYPE_REG), so it travels as a
 * type-3 payload the same way DESCRIBE/HEALTH/PUSH's *requests* do,
 * just unsolicited rather than in response to one.
 */
static void *build_changed_notification_payload(const char *plane, long version, size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_map(&pk, 3);
    pack_str_kv(&pk, "kind", "CHANGED");
    pack_str_kv(&pk, "plane", plane ? plane : "");
    msgpack_pack_str(&pk, 7); msgpack_pack_str_body(&pk, "version", 7);
    msgpack_pack_int(&pk, (int)version);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/*
 * send_changed_notification() — Reuses the exact outbound path
 * build_registration() already sends its own unsolicited WRP type-9
 * message over (g_push_sock), per §11.1's own justification for CHANGED
 * ("diag-server's own build_registration() already sends an unsolicited
 * message... CHANGED is the same shape of thing, over the same kind of
 * channel"). No new channel or endpoint is required for this to be real,
 * working code today -- unlike DESCRIBE/HEALTH/PUSH (all responses to
 * an inbound request), CHANGED's only prerequisite is an outbound
 * connection, which g_push_sock already is.
 *
 * Changed 2026-08-15 (§15 B.4): takes the outbound socket to send on as
 * a parameter rather than hardcoding g_push_sock. In practice this is
 * always g_local_push_sock now that handle_push_request() rejects any
 * PUSH not received via the local endpoint (a successful promote can
 * only originate there), but the function itself stays agnostic about
 * which socket that is -- it just sends on whatever it's given, same as
 * every other response builder here after B.4.
 */
static void send_changed_notification(int sock, const char *plane, long version)
{
    size_t pl = 0;
    void *payload = build_changed_notification_payload(plane, version, &pl);
    /* Unsolicited: source is diag-server's own address; dest/uuid are
     * empty since this isn't a response to any specific transaction. */
    size_t wrp_len = 0;
    void *wrp = build_wrp_response("dns:" SERVICE_NAME, "", "", payload, pl, &wrp_len);
    free(payload);
    if (wrp && wrp_len > 0) {
        int rc = nn_send(sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "CHANGED notification send failed for plane '%s': %s",
                   plane, nn_strerror(nn_errno()));
        else
            syslog(LOG_INFO, "CHANGED notification sent: plane='%s' version=%ld", plane, version);
    }
    free(wrp);
}

/*
 * diag_notify_capability_sync() — Added 2026-08-15 (§13.4), "outbound
 * capability-sync integration." Fired from the same point CHANGED
 * already fires from (right after a successful PUSH promote), but a
 * separate, additive function -- CHANGED is a local, Dispatch-Core-only
 * notification (sent on req->reply_sock, effectively the local endpoint
 * only, per §15 B.4's PUSH restriction); this is capability-sync's
 * durable, cloud-facing sibling, always sent over g_push_sock (the
 * public Parodus connection) regardless of which socket triggered the
 * promote, since its audience is the cloud, not Dispatch Core.
 *
 * A JSON-RPC 2.0 notification (no "id", no response expected), per the
 * design's own confirmed shape:
 *   { "jsonrpc": "2.0", "method": "capability_sync.updated",
 *     "params": { "toolset": "diagnostics", "version": "<str>",
 *                 "capabilities": [ ...same per-tool list DESCRIBE
 *                 already answers... ] } }
 * "version" is a JSON string (not a number), matching the design's own
 * literal example exactly. The capabilities array reuses the identical
 * field extraction pack_one_plane_describe() already does (name, type,
 * plane, timeout) -- built as cJSON here and printed to text, since
 * this payload is JSON text (content_type: application/json), not
 * msgpack, unlike everything else this file sends.
 *
 * Authentication is inherited for free: this rides diag-server's
 * already-authenticated outbound connection to Parodus (the same one
 * build_registration() uses), so "device identity, not session token"
 * holds with no new credential mechanism -- per the design's own
 * reasoning, this is a second instance of the exact "send it unprompted
 * over the connection I already have" pattern build_registration()'s
 * own WRP type-9 message already established.
 */
static void diag_notify_capability_sync(const char *plane_name, long version)
{
    int idx = -1;
    for (int i = 0; i < PLANE_COUNT; i++)
        if (strcmp(g_planes[i].plane, plane_name) == 0) { idx = i; break; }
    if (idx < 0) return; /* unknown plane -- nothing to report */

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "capability_sync.updated");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "params", params);
    cJSON_AddStringToObject(params, "toolset", "diagnostics");
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "%ld", version);
    cJSON_AddStringToObject(params, "version", version_str);

    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToObject(params, "capabilities", caps);

    /* Same lock discipline as build_describe_response_payload(): read
     * the live catalog under g_catalog_mutex. Safe to acquire here --
     * by the time this fires, catalog_apply_push()'s own promote step
     * has already released the mutex internally (§15 B.2's swap-then-
     * unlock ordering), so this is not a re-entrant lock attempt. */
    pthread_mutex_lock(&g_catalog_mutex);
    cJSON *tools = g_planes[idx].catalog ? cJSON_GetObjectItem(g_planes[idx].catalog, "tools") : NULL;
    for (cJSON *e = tools ? tools->child : NULL; e; e = e->next) {
        cJSON *ty = cJSON_GetObjectItem(e, "type");
        cJSON *pl = cJSON_GetObjectItem(e, "plane");
        cJSON *tc = cJSON_GetObjectItem(e, "timeout");
        const char *name    = e->string ? e->string : "";
        const char *type_s  = (ty && cJSON_GetStringValue(ty)) ? cJSON_GetStringValue(ty) : "static";
        const char *plane_s = (pl && cJSON_GetStringValue(pl)) ? cJSON_GetStringValue(pl) : "";
        int timeout_v = (tc && cJSON_IsNumber(tc)) ? tc->valueint : 0;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name);
        cJSON_AddStringToObject(entry, "type", type_s);
        cJSON_AddStringToObject(entry, "plane", plane_s);
        cJSON_AddNumberToObject(entry, "timeout", timeout_v);
        cJSON_AddItemToArray(caps, entry);
    }
    pthread_mutex_unlock(&g_catalog_mutex);

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) return;

    size_t wrp_len = 0;
    void *wrp = build_wrp_json_notification("dns:" SERVICE_NAME, "", "",
                                             json_text, strlen(json_text), &wrp_len);
    free(json_text);
    if (wrp && wrp_len > 0) {
        int rc = nn_send(g_push_sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "capability_sync.updated send failed for plane '%s': %s",
                   plane_name, nn_strerror(nn_errno()));
        else
            syslog(LOG_INFO, "capability_sync.updated sent: plane='%s' version=%ld", plane_name, version);
    }
    free(wrp);
}

static void handle_push_request(wrp_req_t *req)
{
    char *plane = NULL;
    long base_version = 0, target_version = 0;
    cJSON *diff = NULL;

    push_outcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));

    /* Added 2026-08-15 (§15 B.4): PUSH is only accepted via the new
     * local-only endpoint. No ACL/authorization check (§10, §13.4)
     * exists yet on this path, and B.4 puts the public endpoint and the
     * local one live at the same time during the transition -- without
     * this check, any WRP-addressable external caller reaching the
     * public path could push a new catalog with no authorization check
     * at all. Rejected here, before the request is even decoded, rather
     * than inside decode_push_request() or catalog_apply_push(): this is
     * a transport-origin check, not a content check, and belongs ahead
     * of both. DESCRIBE and HEALTH carry no such restriction -- they're
     * read-only/side-effect-free, so leaving them reachable on both
     * sockets during the transition doesn't reopen this gap. */
    if (!req->from_local) {
        outcome.status = PUSH_ERR_FORBIDDEN_TRANSPORT;
        snprintf(outcome.reason, sizeof(outcome.reason),
                 "PUSH is only accepted on the local endpoint");
        syslog(LOG_WARNING, "PUSH rejected: received via public endpoint");
    } else {
        int decoded = 0;
        if (req->payload && req->payload_len > 0)
            decoded = decode_push_request(req->payload, req->payload_len,
                                           &plane, &base_version, &target_version, &diff);
        if (!decoded || !plane) {
            outcome.status = PUSH_ERR_UNKNOWN_PLANE;
            snprintf(outcome.reason, sizeof(outcome.reason), "malformed PUSH request: no 'plane' field");
        } else {
            cJSON *empty_diff = NULL;
            if (!diff) { empty_diff = cJSON_CreateObject(); }
            outcome = catalog_apply_push(plane, base_version, target_version, diff ? diff : empty_diff);
            if (empty_diff) cJSON_Delete(empty_diff);
        }
    }

    size_t pl = 0;
    void *payload = build_push_response_payload(&outcome, plane, &pl);

    const char *resp_src = req->dest   ? req->dest   : "dns:" SERVICE_NAME;
    const char *resp_dst = req->source ? req->source : "unknown";
    const char *uuid     = req->transaction_uuid ? req->transaction_uuid : "unknown";

    size_t wrp_len = 0;
    void *wrp = build_wrp_response(resp_src, resp_dst, uuid, payload, pl, &wrp_len);
    free(payload);
    if (wrp && wrp_len > 0) {
        /* Changed 2026-08-15 (§15 B.4): reply on req->reply_sock -- see
         * handle_request()'s identical change for the full rationale. */
        int rc = nn_send(req->reply_sock, wrp, wrp_len, 0);
        if (rc < 0)
            syslog(LOG_ERR, "PUSH response nn_send failed: %s (uuid=%s)", nn_strerror(nn_errno()), uuid);
    }
    free(wrp);

    /* §15 B.3 point 5: fires only on a successful promote, immediately
     * after the response above -- not before, so the requester's own
     * synchronous accept/reject always arrives first. Sent on
     * req->reply_sock, same as the response just above: PUSH_OK is now
     * only reachable via the local endpoint (the forbidden-transport
     * check above), so this is always g_local_push_sock in practice. */
    if (outcome.status == PUSH_OK && plane) {
        send_changed_notification(req->reply_sock, plane, outcome.new_version);
        /* Added 2026-08-15 (§13.4): capability-sync's outbound
         * notification, fired from this exact point per its own design
         * text -- always over g_push_sock (Parodus), not req->reply_sock,
         * since its audience is the cloud, not whichever local caller
         * happened to send this PUSH. */
        diag_notify_capability_sync(plane, outcome.new_version);
    }

    if (diff) cJSON_Delete(diff);
    free(plane);
    wrp_req_free(req);
    free(req);
}

/* ── Dispatch by "kind" (§15 B.3) ─────────────────────────────────────────── */

/*
 * handle_local_request() — The new entry point main()'s message loop
 * calls instead of handle_request() directly. Peeks "kind" first; if
 * absent or "EXEC", hands off to handle_request() completely unmodified
 * (point 1's "already exists, unchanged" -- this wrapper is the only
 * new thing in EXEC's path, and it adds one cheap map-walk, not a
 * behavior change). Every other recognized kind routes to its own
 * handler; an unrecognized "kind" is dropped with a warning rather than
 * either crashing or silently falling through to EXEC's handling of a
 * payload it was never meant to parse.
 */
static void handle_local_request(wrp_req_t *req)
{
    char *kind = NULL;
    if (req->payload && req->payload_len > 0)
        kind = peek_message_kind(req->payload, req->payload_len);

    if (!kind || strcmp(kind, "EXEC") == 0) {
        free(kind);
        handle_request(req); /* unchanged; takes ownership of req */
        return;
    }
    if (strcmp(kind, "DESCRIBE") == 0) { free(kind); handle_describe_request(req); return; }
    if (strcmp(kind, "HEALTH") == 0)   { free(kind); handle_health_request(req);   return; }
    if (strcmp(kind, "PUSH") == 0)     { free(kind); handle_push_request(req);     return; }

    syslog(LOG_WARNING, "local request with unrecognized kind '%s' -- dropping", kind);
    free(kind);
    wrp_req_free(req);
    free(req);
}

/* ── Per-socket receive/dispatch (§15 B.4) ───────────────────────────────────
 *
 * Added 2026-08-15. Factored out of main()'s old single-socket message
 * loop so the exact same receive/decode/dispatch logic runs for both the
 * public g_pull_sock and the new local g_local_pull_sock, rather than
 * two hand-maintained copies that could quietly drift apart. Stamps
 * reply_sock/from_local on the resulting wrp_req_t before handing off --
 * this is the one and only place those two fields are set (decode_wrp()
 * itself stays transport-agnostic; see wrp_req_t's own comment).
 */
static void service_one_message(int recv_sock, int reply_sock, int from_local)
{
    void *buf = NULL;
    int   rc  = nn_recv(recv_sock, &buf, NN_MSG, 0);
    if (rc < 0) {
        if (nn_errno() == EAGAIN || nn_errno() == ETIMEDOUT) return;
        if (g_running)
            syslog(LOG_ERR, "nn_recv failed: %s", nn_strerror(nn_errno()));
        return;
    }

    wrp_req_t *req = calloc(1, sizeof(wrp_req_t));
    if (req && decode_wrp(buf, (size_t)rc, req)) {
        req->reply_sock = reply_sock;
        req->from_local  = from_local;
        if (req->msg_type == WRP_MSG_TYPE_REQ) {
            /* Dispatch to a detached thread so the poll loop stays
             * responsive to the *other* socket too. */
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr,
                    (void *(*)(void *))handle_local_request, req) == 0) {
                req = NULL; /* thread owns it now */
            } else {
                handle_local_request(req); /* fallback: inline */
                req = NULL;
            }
            pthread_attr_destroy(&attr);
        } else if (req->msg_type == WRP_MSG_TYPE_ALIVE) {
            msgpack_sbuffer sbuf2;
            msgpack_packer  pk2;
            msgpack_sbuffer_init(&sbuf2);
            msgpack_packer_init(&pk2, &sbuf2, msgpack_sbuffer_write);
            msgpack_pack_map(&pk2, 1);
            msgpack_pack_str(&pk2, 8); msgpack_pack_str_body(&pk2, "msg_type", 8);
            msgpack_pack_int(&pk2, WRP_MSG_TYPE_ALIVE);
            nn_send(reply_sock, sbuf2.data, sbuf2.size, 0);
            msgpack_sbuffer_destroy(&sbuf2);
            syslog(LOG_DEBUG, "keepalive ack sent (%s)", from_local ? "local" : "public");
        }
    }
    if (req) { wrp_req_free(req); free(req); }
    nn_freemsg(buf);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* Changed 2026-08-15 (§15 B.5): argv[1], if given, now overrides the
     * catalog *directory* (each plane's file is loaded from inside it),
     * not a single catalog file path -- a deploy-facing CLI change from
     * the prior single-catalog model. */
    if (argc > 1) catalog_dir_override = argv[1];
    openlog(LOG_IDENT, LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "starting (nanomsg-direct) parodus=%s client=%s",
           PARODUS_URL, CLIENT_URL);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int planes_loaded = load_catalogs();
    syslog(LOG_INFO, "catalog load complete: %d/%d plane(s) served", planes_loaded, PLANE_COUNT);
    /* Added 2026-08-14: validate every tool's static catalog command
     * once, here, before any request can be received -- see
     * validate_static_commands()'s design comment for the full
     * rationale. Deliberately called even if load_catalogs() found no
     * planes at all (every g_planes[i].catalog stays NULL and the
     * function is then a no-op) -- this must run exactly once,
     * unconditionally, at this exact point in startup, not be
     * re-triggered anywhere else. Changed 2026-08-15 (§15 B.5): now
     * iterates every loaded plane, not one global catalog -- see its
     * own updated comment. */
    validate_static_commands();

    /* ── Bind PULL socket (receive from parodus) ── */
    g_pull_sock = nn_socket(AF_SP, NN_PULL);
    if (g_pull_sock < 0) {
        syslog(LOG_ERR, "nn_socket(PULL) failed: %s", nn_strerror(nn_errno()));
        return 1;
    }
    int timeout = RECV_TIMEOUT_MS;
    nn_setsockopt(g_pull_sock, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout, sizeof(timeout));
    /* Large receive buffer so keepalives are not dropped while processing a request */
    int rcvbuf = 1024 * 1024;
    nn_setsockopt(g_pull_sock, NN_SOL_SOCKET, NN_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (nn_bind(g_pull_sock, CLIENT_URL) < 0) {
        syslog(LOG_ERR, "nn_bind(%s) failed: %s", CLIENT_URL, nn_strerror(nn_errno()));
        nn_close(g_pull_sock);
        return 1;
    }
    syslog(LOG_INFO, "listening on %s", CLIENT_URL);

    /* ── Connect PUSH socket (send to parodus) ── */
    g_push_sock = nn_socket(AF_SP, NN_PUSH);
    if (g_push_sock < 0) {
        syslog(LOG_ERR, "nn_socket(PUSH) failed: %s", nn_strerror(nn_errno()));
        nn_close(g_pull_sock);
        return 1;
    }
    /* Send timeout so nn_send never blocks indefinitely */
    int snd_timeout = 5000;
    nn_setsockopt(g_push_sock, NN_SOL_SOCKET, NN_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));

    /* Retry connection to parodus with backoff */
    int delay = 1;
    while (g_running) {
        if (nn_connect(g_push_sock, PARODUS_URL) >= 0) break;
        syslog(LOG_ERR, "nn_connect(%s) failed (%s), retrying in %ds",
               PARODUS_URL, nn_strerror(nn_errno()), delay);
        sleep(delay);
        if (delay < 60) delay = (delay * 2 > 60) ? 60 : delay * 2;
    }
    if (!g_running) goto shutdown;

    /* ── Send registration message ──
     * Changed 2026-08-16 (§15 B.4 part 2): gated behind
     * REGISTER_WITH_PARODUS (see its own comment at the top of the
     * file for the full rationale) -- now 0. build_registration() and
     * this send are left fully intact, just unreached, so reverting is
     * a single #define flip, not a rebuild of this logic. */
    if (REGISTER_WITH_PARODUS) {
        size_t reg_len = 0;
        void  *reg_msg = build_registration(&reg_len);
        if (reg_msg) {
            int rc = nn_send(g_push_sock, reg_msg, reg_len, 0);
            if (rc < 0)
                syslog(LOG_ERR, "registration send failed: %s", nn_strerror(nn_errno()));
            else
                syslog(LOG_INFO, "registered with parodus as '%s'", SERVICE_NAME);
            free(reg_msg);
        }
    } else {
        syslog(LOG_INFO, "registration with parodus disabled (§15 B.4 part 2) -- "
               "reachable only via the local endpoint, if active");
    }

    /* ── B.4 part 1: bind the new local-only endpoint, alongside the
     * existing public pair -- additive, not gated. Best-effort: if the
     * local directory/socket path isn't provisioned yet (e.g. Phase C's
     * Dispatch Core side doesn't exist in this deployment), diag-server
     * logs a warning and falls back to serving only the public path,
     * exactly as it did before this section existed. Nothing below this
     * point is allowed to affect the public path's already-established
     * state -- the registration above has already succeeded.
     *
     * NOTE: this deliberately does NOT touch build_registration()'s
     * WRP type-9 registration above, or disable the public path in any
     * way. That is B.4 part 2 ("the actual point of no return"), and per
     * docs/24 §15 B.4 it stays held until Phase D's tests pass -- not
     * implemented here. */
    g_local_pull_sock = nn_socket(AF_SP, NN_PULL);
    if (g_local_pull_sock < 0) {
        syslog(LOG_WARNING, "local endpoint disabled: nn_socket(PULL) failed: %s",
               nn_strerror(nn_errno()));
    } else {
        nn_setsockopt(g_local_pull_sock, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout, sizeof(timeout));
        nn_setsockopt(g_local_pull_sock, NN_SOL_SOCKET, NN_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        if (nn_bind(g_local_pull_sock, DIAG_LOCAL_RECV_URL) < 0) {
            syslog(LOG_WARNING, "local endpoint disabled: nn_bind(%s) failed: %s",
                   DIAG_LOCAL_RECV_URL, nn_strerror(nn_errno()));
            nn_close(g_local_pull_sock);
            g_local_pull_sock = -1;
        }
    }
    if (g_local_pull_sock >= 0) {
        g_local_push_sock = nn_socket(AF_SP, NN_PUSH);
        if (g_local_push_sock < 0) {
            syslog(LOG_WARNING, "local endpoint disabled: nn_socket(PUSH) failed: %s",
                   nn_strerror(nn_errno()));
            nn_close(g_local_pull_sock);
            g_local_pull_sock = -1;
        } else {
            nn_setsockopt(g_local_push_sock, NN_SOL_SOCKET, NN_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
            /* Single attempt, not the retry-with-backoff loop used for
             * PARODUS_URL above: nn_connect() on a PUSH socket queues and
             * reconnects in the background even if no peer is listening
             * yet (Phase C's Dispatch Core side may not exist yet in
             * this deployment) -- looping here would be redundant, and
             * *blocking* startup on it, the way the public path
             * deliberately does for parodus, would let an optional,
             * additive endpoint hold up production startup. That defeats
             * the entire "alongside, nothing removed" point of part 1. */
            if (nn_connect(g_local_push_sock, DIAG_LOCAL_SEND_URL) < 0) {
                syslog(LOG_WARNING, "local endpoint disabled: nn_connect(%s) failed: %s",
                       DIAG_LOCAL_SEND_URL, nn_strerror(nn_errno()));
                nn_close(g_local_push_sock); g_local_push_sock = -1;
                nn_close(g_local_pull_sock); g_local_pull_sock = -1;
            }
        }
    }
    g_local_enabled = (g_local_pull_sock >= 0 && g_local_push_sock >= 0);
    if (g_local_enabled)
        syslog(LOG_INFO, "local endpoint active: recv=%s send=%s",
               DIAG_LOCAL_RECV_URL, DIAG_LOCAL_SEND_URL);
    else
        syslog(LOG_INFO, "local endpoint not active -- public path unaffected");

    /* ── Message loop — poll both sockets, dispatch REQ in threads ──
     * Changed 2026-08-15 (§15 B.4): now services up to two sockets, the
     * existing public g_pull_sock and (only if g_local_enabled) the new
     * g_local_pull_sock, via nn_poll() rather than a single blocking
     * nn_recv(). When the local endpoint isn't active this degrades to
     * exactly the original single-socket behavior -- poll with one fd,
     * same RECV_TIMEOUT_MS, same "timeout -> recheck g_running" pattern
     * -- so this is not a behavior change for the common case where
     * B.4's local endpoint isn't provisioned. */
    struct nn_pollfd pfd[2];
    int pub_idx = 0, loc_idx = -1;
    pfd[pub_idx].fd = g_pull_sock;
    if (g_local_enabled) {
        loc_idx = 1;
        pfd[loc_idx].fd = g_local_pull_sock;
    }
    int nfds = g_local_enabled ? 2 : 1;

    while (g_running) {
        pfd[pub_idx].events = NN_POLLIN; pfd[pub_idx].revents = 0;
        if (loc_idx >= 0) { pfd[loc_idx].events = NN_POLLIN; pfd[loc_idx].revents = 0; }

        int pr = nn_poll(pfd, nfds, RECV_TIMEOUT_MS);
        if (pr < 0) {
            if (nn_errno() == EINTR) continue;
            if (g_running)
                syslog(LOG_ERR, "nn_poll failed: %s", nn_strerror(nn_errno()));
            break;
        }
        if (pr == 0) continue; /* timeout -- loop back and recheck g_running */

        if (pfd[pub_idx].revents & NN_POLLIN)
            service_one_message(g_pull_sock, g_push_sock, 0);
        if (loc_idx >= 0 && (pfd[loc_idx].revents & NN_POLLIN))
            service_one_message(g_local_pull_sock, g_local_push_sock, 1);
    }

shutdown:
    syslog(LOG_INFO, "shutting down");
    if (g_push_sock >= 0) nn_close(g_push_sock);
    if (g_pull_sock >= 0) nn_close(g_pull_sock);
    if (g_local_push_sock >= 0) nn_close(g_local_push_sock);
    if (g_local_pull_sock >= 0) nn_close(g_local_pull_sock);
    /* Changed 2026-08-15 (§15 B.5): free every loaded plane's catalog,
     * not a single global one. */
    for (int i = 0; i < PLANE_COUNT; i++) {
        if (g_planes[i].catalog) cJSON_Delete(g_planes[i].catalog);
    }
    closelog();
    return 0;
}
