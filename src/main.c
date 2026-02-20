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

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

static void shell_quote(char *out, size_t out_len, const char *in) {
    size_t j = 0;
    if (out_len < 3) { if (out_len > 0) out[0] = '\0'; return; }
    out[j++] = '\'';
    for (size_t i = 0; in[i] != '\0' && j + 6 < out_len; i++) {
        if (in[i] == '\'') {
            const char *esc = "'\"'\"'";
            for (int k = 0; esc[k] && j + 1 < out_len; k++) out[j++] = esc[k];
        } else {
            out[j++] = in[i];
        }
    }
    if (j + 1 < out_len) out[j++] = '\'';
    out[j] = '\0';
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
//
// Base files live at <local>/.rmt-base/<relative-path>
// They represent the last-synced state — the common ancestor for 3-way merge.
// ---------------------------------------------------------------------------

static void base_path_for(const char *local_root, const char *rel,
                           char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s/%s", local_root, BASE_DIR_NAME, rel);
}

/* Update the base snapshot for one file after a successful sync. */
static int base_update(const char *local_root, const char *rel,
                       const char *src_path)
{
    char base[MAX_PATH_LEN];
    base_path_for(local_root, rel, base, sizeof(base));

    /* Ensure base directory exists */
    char base_copy[MAX_PATH_LEN];
    strncpy(base_copy, base, sizeof(base_copy) - 1);
    if (mkdir_p(dirname(base_copy)) != 0) return -1;

    /* Copy src -> base atomically */
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

/* Delete base snapshot for a file (when it's been deleted on both sides). */
static void base_delete(const char *local_root, const char *rel)
{
    char base[MAX_PATH_LEN];
    base_path_for(local_root, rel, base, sizeof(base));
    unlink(base);
}

/* Initialise the base cache from the current local tree after mount. */
static int base_init(const char *local_root)
{
    /* Walk local dir (excluding .rmt-base itself) and snapshot every file. */
    char base_dir[MAX_PATH_LEN];
    snprintf(base_dir, sizeof(base_dir), "%s/%s", local_root, BASE_DIR_NAME);
    if (mkdir_p(base_dir) != 0) return -1;

    /* Use find to enumerate — keeps this code simple */
    char cmd[MAX_PATH_LEN * 2 + 256];
    char qlocal[MAX_PATH_LEN * 2];
    char qbase[MAX_PATH_LEN * 2];
    shell_quote(qlocal, sizeof(qlocal), local_root);
    shell_quote(qbase,  sizeof(qbase),  base_dir);

    /* Copy entire local tree into base, excluding .rmt-base itself */
    snprintf(cmd, sizeof(cmd),
             "rsync -a --exclude=%s/ %s/ %s/ 2>/dev/null",
             BASE_DIR_NAME, qlocal, qbase);
    return system(cmd) == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Registry operations (unchanged from original)
// ---------------------------------------------------------------------------

static int load_registry(MountRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
    FILE *f = fopen(get_registry_path(), "rb");
    if (!f) { if (errno == ENOENT) return 0; return -1; }
    int fd = fileno(f);
    if (flock(fd, LOCK_SH) != 0) { fclose(f); return -1; }
    if (fread(&reg->count, sizeof(int), 1, f) != 1) { flock(fd, LOCK_UN); fclose(f); return -1; }
    if (reg->count > MAX_MOUNTS || reg->count < 0) {
        flock(fd, LOCK_UN); fclose(f);
        fprintf(stderr, "Registry corrupted\n"); return -1;
    }
    if (reg->count > 0)
        if (fread(reg->mounts, sizeof(Mount), reg->count, f) != (size_t)reg->count) {
            flock(fd, LOCK_UN); fclose(f); return -1;
        }
    flock(fd, LOCK_UN); fclose(f);
    return 0;
}

static int save_registry(const MountRegistry *reg) {
    if (mkdir_p(get_rmt_dir()) != 0) return -1;
    int fd = open(get_registry_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    FILE *f = fdopen(fd, "wb");
    if (!f) { flock(fd, LOCK_UN); close(fd); return -1; }
    if (fwrite(&reg->count, sizeof(int), 1, f) != 1) { flock(fd, LOCK_UN); fclose(f); return -1; }
    if (reg->count > 0)
        if (fwrite(reg->mounts, sizeof(Mount), reg->count, f) != (size_t)reg->count) {
            flock(fd, LOCK_UN); fclose(f); return -1;
        }
    flock(fd, LOCK_UN); fclose(f);
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

static int remove_mount(MountRegistry *reg, const char *local) {
    char resolved[MAX_PATH_LEN];
    if (realpath(local, resolved)) {
        for (int i = 0; i < reg->count; i++) {
            if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                for (int j = i; j < reg->count - 1; j++) reg->mounts[j] = reg->mounts[j+1];
                reg->count--; return 0;
            }
        }
        return -1;
    }
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(resolved, sizeof(resolved), "%s/%s", cwd, local);
            int len = strlen(resolved);
            while (len > 1 && resolved[len-1] == '/') resolved[--len] = '\0';
            for (int i = 0; i < reg->count; i++) {
                if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                    for (int j = i; j < reg->count - 1; j++) reg->mounts[j] = reg->mounts[j+1];
                    reg->count--; return 0;
                }
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Rsync helpers (kept for mount, push/pull of individual files, and pull-only)
// ---------------------------------------------------------------------------

static int rsync_pull(const char *remote, const char *local, int dry_run) {
    char qremote[MAX_PATH_LEN * 2], qlocal[MAX_PATH_LEN * 2], cmd[8192];
    shell_quote(qremote, sizeof(qremote), remote);
    shell_quote(qlocal,  sizeof(qlocal),  local);
    snprintf(cmd, sizeof(cmd), "rsync -avz%s --exclude=%s/ %s/ %s/ 2>&1",
             dry_run ? "n" : "", BASE_DIR_NAME, qremote, qlocal);
    if (dry_run) printf("Dry run (pull): %s -> %s\n", remote, local);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int rsync_push(const char *local, const char *remote, int dry_run) {
    char qremote[MAX_PATH_LEN * 2], qlocal[MAX_PATH_LEN * 2], cmd[8192];
    shell_quote(qremote, sizeof(qremote), remote);
    shell_quote(qlocal,  sizeof(qlocal),  local);
    snprintf(cmd, sizeof(cmd), "rsync -avz%s --exclude=%s/ %s/ %s/ 2>&1",
             dry_run ? "n" : "", BASE_DIR_NAME, qlocal, qremote);
    if (dry_run) printf("Dry run (push): %s -> %s\n", local, remote);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Push a single file to remote. */
static int rsync_push_file(const char *src_path, const char *remote_spec,
                            const char *rel) {
    char remote_file[MAX_PATH_LEN * 2];
    char qsrc[MAX_PATH_LEN * 2], qrf[MAX_PATH_LEN * 2], cmd[8192];
    snprintf(remote_file, sizeof(remote_file), "%s/%s", remote_spec, rel);
    shell_quote(qsrc, sizeof(qsrc), src_path);
    shell_quote(qrf,  sizeof(qrf),  remote_file);
    /* Ensure remote directory exists */
    char rel_dir[MAX_PATH_LEN];
    strncpy(rel_dir, rel, sizeof(rel_dir) - 1);
    char *rd = dirname(rel_dir);
    if (strcmp(rd, ".") != 0) {
        char ssh_host[MAX_PATH_LEN], remote_path[MAX_PATH_LEN];
        const char *colon = strchr(remote_spec, ':');
        if (colon) {
            size_t hlen = colon - remote_spec;
            strncpy(ssh_host, remote_spec, hlen); ssh_host[hlen] = '\0';
            snprintf(remote_path, sizeof(remote_path), "%s/%s", colon + 1, rd);
            char qhost[MAX_PATH_LEN], qrpath[MAX_PATH_LEN * 2], mkdir_cmd[8192];
            shell_quote(qhost,  sizeof(qhost),  ssh_host);
            shell_quote(qrpath, sizeof(qrpath), remote_path);
            snprintf(mkdir_cmd, sizeof(mkdir_cmd),
                     "ssh %s mkdir -p %s 2>/dev/null", qhost, qrpath);
            system(mkdir_cmd);
        }
    }
    snprintf(cmd, sizeof(cmd), "rsync -az %s %s 2>&1", qsrc, qrf);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// ---------------------------------------------------------------------------
// Directory walk — returns sorted relative paths, excluding BASE_DIR_NAME
// ---------------------------------------------------------------------------

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
        if (strcmp(ent->d_name, BASE_DIR_NAME) == 0) continue; /* skip base cache */

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
//
// For each file in the local tree:
//   1. Pull remote copy into a temp dir
//   2. comp diff base local  -> local_changed
//   3. comp diff base remote -> remote_changed
//   4. Decide action (skip / push / pull / merge)
//   5. On merge conflict: print conflict, stop, return 1
//
// Returns 0 on success, 1 on conflict, -1 on error.
// ---------------------------------------------------------------------------

static int run_comp_diff(const char *a, const char *b) {
    /* Returns 0=same, 1=different, -1=error */
    char qa[MAX_PATH_LEN * 2], qb[MAX_PATH_LEN * 2], cmd[8192];
    shell_quote(qa, sizeof(qa), a);
    shell_quote(qb, sizeof(qb), b);
    snprintf(cmd, sizeof(cmd), COMP_BIN " diff %s %s > /dev/null 2>&1", qa, qb);
    int rc = system(cmd);
    if (!WIFEXITED(rc)) return -1;
    int ex = WEXITSTATUS(rc);
    if (ex == 0) return 0;
    if (ex == 1) return 1;
    return -1;
}

static int run_comp_merge(const char *base, const char *ours, const char *theirs,
                          const char *out) {
    /* Returns 0=clean, 1=conflict, -1=error */
    char qb[MAX_PATH_LEN*2], qo[MAX_PATH_LEN*2], qt[MAX_PATH_LEN*2], qout[MAX_PATH_LEN*2];
    char cmd[8192];
    shell_quote(qb,   sizeof(qb),   base);
    shell_quote(qo,   sizeof(qo),   ours);
    shell_quote(qt,   sizeof(qt),   theirs);
    shell_quote(qout, sizeof(qout), out);
    snprintf(cmd, sizeof(cmd),
             COMP_BIN " merge %s %s %s %s 2>/dev/null", qb, qo, qt, qout);
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
    /* Temp dir for fetching remote copies */
    char tmp_remote[MAX_PATH_LEN];
    snprintf(tmp_remote, sizeof(tmp_remote), "/tmp/rmt_remote_XXXXXX");
    if (!mkdtemp(tmp_remote)) { perror("mkdtemp"); return -1; }

    /* Pull entire remote tree into tmp dir for comparison */
    printf("Fetching remote tree for comparison...\n");
    if (rsync_pull(remote_spec, tmp_remote, 0) != 0) {
        fprintf(stderr, "Failed to fetch remote tree\n");
        /* cleanup */
        char cmd[MAX_PATH_LEN * 2];
        char qtmp[MAX_PATH_LEN * 2];
        shell_quote(qtmp, sizeof(qtmp), tmp_remote);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", qtmp);
        system(cmd);
        return -1;
    }

    PathList *files = local_files(local_root);

    /* Also collect files that exist only on remote */
    PathList *remote_only = malloc(sizeof(PathList));
    remote_only->cap   = 64;
    remote_only->count = 0;
    remote_only->paths = malloc(remote_only->cap * sizeof(char *));
    walk_dir(tmp_remote, "", remote_only);
    qsort(remote_only->paths, remote_only->count, sizeof(char *), pl_cmp);

    /* Build union of local + remote paths */
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

        /* ----------------------------------------------------------------
         * Classify the change
         * ---------------------------------------------------------------- */

        if (!has_local && !has_remote) continue; /* shouldn't happen */

        /* New file only on remote (never synced before) */
        if (!has_local && has_remote && !has_base) {
            printf("  pull (new)    %s\n", rel);
            if (!dry_run) {
                /* Ensure local dir exists */
                char lf_copy[MAX_PATH_LEN];
                strncpy(lf_copy, local_file, sizeof(lf_copy) - 1);
                mkdir_p(dirname(lf_copy));

                /* Copy from tmp_remote to local */
                char cmd[8192];
                char qrf[MAX_PATH_LEN*2], qlf[MAX_PATH_LEN*2];
                shell_quote(qrf, sizeof(qrf), remote_file);
                shell_quote(qlf, sizeof(qlf), local_file);
                snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                system(cmd);
                base_update(local_root, rel, local_file);
            }
            pulled++;
            continue;
        }

        /* File deleted locally, existed at base → push deletion to remote */
        if (!has_local && has_base) {
            int remote_changed = has_remote ? (run_comp_diff(base_file, remote_file) == 1) : 0;
            if (remote_changed) {
                /* Remote also changed — conflict: keep remote */
                printf("  conflict      %s (deleted locally, modified remotely — keeping remote)\n", rel);
                if (!dry_run) {
                    char lf_copy[MAX_PATH_LEN];
                    strncpy(lf_copy, local_file, sizeof(lf_copy) - 1);
                    mkdir_p(dirname(lf_copy));
                    char cmd[8192];
                    char qrf[MAX_PATH_LEN*2], qlf[MAX_PATH_LEN*2];
                    shell_quote(qrf, sizeof(qrf), remote_file);
                    shell_quote(qlf, sizeof(qlf), local_file);
                    snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                    system(cmd);
                    base_update(local_root, rel, local_file);
                }
            } else {
                printf("  delete remote %s\n", rel);
                if (!dry_run) {
                    /* Push deletion: ssh rm */
                    char ssh_host[MAX_PATH_LEN], remote_path[MAX_PATH_LEN];
                    const char *colon = strchr(remote_spec, ':');
                    if (colon) {
                        size_t hlen = colon - remote_spec;
                        strncpy(ssh_host, remote_spec, hlen); ssh_host[hlen] = '\0';
                        snprintf(remote_path, sizeof(remote_path), "%s/%s", colon+1, rel);
                        char cmd[8192];
                        char qhost[MAX_PATH_LEN*2], qrpath[MAX_PATH_LEN*2];
                        shell_quote(qhost,  sizeof(qhost),  ssh_host);
                        shell_quote(qrpath, sizeof(qrpath), remote_path);
                        snprintf(cmd, sizeof(cmd), "ssh %s rm -f %s 2>/dev/null", qhost, qrpath);
                        system(cmd);
                    }
                    base_delete(local_root, rel);
                }
                pushed++;
            }
            continue;
        }

        /* File only local (new, never on remote) */
        if (has_local && !has_remote && !has_base) {
            printf("  push (new)    %s\n", rel);
            if (!dry_run) {
                rsync_push_file(local_file, remote_spec, rel);
                base_update(local_root, rel, local_file);
            }
            pushed++;
            continue;
        }

        /* Both exist (or existed) — diff against base to classify */
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
                char cmd[8192];
                char qrf[MAX_PATH_LEN*2], qlf[MAX_PATH_LEN*2];
                shell_quote(qrf, sizeof(qrf), remote_file);
                shell_quote(qlf, sizeof(qlf), local_file);
                snprintf(cmd, sizeof(cmd), "cp %s %s", qrf, qlf);
                system(cmd);
                base_update(local_root, rel, local_file);
            }
            pulled++;
            continue;
        }

        /* Both changed — attempt 3-way merge */
        printf("  merge         %s\n", rel);
        if (!dry_run) {
            char merged_file[MAX_PATH_LEN];
            snprintf(merged_file, sizeof(merged_file), "%s.rmt_merge_XXXXXX", local_file);
            int mfd = mkstemp(merged_file);
            if (mfd < 0) { perror("mkstemp"); result = -1; break; }
            close(mfd);

            /* base = base_file (or empty if no base) */
            const char *bpath = has_base ? base_file : "/dev/null";
            int mrc = run_comp_merge(bpath, local_file, remote_file, merged_file);

            if (mrc == 0) {
                /* Clean merge — replace local, push to remote, update base */
                rename(merged_file, local_file);
                rsync_push_file(local_file, remote_spec, rel);
                base_update(local_root, rel, local_file);
                merged++;
            } else if (mrc == 1) {
                /* Conflict — write merged (with markers) to local, stop */
                rename(merged_file, local_file);
                print_conflict(local_file);
                result = 1;
            } else {
                unlink(merged_file);
                fprintf(stderr, "comp merge failed for %s\n", rel);
                result = -1;
            }
        } else {
            merged++; /* count as would-merge in dry run */
        }
    }

    free(all);
    pl_free(files);
    pl_free(remote_only);

    /* Cleanup temp remote dir */
    char cmd[MAX_PATH_LEN * 2 + 16];
    char qtmp[MAX_PATH_LEN * 2];
    shell_quote(qtmp, sizeof(qtmp), tmp_remote);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", qtmp);
    system(cmd);

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

    /* Initialise base cache from the freshly pulled tree */
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
                if (rc == 0 && !dry_run) base_init(m->local_path); /* refresh base */
            } else if (push_only) {
                rc = rsync_push(m->local_path, m->remote_spec, dry_run);
            } else {
                rc = smart_sync(m->local_path, m->remote_spec, dry_run);
            }

            if (rc == 1) {
                /* Conflict — stop entirely */
                return 1;
            } else if (rc == 0) {
                if (!dry_run) m->last_sync = time(NULL);
                if (rc == 0 && !pull_only && !push_only) printf("✓ Synced\n\n");
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

    if (rc == 1) return 1; /* conflict already printed */

    if (rc != 0) { fprintf(stderr, "\nSync failed\n"); return 1; }

    if (!dry_run) {
        m->last_sync = time(NULL);
        save_registry(&reg);
    }

    printf("\n✓ Sync complete\n");
    return 0;
}

static int cmd_unmount(const char *local, int keep_local) {
    MountRegistry reg = {0};
    if (load_registry(&reg) != 0) { fprintf(stderr, "Failed to load registry\n"); return 1; }

    Mount *m = find_mount(&reg, local);
    if (!m) { fprintf(stderr, "%s is not a mounted path\n", local); return 1; }

    char saved_local[MAX_PATH_LEN], saved_remote[MAX_PATH_LEN];
    strncpy(saved_local,  m->local_path,  sizeof(saved_local));
    strncpy(saved_remote, m->remote_spec, sizeof(saved_remote));

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

    if (remove_mount(&reg, local) != 0) { fprintf(stderr, "Failed to remove from registry\n"); return 1; }
    if (save_registry(&reg) != 0) fprintf(stderr, "Warning: Failed to save registry\n");

    printf("✓ Unmounted %s\n", saved_local);

    if (keep_local) {
        printf("  Local files kept at: %s\n", saved_local);
        printf("  Note: .rmt-base cache kept alongside local files\n");
    } else {
        printf("  Deleting local copy...\n");
        char cmd[MAX_PATH_LEN * 2 + 16];
        char quoted[MAX_PATH_LEN * 2];
        shell_quote(quoted, sizeof(quoted), saved_local);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", quoted);
        if (system(cmd) == 0) printf("  ✓ Deleted %s\n", saved_local);
        else fprintf(stderr, "  Warning: Failed to delete local files\n");
    }

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