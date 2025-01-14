#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sqlite3.h>
#include <unistd.h>
#include <utility>
#include <sys/stat.h>
#include <gromox/ext_buffer.hpp>
#include <gromox/mapidefs.h>
#include <gromox/pcl.hpp>
#include <gromox/proptags.hpp>
#include <gromox/rop_util.hpp>
#include "mkshared.hpp"

void adjust_rights(int fd)
{
	uid_t uid = -1;
	gid_t gid = -1;
	unsigned int mode = S_IRUSR | S_IWUSR;
	auto sp = getpwnam("gromox");
	if (sp != nullptr)
		uid = sp->pw_uid;
	auto gr = getgrnam("gromox");
	if (gr != nullptr) {
		gid = gr->gr_gid;
		mode |= S_IRGRP | S_IWGRP;
	}
	if (fchown(fd, uid, gid) < 0)
		perror("fchown");
	if (fchmod(fd, mode) < 0)
		perror("fchmod");
}

void adjust_rights(const char *file)
{
	int fd = open(file, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s O_RDWR: %s\n", file, strerror(errno));
		return;
	}
	adjust_rights(fd);
	close(fd);
}

bool add_folderprop_iv(sqlite3_stmt *stmt, uint32_t art_num, bool add_next)
{
	const std::pair<uint32_t, uint32_t> tagvals[] = {
		{PR_DELETED_COUNT_TOTAL, 0},
		{PR_DELETED_FOLDER_COUNT, 0},
		{PR_HIERARCHY_CHANGE_NUM, 0},
		{PR_INTERNET_ARTICLE_NUMBER, art_num},
	};
	for (const auto &v : tagvals) {
		sqlite3_bind_int64(stmt, 1, v.first);
		sqlite3_bind_int64(stmt, 2, v.second);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			return false;
		sqlite3_reset(stmt);
	}
	if (add_next) {
		sqlite3_bind_int64(stmt, 1, PROP_TAG_ARTICLENUMBERNEXT);
		sqlite3_bind_int64(stmt, 2, 1);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			return false;
		sqlite3_reset(stmt);
	}
	return true;
}

bool add_folderprop_sv(sqlite3_stmt *stmt, const char *dispname,
    const char *contcls)
{
	const std::pair<uint32_t, const char *> tagvals[] =
		{{PR_DISPLAY_NAME, dispname}, {PR_COMMENT, ""}};
	for (const auto &v : tagvals) {
		sqlite3_bind_int64(stmt, 1, v.first);
		sqlite3_bind_text(stmt, 2, v.second, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			return false;
		sqlite3_reset(stmt);
	}
	if (contcls != nullptr) {
		sqlite3_bind_int64(stmt, 1, PR_CONTAINER_CLASS);
		sqlite3_bind_text(stmt, 2, contcls, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			return false;
		sqlite3_reset(stmt);
	}
	return true;
}

bool add_folderprop_tv(sqlite3_stmt *stmt)
{
	static constexpr uint32_t tags[] = {
		PR_CREATION_TIME, PR_LAST_MODIFICATION_TIME, PROP_TAG_HIERREV,
		PR_LOCAL_COMMIT_TIME_MAX,
	};
	uint64_t nt_time = rop_util_unix_to_nttime(time(nullptr));
	for (const auto proptag : tags) {
		sqlite3_bind_int64(stmt, 1, proptag);
		sqlite3_bind_int64(stmt, 2, nt_time);
		if (sqlite3_step(stmt) != SQLITE_DONE)
			return false;
		sqlite3_reset(stmt);
	}
	return true;
}

bool add_changenum(sqlite3_stmt *stmt, enum cnguid_type cng, uint64_t user_id,
    uint64_t change_num)
{
	XID xid{cng == CN_DOMAIN ? rop_util_make_domain_guid(user_id) :
	        rop_util_make_user_guid(user_id), change_num};
	uint8_t tmp_buff[24];
	EXT_PUSH ext_push;
	if (!ext_push.init(tmp_buff, sizeof(tmp_buff), 0) ||
	    ext_push.p_xid(xid) != EXT_ERR_SUCCESS)
		return false;
	sqlite3_bind_int64(stmt, 1, PR_CHANGE_KEY);
	sqlite3_bind_blob(stmt, 2, ext_push.m_udata, ext_push.m_offset, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		return false;
	sqlite3_reset(stmt);
	PCL ppcl;
	if (!ppcl.append(xid))
		return false;
	auto pbin = ppcl.serialize();
	if (pbin == nullptr) {
		return false;
	}
	ppcl.clear();
	sqlite3_bind_int64(stmt, 1, PR_PREDECESSOR_CHANGE_LIST);
	sqlite3_bind_blob(stmt, 2, pbin->pb, pbin->cb, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		rop_util_free_binary(pbin);
		return false;
	}
	rop_util_free_binary(pbin);
	sqlite3_reset(stmt);
	return true;
}
