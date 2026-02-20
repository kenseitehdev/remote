1. `shell_quote` has an off-by-one — the comment says it changed from 6 to 5 but the escape sequence `'\"'\"'` is actually 6 chars, so long paths with single quotes could get truncated silently.

2. `cmd_unmount` calls `find_mount` to get `m`, then calls `remove_mount` which does its own path resolution separately. If `realpath` behavior differs between the two calls (e.g. the directory was deleted between calls), they could resolve differently and `remove_mount` returns -1 while the registry is left intact.

3. `rsync_bidirectional` during unmount is called with `dry_run=1` to check for unsynced changes, but a dry-run pull output isn't actually meaningful for detecting conflicts — rsync dry-run just shows what *would* happen, it doesn't tell you if local has unpushed changes.

4. The registry is a raw binary `fwrite` of structs — not portable across machines and will silently corrupt if `Mount` struct layout ever changes (e.g. you add a field).

5. `MAX_PATH_LEN * 2` for `qremote`/`qlocal` buffers isn't actually enough if the path has many single quotes, since each `'` expands to 6 chars.

6. `system()` is used throughout — if `$PATH` or `$SHELL` is weird on the user's machine, rsync might not be found even if installed.

7. improve sync to merge