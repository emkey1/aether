#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/fs.h"
#include "debug.h"
#include "kernel/errno.h"
#include "fs/fake-db.h"
#include "fs/fake-path.h"
#include "fs/sqlutil.h"

// The value of the user_version pragma is used to decide what needs migrating.

// version 4: rename host files to the escaped on-disk form (fs/fake-path.h),
// so guest names differing only in ASCII case stop colliding on
// case-insensitive host filesystems (APFS).
//
// Walked in path order, so a directory is renamed before anything inside it;
// each entry's current host location is therefore escape(dirname) plus its
// own still-unescaped name. Best-effort by design: a root that already
// suffered case collisions has host state that can't be fully recovered (two
// DB paths sharing one host file), and a half-finished previous run leaves
// entries already in escaped form -- both are detected and skipped, so the
// migration is safe to re-run.

// Find the on-disk name (into name_out) of the directory entry matching name
// case-insensitively (ASCII). Returns true if found.
static bool find_ondisk_name(int root_fd, const char *host_dir, const char *name, char *name_out, size_t name_out_size) {
    int dirfd = openat(root_fd, host_dir[0] == '\0' ? "." : host_dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        return false;
    DIR *dir = fdopendir(dirfd);
    if (dir == NULL) {
        close(dirfd);
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcasecmp(ent->d_name, name) == 0) {
            size_t len = strlen(ent->d_name);
            if (len < name_out_size) {
                memcpy(name_out, ent->d_name, len + 1);
                found = true;
            }
            break;
        }
    }
    closedir(dir);
    return found;
}

static void migrate_escape_host_names(struct fakefs_db *fs, int root_fd) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(fs->db, "select path from paths order by path", -1, &stmt, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *path_blob = sqlite3_column_blob(stmt, 0);
        int path_len = sqlite3_column_bytes(stmt, 0);
        if (path_blob == NULL || path_len <= 0 || path_len > MAX_PATH)
            continue;
        char path[MAX_PATH + 1];
        memcpy(path, path_blob, path_len);
        path[path_len] = '\0';
        if (!fake_path_needs_escape(path))
            continue;

        // DB paths look like "/usr/share/Foo" ("" for the root). Split into
        // dirname (already migrated, so escaped on the host) and basename.
        char *slash = strrchr(path, '/');
        if (slash == NULL)
            continue; // not a real path ("" root can't need escaping anyway)
        *slash = '\0';
        const char *base = slash + 1;
        char host_dir[MAX_PATH + 1];
        if (fake_path_to_host(path[0] == '\0' ? "" : path + 1, host_dir, sizeof(host_dir)) == NULL)
            continue;

        char host_base[NAME_MAX * 2 + 2];
        if (fake_path_to_host(base, host_base, sizeof(host_base)) == NULL)
            continue;
        if (strcmp(base, host_base) == 0)
            continue; // only the dirname needed escaping

        char host_src[MAX_PATH + 1], host_dst[MAX_PATH + 1];
        if (snprintf(host_src, sizeof(host_src), "%s%s%s", host_dir, host_dir[0] != '\0' ? "/" : "", base) >= (int) sizeof(host_src))
            continue;
        if (snprintf(host_dst, sizeof(host_dst), "%s%s%s", host_dir, host_dir[0] != '\0' ? "/" : "", host_base) >= (int) sizeof(host_dst))
            continue;

        // A case-insensitive host resolves host_src to *some* entry -- maybe
        // this path's own file, maybe a case-twin's. Find the true on-disk
        // name to tell those apart (and to detect an already-migrated entry
        // after an interrupted run).
        char ondisk[NAME_MAX + 1];
        if (!find_ondisk_name(root_fd, host_dir, base, ondisk, sizeof(ondisk))) {
            struct stat st;
            if (fstatat(root_fd, host_dst, &st, AT_SYMLINK_NOFOLLOW) == 0)
                continue; // already migrated (resumed run)
            printk("fakefs migrate: %s: missing on host, skipped\n", host_src);
            continue;
        }
        if (strcmp(ondisk, host_base) == 0)
            continue; // already migrated (resumed run)

        if (strcmp(ondisk, base) == 0) {
            // The normal case: the file is really ours; move it.
            if (renameat(root_fd, host_src, root_fd, host_dst) < 0)
                printk("fakefs migrate: rename %s -> %s failed: %d\n", host_src, host_dst, errno);
            continue;
        }

        // The on-disk name differs in case: this DB path collided with a
        // case-twin when it was created and never got its own host file. The
        // twin keeps the shared file; give this path its own entry so it at
        // least still resolves (the shared content was already merged and
        // can't be un-merged).
        struct stat st;
        if (fstatat(root_fd, host_src, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            printk("fakefs migrate: %s: missing on host, skipped\n", host_src);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mkdirat(root_fd, host_dst, 0777) < 0 && errno != EEXIST)
                printk("fakefs migrate: mkdir %s failed: %d\n", host_dst, errno);
            printk("fakefs migrate: %s collided with %s/%s; contents stay with the latter\n",
                    host_dst, host_dir, ondisk);
        } else {
            if (linkat(root_fd, host_src, root_fd, host_dst, 0) < 0 && errno != EEXIST)
                printk("fakefs migrate: link %s -> %s failed: %d\n", host_src, host_dst, errno);
        }
    }
    sqlite3_finalize(stmt);
}

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct migration {
    const char *sql;
    void (*migrate)(struct fakefs_db *fs, int root_fd);
} migrations[] = {
    // version 1: add another index
    {
        "create index inode_to_path on paths (inode, path);"
    },
    // version 2: add foreign key constraint on paths, create trigger to automatically cleanup stats
    {
        "create table paths_new (path blob primary key, inode integer references stats(inode));"
        "insert into paths_new select * from paths where exists (select 1 from stats where inode = paths.inode);"
        "drop table paths; alter table paths_new rename to paths;"
        "create index inode_to_path on paths (inode, path);"
        "delete from stats where not exists (select 1 from paths where inode = stats.inode);"
        "create trigger delete_path after delete on paths "
        "when not exists (select 1 from paths where inode = old.inode) "
        "begin "
            "delete from stats where not exists (select 1 from paths where inode = old.inode) and inode = old.inode; "
        "end;"
    },
    // version 3: the trigger was a mistake
    {
        "drop trigger delete_path"
    },
    // version 4: escape host file names (see comment on
    // migrate_escape_host_names above)
    {
        NULL, migrate_escape_host_names
    },
};

int fakefs_migrate(struct fakefs_db *fs, int root_fd) {
    sqlite3 *db = fs->db;
    int err;
    sqlite3_stmt *user_version = PREPARE_RET("pragma user_version");
    STEP_RET(user_version);
    int version = sqlite3_column_int(user_version, 0);
    FINALIZE_RET(user_version);

    EXEC_RET("begin");
    int versions = sizeof(migrations)/sizeof(migrations[0]);
    while (version < versions) {
        struct migration m = migrations[version];
        if (m.sql != NULL)
            EXEC_RET(m.sql);
        if (m.migrate != NULL)
            m.migrate(fs, root_fd);
        version++;
    }
    // for some reason placeholders aren't allowed in pragmas
    char *pragma_user_version = sqlite3_mprintf("pragma user_version = %d", version);
    EXEC_RET(pragma_user_version);
    sqlite3_free(pragma_user_version);
    EXEC_RET("commit");

    return 0;
}
