#pragma once
#include <map>
#include <string>
#include <vector>
#include <gromox/common_types.hpp>
#include <gromox/mapidefs.h>
#include "../authmgr.hpp"

enum {
	/* Reason codes (users.address_status) for forbidden login */
	AF_USER_NORMAL      = 0x00,
	AF_USER_SUSPENDED   = 0x01,
	AF_USER_OUTOFDATE   = 0x02,
	AF_USER_DELETED     = 0x03,
	AF_USER_SHAREDMBOX  = 0x04,
	AF_USER__MASK       = 0x0F,

	// historically: groups with AF_GROUP__MASK = 0xC0, with statuses NORMAL..DELETED
	AF_DOMAIN_NORMAL    = 0x00,
	AF_DOMAIN_SUSPENDED = 0x10,
	AF_DOMAIN_OUTOFDATE = 0x20,
	AF_DOMAIN_DELETED   = 0x30,
	AF_DOMAIN__MASK     = 0x30,

	/* note: users.address_status is a tinyint(4), so only 7 "usable" bits */
};

enum mlist_type {
	MLIST_TYPE_NORMAL = 0,
	MLIST_TYPE_GROUP,
	MLIST_TYPE_DOMAIN,
	MLIST_TYPE_CLASS,
};

enum sql_schema_upgrade {
	S_ABORT, S_SKIP, S_AUTOUP,
};

struct mysql_adaptor_init_param {
	std::string host, user, pass, dbname;
	int port = 0, conn_num = 0, timeout = 0;
	enum sql_schema_upgrade schema_upgrade = S_ABORT;
	bool enable_firsttimepw = false;
};

struct sql_domain {
	std::string name, title, address;
};

/**
 * @dtex:	%DT_* type as specified for PR_DISPLAY_TYPE_EX.
 * @list_type:	%MLIST_TYPE_* value; only interpret field when
 * 		addr_type==ADDRESS_TYPE_MLIST.
 */
struct sql_user {
	enum display_type dtypx = DT_MAILUSER;
	int id = 0;
	enum mlist_type list_type = MLIST_TYPE_NORMAL;
	int list_priv = 0;
	std::string username, maildir;
	std::vector<std::string> aliases; /* email addresses */
	std::map<unsigned int, std::string> propvals;
};

struct sql_group {
	int id;
	std::string name, title;
};

struct sql_class {
	int child_id;
	std::string name;
};

extern void mysql_adaptor_init(mysql_adaptor_init_param &&);
extern int mysql_adaptor_run();
extern void mysql_adaptor_stop();
extern BOOL mysql_adaptor_meta(const char *username, const char *password, char *maildir, char *lang, char *reason, int length, unsigned int mode, char *encrypted_passwd, size_t enc_size, uint8_t *externid_present);
extern BOOL mysql_adaptor_login2(const char *username, const char *password, char *encrypt_passwd, size_t enc_size, char *reason, int length);
BOOL mysql_adaptor_setpasswd(const char *username,
	const char *password, const char *new_password);
extern BOOL mysql_adaptor_get_username_from_id(int user_id, char *username, size_t);
BOOL mysql_adaptor_get_id_from_username(const char *username, int *puser_id);
BOOL mysql_adaptor_get_id_from_maildir(const char *maildir, int *puser_id);
extern bool mysql_adaptor_get_user_displayname(const char *username, char *dispname, size_t);
BOOL mysql_adaptor_get_user_privilege_bits(
	const char *username, uint32_t *pprivilege_bits);
extern bool mysql_adaptor_get_user_lang(const char *username, char *lang, size_t);
BOOL mysql_adaptor_set_user_lang(const char *username, const char *lang);
extern bool mysql_adaptor_get_timezone(const char *username, char *timezone, size_t);
BOOL mysql_adaptor_set_timezone(const char *username, const char *timezone);
extern bool mysql_adaptor_get_maildir(const char *username, char *maildir, size_t);
extern bool mysql_adaptor_get_homedir(const char *domainname, char *homedir, size_t);
extern bool mysql_adaptor_get_homedir_by_id(int domain_id, char *homedir, size_t);
BOOL mysql_adaptor_get_id_from_homedir(const char *homedir, int *pdomain_id);
extern BOOL mysql_adaptor_get_user_ids(const char *username, int *user_id, int *domain_id, enum display_type *);
BOOL mysql_adaptor_get_domain_ids(const char *domainname,
	int *pdomain_id, int *porg_id);
BOOL mysql_adaptor_get_mlist_ids(int user_id,
	int *pgroup_id, int *pdomain_id);
extern BOOL mysql_adaptor_get_org_domains(int org_id, std::vector<int> &);
extern BOOL mysql_adaptor_get_domain_info(int domain_id, sql_domain &);
BOOL mysql_adaptor_check_same_org(int domain_id1, int domain_id2);
extern BOOL mysql_adaptor_get_domain_groups(int domain_id, std::vector<sql_group> &);
extern BOOL mysql_adaptor_get_group_classes(int group_id, std::vector<sql_class> &);
extern BOOL mysql_adaptor_get_sub_classes(int class_id, std::vector<sql_class> &);
extern int mysql_adaptor_get_class_users(int class_id, std::vector<sql_user> &);
extern int mysql_adaptor_get_group_users(int group_id, std::vector<sql_user> &);
extern int mysql_adaptor_get_domain_users(int domain_id, std::vector<sql_user> &);
BOOL mysql_adaptor_check_mlist_include(
	const char *mlist_name, const char *account);
extern BOOL mysql_adaptor_check_same_org2(const char *domainname1, const char *domainname2);
extern bool mysql_adaptor_check_user(const char *username, char *path, size_t);
extern BOOL mysql_adaptor_get_mlist(const char *username, const char *from, int *presult, std::vector<std::string> &);
extern bool mysql_adaptor_get_user_info(const char *username, char *maildir, size_t msize, char *lang, size_t lsize, char *timezone, size_t tsize);
