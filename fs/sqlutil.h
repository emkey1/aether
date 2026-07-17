#ifndef FS_SQLUTIL_H
#define FS_SQLUTIL_H
#include <sqlite3.h>

// Some nice sqlite macros for anything outside of fs/fake.c

#define Q(...) #__VA_ARGS__

#define HANDLE_ERR(db) \
    die("sqlite error while rebuilding: %s\n", sqlite3_errmsg(db))

#define CHECK_ERR() \
    if (err != SQLITE_OK && err != SQLITE_ROW && err != SQLITE_DONE) \
        HANDLE_ERR(db)
#define EXEC(sql) \
    err = sqlite3_exec(db, sql, NULL, NULL, NULL); \
    CHECK_ERR();
#define PREPARE(sql) ({ \
    sqlite3_stmt *stmt; \
    err = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL); \
    CHECK_ERR(); \
    stmt; \
})
#define STEP(stmt) ({ \
    err = sqlite3_step(stmt); \
    CHECK_ERR(); \
    err == SQLITE_ROW; \
})
#define RESET(stmt) \
    err = sqlite3_reset(stmt); \
    CHECK_ERR()
#define STEP_RESET(stmt) \
    STEP(stmt); \
    RESET(stmt)
#define FINALIZE(stmt) \
    err = sqlite3_finalize(stmt); \
    CHECK_ERR()

// Non-fatal counterparts, for code that runs at mount(2) time against a
// database that may be foreign, corrupt, or otherwise not what this project's
// own tooling produced (fs/fake-migrate.c, fs/fake-rebuild.c): any guest
// process with root can trigger this path with an arbitrary `mount -t fake
// <source> <target>`, so a sqlite error here must fail the mount cleanly
// (returning an errno-style value, e.g. _EIO) instead of die()ing and
// aborting the whole host process out from under every other running guest
// task. Callers must be functions that return an int error code.
#define HANDLE_ERR_RET(db) do { \
    printk("ERROR: sqlite error: %s\n", sqlite3_errmsg(db)); \
    return _EIO; \
} while (0)
#define CHECK_ERR_RET() \
    if (err != SQLITE_OK && err != SQLITE_ROW && err != SQLITE_DONE) \
        HANDLE_ERR_RET(db)
#define EXEC_RET(sql) \
    err = sqlite3_exec(db, sql, NULL, NULL, NULL); \
    CHECK_ERR_RET();
#define PREPARE_RET(sql) ({ \
    sqlite3_stmt *stmt; \
    err = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL); \
    CHECK_ERR_RET(); \
    stmt; \
})
#define STEP_RET(stmt) ({ \
    err = sqlite3_step(stmt); \
    CHECK_ERR_RET(); \
    err == SQLITE_ROW; \
})
#define RESET_RET(stmt) \
    err = sqlite3_reset(stmt); \
    CHECK_ERR_RET()
#define STEP_RESET_RET(stmt) \
    STEP_RET(stmt); \
    RESET_RET(stmt)
#define FINALIZE_RET(stmt) \
    err = sqlite3_finalize(stmt); \
    CHECK_ERR_RET()

#endif
