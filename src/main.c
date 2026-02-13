// rmt - Remote Mount Tool
// Sync remote directories locally to use with goto, vic, peek, ff
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 
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

#define MAX_MOUNTS 32
#define MAX_PATH_LEN 4096
#define VERSION "1.0.0"

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

// --- Utility functions ---

static void shell_quote(char *out, size_t out_len, const char *in) {
    size_t j = 0;
    if (out_len < 3) {  // Need at least room for '' and null
        if (out_len > 0) out[0] = '\0';
        return;
    }

    out[j++] = '\'';
    for (size_t i = 0; in[i] != '\0' && j + 5 < out_len; i++) {  // Changed from 6 to 5
        if (in[i] == '\'') {
            // Need 5 chars: '\"'\"'
            const char *esc = "'\"'\"'";
            for (int k = 0; esc[k] && j + 1 < out_len; k++) {
                out[j++] = esc[k];
            }
        } else {
            out[j++] = in[i];
        }
    }
    if (j + 1 < out_len) out[j++] = '\'';
    out[j] = '\0';
}

static int mkdir_p(const char *path) {
    char tmp[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int validate_remote_spec(const char *spec) {
    if (!spec || !*spec) return 0;

    // Must contain a colon
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec) return 0;

    // Path after colon should not be empty
    if (!*(colon + 1)) return 0;

    // If there's an @, validate user@host format
    const char *at = strchr(spec, '@');
    if (at) {
        // @ must come before :
        if (at > colon) return 0;
        // Must have user part
        if (at == spec) return 0;
    }

    return 1;
}

static void normalize_path(char *path) {
    if (!path) return;

    int len = strlen(path);

    // Remove trailing slashes
    while (len > 1 && path[len-1] == '/') {
        path[--len] = '\0';
    }

    // Remove ./ patterns
    char *src = path;
    char *dst = path;

    while (*src) {
        if (src[0] == '.' && src[1] == '/') {
            src += 2;
            continue;
        }
        if (src[0] == '/' && src[1] == '.' && src[2] == '/') {
            src += 2;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';

    // Remove duplicate slashes
    src = path;
    dst = path;
    while (*src) {
        *dst++ = *src++;
        if (src[-1] == '/') {
            while (*src == '/') src++;
        }
    }
    *dst = '\0';
}

static const char *get_rmt_dir(void) {
    static char path[MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    int written = snprintf(path, sizeof(path), "%s/.rmt", home);
    if (written < 0 || written >= (int)sizeof(path)) {
        fprintf(stderr, "Warning: HOME path too long, using /tmp/.rmt\n");
        strncpy(path, "/tmp/.rmt", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    return path;
}

static const char *get_registry_path(void) {
    static char path[MAX_PATH_LEN];

    int written = snprintf(path, sizeof(path), "%s/registry", get_rmt_dir());
    if (written < 0 || written >= (int)sizeof(path)) {
        fprintf(stderr, "Error: registry path too long\n");
        return NULL;
    }

    return path;
}

// --- Registry operations ---

static int load_registry(MountRegistry *reg) {
    memset(reg, 0, sizeof(*reg));

    FILE *f = fopen(get_registry_path(), "rb");
    if (!f) {
        if (errno == ENOENT) return 0; // No registry yet, that's ok
        return -1;
    }

    // Lock the file for reading
    int fd = fileno(f);
    if (flock(fd, LOCK_SH) != 0) {
        fclose(f);
        fprintf(stderr, "Failed to lock registry: %s\n", strerror(errno));
        return -1;
    }

    if (fread(&reg->count, sizeof(int), 1, f) != 1) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    if (reg->count > MAX_MOUNTS || reg->count < 0) {
        flock(fd, LOCK_UN);
        fclose(f);
        fprintf(stderr, "Registry corrupted (invalid mount count: %d)\n", reg->count);
        return -1;
    }

    if (reg->count > 0) {
        if (fread(reg->mounts, sizeof(Mount), reg->count, f) != (size_t)reg->count) {
            flock(fd, LOCK_UN);
            fclose(f);
            return -1;
        }
    }

    flock(fd, LOCK_UN);
    fclose(f);
    return 0;
}

static int save_registry(const MountRegistry *reg) {
    // Ensure directory exists
    if (mkdir_p(get_rmt_dir()) != 0) {
        fprintf(stderr, "Failed to create %s: %s\n", get_rmt_dir(), strerror(errno));
        return -1;
    }

    // Open with proper permissions
    int fd = open(get_registry_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "Failed to open registry: %s\n", strerror(errno));
        return -1;
    }

    // Exclusive lock for writing
    if (flock(fd, LOCK_EX) != 0) {
        fprintf(stderr, "Failed to lock registry: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    FILE *f = fdopen(fd, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create file stream: %s\n", strerror(errno));
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }

    if (fwrite(&reg->count, sizeof(int), 1, f) != 1) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    if (reg->count > 0) {
        if (fwrite(reg->mounts, sizeof(Mount), reg->count, f) != (size_t)reg->count) {
            flock(fd, LOCK_UN);
            fclose(f);
            return -1;
        }
    }

    flock(fd, LOCK_UN);
    fclose(f);  // This also closes fd
    return 0;
}

static Mount *find_mount(MountRegistry *reg, const char *local) {
    char resolved[MAX_PATH_LEN];

    // Try realpath first (works if path exists)
    if (realpath(local, resolved)) {
        normalize_path(resolved);
        for (int i = 0; i < reg->count; i++) {
            if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                return &reg->mounts[i];
            }
        }
        return NULL;
    }

    // If realpath failed, try to resolve relative path manually
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(resolved, sizeof(resolved), "%s/%s", cwd, local);
            normalize_path(resolved);

            for (int i = 0; i < reg->count; i++) {
                if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                    return &reg->mounts[i];
                }
            }
        }
    }

    return NULL;
}

static int remove_mount(MountRegistry *reg, const char *local) {
    char resolved[MAX_PATH_LEN];

    // Try realpath first
    if (realpath(local, resolved)) {
        for (int i = 0; i < reg->count; i++) {
            if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                for (int j = i; j < reg->count - 1; j++) {
                    reg->mounts[j] = reg->mounts[j + 1];
                }
                reg->count--;
                return 0;
            }
        }
        return -1;
    }

    // If realpath failed, try manual resolution
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(resolved, sizeof(resolved), "%s/%s", cwd, local);
            int len = strlen(resolved);
            while (len > 1 && resolved[len-1] == '/') {
                resolved[--len] = '\0';
            }

            for (int i = 0; i < reg->count; i++) {
                if (strcmp(reg->mounts[i].local_path, resolved) == 0) {
                    for (int j = i; j < reg->count - 1; j++) {
                        reg->mounts[j] = reg->mounts[j + 1];
                    }
                    reg->count--;
                    return 0;
                }
            }
        }
    }

    return -1;
}

// --- Rsync operations ---

static int rsync_pull(const char *remote, const char *local, int dry_run) {
    char qremote[MAX_PATH_LEN * 2];
    char qlocal[MAX_PATH_LEN * 2];
    char cmd[8192];

    shell_quote(qremote, sizeof(qremote), remote);
    shell_quote(qlocal, sizeof(qlocal), local);

    // SAFE: No --delete flag - only adds/updates files
    snprintf(cmd, sizeof(cmd),
             "rsync -avz%s %s/ %s/ 2>&1",
             dry_run ? "n" : "",
             qremote, qlocal);

    if (dry_run) {
        printf("Dry run (pull): %s -> %s\n", remote, local);
    }

    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int rsync_push(const char *local, const char *remote, int dry_run) {
    char qremote[MAX_PATH_LEN * 2];
    char qlocal[MAX_PATH_LEN * 2];
    char cmd[8192];

    shell_quote(qremote, sizeof(qremote), remote);
    shell_quote(qlocal, sizeof(qlocal), local);

    // SAFE: No --delete flag - only adds/updates files
    snprintf(cmd, sizeof(cmd),
             "rsync -avz%s %s/ %s/ 2>&1",
             dry_run ? "n" : "",
             qlocal, qremote);

    if (dry_run) {
        printf("Dry run (push): %s -> %s\n", local, remote);
    }

    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int rsync_bidirectional(const char *local, const char *remote, int dry_run) {
    printf("Pulling changes from remote...\n");
    if (rsync_pull(remote, local, dry_run) != 0) {
        fprintf(stderr, "Pull failed\n");
        return -1;
    }

    printf("\nPushing local changes to remote...\n");
    if (rsync_push(local, remote, dry_run) != 0) {
        fprintf(stderr, "Push failed\n");
        return -1;
    }

    return 0;
}

// --- Command implementations ---

static int cmd_mount(const char *remote, const char *local) {
    // Validate remote spec format
    if (!validate_remote_spec(remote)) {
        fprintf(stderr, "Invalid remote spec: %s\n", remote);
        fprintf(stderr, "Expected format: [user@]host:/path\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  user@server:/var/www\n");
        fprintf(stderr, "  server:/home/user/project\n");
        return 1;
    }

    MountRegistry reg = {0};

    if (load_registry(&reg) != 0) {
        fprintf(stderr, "Failed to load registry\n");
        return 1;
    }

    // Resolve local path
    char resolved_local[MAX_PATH_LEN];
    if (local[0] != '/') {
        char cwd[MAX_PATH_LEN];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "Failed to get current directory\n");
            return 1;
        }
        snprintf(resolved_local, sizeof(resolved_local), "%s/%s", cwd, local);
    } else {
        strncpy(resolved_local, local, sizeof(resolved_local) - 1);
        resolved_local[sizeof(resolved_local) - 1] = '\0';
    }

    // Normalize the path (remove ./ and //)
    normalize_path(resolved_local);

    // Check if already mounted
    if (find_mount(&reg, resolved_local)) {
        fprintf(stderr, "Already mounted at %s\n", resolved_local);
        fprintf(stderr, "Use 'rmt sync' to update or 'rmt unmount' first\n");
        return 1;
    }

    // Check if we have space
    if (reg.count >= MAX_MOUNTS) {
        fprintf(stderr, "Maximum number of mounts (%d) reached\n", MAX_MOUNTS);
        return 1;
    }

    // Create local directory
    if (mkdir_p(resolved_local) != 0) {
        fprintf(stderr, "Failed to create %s: %s\n", resolved_local, strerror(errno));
        return 1;
    }

    // Initial sync (pull)
    printf("Mounting %s at %s...\n", remote, resolved_local);
    printf("Initial sync (this may take a while)...\n\n");

    if (rsync_pull(remote, resolved_local, 0) != 0) {
        fprintf(stderr, "\nMount failed: rsync error\n");
        fprintf(stderr, "Check that:\n");
        fprintf(stderr, "  - SSH connection works: ssh %s\n", remote);
        fprintf(stderr, "  - Remote path exists\n");
        fprintf(stderr, "  - You have rsync installed\n");
        return 1;
    }

    // Add to registry
    Mount *m = &reg.mounts[reg.count++];
    strncpy(m->local_path, resolved_local, MAX_PATH_LEN - 1);
    m->local_path[MAX_PATH_LEN - 1] = '\0';
    strncpy(m->remote_spec, remote, MAX_PATH_LEN - 1);
    m->remote_spec[MAX_PATH_LEN - 1] = '\0';
    m->mounted_at = time(NULL);
    m->last_sync = time(NULL);

    if (save_registry(&reg) != 0) {
        fprintf(stderr, "Warning: Failed to save registry\n");
    }

    printf("\n✓ Mounted successfully\n");
    printf("  Local:  %s\n", resolved_local);
    printf("  Remote: %s\n", remote);
    printf("\nYou can now use your tools on the local path:\n");
    printf("  goto %s\n", resolved_local);
    printf("  vic %s/file.c\n", resolved_local);
    printf("  peek %s/logs/app.log\n", resolved_local);
    printf("\nSync changes with: rmt sync %s\n", resolved_local);

    return 0;
}

static int cmd_sync(const char *local, int dry_run, int pull_only, int push_only) {
    MountRegistry reg = {0};

    if (load_registry(&reg) != 0) {
        fprintf(stderr, "Failed to load registry\n");
        return 1;
    }

    // If no path given, sync all mounts
    if (!local) {
        if (reg.count == 0) {
            printf("No active mounts\n");
            return 0;
        }

        printf("Syncing all mounts...\n\n");
        int failed = 0;
        for (int i = 0; i < reg.count; i++) {
            Mount *m = &reg.mounts[i];
            printf("=== %s ===\n", m->local_path);

            int rc;
            if (pull_only) {
                rc = rsync_pull(m->remote_spec, m->local_path, dry_run);
            } else if (push_only) {
                rc = rsync_push(m->local_path, m->remote_spec, dry_run);
            } else {
                rc = rsync_bidirectional(m->local_path, m->remote_spec, dry_run);
            }

            if (rc == 0) {
                if (!dry_run) {
                    m->last_sync = time(NULL);
                }
                printf("✓ Synced\n\n");
            } else {
                printf("✗ Failed\n\n");
                failed++;
            }
        }

        if (!dry_run) {
            save_registry(&reg);
        }

        return failed > 0 ? 1 : 0;
    }

    // Sync specific mount
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
    } else if (push_only) {
        rc = rsync_push(m->local_path, m->remote_spec, dry_run);
    } else {
        rc = rsync_bidirectional(m->local_path, m->remote_spec, dry_run);
    }

    if (rc != 0) {
        fprintf(stderr, "\nSync failed\n");
        return 1;
    }

    if (!dry_run) {
        m->last_sync = time(NULL);
        save_registry(&reg);
    }

    printf("\n✓ Sync complete\n");
    return 0;
}

static int cmd_unmount(const char *local, int keep_local) {
    MountRegistry reg = {0};

    if (load_registry(&reg) != 0) {
        fprintf(stderr, "Failed to load registry\n");
        return 1;
    }

    Mount *m = find_mount(&reg, local);
    if (!m) {
        fprintf(stderr, "%s is not a mounted path\n", local);
        return 1;
    }

    // Save mount info before removing
    char saved_local[MAX_PATH_LEN];
    char saved_remote[MAX_PATH_LEN];
    strncpy(saved_local, m->local_path, sizeof(saved_local));
    strncpy(saved_remote, m->remote_spec, sizeof(saved_remote));

    // Check for unsync'd changes (dry run) only if not keeping
    if (!keep_local) {
        printf("Checking for unsync'd changes...\n");
        if (rsync_bidirectional(m->local_path, m->remote_spec, 1) != 0) {
            fprintf(stderr, "\nWarning: Cannot determine sync status\n");
            fprintf(stderr, "Continue with unmount anyway? [y/N] ");

            char resp[16];
            if (!fgets(resp, sizeof(resp), stdin) || (resp[0] != 'y' && resp[0] != 'Y')) {
                printf("Unmount cancelled\n");
                return 1;
            }
        } else {
            printf("No changes detected\n\n");
        }
    }

    // Remove from registry
    if (remove_mount(&reg, local) != 0) {
        fprintf(stderr, "Failed to remove from registry\n");
        return 1;
    }

    if (save_registry(&reg) != 0) {
        fprintf(stderr, "Warning: Failed to save registry\n");
    }

    printf("✓ Unmounted %s\n", saved_local);

    if (keep_local) {
        printf("  Local files kept at: %s\n", saved_local);
    } else {
        // Delete local directory
        printf("  Deleting local copy...\n");

        char cmd[MAX_PATH_LEN * 2];
        char quoted[MAX_PATH_LEN * 2];
        shell_quote(quoted, sizeof(quoted), saved_local);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", quoted);

        if (system(cmd) == 0) {
            printf("  ✓ Deleted %s\n", saved_local);
        } else {
            fprintf(stderr, "  Warning: Failed to delete local files\n");
            fprintf(stderr, "  You may need to manually remove: %s\n", saved_local);
        }
    }

    return 0;
}

static int cmd_status(void) {
    MountRegistry reg = {0};

    if (load_registry(&reg) != 0) {
        fprintf(stderr, "Failed to load registry\n");
        return 1;
    }

    if (reg.count == 0) {
        printf("No active mounts\n");
        printf("\nMount a remote directory with:\n");
        printf("  rmt mount user@host:/path ~/local/path\n");
        return 0;
    }

    printf("Active mounts:\n\n");

    time_t now = time(NULL);
    for (int i = 0; i < reg.count; i++) {
        Mount *m = &reg.mounts[i];

        int hours_since_sync = (int)((now - m->last_sync) / 3600);
        int days_since_sync = hours_since_sync / 24;

        printf("  [%d] %s\n", i + 1, m->local_path);
        printf("      Remote: %s\n", m->remote_spec);

        if (days_since_sync > 0) {
            printf("      Last sync: %d day%s ago\n",
                   days_since_sync, days_since_sync == 1 ? "" : "s");
        } else if (hours_since_sync > 0) {
            printf("      Last sync: %d hour%s ago\n",
                   hours_since_sync, hours_since_sync == 1 ? "" : "s");
        } else {
            printf("      Last sync: <1 hour ago\n");
        }

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
    printf("  sync     Sync changes (bidirectional by default)\n");
    printf("  unmount  Unmount and remove from registry\n");
    printf("  status   Show all active mounts\n");
    printf("  reset    Clear the registry (fixes corrupted state)\n");
    printf("\n");
    printf("Sync options:\n");
    printf("  --dry-run  Show what would be synced without doing it\n");
    printf("  --pull     Only pull changes from remote (one-way)\n");
    printf("  --push     Only push changes to remote (one-way)\n");
    printf("\n");
    printf("Unmount options:\n");
    printf("  --keep     Keep local files (default: delete)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s mount dev-server:/var/www ~/work/site\n", prog);
    printf("  %s sync ~/work/site\n", prog);
    printf("  %s sync --dry-run\n", prog);
    printf("  %s unmount ~/work/site              # Deletes local copy\n", prog);
    printf("  %s unmount ~/work/site --keep       # Keeps local copy\n", prog);
    printf("\n");
    printf("After mounting, use your tools normally:\n");
    printf("  goto ~/work/site\n");
    printf("  vic ~/work/site/index.html\n");
    printf("  peek ~/work/site/logs/error.log\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

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
        int dry_run = 0;
        int pull_only = 0;
        int push_only = 0;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--dry-run") == 0) {
                dry_run = 1;
            } else if (strcmp(argv[i], "--pull") == 0) {
                pull_only = 1;
            } else if (strcmp(argv[i], "--push") == 0) {
                push_only = 1;
            } else if (argv[i][0] != '-') {
                path = argv[i];
            }
        }

        if (pull_only && push_only) {
            fprintf(stderr, "Cannot use both --pull and --push\n");
            return 1;
        }

        return cmd_sync(path, dry_run, pull_only, push_only);
    }

    if (strcmp(cmd, "unmount") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s unmount <local-path> [--keep]\n", argv[0]);
            return 1;
        }

        int keep = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--keep") == 0) {
                keep = 1;
            }
        }

        return cmd_unmount(argv[2], keep);
    }

    if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    }

    if (strcmp(cmd, "reset") == 0) {
        const char *registry = get_registry_path();
        printf("This will delete the registry at: %s\n", registry);
        printf("All mount information will be lost (local files will remain).\n");
        printf("Continue? [y/N] ");

        char resp[16];
        if (!fgets(resp, sizeof(resp), stdin) || (resp[0] != 'y' && resp[0] != 'Y')) {
            printf("Reset cancelled\n");
            return 0;
        }

        if (unlink(registry) == 0) {
            printf("✓ Registry reset\n");
            return 0;
        } else if (errno == ENOENT) {
            printf("Registry already empty\n");
            return 0;
        } else {
            fprintf(stderr, "Failed to reset: %s\n", strerror(errno));
            return 1;
        }
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    fprintf(stderr, "Try '%s --help' for usage\n", argv[0]);
    return 1;
}