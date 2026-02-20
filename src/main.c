// rmt - Remote Mount Tool
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include <stdlib.h>
#include <ctype.h>
#define MAX_MOUNTS   32
#define MAX_PATH_LEN 4096
#define VERSION      "1.1.0"
#define COMP_BIN     "/usr/local/bin/comp"
#define BASE_DIR_NAME ".rmt-base"

typedef struct {
    char local_path[MAX_PATH_LEN];
    char remote_spec[MAX_PATH_LEN];  // user@host:/path
    time_t mounted_at;
    time_t last_sync;
} Mount;

typedef struct {
    Mount mounts[MAX_MOUNTS];
    int count;
} MountRegistry;

// --- Forward declarations ---
static int cmd_mount(const char *remote, const char *local);
static int cmd_sync(const char *local, int dry_run, int pull_only, int push_only);
static int cmd_unmount(const char *local, int keep_local);
static int cmd_status(void);
static void usage(const char *prog);
static int smart_sync(const char *local_root, const char *remote_spec, int dry_run);

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------
static const char *find_rsync_path(void) {
    // Prefer PATH rsync; fallback to common locations.
    static char path[PATH_MAX];

    FILE *fp = popen("command -v rsync 2>/dev/null", "r");
    if (fp) {
        if (fgets(path, sizeof(path), fp)) {
            path[strcspn(path, "\n")] = '\0';
            pclose(fp);
            if (path[0]) return path;
        }
        pclose(fp);
    }

    // Fallbacks
    if (access("/opt/homebrew/bin/rsync", X_OK) == 0) return "/opt/homebrew/bin/rsync";
    if (access("/usr/local/bin/rsync", X_OK) == 0)    return "/usr/local/bin/rsync";
    if (access("/usr/bin/rsync", X_OK) == 0)          return "/usr/bin/rsync";
    return "rsync";
}

static int rsync_version_major(const char *rsync_path) {
    if (!rsync_path) rsync_path = "rsync";

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", rsync_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char line[256];
    int major = 0;

    if (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "version");
        if (p) {
            p += (int)strlen("version");
            while (*p && isspace((unsigned char)*p)) p++;
            major = atoi(p);
        }
    }
    pclose(fp);
    return major;
}

static char *shell_quote(const char *in) {
    size_t len = strlen(in);
    char *out = malloc(len * 5 + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\'') {
            const char *esc = "'\"'\"'";
            for (int k = 0; esc[k]; k++) out[j++] = esc[k];
        } else {
            out[j++] = in[i];
        }
    }
    out[j++] = '\'';
    out[j]   = '\0';
    return out;
}


// Escape only the remote PATH portion after ':' so old rsync + remote shell won�t split.
static char *rsync_escape_remote_spec_legacy(const char *remote_spec) {
    if (!remote_spec) return NULL;

    const char *colon = strchr(remote_spec, ':');
    if (!colon) return strdup(remote_spec);

    size_t host_len = (size_t)(colon - remote_spec);
    const char *rpath = colon + 1;
    size_t rlen = strlen(rpath);

    size_t cap = host_len + 1 + (rlen * 2) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    memcpy(out, remote_spec, host_len);
    out[host_len] = ':';

    size_t j = host_len + 1;
    for (size_t i = 0; i < rlen; i++) {
        unsigned char c = (unsigned char)rpath[i];
        switch (c) {
            case ' ': case '\t': case '\n':
            case '\\':
            case '\'': case '"':
            case '$': case '`': case '!':
            case '(': case ')':
            case '{': case '}':
            case '[': case ']':
            case '*': case '?':
            case '&': case ';':
            case '<': case '>':
            case '|':
                out[j++] = '\\';
                out[j++] = (char)c;
                break;
            default:
                out[j++] = (char)c;
                break;
        }
        if (j + 2 >= cap) break;
    }
    out[j] = '\0';
    return out;
}
static int mkdir_p(const char *path) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int validate_remote_spec(const char *spec) {
    if (!spec || !*spec) return 0;
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec) return 0;
    if (!*(colon + 1)) return 0;
    const char *at = strchr(spec, '@');
    if (at) {
        if (at > colon) return 0;
        if (at == spec) return 0;
    }
    return 1;
}

static void normalize_path(char *path) {
    if (!path) return;
    int len = strlen(path);
    while (len > 1 && path[len-1] == '/') path[--len] = '\0';
    char *src = path, *dst = path;
    while (*src) {
        if (src[0] == '.' && src[1] == '/') { src += 2; continue; }
        if (src[0] == '/' && src[1] == '.' && src[2] == '/') { src += 2; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
    src = path; dst = path;
    while (*src) {
        *dst++ = *src++;
        if (src[-1] == '/') while (*src == '/') src++;
    }
    *dst = '\0';
}

static const char *get_rmt_dir(void) {
    static char path[MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.rmt", home);
    return path;
}

static const char *get_registry_path(void) {
    static char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/registry", get_rmt_dir());
    return path;
}

// ---------------------------------------------------------------------------
// Base cache helpers
// ---------------------------------------------------------------------------

static void base_path_for(const char *local_root, const char *rel,
                           char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s/%s", local_root, BASE_DIR_NAME, rel);
}

static int base_update(const char *local_root, const char *rel,
                       const char *src_path)
{
    char base[MAX_PATH_LEN];
    base_path_for(local_root, rel, base, sizeof(base));

    char base_copy[MAX_PATH_LEN];
    strncpy(base_copy, base, sizeof(base_copy) - 1);
    if (mkdir_p(dirname(base_copy)) != 0) return -1;

    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp_XXXXXX", base);
    int fd = mkstemp(tmp);
    if (fd < 0) { perror("mkstemp"); return -1; }

    FILE *in  = fopen(src_path, "rb");
    FILE *out = fdopen(fd, "wb");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out); else close(fd);
        unlink(tmp);
        return -1;
    }

    char buf[8192];
    size_t nr;
    int rc = 0;
    while ((nr = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, nr, out) != nr) { rc = -1; break; }

    fclose(in);
    fclose(out);

    if (rc == 0 && rename(tmp, base) != 0) { perror("rename"); unlink(tmp); rc = -1; }
    else if (rc != 0) unlink(tmp);

    return rc;
}

static void base_delete(const char *local_root, const char *rel)
{
    char base[MAX_PATH_LEN];
    base_path_for(local_root, rel, base, sizeof(base));
    unlink(base);
}

static int base_init(const char *local_root)
{
    char base_dir[MAX_PATH_LEN];
    snprintf(base_dir, sizeof(base_dir), "%s/%s", local_root, BASE_DIR_NAME);
    if (mkdir_p(base_dir) != 0) return -1;

    char *qlocal = shell_quote(local_root);
    char *qbase  = shell_quote(base_dir);
    if (!qlocal || !qbase) { free(qlocal); free(qbase); return -1; }

    char cmd[MAX_PATH_LEN * 2 + 256];
    snprintf(cmd, sizeof(cmd),
             "rsync -a --exclude=%s/ %s/ %s/ 2>/dev/null",
             BASE_DIR_NAME, qlocal, qbase);
    int rc = system(cmd);
    free(qlocal);
    free(qbase);
    return rc == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Registry operations
// ---------------------------------------------------------------------------

static int load_registry(MountRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
    FILE *f = fopen(get_registry_path(), "r");
    if (!f) { if (errno == ENOENT) return 0; return -1; }
    int fd = fileno(f);
    if (flock(fd, LOCK_SH) != 0) { fclose(f); return -1; }

    char line[MAX_PATH_LEN * 2 + 64];
    while (fgets(line, sizeof(line), f) && reg->count < MAX_MOUNTS) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        Mount *m = &reg->mounts[reg->count];
        char mounted_at_s[32], last_sync_s[32];

        char *p = line;
        char *tok;

        #define NEXT_FIELD(dst, dstsz) \
            tok = p; \
            p = strchr(p, '|'); \
            if (!p) goto bad_line; \
            *p++ = '\0'; \
            strncpy(dst, tok, (dstsz) - 1); \
            dst[(dstsz) - 1] = '\0';

        NEXT_FIELD(m->local_path,  MAX_PATH_LEN)
        NEXT_FIELD(m->remote_spec, MAX_PATH_LEN)
        NEXT_FIELD(mounted_at_s,   sizeof(mounted_at_s))

        strncpy(last_sync_s, p, sizeof(last_sync_s) - 1);
        last_sync_s[sizeof(last_sync_s) - 1] = '\0';

        #undef NEXT_FIELD

        m->mounted_at = (time_t)strtoll(mounted_at_s, NULL, 10);
        m->last_sync  = (time_t)strtoll(last_sync_s,  NULL, 10);
        reg->count++;
        continue;

    bad_line:
        fprintf(stderr, "Warning: skipping malformed registry line\n");
    }

    flock(fd, LOCK_UN);
    fclose(f);
    return 0;
}

static int save_registry(const MountRegistry *reg) {
    if (mkdir_p(get_rmt_dir()) != 0) return -1;
    int fd = open(get_registry_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    FILE *f = fdopen(fd, "w");
    if (!f) { flock(fd, LOCK_UN); close(fd); return -1; }

    fprintf(f, "# rmt registry v2 do not edit manually\n");
    for (int i = 0; i < reg->count; i++) {
        const Mount *m = &reg->mounts[i];
        fprintf(f, "%s|%s|%lld|%lld\n",
                m->local_path,
                m->remote_spec,
                (long long)m->mounted_at,
                (long long)m->last_sync);
    }

    flock(fd, LOCK_UN);
    fclose(f);
    return 0;
}

static Mount *find_mount(MountRegistry *reg, const char *local) {
    char resolved[MAX_PATH_LEN];
    if (realpath(local, resolved)) {
        normalize_path(resolved);
        for (int i = 0; i < reg->count; i++)
            if (strcmp(reg->mounts[i].local_path, resolved) == 0) return &reg->mounts[i];
        return NULL;
    }
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(resolved, sizeof(resolved), "%s/%s", cwd, local);
            normalize_path(resolved);
            for (int i = 0; i < reg->count; i++)
                if (strcmp(reg->mounts[i].local_path, resolved) == 0) return &reg->mounts[i];
        }
    }
    return NULL;
}

// Takes an already-resolved absolute path — no second realpath call
static int remove_mount_by_resolved(MountRegistry *reg, const char *resolved) {
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
            for (int j = i; j < reg->count - 1; j++) reg->mounts[j] = reg->mounts[j+1];
            reg->count--;
            return 0;
        }
    }
    return -1;
}

static int cmd_unmount(const char *local, int keep_local) {
    MountRegistry reg = {0};
    if (load_registry(&reg) != 0) { fprintf(stderr, "Failed to load registry\n"); return 1; }

    Mount *m = find_mount(&reg, local);
    if (!m) { fprintf(stderr, "%s is not a mounted path\n", local); return 1; }

    // Capture the resolved path now, before anything touches the filesystem
    char resolved[MAX_PATH_LEN];
    strncpy(resolved, m->local_path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    if (!keep_local) {
        printf("Doing final sync before unmount...\n");
        int rc = smart_sync(m->local_path, m->remote_spec, 0);
        if (rc == 1) {
            fprintf(stderr, "\nCannot unmount: unresolved conflicts.\n");
            fprintf(stderr, "Resolve conflicts then run: rmt unmount %s\n", local);
            return 1;
        } else if (rc != 0) {
            fprintf(stderr, "\nWarning: final sync failed\n");
            fprintf(stderr, "Continue with unmount anyway? [y/N] ");
            char resp[16];
            if (!fgets(resp, sizeof(resp), stdin) || (resp[0] != 'y' && resp[0] != 'Y')) {
                printf("Unmount cancelled\n");
                return 1;
            }
        }
    }

    // Use pre-resolved path — no second realpath, no race
    if (remove_mount_by_resolved(&reg, resolved) != 0) {
        fprintf(stderr, "Failed to remove from registry\n");
        return 1;
    }
    if (save_registry(&reg) != 0) fprintf(stderr, "Warning: Failed to save registry\n");

    printf("✓ Unmounted %s\n", resolved);

    if (keep_local) {
        printf("  Local files kept at: %s\n", resolved);
        printf("  Note: .rmt-base cache kept alongside local files\n");
    } else {
        printf("  Deleting local copy...\n");
        char *quoted = shell_quote(resolved);
        if (!quoted) { fprintf(stderr, "  Warning: OOM quoting path\n"); return 1; }
        char cmd[MAX_PATH_LEN * 2 + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", quoted);
        free(quoted);
        if (system(cmd) == 0) printf("  ✓ Deleted %s\n", resolved);
        else fprintf(stderr, "  Warning: Failed to delete local files\n");
    }

    return 0;
}
static int rsync_pull(const char *remote, const char *local, int dry_run) {
    // Always use legacy-safe escaping for remote path portion
    char *remote_arg = rsync_escape_remote_spec_legacy(remote);
    if (!remote_arg) return -1;

    char *qremote = shell_quote(remote_arg);
    char *qlocal  = shell_quote(local);
    free(remote_arg);

    if (!qremote || !qlocal) { free(qremote); free(qlocal); return -1; }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "rsync -avz%s --exclude=%s/ %s/ %s/ 2>&1",
        dry_run ? "n" : "", BASE_DIR_NAME, qremote, qlocal);

    free(qremote);
    free(qlocal);

    if (dry_run) printf("Dry run (pull): %s -> %s\n", remote, local);

    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
static int rsync_push(const char *local, const char *remote, int dry_run) {
    char *remote_arg = rsync_escape_remote_spec_legacy(remote);
    if (!remote_arg) return -1;

    char *qlocal  = shell_quote(local);
    char *qremote = shell_quote(remote_arg);
    free(remote_arg);

    if (!qlocal || !qremote) { free(qlocal); free(qremote); return -1; }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "rsync -avz%s --exclude=%s/ %s/ %s/ 2>&1",
        dry_run ? "n" : "", BASE_DIR_NAME, qlocal, qremote);

    free(qlocal);
    free(qremote);

    if (dry_run) printf("Dry run (push): %s -> %s\n", local, remote);

    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
static int rsync_push_file(const char *src_path, const char *remote_spec,
                           const char *rel) {
    // Build remote destination "user@host:/base/rel"
    char remote_file[MAX_PATH_LEN * 2];
    snprintf(remote_file, sizeof(remote_file), "%s/%s", remote_spec, rel);

    // Always legacy-escape remote path portion after ':'
    char *remote_arg = rsync_escape_remote_spec_legacy(remote_file);
    if (!remote_arg) return -1;

    char *qsrc = shell_quote(src_path);
    char *qdst = shell_quote(remote_arg);
    free(remote_arg);

    if (!qsrc || !qdst) { free(qsrc); free(qdst); return -1; }

    /* Ensure remote directory exists via ssh mkdir -p */
    char rel_dir[MAX_PATH_LEN];
    strncpy(rel_dir, rel, sizeof(rel_dir) - 1);
    rel_dir[sizeof(rel_dir) - 1] = '\0';

    char *rd = dirname(rel_dir);
    if (strcmp(rd, ".") != 0) {
        char ssh_host[MAX_PATH_LEN], remote_path[MAX_PATH_LEN];
        const char *colon = strchr(remote_spec, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - remote_spec);
            if (hlen >= sizeof(ssh_host)) hlen = sizeof(ssh_host) - 1;
            strncpy(ssh_host, remote_spec, hlen);
            ssh_host[hlen] = '\0';

            snprintf(remote_path, sizeof(remote_path), "%s/%s", colon + 1, rd);

            char *qhost  = shell_quote(ssh_host);
            char *qrpath = shell_quote(remote_path);
            if (qhost && qrpath) {
                char mkdir_cmd[8192];
                snprintf(mkdir_cmd, sizeof(mkdir_cmd),
                         "ssh %s mkdir -p %s 2>/dev/null", qhost, qrpath);
                system(mkdir_cmd);
            }
            free(qhost);
            free(qrpath);
        }
    }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "rsync -az %s %s 2>&1", qsrc, qdst);

    free(qsrc);
    free(qdst);

    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
typedef struct { char **paths; int count; int cap; } PathList;

static void pl_push(PathList *pl, const char *rel) {
    if (pl->count == pl->cap) {
        pl->cap *= 2;
        pl->paths = realloc(pl->paths, pl->cap * sizeof(char *));
    }
    pl->paths[pl->count++] = strdup(rel);
}

static int pl_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void walk_dir(const char *root, const char *rel, PathList *pl) {
    char full[MAX_PATH_LEN];
    if (rel[0] == '\0') snprintf(full, sizeof(full), "%s", root);
    else                snprintf(full, sizeof(full), "%s/%s", root, rel);

    DIR *d = opendir(full);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, BASE_DIR_NAME) == 0) continue;

        char entry_rel[MAX_PATH_LEN];
        if (rel[0] == '\0') snprintf(entry_rel, sizeof(entry_rel), "%s", ent->d_name);
        else                snprintf(entry_rel, sizeof(entry_rel), "%s/%s", rel, ent->d_name);

        char entry_full[MAX_PATH_LEN];
        snprintf(entry_full, sizeof(entry_full), "%s/%s", root, entry_rel);

        struct stat st;
        if (lstat(entry_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) walk_dir(root, entry_rel, pl);
        else if (S_ISREG(st.st_mode)) pl_push(pl, entry_rel);
    }
    closedir(d);
}

static PathList *local_files(const char *local_root) {
    PathList *pl = malloc(sizeof(PathList));
    pl->cap   = 64;
    pl->count = 0;
    pl->paths = malloc(pl->cap * sizeof(char *));
    walk_dir(local_root, "", pl);
    qsort(pl->paths, pl->count, sizeof(char *), pl_cmp);
    return pl;
}

static void pl_free(PathList *pl) {
    if (!pl) return;
    for (int i = 0; i < pl->count; i++) free(pl->paths[i]);
    free(pl->paths);
    free(pl);
}

// ---------------------------------------------------------------------------
// comp-based smart sync
// ---------------------------------------------------------------------------

static int run_comp_diff(const char *a, const char *b) {
    char *qa = shell_quote(a);
    char *qb = shell_quote(b);
    if (!qa || !qb) { free(qa); free(qb); return -1; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), COMP_BIN " diff %s %s > /dev/null 2>&1", qa, qb);
    free(qa);
    free(qb);
    int rc = system(cmd);
    if (!WIFEXITED(rc)) return -1;
    int ex = WEXITSTATUS(rc);
    if (ex == 0) return 0;
    if (ex == 1) return 1;
    return -1;
}

static int run_comp_merge(const char *base, const char *ours, const char *theirs,
                          const char *out) {
    char *qb   = shell_quote(base);
    char *qo   = shell_quote(ours);
    char *qt   = shell_quote(theirs);
    char *qout = shell_quote(out);
    if (!qb || !qo || !qt || !qout) {
        free(qb); free(qo); free(qt); free(qout); return -1;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             COMP_BIN " merge %s %s %s %s 2>/dev/null", qb, qo, qt, qout);
    free(qb); free(qo); free(qt); free(qout);
    int rc = system(cmd);
    if (!WIFEXITED(rc)) return -1;
    int ex = WEXITSTATUS(rc);
    if (ex == 0) return 0;
    if (ex == 1) return 1;
    return -1;
}

static void print_conflict(const char *local_file) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║                   MERGE CONFLICT                        ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  File: %-50s║\n", local_file);
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Both local and remote changed this file since last      ║\n");
    printf("║  sync and the changes could not be merged automatically. ║\n");
    printf("║                                                          ║\n");
    printf("║  The file has been written with conflict markers:        ║\n");
    printf("║    <<<<<<< ours                                          ║\n");
    printf("║    =======                                               ║\n");
    printf("║    >>>>>>> theirs                                        ║\n");
    printf("║                                                          ║\n");
    printf("║  Resolve the conflict, then run:                         ║\n");
    printf("║    rmt sync <path> --push                                ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

static int smart_sync(const char *local_root, const char *remote_spec, int dry_run) {
    char tmp_remote[MAX_PATH_LEN];
    snprintf(tmp_remote, sizeof(tmp_remote), "/tmp/rmt_remote_XXXXXX");
    if (!mkdtemp(tmp_remote)) { perror("mkdtemp"); return -1; }

    printf("Fetching remote tree for comparison...\n");
    if (rsync_pull(remote_spec, tmp_remote, 0) != 0) {
        fprintf(stderr, "Failed to fetch remote tree\n");
        char *qtmp = shell_quote(tmp_remote);
        if (qtmp) {
            char cmd[MAX_PATH_LEN * 2];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", qtmp);
            system(cmd);
            free(qtmp);
        }
        return -1;
    }

    PathList *files = local_files(local_root);

    PathList *remote_only = malloc(sizeof(PathList));
    remote_only->cap   = 64;
    remote_only->count = 0;
    remote_only->paths = malloc(remote_only->cap * sizeof(char *));
    walk_dir(tmp_remote, "", remote_only);
    qsort(remote_only->paths, remote_only->count, sizeof(char *), pl_cmp);

    int total = files->count + remote_only->count;
    char **all = malloc(total * sizeof(char *));
    int n = 0;
    for (int i = 0; i < files->count;       i++) all[n++] = files->paths[i];
    for (int i = 0; i < remote_only->count; i++) all[n++] = remote_only->paths[i];
    qsort(all, n, sizeof(char *), pl_cmp);

    int pushed = 0, pulled = 0, merged = 0, skipped = 0;
    int result = 0;

    const char *prev = NULL;
    for (int i = 0; i < n && result == 0; i++) {
        const char *rel = all[i];
        if (prev && strcmp(rel, prev) == 0) continue;
        prev = rel;

        char local_file[MAX_PATH_LEN], base_file[MAX_PATH_LEN], remote_file[MAX_PATH_LEN];
        snprintf(local_file,  sizeof(local_file),  "%s/%s", local_root, rel);
        snprintf(remote_file, sizeof(remote_file), "%s/%s", tmp_remote, rel);
        base_path_for(local_root, rel, base_file, sizeof(base_file));

        int has_local  = (access(local_file,  F_OK) == 0);
        int has_remote = (access(remote_file, F_OK) == 0);
        int has_base   = (access(base_file,   F_OK) == 0);

        if (!has_local && !has_remote) continue;

        /* New file only on remote */
        if (!has_local && has_remote && !has_base) {
            printf("  pull (new)    %s\n", rel);
            if (!dry_run) {
                char lf_copy[MAX_PATH_LEN];
                strncpy(lf_copy, local_file, sizeof(lf_copy) - 1);
                mkdir_p(dirname(lf_copy));
                char *qrf = shell_quote(remote_file);
                char *qlf = shell_quote(local_file);
                if (qrf && qlf) {
                    char cmd[8192];
                    snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                    system(cmd);
                }
                free(qrf); free(qlf);
                base_update(local_root, rel, local_file);
            }
            pulled++;
            continue;
        }

        /* File deleted locally, existed at base */
        if (!has_local && has_base) {
            int remote_changed = has_remote ? (run_comp_diff(base_file, remote_file) == 1) : 0;
            if (remote_changed) {
                printf("  conflict      %s (deleted locally, modified remotely — keeping remote)\n", rel);
                if (!dry_run) {
                    char lf_copy[MAX_PATH_LEN];
                    strncpy(lf_copy, local_file, sizeof(lf_copy) - 1);
                    mkdir_p(dirname(lf_copy));
                    char *qrf = shell_quote(remote_file);
                    char *qlf = shell_quote(local_file);
                    if (qrf && qlf) {
                        char cmd[8192];
                        snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                        system(cmd);
                    }
                    free(qrf); free(qlf);
                    base_update(local_root, rel, local_file);
                }
            } else {
                printf("  delete remote %s\n", rel);
                if (!dry_run) {
                    char ssh_host[MAX_PATH_LEN], remote_path[MAX_PATH_LEN];
                    const char *colon = strchr(remote_spec, ':');
                    if (colon) {
                        size_t hlen = colon - remote_spec;
                        strncpy(ssh_host, remote_spec, hlen); ssh_host[hlen] = '\0';
                        snprintf(remote_path, sizeof(remote_path), "%s/%s", colon+1, rel);
                        char *qhost  = shell_quote(ssh_host);
                        char *qrpath = shell_quote(remote_path);
                        if (qhost && qrpath) {
                            char cmd[8192];
                            snprintf(cmd, sizeof(cmd), "ssh %s rm -f %s 2>/dev/null", qhost, qrpath);
                            system(cmd);
                        }
                        free(qhost); free(qrpath);
                    }
                    base_delete(local_root, rel);
                }
                pushed++;
            }
            continue;
        }

        /* File only local (new) */
        if (has_local && !has_remote && !has_base) {
            printf("  push (new)    %s\n", rel);
            if (!dry_run) {
                rsync_push_file(local_file, remote_spec, rel);
                base_update(local_root, rel, local_file);
            }
            pushed++;
            continue;
        }

        /* Both exist — diff against base */
        int local_changed  = has_base ? (run_comp_diff(base_file, local_file)  == 1) : 1;
        int remote_changed = has_base ? (run_comp_diff(base_file, remote_file) == 1) : 1;

        if (!local_changed && !remote_changed) {
            skipped++;
            continue;
        }

        if (local_changed && !remote_changed) {
            printf("  push          %s\n", rel);
            if (!dry_run) {
                rsync_push_file(local_file, remote_spec, rel);
                base_update(local_root, rel, local_file);
            }
            pushed++;
            continue;
        }

        if (!local_changed && remote_changed) {
            printf("  pull          %s\n", rel);
            if (!dry_run) {
                char lf_copy[MAX_PATH_LEN];
                strncpy(lf_copy, local_file, sizeof(lf_copy) - 1);
                mkdir_p(dirname(lf_copy));
                char *qrf = shell_quote(remote_file);
                char *qlf = shell_quote(local_file);
                if (qrf && qlf) {
                    char cmd[8192];
                    snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                    system(cmd);
                }
                free(qrf); free(qlf);
                base_update(local_root, rel, local_file);
            }
            pulled++;
            continue;
        }

        /* Both changed — 3-way merge */
        printf("  merge         %s\n", rel);
        if (!dry_run) {
            char merged_file[MAX_PATH_LEN];
            snprintf(merged_file, sizeof(merged_file), "%s.rmt_merge_XXXXXX", local_file);
            int mfd = mkstemp(merged_file);
            if (mfd < 0) { perror("mkstemp"); result = -1; break; }
            close(mfd);

            const char *bpath = has_base ? base_file : "/dev/null";
            int mrc = run_comp_merge(bpath, local_file, remote_file, merged_file);

            if (mrc == 0) {
                rename(merged_file, local_file);
                rsync_push_file(local_file, remote_spec, rel);
                base_update(local_root, rel, local_file);
                merged++;
            } else if (mrc == 1) {
                rename(merged_file, local_file);
                print_conflict(local_file);
                result = 1;
            } else {
                unlink(merged_file);
                fprintf(stderr, "comp merge failed for %s\n", rel);
                result = -1;
            }
        } else {
            merged++;
        }
    }

    free(all);
    pl_free(files);
    pl_free(remote_only);

    /* Cleanup temp remote dir */
    char *qtmp = shell_quote(tmp_remote);
    if (qtmp) {
        char cmd[MAX_PATH_LEN * 2 + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", qtmp);
        system(cmd);
        free(qtmp);
    }

    if (result == 0 && !dry_run) {
        printf("\n");
        printf("  pushed:  %d\n", pushed);
        printf("  pulled:  %d\n", pulled);
        printf("  merged:  %d\n", merged);
        printf("  skipped: %d\n", skipped);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

static int cmd_mount(const char *remote, const char *local) {
    if (!validate_remote_spec(remote)) {
        fprintf(stderr, "Invalid remote spec: %s\n", remote);
        fprintf(stderr, "Expected format: [user@]host:/path\n");
        return 1;
    }

    MountRegistry reg = {0};
    if (load_registry(&reg) != 0) { fprintf(stderr, "Failed to load registry\n"); return 1; }

    char resolved_local[MAX_PATH_LEN];
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (!getcwd(cwd, sizeof(cwd))) { fprintf(stderr, "Failed to get cwd\n"); return 1; }
        snprintf(resolved_local, sizeof(resolved_local), "%s/%s", cwd, local);
    } else {
        strncpy(resolved_local, local, sizeof(resolved_local) - 1);
        resolved_local[sizeof(resolved_local) - 1] = '\0';
    }
    normalize_path(resolved_local);

    if (find_mount(&reg, resolved_local)) {
        fprintf(stderr, "Already mounted at %s\n", resolved_local);
        return 1;
    }
    if (reg.count >= MAX_MOUNTS) { fprintf(stderr, "Max mounts reached\n"); return 1; }
    if (mkdir_p(resolved_local) != 0) {
        fprintf(stderr, "Failed to create %s: %s\n", resolved_local, strerror(errno));
        return 1;
    }

    printf("Mounting %s at %s...\n", remote, resolved_local);
    printf("Initial sync (this may take a while)...\n\n");

    if (rsync_pull(remote, resolved_local, 0) != 0) {
        fprintf(stderr, "\nMount failed: rsync error\n");
        return 1;
    }

    printf("Initialising base cache...\n");
    if (base_init(resolved_local) != 0) {
        fprintf(stderr, "Warning: failed to initialise base cache\n");
        fprintf(stderr, "First sync will treat all files as locally changed\n");
    }

    Mount *m = &reg.mounts[reg.count++];
    strncpy(m->local_path,   resolved_local, MAX_PATH_LEN - 1);
    strncpy(m->remote_spec,  remote,         MAX_PATH_LEN - 1);
    m->mounted_at = time(NULL);
    m->last_sync  = time(NULL);

    if (save_registry(&reg) != 0)
        fprintf(stderr, "Warning: Failed to save registry\n");

    printf("\n✓ Mounted successfully\n");
    printf("  Local:  %s\n", resolved_local);
    printf("  Remote: %s\n", remote);
    printf("\nSync changes with: rmt sync %s\n", resolved_local);
    return 0;
}

static int cmd_sync(const char *local, int dry_run, int pull_only, int push_only) {
    MountRegistry reg = {0};
    if (load_registry(&reg) != 0) { fprintf(stderr, "Failed to load registry\n"); return 1; }

    if (!local) {
        if (reg.count == 0) { printf("No active mounts\n"); return 0; }
        printf("Syncing all mounts...\n\n");
        int failed = 0;
        for (int i = 0; i < reg.count; i++) {
            Mount *m = &reg.mounts[i];
            printf("=== %s ===\n", m->local_path);

            int rc;
            if (pull_only) {
                rc = rsync_pull(m->remote_spec, m->local_path, dry_run);
                if (rc == 0 && !dry_run) base_init(m->local_path);
            } else if (push_only) {
                rc = rsync_push(m->local_path, m->remote_spec, dry_run);
            } else {
                rc = smart_sync(m->local_path, m->remote_spec, dry_run);
            }

            if (rc == 1) {
                return 1;
            } else if (rc == 0) {
                if (!dry_run) m->last_sync = time(NULL);
                if (!pull_only && !push_only) printf("✓ Synced\n\n");
            } else {
                printf("✗ Failed\n\n");
                failed++;
            }
        }
        if (!dry_run) save_registry(&reg);
        return failed > 0 ? 1 : 0;
    }

    Mount *m = find_mount(&reg, local);
    if (!m) {
        fprintf(stderr, "%s is not a mounted path\n", local);
        fprintf(stderr, "Use 'rmt status' to see active mounts\n");
        return 1;
    }

    printf("Syncing %s <-> %s...\n\n", m->local_path, m->remote_spec);

    int rc;
    if (pull_only) {
        rc = rsync_pull(m->remote_spec, m->local_path, dry_run);
        if (rc == 0 && !dry_run) base_init(m->local_path);
    } else if (push_only) {
        rc = rsync_push(m->local_path, m->remote_spec, dry_run);
    } else {
        rc = smart_sync(m->local_path, m->remote_spec, dry_run);
    }

    if (rc == 1) return 1;
    if (rc != 0) { fprintf(stderr, "\nSync failed\n"); return 1; }

    if (!dry_run) {
        m->last_sync = time(NULL);
        save_registry(&reg);
    }

    printf("\n✓ Sync complete\n");
    return 0;
}

static int cmd_status(void) {
    MountRegistry reg = {0};
    if (load_registry(&reg) != 0) { fprintf(stderr, "Failed to load registry\n"); return 1; }

    if (reg.count == 0) {
        printf("No active mounts\n\n");
        printf("Mount a remote directory with:\n");
        printf("  rmt mount user@host:/path ~/local/path\n");
        return 0;
    }

    printf("Active mounts:\n\n");
    time_t now = time(NULL);
    for (int i = 0; i < reg.count; i++) {
        Mount *m = &reg.mounts[i];
        int hours = (int)((now - m->last_sync) / 3600);
        int days  = hours / 24;
        printf("  [%d] %s\n", i + 1, m->local_path);
        printf("      Remote: %s\n", m->remote_spec);
        if      (days  > 0) printf("      Last sync: %d day%s ago\n",  days,  days  == 1 ? "" : "s");
        else if (hours > 0) printf("      Last sync: %d hour%s ago\n", hours, hours == 1 ? "" : "s");
        else                printf("      Last sync: <1 hour ago\n");
        printf("\n");
    }

    printf("Commands:\n");
    printf("  rmt sync [path]     Sync mount (or all if no path given)\n");
    printf("  rmt unmount <path>  Unmount and remove from registry\n");
    return 0;
}

static void usage(const char *prog) {
    printf("rmt - Remote Mount Tool v%s\n\n", VERSION);
    printf("Usage:\n");
    printf("  %s mount <user@host:/remote> <local-path>\n", prog);
    printf("  %s sync [local-path] [--dry-run] [--pull] [--push]\n", prog);
    printf("  %s unmount <local-path> [--keep]\n", prog);
    printf("  %s status\n", prog);
    printf("  %s reset\n", prog);
    printf("\n");
    printf("Commands:\n");
    printf("  mount    Mount a remote directory locally\n");
    printf("  sync     Smart sync using comp for conflict detection and merge\n");
    printf("  unmount  Final sync, then unmount and remove from registry\n");
    printf("  status   Show all active mounts\n");
    printf("  reset    Clear the registry\n");
    printf("\n");
    printf("Sync options:\n");
    printf("  --dry-run  Show what would be synced without doing it\n");
    printf("  --pull     Only pull changes from remote (one-way, updates base)\n");
    printf("  --push     Only push changes to remote (one-way)\n");
    printf("\n");
    printf("Unmount options:\n");
    printf("  --keep     Keep local files (default: final sync then delete)\n");
    printf("\n");
    printf("How sync works:\n");
    printf("  Each file is compared against its last-synced state (.rmt-base/).\n");
    printf("  Only local changed  -> push\n");
    printf("  Only remote changed -> pull\n");
    printf("  Both changed        -> 3-way merge via comp\n");
    printf("  Conflict            -> write conflict markers, stop, report\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "mount") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s mount <user@host:/remote> <local-path>\n", argv[0]);
            return 1;
        }
        return cmd_mount(argv[2], argv[3]);
    }

    if (strcmp(cmd, "sync") == 0) {
        const char *path = NULL;
        int dry_run = 0, pull_only = 0, push_only = 0;
        for (int i = 2; i < argc; i++) {
            if      (strcmp(argv[i], "--dry-run") == 0) dry_run   = 1;
            else if (strcmp(argv[i], "--pull")    == 0) pull_only = 1;
            else if (strcmp(argv[i], "--push")    == 0) push_only = 1;
            else if (argv[i][0] != '-')                 path      = argv[i];
        }
        if (pull_only && push_only) { fprintf(stderr, "Cannot use both --pull and --push\n"); return 1; }
        return cmd_sync(path, dry_run, pull_only, push_only);
    }

    if (strcmp(cmd, "unmount") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s unmount <local-path> [--keep]\n", argv[0]); return 1; }
        int keep = 0;
        for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--keep") == 0) keep = 1;
        return cmd_unmount(argv[2], keep);
    }

    if (strcmp(cmd, "status") == 0) return cmd_status();

    if (strcmp(cmd, "reset") == 0) {
        const char *registry = get_registry_path();
        printf("This will delete the registry at: %s\n", registry);
        printf("All mount information will be lost (local files will remain).\n");
        printf("Continue? [y/N] ");
        char resp[16];
        if (!fgets(resp, sizeof(resp), stdin) || (resp[0] != 'y' && resp[0] != 'Y')) {
            printf("Reset cancelled\n"); return 0;
        }
        if (unlink(registry) == 0) { printf("✓ Registry reset\n"); return 0; }
        else if (errno == ENOENT)  { printf("Registry already empty\n"); return 0; }
        else { fprintf(stderr, "Failed to reset: %s\n", strerror(errno)); return 1; }
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) { usage(argv[0]); return 0; }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    fprintf(stderr, "Try '%s --help' for usage\n", argv[0]);
    return 1;
}