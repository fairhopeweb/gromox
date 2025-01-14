#pragma once
#include <cstdint>
#include <sqlite3.h>
enum cnguid_type { CN_USER, CN_DOMAIN };
extern void adjust_rights(int fd);
extern void adjust_rights(const char *file);
extern bool add_folderprop_iv(sqlite3_stmt *, uint32_t art_num, bool add_next);
extern bool add_folderprop_sv(sqlite3_stmt *, const char *dispname, const char *contcls);
extern bool add_folderprop_tv(sqlite3_stmt *);
extern bool add_changenum(sqlite3_stmt *, enum cnguid_type, uint64_t user_id, uint64_t change_num);
