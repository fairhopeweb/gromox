// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/dsn.hpp>
#include <gromox/rtf.hpp>
#include <gromox/html.hpp>
#include <gromox/guid.hpp>
#include <gromox/util.hpp>
#include <gromox/tnef.hpp>
#include <gromox/rtfcp.hpp>
#include <gromox/oxvcard.hpp>
#include <gromox/oxcical.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <gromox/mail_func.hpp>
#include <gromox/tarray_set.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/apple_util.hpp>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

/* uncomment below macro if you need system to verify X-MS-TNEF-Correlator */
/* #define VERIFY_TNEF_CORRELATOR */

#define MAXIMUM_SEARCHING_DEPTH					10

/*
	Caution. If any errors in parsing any sub type, ignore this sub type.
	for example, if an error appears when parsing tnef attachment, treat
	this tnef sub type as normal attachment. there will be no error for
	parsing email object into message object!
*/

using namespace gromox;
using namemap = std::unordered_map<int, PROPERTY_NAME>;

namespace {

struct FIELD_ENUM_PARAM {
	FIELD_ENUM_PARAM(namemap &r) : phash(r) {}
	FIELD_ENUM_PARAM(FIELD_ENUM_PARAM &&) = delete;
	void operator=(FIELD_ENUM_PARAM &&) = delete;

	EXT_BUFFER_ALLOC alloc{};
	MESSAGE_CONTENT *pmsg = nullptr;
	namemap &phash;
	uint16_t last_propid = 0;
	const char *charset = nullptr;
	BOOL b_classified = false, b_flag_del = false;
	MAIL *pmail = nullptr;
};

struct MIME_ENUM_PARAM {
	MIME_ENUM_PARAM(namemap &r) : phash(r) {}
	MIME_ENUM_PARAM(MIME_ENUM_PARAM &&) = delete;
	void operator=(MIME_ENUM_PARAM &&) = delete;

	BOOL b_result = false;
	int attach_id = 0;
	const char *charset = nullptr, *str_zone = nullptr;
	GET_PROPIDS get_propids{};
	EXT_BUFFER_ALLOC alloc{};
	std::shared_ptr<MIME_POOL> pmime_pool;
	MESSAGE_CONTENT *pmsg = nullptr;
	namemap phash;
	uint16_t last_propid = 0;
	uint64_t nttime_stamp = 0;
	MIME *pplain = nullptr, *phtml = nullptr, *penriched = nullptr;
	MIME *pcalendar = nullptr, *preport = nullptr;
};

struct DSN_ENUM_INFO {
	int action_severity;
	TARRAY_SET *prcpts;
	uint64_t submit_time;
};

struct DSN_FILEDS_INFO {
	char final_recipient[UADDR_SIZE];
	int action_severity;
	char remote_mta[128];
	const char *status;
	const char *diagnostic_code;
	const char *x_supplementary_info;
	const char *x_display_name;
};

}

enum {
	MAIL_TYPE_NORMAL,
	MAIL_TYPE_SIGNED,
	MAIL_TYPE_ENCRYPTED,
	MAIL_TYPE_DSN,
	MAIL_TYPE_MDN,
	MAIL_TYPE_CALENDAR,
	MAIL_TYPE_TNEF
};

namespace {
struct MIME_SKELETON {
	int mail_type;
	int body_type;
	char *pplain;
	BINARY *phtml;
	BOOL b_inline;
	BINARY rtf_bin;
	BOOL b_attachment;
	const char *charset;
	const char *pmessage_class;
	ATTACHMENT_LIST *pattachments;
};
}

static constexpr char
	PidNameAttachmentMacContentType[] = "AttachmentMacContentType",
	PidNameAttachmentMacInfo[] = "AttachmentMacInfo",
	PidNameContentClass[] = "Content-Class",
	PidNameKeywords[] = "Keywords";
static constexpr size_t namemap_limit = 0x1000;
static char g_oxcmail_org_name[256];
static GET_USER_IDS oxcmail_get_user_ids;
static GET_USERNAME oxcmail_get_username;
static LTAG_TO_LCID oxcmail_ltag_to_lcid;
static LCID_TO_LTAG oxcmail_lcid_to_ltag;
static CHARSET_TO_CPID oxcmail_charset_to_cpid;
static CPID_TO_CHARSET oxcmail_cpid_to_charset;
static MIME_TO_EXTENSION oxcmail_mime_to_extension;
static EXTENSION_TO_MIME oxcmail_extension_to_mime;

static inline size_t worst_encoding_overhead(size_t in)
{
	/*
	 * (To be used for conversions _from UTF-8_ to any other encoding.)
	 * UTF-7 can be *so* pathalogical.
	 */
	return 5 * in;
}
	
static int namemap_add(namemap &phash, uint32_t id, PROPERTY_NAME &&el) try
{
	/* Avoid uninitialized read when the copy/transfer is made */
	if (el.kind == MNID_ID)
		el.pname = nullptr;
	else
		el.lid = 0;
	if (phash.size() >= namemap_limit)
		return -ENOSPC;
	if (!phash.emplace(id, std::move(el)).second)
		return -EEXIST;
	return 0;
} catch (const std::bad_alloc &) {
	return -ENOMEM;
}

BOOL oxcmail_init_library(const char *org_name,
	GET_USER_IDS get_user_ids, GET_USERNAME get_username,
	LTAG_TO_LCID ltag_to_lcid, LCID_TO_LTAG lcid_to_ltag,
	CHARSET_TO_CPID charset_to_cpid, CPID_TO_CHARSET
	cpid_to_charset, MIME_TO_EXTENSION mime_to_extension,
	EXTENSION_TO_MIME extension_to_mime)
{
	gx_strlcpy(g_oxcmail_org_name, org_name, arsizeof(g_oxcmail_org_name));
	oxcmail_get_user_ids = get_user_ids;
	oxcmail_get_username = get_username;
	oxcmail_ltag_to_lcid = ltag_to_lcid;
	oxcmail_lcid_to_ltag = lcid_to_ltag;
	oxcmail_charset_to_cpid = charset_to_cpid;
	oxcmail_cpid_to_charset = cpid_to_charset;
	oxcmail_mime_to_extension = mime_to_extension;
	oxcmail_extension_to_mime = extension_to_mime;
	tnef_init_library(cpid_to_charset);
	if (!rtf_init_library(cpid_to_charset) ||
		FALSE == html_init_library(cpid_to_charset)) {
		return FALSE;	
	}
	return TRUE;
}

static BOOL oxcmail_username_to_essdn(const char *username,
    char *pessdn, enum display_type *dtpp)
{
	int user_id;
	int domain_id;
	char *pdomain;
	char tmp_name[UADDR_SIZE];
	char hex_string[16];
	char hex_string2[16];
	
	gx_strlcpy(tmp_name, username, GX_ARRAY_SIZE(tmp_name));
	pdomain = strchr(tmp_name, '@');
	if (NULL == pdomain) {
		return FALSE;
	}
	*pdomain = '\0';
	pdomain ++;
	enum display_type dtypx = DT_MAILUSER;
	if (!oxcmail_get_user_ids(username, &user_id, &domain_id, &dtypx)) {
		return FALSE;
	}
	encode_hex_int(user_id, hex_string);
	encode_hex_int(domain_id, hex_string2);
	snprintf(pessdn, 1024, "/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
		g_oxcmail_org_name, hex_string2, hex_string, tmp_name);
	HX_strupper(pessdn);
	if (dtpp != nullptr)
		*dtpp = dtypx;
	return TRUE;
}

static BOOL oxcmail_essdn_to_username(const char *pessdn,
    char *username, size_t ulen)
{
	int user_id;
	char tmp_buff[1024];
	
	auto tmp_len = gx_snprintf(tmp_buff, GX_ARRAY_SIZE(tmp_buff),
	               "/o=%s/ou=Exchange Administrative"
		" Group (FYDIBOHF23SPDLT)/cn=Recipients/cn=", g_oxcmail_org_name);
	if (0 != strncasecmp(pessdn, tmp_buff, tmp_len)) {
		return FALSE;
	}
	user_id = decode_hex_int(pessdn + tmp_len + 8);
	return oxcmail_get_username(user_id, username, ulen);
}

static BOOL oxcmail_entryid_to_username(const BINARY *pbin,
    EXT_BUFFER_ALLOC alloc, char *username, size_t ulen)
{
	uint32_t flags;
	EXT_PULL ext_pull;
	FLATUID provider_uid;
	ONEOFF_ENTRYID oneoff_entry;
	ADDRESSBOOK_ENTRYID ab_entryid;
	
	if (pbin->cb < 20) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, 20, alloc, 0);
	if (ext_pull.g_uint32(&flags) != EXT_ERR_SUCCESS || flags != 0 ||
	    ext_pull.g_guid(&provider_uid) != EXT_ERR_SUCCESS)
		return FALSE;
	if (provider_uid == muidEMSAB) {
		ext_pull.init(pbin->pb, pbin->cb, alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_abk_eid(&ab_entryid) != EXT_ERR_SUCCESS)
			return FALSE;
		if (ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER != ab_entryid.type) {
			return FALSE;
		}
		return oxcmail_essdn_to_username(ab_entryid.px500dn, username, ulen);
	}
	if (provider_uid == muidOOP) {
		ext_pull.init(pbin->pb, pbin->cb, alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_oneoff_eid(&oneoff_entry) != EXT_ERR_SUCCESS)
			return FALSE;
		if (0 != strcasecmp(oneoff_entry.paddress_type, "SMTP")) {
			return FALSE;
		}
		gx_strlcpy(username, oneoff_entry.pmail_address, ulen);
		return TRUE;
	}
	return FALSE;
}

static BOOL oxcmail_username_to_oneoff(const char *username,
	const char *pdisplay_name, BINARY *pbin)
{
	int status;
	EXT_PUSH ext_push;
	ONEOFF_ENTRYID tmp_entry;
	
	tmp_entry.flags = 0;
	tmp_entry.provider_uid = muidOOP;
	tmp_entry.version = 0;
	tmp_entry.ctrl_flags = CTRL_FLAG_NORICH | CTRL_FLAG_UNICODE;
	if (NULL != pdisplay_name && '\0' != pdisplay_name[0]) {
		tmp_entry.pdisplay_name = deconst(pdisplay_name);
	} else {
		tmp_entry.pdisplay_name = deconst(username);
	}
	tmp_entry.paddress_type = deconst("SMTP");
	tmp_entry.pmail_address = deconst(username);
	if (!ext_push.init(pbin->pb, 1280, EXT_FLAG_UTF16))
		return false;
	status = ext_push.p_oneoff_eid(&tmp_entry);
	if (EXT_ERR_CHARCNV == status) {
		tmp_entry.ctrl_flags = CTRL_FLAG_NORICH;
		status = ext_push.p_oneoff_eid(&tmp_entry);
	}
	if (EXT_ERR_SUCCESS != status) {
		return FALSE;
	}
	pbin->cb = ext_push.m_offset;
	return TRUE;
}

static BOOL oxcmail_essdn_to_entryid(const char *pessdn, BINARY *pbin)
{
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	tmp_entryid.flags = 0;
	tmp_entryid.provider_uid = muidEMSAB;
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = deconst(pessdn);
	if (!ext_push.init(pbin->pb, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return false;
	pbin->cb = ext_push.m_offset;
	return TRUE;
}

static BOOL oxcmail_username_to_entryid(const char *username,
    const char *pdisplay_name, BINARY *pbin, enum display_type *dtpp)
{
	char x500dn[1024];

	if (oxcmail_username_to_essdn(username, x500dn, dtpp))
		return oxcmail_essdn_to_entryid(x500dn, pbin);
	if (dtpp != nullptr)
		*dtpp = DT_MAILUSER;
	return oxcmail_username_to_oneoff(
	       username, pdisplay_name, pbin);
}

static bool oxcmail_check_ascii(const char *pstring)
{
	int i, len;
	
	len = strlen(pstring);
	for (i=0; i<len; i++) {
		if (0 == isascii(pstring[i])) {
			return false;
		}
	}
	return true;
}

static unsigned int pick_strtype(const char *token)
{
	return oxcmail_check_ascii(token) ? PT_UNICODE : PT_STRING8;
}

static BOOL oxcmail_check_crlf(const char *pstring)
{
	int i, len;
	
	len = strlen(pstring);
	for (i=0; i<len; i++) {
		if ('\r' == pstring[i] || '\n' == pstring[i]) {
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL oxcmail_get_content_param(MIME *pmime,
	const char *tag, char *value, int length)
{
	int tmp_len;
	
	if (FALSE == mime_get_content_param(
		pmime, tag, value, length)) {
		return FALSE;
	}
	tmp_len = strlen(value);
	if (('"' == value[0] && '"' == value[tmp_len - 1]) ||
		('\'' == value[0] && '\'' == value[tmp_len - 1])) {
		value[tmp_len - 1] = '\0';
		memmove(value, value + 1, tmp_len - 1);
	}
	if ('\0' == value[0]) {
		return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_get_field_param(char *field,
	const char *tag, char *value, int length)
{
	char *pend;
	int tmp_len;
	char *pbegin;
	char *ptoken;
	
	ptoken = strchr(field, ';');
	if (NULL == ptoken) {
		return FALSE;
	}
	ptoken ++;
	pbegin = strcasestr(ptoken, tag);
	if (NULL == pbegin) {
		return FALSE;
	}
	pbegin += strlen(tag);
	if ('=' != *pbegin) {
		return FALSE;
	}
	pbegin ++;
	pend = strchr(pbegin, ';');
	if (NULL == pend) {
		tmp_len = strlen(pbegin);
	} else {
		tmp_len = pend - pbegin;
	}
	if (tmp_len >= length) {
		return FALSE;
	}
	memmove(value, pbegin, tmp_len);
	value[tmp_len] = '\0';
	HX_strrtrim(value);
	HX_strltrim(value);
	tmp_len = strlen(value);
	if (('"' == value[0] && '"' == value[tmp_len - 1]) ||
		('\'' == value[0] && '\'' == value[tmp_len - 1])) {
		value[tmp_len - 1] = '\0';
		memmove(value, value + 1, tmp_len - 1);
	}
	if ('\0' == value[0]) {
		return FALSE;
	}
	return TRUE;
}

static void oxcmail_split_filename(char *file_name, char *extension)
{
	int i;
	int tmp_len;
	char *ptoken;
	
	tmp_len = strlen(file_name);
	for (i=0; i<tmp_len; i++) {
		if ('"' == file_name[i] || '/' == file_name[i] ||
			':' == file_name[i] || '<' == file_name[i] ||
			'>' == file_name[i] || '|' == file_name[i] ||
			'\\' == file_name[i] || (file_name[i] >= 0 &&
			file_name[i] <= 0x1F)) {
			file_name[i] = '_';
		}
	}
	for (i=0; i<tmp_len; i++) {
		if ('.' == file_name[i]) {
			file_name[i] = ' ';
			continue;
		}
		break;
	}
	HX_strltrim(file_name);
	tmp_len = strlen(file_name);
	for (i=tmp_len-1; i>=0; i--) {
		if ('.' == file_name[i]) {
			file_name[i] = ' ';
			continue;
		}
		break;
	}
	HX_strrtrim(file_name);
	ptoken = strrchr(file_name, '.');
	if (NULL == ptoken || strlen(ptoken) >= 16) {
		extension[0] = '\0';
	} else {
		strcpy(extension, ptoken);
	}
}

static BOOL oxcmail_parse_recipient(const char *charset,
	EMAIL_ADDR *paddr, uint32_t rcpt_type, TARRAY_SET *pset)
{
	int tmp_len;
	BINARY tmp_bin;
	char essdn[1024];
	uint8_t tmp_byte;
	uint32_t tmp_int32;
	char username[UADDR_SIZE], display_name[UADDR_SIZE];
	char tmp_buff[1280];
	char utf8_field[512];
	TPROPVAL_ARRAY *pproplist;
	
	if ('\0' != paddr->display_name[0] ||
		('\0' != paddr->local_part[0] &&
		'\0' != paddr->domain[0])) {
		pproplist = tpropval_array_init();
		if (NULL == pproplist) {
			return FALSE;
		}
	} else {
		return TRUE;
	}
	if (!tarray_set_append_internal(pset, pproplist)) {
		tpropval_array_free(pproplist);
		return FALSE;
	}
	utf8_field[0] = '\0';
	if ('\0' != paddr->display_name[0]) {
		gx_strlcpy(display_name, paddr->display_name, GX_ARRAY_SIZE(display_name));
	} else {
		snprintf(display_name, GX_ARRAY_SIZE(display_name), "%s@%s",
			paddr->local_part, paddr->domain);
	}
	if (TRUE == mime_string_to_utf8(charset, display_name, utf8_field)) {
		tmp_len = strlen(utf8_field);
		if (tmp_len > 1 && '"' == utf8_field[0]
			&& '"' == utf8_field[tmp_len - 1]) {
			tmp_len --;
			utf8_field[tmp_len] = '\0';
			memmove(utf8_field, utf8_field + 1, tmp_len);
			tmp_len --;
		}
		if (tmp_len > 1 && '\'' == utf8_field[0]
			&& '\'' == utf8_field[tmp_len - 1]) {
			tmp_len --;
			utf8_field[tmp_len] = '\0';
			memmove(utf8_field, utf8_field + 1, tmp_len);
			tmp_len --;
		}
		const char *newval;
		if (0 == tmp_len) {
			snprintf(display_name, arsizeof(display_name), "%s@%s",
				paddr->local_part, paddr->domain);
			newval = display_name;
		} else {
			newval = utf8_field;
		}
		if (pproplist->set(PR_DISPLAY_NAME, newval) != 0 ||
		    pproplist->set(PR_TRANSMITABLE_DISPLAY_NAME, utf8_field) != 0)
			return FALSE;
	} else {
		if (pproplist->set(PR_DISPLAY_NAME_A, display_name) != 0 ||
		    pproplist->set(PR_TRANSMITABLE_DISPLAY_NAME_A, display_name) != 0)
			return FALSE;
	}
	if (paddr->local_part[0] != '\0' && paddr->domain[0] != '\0' &&
	    oxcmail_check_ascii(paddr->local_part) &&
	    oxcmail_check_ascii(paddr->domain)) {
		snprintf(username, GX_ARRAY_SIZE(username), "%s@%s", paddr->local_part, paddr->domain);
		auto dtypx = DT_MAILUSER;
		if (!oxcmail_username_to_essdn(username, essdn, &dtypx)) {
			essdn[0] = '\0';
			dtypx = DT_MAILUSER;
			tmp_bin.cb = snprintf(tmp_buff, arsizeof(tmp_buff), "SMTP:%s", username) + 1;
			HX_strupper(tmp_buff);
			if (pproplist->set(PR_ADDRTYPE, "SMTP") != 0 ||
			    pproplist->set(PR_EMAIL_ADDRESS, username) != 0)
				return FALSE;
		} else {
			tmp_bin.cb = snprintf(tmp_buff, arsizeof(tmp_buff), "EX:%s", essdn) + 1;
			if (pproplist->set(PR_ADDRTYPE, "EX") != 0 ||
			    pproplist->set(PR_EMAIL_ADDRESS, essdn) != 0)
				return FALSE;
		}
		tmp_bin.pc = tmp_buff;
		if (pproplist->set(PR_SMTP_ADDRESS, username) != 0 ||
		    pproplist->set(PR_SEARCH_KEY, &tmp_bin) != 0)
			return FALSE;
		tmp_bin.cb = 0;
		tmp_bin.pc = tmp_buff;
		if ('\0' == essdn[0]) {
			if (FALSE == oxcmail_username_to_oneoff(
				username, utf8_field, &tmp_bin)) {
				return FALSE;
			}
		} else {
			if (FALSE == oxcmail_essdn_to_entryid(essdn, &tmp_bin)) {
				return FALSE;
			}
		}
		if (pproplist->set(PR_ENTRYID, &tmp_bin) != 0 ||
		    pproplist->set(PR_RECIPIENT_ENTRYID, &tmp_bin) != 0 ||
		    pproplist->set(PR_RECORD_KEY, &tmp_bin) != 0)
			return FALSE;
		tmp_int32 = dtypx == DT_DISTLIST ? MAPI_DISTLIST : MAPI_MAILUSER;
		if (pproplist->set(PR_OBJECT_TYPE, &tmp_int32) != 0)
			return FALSE;
		tmp_int32 = static_cast<uint32_t>(dtypx);
		if (pproplist->set(PR_DISPLAY_TYPE, &tmp_int32) != 0)
			return FALSE;
		tmp_int32 = 1;
		if (pproplist->set(PROP_TAG_RECIPIENTFLAGS, &tmp_int32) != 0)
			return FALSE;
	}
	tmp_byte = 1;
	if (pproplist->set(PROP_TAG_RESPONSIBILITY, &tmp_byte) != 0)
		return FALSE;
	tmp_int32 = 1;
	if (pproplist->set(PROP_TAG_RECIPIENTFLAGS, &tmp_int32) != 0)
		return FALSE;
	return pproplist->set(PR_RECIPIENT_TYPE, &rcpt_type) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_addresses(const char *charset,
	char *field, uint32_t rcpt_type, TARRAY_SET *pset)
{
	int i, len;
	char *ptoken;
	BOOL b_quote;
	char *ptoken_prev;
	EMAIL_ADDR email_addr;
	char temp_address[1024];
	
	len = strlen(field);
	field[len] = ';';
	len ++;
	ptoken_prev = field;
	b_quote = FALSE;
	for (i=0; i<len; i++) {
		if ('"' == field[i]) {
			if (FALSE == b_quote) {
				b_quote = TRUE;
			} else {
				b_quote = FALSE;
			}
		}
		if (',' == field[i] || ';' == field[i]) {
			ptoken = field + i;
			if (ptoken - ptoken_prev >= 1024) {
				ptoken_prev = ptoken + 1;
				continue;
			}
			memcpy(temp_address, ptoken_prev, ptoken - ptoken_prev);
			temp_address[ptoken - ptoken_prev] = '\0';
			parse_mime_addr(&email_addr, temp_address);
			if ('\0' == email_addr.local_part[0] && TRUE == b_quote) {
				continue;
			}
			if (FALSE == oxcmail_parse_recipient(charset,
				&email_addr, rcpt_type, pset)) {
				return FALSE;
			}
			ptoken_prev = ptoken + 1;
			b_quote = FALSE;
		}
	}
	return TRUE;
}

static BOOL oxcmail_parse_address(const char *charset,
	EMAIL_ADDR *paddr, uint32_t proptag1, uint32_t proptag2,
	uint32_t proptag3, uint32_t proptag4, uint32_t proptag5,
	 uint32_t proptag6, TPROPVAL_ARRAY *pproplist)
{
	BINARY tmp_bin;
	char essdn[1024];
	char username[UADDR_SIZE];
	char tmp_buff[1280];
	char utf8_field[512];
	
	if ('\0' != paddr->display_name[0]) {
		if (TRUE == mime_string_to_utf8(charset,
			paddr->display_name, utf8_field)) {
			if (pproplist->set(proptag1, utf8_field) != 0)
				return false;
		} else {
			if (pproplist->set(CHANGE_PROP_TYPE(proptag1, PT_STRING8), paddr->display_name) != 0)
				return false;
		}
	} else if ('\0' != paddr->local_part[0] && '\0' != paddr->domain[0]) {
		snprintf(username, GX_ARRAY_SIZE(username), "%s@%s", paddr->local_part, paddr->domain);
		uint32_t tag = oxcmail_check_ascii(username) ? proptag1 :
		                  CHANGE_PROP_TYPE(proptag1, PT_STRING8);
		if (pproplist->set(tag, username) != 0)
			return FALSE;
	}
	if (paddr->local_part[0] != '\0' && paddr->domain[0] != '\0' &&
	    oxcmail_check_ascii(paddr->local_part) &&
	    oxcmail_check_ascii(paddr->domain)) {
		snprintf(username, GX_ARRAY_SIZE(username), "%s@%s", paddr->local_part, paddr->domain);
		if (pproplist->set(proptag2, "SMTP") != 0 ||
		    pproplist->set(proptag3, username) != 0 ||
		    pproplist->set(proptag4, username) != 0)
			return FALSE;
		if (FALSE == oxcmail_username_to_essdn(username, essdn, NULL)) {
			essdn[0] = '\0';
			tmp_bin.cb = snprintf(tmp_buff, arsizeof(tmp_buff), "SMTP:%s", username) + 1;
			HX_strupper(tmp_buff);
		} else {
			tmp_bin.cb = snprintf(tmp_buff, arsizeof(tmp_buff), "EX:%s", essdn) + 1;
		}
		tmp_bin.pc = tmp_buff;
		if (pproplist->set(proptag5, &tmp_bin) != 0)
			return FALSE;
		tmp_bin.cb = 0;
		tmp_bin.pc = tmp_buff;
		if ('\0' == essdn[0]) {
			if (FALSE == oxcmail_username_to_oneoff(
				username, utf8_field, &tmp_bin)) {
				return FALSE;
			}
		} else {
			if (FALSE == oxcmail_essdn_to_entryid(essdn, &tmp_bin)) {
				return FALSE;
			}
		}
		if (pproplist->set(proptag6, &tmp_bin) != 0)
			return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_parse_reply_to(const char *charset,
	char *field, TPROPVAL_ARRAY *pproplist)
{
	int i, len;
	int status;
	char *ptoken;
	BOOL b_quote;
	uint32_t count;
	BINARY tmp_bin;
	int str_offset;
	uint8_t pad_len;
	EXT_PUSH ext_push;
	char *ptoken_prev;
	char tmp_buff[UADDR_SIZE];
	char utf8_field[512];
	EMAIL_ADDR email_addr;
	char temp_address[1024];
	ONEOFF_ENTRYID tmp_entry;
	uint8_t bin_buff[256*1024];
	char str_buff[MIME_FIELD_LEN];
	static constexpr uint8_t pad_bytes[3]{};
	
	len = strlen(field);
	field[len] = ';';
	len ++;
	ptoken_prev = field;
	count = 0;
	if (!ext_push.init(bin_buff, sizeof(bin_buff), EXT_FLAG_UTF16))
		return false;
	if (ext_push.advance(sizeof(uint32_t)) != EXT_ERR_SUCCESS)
		return FALSE;
	uint32_t offset = ext_push.m_offset;
	if (ext_push.advance(sizeof(uint32_t)) != EXT_ERR_SUCCESS)
		return FALSE;
	str_offset = 0;
	tmp_entry.flags = 0;
	tmp_entry.provider_uid = muidOOP;
	tmp_entry.version = 0;
	tmp_entry.pdisplay_name = utf8_field;
	tmp_entry.paddress_type = deconst("SMTP");
	tmp_entry.pmail_address = tmp_buff;
	b_quote = FALSE;
	for (i=0; i<len; i++) {
		if ('"' == field[i]) {
			if (FALSE == b_quote) {
				b_quote = TRUE;
			} else {
				b_quote = FALSE;
			}
		}
		if (',' == field[i] || ';' == field[i]) {
			ptoken = field + i;
			if (ptoken - ptoken_prev >= 1024) {
				ptoken_prev = ptoken + 1;
				continue;
			}
			memcpy(temp_address, ptoken_prev, ptoken - ptoken_prev);
			temp_address[ptoken - ptoken_prev] = '\0';
			parse_mime_addr(&email_addr, temp_address);
			if ('\0' == email_addr.local_part[0] && TRUE == b_quote) {
				continue;
			}
			if ('\0' == email_addr.display_name[0] ||
				FALSE == mime_string_to_utf8(charset,
				email_addr.display_name, utf8_field)) {
				sprintf(utf8_field, "%s@%s",
					email_addr.local_part, email_addr.domain);
			}
			if (0 == str_offset) {
				str_offset = sprintf(str_buff, "%s", utf8_field);
			} else {
				str_offset += gx_snprintf(str_buff + str_offset,
					sizeof(str_buff) - str_offset, ";%s", utf8_field);
			}
			if (email_addr.local_part[0] != '\0' && email_addr.domain[0] != '\0' &&
			    oxcmail_check_ascii(email_addr.local_part) &&
			    oxcmail_check_ascii(email_addr.domain)) {
				uint32_t offset1 = ext_push.m_offset;
				if (ext_push.advance(sizeof(uint32_t)) != EXT_ERR_SUCCESS)
					return FALSE;
				snprintf(tmp_buff, arsizeof(tmp_buff), "%s@%s",
					email_addr.local_part, email_addr.domain);
				tmp_entry.ctrl_flags = CTRL_FLAG_NORICH | CTRL_FLAG_UNICODE;
				status = ext_push.p_oneoff_eid(&tmp_entry);
				if (EXT_ERR_CHARCNV == status) {
					ext_push.m_offset = offset1 + sizeof(uint32_t);
					tmp_entry.ctrl_flags = CTRL_FLAG_NORICH;
					status = ext_push.p_oneoff_eid(&tmp_entry);
				}
				if (EXT_ERR_SUCCESS != status) {
					return FALSE;
				}
				uint32_t offset2 = ext_push.m_offset;
				uint32_t bytes = offset2 - (offset1 + sizeof(uint32_t));
				ext_push.m_offset = offset1;
				if (ext_push.p_uint32(bytes) != EXT_ERR_SUCCESS)
					return FALSE;
				ext_push.m_offset = offset2;
				pad_len = ((bytes + 3) & ~3) - bytes;
				if (ext_push.p_bytes(pad_bytes, pad_len) != EXT_ERR_SUCCESS)
					return FALSE;
				count ++;
			}
			ptoken_prev = ptoken + 1;
			b_quote = FALSE;
		}
	}
	if (0 != count) {
		tmp_bin.cb = ext_push.m_offset;
		tmp_bin.pb = bin_buff;
		uint32_t bytes = ext_push.m_offset - (offset + sizeof(uint32_t));
		ext_push.m_offset = 0;
		if (ext_push.p_uint32(count) != EXT_ERR_SUCCESS)
			return FALSE;
		ext_push.m_offset = offset;
		if (ext_push.p_uint32(bytes) != EXT_ERR_SUCCESS)
			return FALSE;
		if (pproplist->set(PROP_TAG_REPLYRECIPIENTENTRIES, &tmp_bin) != 0)
			return FALSE;
	}
	if (str_offset > 0 &&
	    pproplist->set(PROP_TAG_REPLYRECIPIENTNAMES, str_buff) != 0)
		return FALSE;
	return TRUE;
}

static BOOL oxcmail_parse_subject(const char *charset,
	char *field, TPROPVAL_ARRAY *pproplist)
{
	int i;
	int tmp_len;
	char *ptoken;
	int subject_len;
	char tmp_buff1[4096];
	char prefix_buff[32];
	char tmp_buff[MIME_FIELD_LEN];
	char utf8_field[MIME_FIELD_LEN];
	static constexpr uint8_t seperator[] = {':', 0x00, ' ', 0x00};
	
	if (TRUE == mime_string_to_utf8(
		charset, field, utf8_field)) {
		subject_len = utf8_to_utf16le(utf8_field,
					tmp_buff, sizeof(tmp_buff));
		if (subject_len < 0) {
			utf8_truncate(utf8_field, 255);
			subject_len = utf8_to_utf16le(utf8_field,
					tmp_buff, sizeof(tmp_buff));
			if (subject_len < 0) {
				subject_len = 0;
			}
		}
		if (subject_len > 512) {
			subject_len = 512;
			tmp_buff[510] = '\0';
			tmp_buff[511] = '\0';
		}
		utf16le_to_utf8(tmp_buff, subject_len, tmp_buff1, sizeof(tmp_buff1));
		if (pproplist->set(PR_SUBJECT, tmp_buff1) != 0)
			return FALSE;
		ptoken = static_cast<char *>(memmem(tmp_buff, subject_len, seperator, 4));
		if (NULL == ptoken) {
			return TRUE;
		}
		tmp_len = ptoken - tmp_buff;
		if (tmp_len < 2 || tmp_len > 6) {
			return TRUE;
		}
		for (i=0; i<tmp_len; i+=2) {
			if ((':' == tmp_buff[i] ||
				' ' == tmp_buff[i] ||
			    HX_isdigit(tmp_buff[0])) &&
				'\0' == tmp_buff[i + 1]) {
				return TRUE;
			}
		}
		tmp_len += sizeof(seperator);
		memcpy(tmp_buff1, tmp_buff, tmp_len);
		tmp_buff1[tmp_len] = '\0';
		tmp_buff1[tmp_len + 1] = '\0';
		utf16le_to_utf8(tmp_buff1, tmp_len + 2,
			prefix_buff, sizeof(prefix_buff));
		if (pproplist->set(PR_SUBJECT_PREFIX, prefix_buff) != 0)
			return FALSE;
		utf16le_to_utf8(tmp_buff + tmp_len,
			subject_len - tmp_len, tmp_buff1,
			sizeof(tmp_buff1));
		return pproplist->set(PR_NORMALIZED_SUBJECT, tmp_buff1) == 0 ? TRUE : false;
	} else {
		return pproplist->set(PR_SUBJECT_A, field) == 0 ? TRUE : false;
	}
}

static BOOL oxcmail_parse_thread_topic(const char *charset,
	char *field, TPROPVAL_ARRAY *pproplist)
{
	char utf8_field[MIME_FIELD_LEN];
	
	if (TRUE == mime_string_to_utf8(
		charset, field, utf8_field)) {
		return pproplist->set(PROP_TAG_CONVERSATIONTOPIC, utf8_field) == 0 ? TRUE : false;
	} else {
		return pproplist->set(PROP_TAG_CONVERSATIONTOPIC_STRING8, field) == 0 ? TRUE : false;
	}
}

static BOOL oxcmail_parse_thread_index(
	char *field,  TPROPVAL_ARRAY *pproplist)
{
	BINARY tmp_bin;
	char tmp_buff[MIME_FIELD_LEN];
	
	/* remove space(s) produced by mime lib */
	auto len = strlen(field);
	for (size_t i = 0; i < len; ++i) {
		if (' ' == field[i]) {
			memmove(field + i, field + i + 1, len - i);
			len --;
			i --;
		}
	}
	len = sizeof(tmp_buff);
	if (0 != decode64(field, strlen(field), tmp_buff, &len)) {
		return TRUE;
	}
	tmp_bin.pc = tmp_buff;
	tmp_bin.cb = len;
	return pproplist->set(PROP_TAG_CONVERSATIONINDEX, &tmp_bin) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_keywords(const char *charset,
	char *field, uint16_t propid, TPROPVAL_ARRAY *pproplist)
{
	int i, len;
	BOOL b_start;
	char *ptoken_prev;
	STRING_ARRAY strings;
	char* string_buff[1024];
	char tmp_buff[MIME_FIELD_LEN];
	uint32_t tag;
	
	if (FALSE == mime_string_to_utf8(
		charset, field, tmp_buff)) {
		tag = PROP_TAG(PT_MV_STRING8, propid);
		gx_strlcpy(tmp_buff, field, GX_ARRAY_SIZE(tmp_buff));
	} else {
		tag = PROP_TAG(PT_MV_UNICODE, propid);
	}
	strings.count = 0;
	strings.ppstr = string_buff;
	len = strlen(tmp_buff);
	tmp_buff[len] = ';';
	len ++;
	ptoken_prev = tmp_buff;
	b_start = FALSE;
	for (i=0; i<len&&strings.count<1024; i++) {
		if (FALSE == b_start && (' ' == tmp_buff[i]
			|| '\t' == tmp_buff[i])) {
			ptoken_prev = tmp_buff + i + 1;
			continue;
		}
		b_start = TRUE;
		if (',' == tmp_buff[i] || ';' == tmp_buff[i]) {
			tmp_buff[i] = '\0';
			strings.ppstr[strings.count] = ptoken_prev;
			strings.count ++;
			b_start = FALSE;
			ptoken_prev = tmp_buff + i + 1;
		}
	}
	if (0 == strings.count) {
		return TRUE;
	}
	return pproplist->set(tag, &strings) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_response_suppress(
	char *field, TPROPVAL_ARRAY *pproplist)
{
	int i, len;
	BOOL b_start;
	char *ptoken_prev;
	uint32_t tmp_int32;
	
	if (0 == strcasecmp(field, "NONE")) {
		return TRUE;
	} else if (0 == strcasecmp(field, "ALL")) {
		tmp_int32 = 0xFFFFFFFF;
		return pproplist->set(PROP_TAG_AUTORESPONSESUPPRESS, &tmp_int32) == 0 ? TRUE : false;
	}
	len = strlen(field);
	field[len] = ';';
	len ++;
	ptoken_prev = field;
	b_start = FALSE;
	tmp_int32 = 0;
	for (i=0; i<len; i++) {
		if (FALSE == b_start && (' ' == field[i] || '\t' == field[i])) {
			ptoken_prev = field + i + 1;
			continue;
		}
		b_start = TRUE;
		if (',' == field[i] || ';' == field[i]) {
			field[i] = '\0';
			if (0 == strcasecmp("DR", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_DR;
			} else if (0 == strcasecmp("NDR", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_NDR;
			} else if (0 == strcasecmp("RN", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_RN;
			} else if (0 == strcasecmp("NRN", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_NRN;
			} else if (0 == strcasecmp("OOF", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_OOF;
			} else if (0 == strcasecmp("AutoReply", ptoken_prev)) {
				tmp_int32 |= AUTO_RESPONSE_SUPPRESS_AUTOREPLY;
			}
			b_start = FALSE;
			ptoken_prev = field + i + 1;
		}
	}
	if (0 == tmp_int32) {
		return TRUE;
	}
	return pproplist->set(PROP_TAG_AUTORESPONSESUPPRESS, &tmp_int32) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_content_class(char *field, MAIL *pmail,
    uint16_t *plast_propid, namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	char *ptoken;
	GUID tmp_guid;
	char tmp_class[1024];
	PROPERTY_NAME propname;
	const char *mclass;
	
	if (0 == strcasecmp(field, "fax")) {
		auto pmime = pmail->get_head();
		if (0 != strcasecmp("multipart/mixed",
			mime_get_content_type(pmime))) {
			return TRUE;
		}
		pmime = mime_get_child(pmime);
		if (NULL == pmime) {
			return TRUE;
		}
		if (0 != strcasecmp("text/html",
			mime_get_content_type(pmime))) {
			return TRUE;
		}
		pmime = mime_get_sibling(pmime);
		if (NULL == pmime) {
			return TRUE;
		}
		if (0 != strcasecmp("image/tiff",
			mime_get_content_type(pmime))) {
			return TRUE;
		}
		mclass = "IPM.Note.Microsoft.Fax";
	} else if (0 == strcasecmp(field, "fax-ca")) {
		mclass = "IPM.Note.Microsoft.Fax.CA";
	} else if (0 == strcasecmp(field, "missedcall")) {
		auto pmime = pmail->get_head();
		if (0 != strcasecmp("audio/gsm", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/mp3", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wav", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wma", mime_get_content_type(pmime))) {
			return TRUE;
		}
		mclass = "IPM.Note.Microsoft.Missed.Voice";
	} else if (0 == strcasecmp(field, "voice-uc")) {
		auto pmime = pmail->get_head();
		if (0 != strcasecmp("audio/gsm", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/mp3", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wav", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wma", mime_get_content_type(pmime))) {
			return TRUE;
		}
		mclass = "IPM.Note.Microsoft.Conversation.Voice";
	} else if (0 == strcasecmp(field, "voice-ca")) {
		auto pmime = pmail->get_head();
		if (0 != strcasecmp("audio/gsm", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/mp3", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wav", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wma", mime_get_content_type(pmime))) {
			return TRUE;
		}
		mclass = "IPM.Note.Microsoft.Voicemail.UM.CA";
	} else if (0 == strcasecmp(field, "voice")) {
		auto pmime = pmail->get_head();
		if (0 != strcasecmp("audio/gsm", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/mp3", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wav", mime_get_content_type(pmime)) &&
			0 != strcasecmp("audio/wma", mime_get_content_type(pmime))) {
			return TRUE;
		}
		mclass = "IPM.Note.Microsoft.Voicemail.UM";
	} else if (0 == strncasecmp(field, "urn:content-class:custom.", 25)) {
		snprintf(tmp_class, arsizeof(tmp_class), "IPM.Note.Custom.%s", field + 25);
		mclass = tmp_class;
	} else if (0 == strncasecmp(field, "InfoPathForm.", 13)) {
		ptoken = strchr(field + 13, '.');
		if (NULL != ptoken) {
			*ptoken = '\0';
			ptoken ++;
			if (TRUE == guid_from_string(&tmp_guid, field + 13)) {
				propname.guid = PSETID_COMMON;
				propname.kind = MNID_ID;
				propname.lid = PidLidInfoPathFromName;
				if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
					return FALSE;
				uint32_t tag = PROP_TAG(pick_strtype(ptoken), *plast_propid);
				if (pproplist->set(tag, ptoken) != 0)
					return FALSE;
				(*plast_propid) ++;
			}
		}
		snprintf(tmp_class, arsizeof(tmp_class), "IPM.InfoPathForm.%s", field + 13);
		mclass = tmp_class;
	} else {
		propname.guid = PS_INTERNET_HEADERS;
		propname.kind = MNID_STRING;
		propname.pname = deconst(PidNameContentClass);
		if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
			return FALSE;
		uint32_t tag = PROP_TAG(pick_strtype(field), *plast_propid);
		if (pproplist->set(tag, field) != 0)
			return FALSE;
		(*plast_propid) ++;
		return TRUE;
	}
	return pproplist->set(PR_MESSAGE_CLASS, mclass) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_message_flag(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	void *pvalue;
	BOOL b_unicode;
	uint8_t tmp_byte;
	double tmp_double;
	uint32_t tmp_int32;
	PROPERTY_NAME propname;
	
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidFlagRequest;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	uint32_t tag = PROP_TAG(pick_strtype(field), *plast_propid);
	if (pproplist->set(tag, field) != 0)
		return FALSE;
	(*plast_propid) ++;
	tmp_int32 = FLAG_STATUS_FOLLOWUPFLAGGED;
	if (pproplist->set(PROP_TAG_FLAGSTATUS, &tmp_int32) != 0)
		return FALSE;
	
	pvalue = pproplist->getval(PR_SUBJECT);
	if (NULL != pvalue) {
		b_unicode = TRUE;
	} else {
		b_unicode = FALSE;
		pvalue = pproplist->getval(PR_SUBJECT_A);
	}
	if (NULL != pvalue) {
		propname.guid = PSETID_COMMON;
		propname.kind = MNID_ID;
		propname.lid = PidLidFlagRequest;
		if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
			return FALSE;
		uint32_t tag = PROP_TAG(b_unicode ? PT_UNICODE : PT_STRING8, *plast_propid);
		if (pproplist->set(tag, pvalue) != 0)
			return FALSE;
		(*plast_propid) ++;
	}
	
	propname.guid = PSETID_TASK;
	propname.kind = MNID_ID;
	propname.lid = PidLidTaskStatus;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	tmp_int32 = 0;
	if (pproplist->set(PROP_TAG(PT_LONG, *plast_propid), &tmp_int32) != 0)
		return FALSE;
	(*plast_propid) ++;
	
	propname.guid = PSETID_TASK;
	propname.kind = MNID_ID;
	propname.lid = PidLidTaskComplete;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	tmp_byte = 0;
	if (pproplist->set(PROP_TAG(PT_BOOLEAN, *plast_propid), &tmp_byte) != 0)
		return FALSE;
	(*plast_propid) ++;
	
	propname.guid = PSETID_TASK;
	propname.kind = MNID_ID;
	propname.lid = PidLidPercentComplete;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	tmp_double = 0.0;
	if (pproplist->set(PROP_TAG(PT_DOUBLE, *plast_propid), &tmp_double) != 0)
		return FALSE;
	(*plast_propid) ++;
	tmp_int32 = TODO_ITEM_RECIPIENTFLAGGED;
	return pproplist->set(PROP_TAG_TODOITEMFLAGS, &tmp_int32) == 0 ? TRUE : false;
}

static BOOL oxcmail_parse_classified(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	uint8_t tmp_byte;
	PROPERTY_NAME propname;
	
	if (0 == strcasecmp(field, "true")) {
		propname.guid = PSETID_COMMON;
		propname.kind = MNID_ID;
		propname.lid = PidLidClassified;
		if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
			return FALSE;
		tmp_byte = 1;
		if (pproplist->set(PROP_TAG(PT_BOOLEAN, *plast_propid), &tmp_byte) != 0)
			return FALSE;
		(*plast_propid) ++;
	}
	return TRUE;
}

static BOOL oxcmail_parse_classkeep(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	uint8_t tmp_byte;
	PROPERTY_NAME propname;
	
	if (0 == strcasecmp(field, "true") || 0 == strcasecmp(field, "false")) {
		propname.guid = PSETID_COMMON;
		propname.kind = MNID_ID;
		propname.lid = PidLidClassificationKeep;
		if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
			return FALSE;
		if (0 == strcasecmp(field, "true")) {
			tmp_byte = 1;
		} else if (0 == strcasecmp(field, "false")) {
			tmp_byte = 0;
		}
		if (pproplist->set(PROP_TAG(PT_BOOLEAN, *plast_propid), &tmp_byte) != 0)
			return FALSE;
		(*plast_propid) ++;
	}
	return TRUE;
}

static BOOL oxcmail_parse_classification(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	PROPERTY_NAME propname;
	
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassification;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	uint32_t tag = PROP_TAG(pick_strtype(field), *plast_propid);
	if (pproplist->set(tag, field) != 0)
		return FALSE;
	(*plast_propid) ++;
	return TRUE;
}

static BOOL oxcmail_parse_classdesc(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	PROPERTY_NAME propname;
	
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassificationDescription;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	uint32_t tag = PROP_TAG(pick_strtype(field), *plast_propid);
	if (pproplist->set(tag, field) != 0)
		return FALSE;
	(*plast_propid) ++;
	return TRUE;
}

static BOOL oxcmail_parse_classid(char *field, uint16_t *plast_propid,
    namemap &phash, TPROPVAL_ARRAY *pproplist)
{
	PROPERTY_NAME propname;
	
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassificationGuid;
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	uint32_t tag = PROP_TAG(pick_strtype(field), *plast_propid);
	if (pproplist->set(tag, field) != 0)
		return FALSE;
	(*plast_propid) ++;
	return TRUE;
}

static BOOL oxcmail_enum_mail_head(
	const char *tag, char *field, void *pparam)
{
	void *pvalue;
	time_t tmp_time;
	uint8_t tmp_byte;
	uint64_t tmp_int32;
	uint64_t tmp_int64;
	EMAIL_ADDR email_addr;
	PROPERTY_NAME propname;
	FIELD_ENUM_PARAM *penum_param;
	
	penum_param = (FIELD_ENUM_PARAM*)pparam;
	if (0 == strcasecmp(tag, "From")) {
		parse_mime_addr(&email_addr, field);
		if (!oxcmail_parse_address(penum_param->charset, &email_addr,
		    PR_SENT_REPRESENTING_NAME, PR_SENT_REPRESENTING_ADDRTYPE,
		    PR_SENT_REPRESENTING_EMAIL_ADDRESS, PR_SENT_REPRESENTING_SMTP_ADDRESS,
		    PR_SENT_REPRESENTING_SEARCH_KEY, PR_SENT_REPRESENTING_ENTRYID,
		    &penum_param->pmsg->proplist))
			return FALSE;
	} else if (0 == strcasecmp(tag, "Sender")) {
		parse_mime_addr(&email_addr, field);
		if (!oxcmail_parse_address(penum_param->charset, &email_addr,
		    PR_SENDER_NAME, PR_SENDER_ADDRTYPE, PR_SENDER_EMAIL_ADDRESS,
		    PROP_TAG_SENDERSMTPADDRESS, PR_SENDER_SEARCH_KEY,
		    PR_SENDER_ENTRYID, &penum_param->pmsg->proplist))
			return FALSE;
	} else if (0 == strcasecmp(tag, "Reply-To")) {
		if (FALSE == oxcmail_parse_reply_to(
			penum_param->charset, field,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "To")) {
		if (!oxcmail_parse_addresses(penum_param->charset, field, MAPI_TO,
		    penum_param->pmsg->children.prcpts))
			return FALSE;
	} else if (0 == strcasecmp(tag, "Cc")) {
		if (!oxcmail_parse_addresses(penum_param->charset, field, MAPI_CC,
		    penum_param->pmsg->children.prcpts))
			return FALSE;
	} else if (0 == strcasecmp(tag, "Bcc")) {
		if (!oxcmail_parse_addresses(penum_param->charset, field, MAPI_BCC,
		    penum_param->pmsg->children.prcpts))
			return FALSE;
	} else if (0 == strcasecmp(tag, "Return-Receipt-To")) {
		tmp_byte = 1;
		if (penum_param->pmsg->proplist.set(PROP_TAG_ORIGINATORDELIVERYREPORTREQUESTED, &tmp_byte) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Disposition-Notification-To")) {
		tmp_byte = 1;
		if (penum_param->pmsg->proplist.set(PR_READ_RECEIPT_REQUESTED, &tmp_byte) != 0)
			return FALSE;
		tmp_byte = 1;
		if (penum_param->pmsg->proplist.set(PR_NON_RECEIPT_NOTIFICATION_REQUESTED, &tmp_byte) != 0)
			return FALSE;
		parse_mime_addr(&email_addr, field);
		if (FALSE == oxcmail_parse_address(penum_param->charset,
			&email_addr, PROP_TAG_READRECEIPTNAME,
			PROP_TAG_READRECEIPTADDRESSTYPE,
			PROP_TAG_READRECEIPTEMAILADDRESS,
			PROP_TAG_READRECEIPTSMTPADDRESS,
			PR_READ_RECEIPT_SEARCH_KEY,
			PR_READ_RECEIPT_ENTRYID,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "Message-ID")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_INTERNETMESSAGEID :
		                  PROP_TAG_INTERNETMESSAGEID_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Date")) {
		if (TRUE == parse_rfc822_timestamp(field, &tmp_time)) {
			tmp_int64 = rop_util_unix_to_nttime(tmp_time);
			if (penum_param->pmsg->proplist.set(PROP_TAG_CLIENTSUBMITTIME, &tmp_int64) != 0)
				return FALSE;
		}
	} else if (0 == strcasecmp(tag, "References")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_INTERNETREFERENCES :
		                  PROP_TAG_INTERNETREFERENCES_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Sensitivity")) {
		/* MS-OXCMAIL v22 §2.1.3.2.6 pg 31 */
		if (0 == strcasecmp(field, "Normal")) {
			tmp_int32 = SENSITIVITY_NONE;
		} else if (0 == strcasecmp(field, "Personal")) {
			tmp_int32 = SENSITIVITY_PERSONAL;
		} else if (0 == strcasecmp(field, "Private")) {
			tmp_int32 = SENSITIVITY_PRIVATE;
		} else if (0 == strcasecmp(field, "Company-Confidential")) {
			tmp_int32 = SENSITIVITY_COMPANY_CONFIDENTIAL;
		} else {
			tmp_int32 = SENSITIVITY_NONE;
		}
		if (penum_param->pmsg->proplist.set(PR_SENSITIVITY, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Importance") ||
		0 == strcasecmp(tag, "X-MSMail-Priority")) {
		/* MS-OXCMAIL v22 §2.2.3.2.5 pg 61 */
		if (0 == strcasecmp(field, "Low")) {
			tmp_int32 = IMPORTANCE_LOW;
		} else if (0 == strcasecmp(field, "Normal")) {
			tmp_int32 = IMPORTANCE_NORMAL;
		} else if (0 == strcasecmp(field, "High")) {
			tmp_int32 = IMPORTANCE_HIGH;
		} else {
			tmp_int32 = IMPORTANCE_NORMAL;
		}
		if (penum_param->pmsg->proplist.set(PR_IMPORTANCE, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Priority")) {
		/* RFC 2156 §5.3.6 pg 96, MS-OXCMAIL v22 §2.2.3.2.5 pg 60 */
		if (0 == strcasecmp(field, "Non-Urgent")) {
			tmp_int32 = IMPORTANCE_LOW;
		} else if (0 == strcasecmp(field, "Normal")) {
			tmp_int32 = IMPORTANCE_NORMAL;
		} else if (0 == strcasecmp(field, "Urgent")) {
			tmp_int32 = IMPORTANCE_HIGH;
		} else {
			tmp_int32 = IMPORTANCE_NORMAL;
		}
		if (penum_param->pmsg->proplist.set(PR_IMPORTANCE, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-Priority")) {
		/* MS-OXCMAIL v22 §2.2.3.2.5 pg 61 */
		switch (field[0]) {
		case '5':
		case '4':
			tmp_int32 = IMPORTANCE_LOW;
			break;
		case '3':
			tmp_int32 = IMPORTANCE_NORMAL;
			break;
		case '2':
		case '1':
			tmp_int32 = IMPORTANCE_HIGH;
			break;
		default:
			tmp_int32 = IMPORTANCE_NORMAL;
			break;
		}
		if (penum_param->pmsg->proplist.set(PR_IMPORTANCE, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Subject")) {
		if (FALSE == oxcmail_parse_subject(
			penum_param->charset, field,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
		if (!penum_param->pmsg->proplist.has(PR_SUBJECT_PREFIX)) {
			tmp_byte = '\0';
			if (penum_param->pmsg->proplist.set(PR_SUBJECT_PREFIX, &tmp_byte) != 0)
				return FALSE;
			pvalue = penum_param->pmsg->proplist.getval(PR_SUBJECT);
			if (NULL == pvalue) {
				pvalue = penum_param->pmsg->proplist.getval(PR_SUBJECT_A);
				if (pvalue != nullptr &&
				    penum_param->pmsg->proplist.set(PR_NORMALIZED_SUBJECT_A, pvalue) != 0)
					return FALSE;
			} else if (penum_param->pmsg->proplist.set(PR_NORMALIZED_SUBJECT, pvalue) != 0) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "Thread-Topic")) {
		if (FALSE == oxcmail_parse_thread_topic(
			penum_param->charset, field,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "Thread-Index")) {
		if (FALSE == oxcmail_parse_thread_index(
			field, &penum_param->pmsg->proplist)) {
				return FALSE;
			}
	} else if (0 == strcasecmp(tag, "In-Reply-To")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_INREPLYTOID :
		                  PROP_TAG_INREPLYTOID_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Reply-By")) {
		if (TRUE == parse_rfc822_timestamp(field, &tmp_time)) {
			tmp_int64 = rop_util_unix_to_nttime(tmp_time);
			if (penum_param->pmsg->proplist.set(PROP_TAG_REPLYTIME, &tmp_int64) != 0)
				return FALSE;
		}
	} else if (0 == strcasecmp(tag, "Content-Language")) {
		tmp_int32 = oxcmail_ltag_to_lcid(field);
		if (tmp_int32 != 0 &&
		    penum_param->pmsg->proplist.set(PR_MESSAGE_LOCALE_ID, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Accept-Language") ||
		0 == strcasecmp(tag, "X-Accept-Language")) {
		propname.guid = PS_INTERNET_HEADERS;
		propname.kind = MNID_STRING;
		propname.pname = deconst("Accept-Language");
		if (namemap_add(penum_param->phash, penum_param->last_propid,
		    std::move(propname)) != 0)
			return FALSE;
		uint32_t tag = PROP_TAG(pick_strtype(field), penum_param->last_propid);
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
		penum_param->last_propid ++;
	} else if (0 == strcasecmp(tag, "Keywords")) {
		propname.guid = PS_PUBLIC_STRINGS;
		propname.kind = MNID_STRING;
		propname.pname = deconst(PidNameKeywords);
		if (namemap_add(penum_param->phash, penum_param->last_propid,
		    std::move(propname)) != 0)
			return FALSE;
		if (FALSE == oxcmail_parse_keywords(
			penum_param->charset, field,
			penum_param->last_propid,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
		penum_param->last_propid ++;
	} else if (0 == strcasecmp(tag, "Expires") ||
		0 == strcasecmp(tag, "Expiry-Date")) {
		if (TRUE == parse_rfc822_timestamp(field, &tmp_time)) {
			tmp_int64 = rop_util_unix_to_nttime(tmp_time);
			if (penum_param->pmsg->proplist.set(PROP_TAG_EXPIRYTIME, &tmp_int64) != 0)
				return FALSE;
		}
	} else if (0 == strcasecmp(tag, "X-Auto-Response-Suppress")) {
		if (FALSE == oxcmail_parse_response_suppress(
			field, &penum_param->pmsg->proplist)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "Content-Class")) {
		if (FALSE == oxcmail_parse_content_class(field,
			penum_param->pmail, &penum_param->last_propid,
			penum_param->phash, &penum_param->pmsg->proplist)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "X-Message-Flag")) {
		if (FALSE == oxcmail_parse_message_flag(field,
			&penum_param->last_propid, penum_param->phash,
			&penum_param->pmsg->proplist)) {
			return FALSE;
		}
		penum_param->b_flag_del = TRUE;
	} else if (0 == strcasecmp(tag, "List-Help") ||
		0 == strcasecmp(tag, "X-List-Help")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_LISTHELP : PROP_TAG_LISTHELP_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "List-Subscribe") ||
		0 == strcasecmp(tag, "X-List-Subscribe")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_LISTSUBSCRIBE :
		                  PROP_TAG_LISTSUBSCRIBE_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "List-Unsubscribe") ||
		0 == strcasecmp(tag, "X-List-Unsubscribe")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_LISTUNSUBSCRIBE :
		                  PROP_TAG_LISTUNSUBSCRIBE_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-Payload-Class")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PR_ATTACH_PAYLOAD_CLASS :
		                  PR_ATTACH_PAYLOAD_CLASS_A;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-MS-Exchange-Organization-PRD")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_PURPORTEDSENDERDOMAIN :
		                  PROP_TAG_PURPORTEDSENDERDOMAIN_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag,
		"X-MS-Exchange-Organization-SenderIdResult")) {
		if (0 == strcasecmp(field, "Neutral")) {
			tmp_int32 = 1;
		} else if (0 == strcasecmp(field, "Pass")) {
			tmp_int32 = 2;
		} else if (0 == strcasecmp(field, "Fail")) {
			tmp_int32 = 3;
		} else if (0 == strcasecmp(field, "SoftFail")) {
			tmp_int32 = 4;
		} else if (0 == strcasecmp(field, "None")) {
			tmp_int32 = 5;
		} else if (0 == strcasecmp(field, "TempError")) {
			tmp_int32 = 6;
		} else if (0 == strcasecmp(field, "PermError")) {
			tmp_int32 = 7;
		} else {
			tmp_int32 = 0;
		}
		if (tmp_int32 != 0 &&
		    penum_param->pmsg->proplist.set(PR_SENDER_ID_STATUS, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-MS-Exchange-Organization-SCL")) {
		tmp_int32 = strtol(field, nullptr, 0);
		if (penum_param->pmsg->proplist.set(PROP_TAG_CONTENTFILTERSPAMCONFIDENCELEVEL, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-Microsoft-Classified")) {
		if (0 == strcasecmp(field, "true") ||
			0 == strcasecmp(field, "false")) {
			if (FALSE == oxcmail_parse_classified(field,
				&penum_param->last_propid, penum_param->phash,
				&penum_param->pmsg->proplist)) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "X-Microsoft-ClassKeep")) {
		if (TRUE == penum_param->b_classified) {
			if (FALSE == oxcmail_parse_classkeep(field,
				&penum_param->last_propid, penum_param->phash,
				&penum_param->pmsg->proplist)) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "X-Microsoft-Classification")) {
		if (TRUE == penum_param->b_classified) {
			if (FALSE == oxcmail_parse_classification(field,
				&penum_param->last_propid, penum_param->phash,
				&penum_param->pmsg->proplist)) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "X-Microsoft-ClassDesc")) {
		if (TRUE == penum_param->b_classified) {
			if (FALSE == oxcmail_parse_classdesc(field,
				&penum_param->last_propid, penum_param->phash,
				&penum_param->pmsg->proplist)) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "X-Microsoft-ClassID")) {
		if (TRUE == penum_param->b_classified) {
			if (FALSE == oxcmail_parse_classid(field,
				&penum_param->last_propid, penum_param->phash,
				&penum_param->pmsg->proplist)) {
				return FALSE;
			}
		}
	} else if (0 == strcasecmp(tag, "X-CallingTelephoneNumber")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		               PR_SENDER_TELEPHONE_NUMBER :
		               PR_SENDER_TELEPHONE_NUMBER_A;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-VoiceMessageSenderName")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_VOICEMESSAGESENDERNAME :
		                  PROP_TAG_VOICEMESSAGESENDERNAME_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-AttachmentOrder")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_VOICEMESSAGEATTACHMENTORDER :
		                  PROP_TAG_VOICEMESSAGEATTACHMENTORDER_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-CallID")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_CALLID : PROP_TAG_CALLID_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-VoiceMessageDuration")) {
		tmp_int32 = strtol(field, nullptr, 0);
		if (penum_param->pmsg->proplist.set(PROP_TAG_VOICEMESSAGEDURATION, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-FaxNumverOfPages")) {
		tmp_int32 = strtol(field, nullptr, 0);
		if (penum_param->pmsg->proplist.set(PROP_TAG_FAXNUMBEROFPAGES, &tmp_int32) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "Content-ID")) {
		tmp_int32 = strlen(field);
		if (tmp_int32 > 0) {
			if ('>' == field[tmp_int32 - 1]) {
				field[tmp_int32 - 1] = '\0';
			}
			if ('<' == field[0]) {
				pvalue = field + 1;
			} else {
				pvalue = field;
			}
			uint32_t tag = oxcmail_check_ascii(static_cast<char *>(pvalue)) ?
			                  PROP_TAG_BODYCONTENTID :
			                  PROP_TAG_BODYCONTENTID_STRING8;
			if (penum_param->pmsg->proplist.set(tag, pvalue) != 0)
				return FALSE;
		}
	} else if (0 == strcasecmp(tag, "Content-Base")) {
		propname.guid = PS_INTERNET_HEADERS;
		propname.kind = MNID_STRING;
		propname.pname = deconst("Content-Base");
		if (namemap_add(penum_param->phash, penum_param->last_propid,
		    std::move(propname)) != 0)
			return FALSE;
		uint32_t tag = PROP_TAG(pick_strtype(field), penum_param->last_propid);
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
		penum_param->last_propid ++;
	} else if (0 == strcasecmp(tag, "Content-Location")) {
		uint32_t tag = oxcmail_check_ascii(field) ?
		                  PROP_TAG_BODYCONTENTLOCATION :
		                  PROP_TAG_BODYCONTENTLOCATION_STRING8;
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
	} else if (0 == strcasecmp(tag, "X-MS-Exchange-Organization-AuthAs") ||
		0 == strcasecmp(tag, "X-MS-Exchange-Organization-AuthDomain") ||
		0 == strcasecmp(tag, "X-MS-Exchange-Organization-AuthMechanism") ||
		0 == strcasecmp(tag, "X-MS-Exchange-Organization-AuthSource") ||
		0 == strcasecmp(tag, "X-Mailer") ||
		0 == strcasecmp(tag, "User-Agent")) {
		propname.guid = PS_INTERNET_HEADERS;
		propname.kind = MNID_STRING;
		propname.pname = static_cast<char *>(penum_param->alloc(strlen(tag) + 1));
		if (NULL == propname.pname) {
			return FALSE;
		}
		strcpy(propname.pname, tag);
		if (namemap_add(penum_param->phash, penum_param->last_propid,
		    std::move(propname)) != 0)
			return FALSE;
		uint32_t tag = PROP_TAG(pick_strtype(field), penum_param->last_propid);
		if (penum_param->pmsg->proplist.set(tag, field) != 0)
			return FALSE;
		penum_param->last_propid ++;
	}
	return TRUE;
}

static BOOL oxcmail_parse_transport_message_header(
	MIME *pmime, TPROPVAL_ARRAY *pproplist)
{
	size_t tmp_len;
	char tmp_buff[1024*1024];
	
	tmp_len = sizeof(tmp_buff) - 1;
	if (TRUE == mime_read_head(pmime, tmp_buff, &tmp_len)) {
		tmp_buff[tmp_len + 1] = '\0';
		uint32_t tag = oxcmail_check_ascii(tmp_buff) ?
		                  PROP_TAG_TRANSPORTMESSAGEHEADERS :
		                  PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
		if (pproplist->set(tag, tmp_buff) != 0)
			return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_parse_message_body(const char *charset,
	MIME *pmime, TPROPVAL_ARRAY *pproplist)
{
	BINARY tmp_bin;
	uint32_t tmp_int32;
	char best_charset[32];
	char temp_charset[32];
	const char *content_type;
	
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t length = rdlength;
	auto pcontent = static_cast<char *>(malloc(3 * length + 2));
	if (NULL == pcontent) {
		return FALSE;
	}
	if (FALSE == mime_read_content(pmime, pcontent, &length)) {
		free(pcontent);
		return TRUE;
	}
	if (TRUE == oxcmail_get_content_param(
		pmime, "charset", temp_charset, 32)) {
		gx_strlcpy(best_charset, temp_charset, GX_ARRAY_SIZE(best_charset));
	} else {
		gx_strlcpy(best_charset, charset, GX_ARRAY_SIZE(best_charset));
	}
	content_type = mime_get_content_type(pmime);
	if (0 == strcasecmp(content_type, "text/html")) {
		tmp_int32 = oxcmail_charset_to_cpid(best_charset);
		if (pproplist->set(PR_INTERNET_CPID, &tmp_int32) != 0) {
			free(pcontent);
			return FALSE;
		}
		tmp_bin.cb = length;
		tmp_bin.pc = pcontent;
		if (pproplist->set(PROP_TAG_HTML, &tmp_bin) != 0) {
			free(pcontent);
			return false;
		}
	} else if (0 == strcasecmp(content_type, "text/plain")) {
		pcontent[length] = '\0';
		TAGGED_PROPVAL propval;
		if (TRUE == string_to_utf8(best_charset,
			pcontent, pcontent + length + 1)) {
			propval.proptag = PR_BODY;
			propval.pvalue = pcontent + length + 1;
			if (!utf8_check(static_cast<char *>(propval.pvalue)))
				utf8_filter(static_cast<char *>(propval.pvalue));
		} else {
			propval.proptag = PR_BODY_A;
			propval.pvalue = pcontent;
		}
		if (pproplist->set(propval) != 0) {
			free(pcontent);
			return false;
		}
	} else if (0 == strcasecmp(content_type, "text/enriched")) {
		pcontent[length] = '\0';
		enriched_to_html(pcontent, pcontent + length + 1, 2*length);
		tmp_int32 = oxcmail_charset_to_cpid(best_charset);
		if (pproplist->set(PR_INTERNET_CPID, &tmp_int32) != 0) {
			free(pcontent);
			return FALSE;
		}
		tmp_bin.cb = strlen(pcontent + length + 1);
		tmp_bin.pc = pcontent + length + 1;
		if (pproplist->set(PROP_TAG_HTML, &tmp_bin) != 0) {
			free(pcontent);
			return false;
		}
	}
	free(pcontent);
	return TRUE;
}

static void oxcmail_compose_mac_additional(
	uint32_t type, uint32_t creator, BINARY *pbin)
{
	int i, tmp_len;
	uint8_t tmp_buff[16];
	
	tmp_len = 0;
	tmp_buff[tmp_len] = ':';
	tmp_len ++;
	memcpy(tmp_buff + tmp_len, &creator, 4);
	tmp_len += 4;
	tmp_buff[tmp_len] = ':';
	tmp_len ++;
	memcpy(tmp_buff + tmp_len, &type, 4);
	tmp_len += 4;
	tmp_buff[tmp_len] = '\0';
	tmp_len ++;
	pbin->cb = 0;
	for (i=0; i<tmp_len; i++) {
		if (0 == i || 5 == i) {
			pbin->pb[pbin->cb] = ':';
			pbin->cb ++;
			continue;
		}
		if ('\\' == tmp_buff[i] || ':' == tmp_buff[i] || ';' == tmp_buff[i]) {
			pbin->pb[pbin->cb] = '\\';
			pbin->cb ++;
			pbin->pb[pbin->cb] = tmp_buff[i];
			pbin->cb ++;
		} else if (tmp_buff[i] < 32 || tmp_buff[i] > 251 || 127 == tmp_buff[i]) {
			pbin->pb[pbin->cb] = '\\';
			pbin->cb ++;
			sprintf(pbin->pc + pbin->cb, "%03o", tmp_buff[i]);
		} else {
			pbin->pb[pbin->cb] = tmp_buff[i];
			pbin->cb ++;
		}
	}
}

static BOOL oxcmail_set_mac_attachname(TPROPVAL_ARRAY *pproplist,
	BOOL b_description, char *tmp_buff)
{
	char extension[64];
	
	oxcmail_split_filename(tmp_buff, extension);
	if (!b_description && pproplist->set(PR_DISPLAY_NAME_A, tmp_buff) != 0)
		return FALSE;
	if (extension[0] != '\0' &&
	    pproplist->set(PR_ATTACH_EXTENSION_A, extension) != 0)
		return FALSE;
	if (pproplist->set(PR_ATTACH_LONG_FILENAME_A, tmp_buff) != 0)
		return FALSE;
	return TRUE;
}

static BOOL oxcmail_parse_binhex(MIME *pmime, ATTACHMENT_CONTENT *pattachment,
    BOOL b_filename, BOOL b_description, uint16_t *plast_propid, namemap &phash)
{
	BINARY *pbin;
	BINHEX binhex;
	BINARY tmp_bin;
	char tmp_buff[256];
	PROPERTY_NAME propname;
	
	tmp_bin.cb = sizeof(MACBINARY_ENCODING);
	tmp_bin.pb = deconst(MACBINARY_ENCODING);
	if (pattachment->proplist.set(PR_ATTACH_ENCODING, &tmp_bin) != 0)
		return FALSE;
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t content_len = rdlength;
	auto pcontent = static_cast<char *>(malloc(content_len));
	if (NULL == pcontent) {
		return FALSE;
	}
	if (FALSE == mime_read_content(pmime, pcontent, &content_len)) {
		free(pcontent);
		return FALSE;
	}
	if (!binhex_deserialize(&binhex, pcontent, content_len)) {
		free(pcontent);
		return FALSE;
	}
	free(pcontent);
	if (FALSE == b_filename) {
		strcpy(tmp_buff, binhex.file_name);
		if (FALSE == oxcmail_set_mac_attachname(
			&pattachment->proplist, b_description, tmp_buff)) {
			binhex_clear(&binhex);
			return FALSE;
		}
	}
	tmp_bin.cb = 0;
	tmp_bin.pc = tmp_buff;
	oxcmail_compose_mac_additional(binhex.type, binhex.creator, &tmp_bin);
	if (pattachment->proplist.set(PR_ATTACH_ADDITIONAL_INFO, &tmp_bin) != 0) {
		binhex_clear(&binhex);
		return FALSE;
	}
	pbin = apple_util_binhex_to_appledouble(&binhex);
	if (NULL == pbin) {
		binhex_clear(&binhex);
		return FALSE;
	}
	propname.guid = PSETID_ATTACHMENT;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameAttachmentMacInfo);
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0) {
		rop_util_free_binary(pbin);
		binhex_clear(&binhex);
		return FALSE;
	}
	if (pattachment->proplist.set(PROP_TAG(PT_BINARY, *plast_propid), pbin) != 0) {
		rop_util_free_binary(pbin);
		binhex_clear(&binhex);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	(*plast_propid) ++;
	pbin = apple_util_binhex_to_macbinary(&binhex);
	if (NULL == pbin) {
		binhex_clear(&binhex);
		return FALSE;
	}
	if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, pbin) != 0) {
		rop_util_free_binary(pbin);
		binhex_clear(&binhex);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	binhex_clear(&binhex);
	return TRUE;
}

static BOOL oxcmail_parse_appledouble(MIME *pmime,
    ATTACHMENT_CONTENT *pattachment, BOOL b_filename, BOOL b_description,
    EXT_BUFFER_ALLOC alloc, uint16_t *plast_propid, namemap &phash)
{
	int i;
	MIME *psub;
	MIME *phmime;
	MIME *pdmime;
	BINARY *pbin;
	BINARY tmp_bin;
	EXT_PULL ext_pull;
	char tmp_buff[256];
	APPLEFILE applefile;
	PROPERTY_NAME propname;
	
	phmime = NULL;
	pdmime = NULL;
	psub = mime_get_child(pmime);
	if (NULL == psub) {
		return FALSE;
	}
	if (0 == strcasecmp("application/applefile",
		mime_get_content_type(psub))) {
		phmime = psub;
	} else {
		pdmime = psub;
	}
	psub = mime_get_sibling(psub);
	if (NULL == psub) {
		return FALSE;
	}
	if (NULL == phmime) {
		if (0 == strcasecmp("application/applefile",
			mime_get_content_type(psub))) {
			phmime = psub;
		} else {
			return FALSE;
		}
	} else {
		pdmime = psub;
	}
	tmp_bin.cb = sizeof(MACBINARY_ENCODING);
	tmp_bin.pb = deconst(MACBINARY_ENCODING);
	if (pattachment->proplist.set(PR_ATTACH_ENCODING, &tmp_bin) != 0)
		return FALSE;
	propname.guid = PSETID_ATTACHMENT;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameAttachmentMacContentType);
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0)
		return FALSE;
	if (pattachment->proplist.set(PROP_TAG(PT_UNICODE, *plast_propid), mime_get_content_type(pdmime)) != 0)
		return FALSE;
	(*plast_propid) ++;
	auto rdlength = mime_get_length(phmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t content_len = rdlength;
	auto pcontent = static_cast<char *>(malloc(content_len));
	if (NULL == pcontent) {
		return FALSE;
	}
	if (FALSE == mime_read_content(phmime, pcontent, &content_len)) {
		free(pcontent);
		return FALSE;
	}
	propname.guid = PSETID_ATTACHMENT;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameAttachmentMacInfo);
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0) {
		free(pcontent);
		return FALSE;
	}
	tmp_bin.cb = content_len;
	tmp_bin.pb = (uint8_t*)pcontent;
	if (pattachment->proplist.set(PROP_TAG(PT_BINARY, *plast_propid), &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	(*plast_propid) ++;
	ext_pull.init(pcontent, content_len, alloc, 0);
	if (EXT_ERR_SUCCESS != applefile_pull_file(&ext_pull, &applefile)) {
		free(pcontent);
		return FALSE;
	}
	for (i=0; i<applefile.count; i++) {
		if (FALSE == b_filename && AS_REALNAME ==
			applefile.pentries[i].entry_id) {
			memset(tmp_buff, 0, arsizeof(tmp_buff));
			auto bv = static_cast<BINARY *>(applefile.pentries[i].pentry);
			memcpy(tmp_buff, bv->pb, std::min(bv->cb, static_cast<uint32_t>(255)));
			if (FALSE == oxcmail_set_mac_attachname(
				&pattachment->proplist, b_description, tmp_buff)) {
				free(pcontent);
				return FALSE;
			}
		}
		if (AS_FINDERINFO == applefile.pentries[i].entry_id) {
			tmp_bin.cb = 0;
			tmp_bin.pc = tmp_buff;
			auto as = static_cast<ASFINDERINFO *>(applefile.pentries[i].pentry);
			oxcmail_compose_mac_additional(as->finfo.fd_type,
				as->finfo.fd_creator, &tmp_bin);
			if (pattachment->proplist.set(PR_ATTACH_ADDITIONAL_INFO, &tmp_bin) != 0) {
				free(pcontent);
				return FALSE;
			}
		}
	}
	rdlength = mime_get_length(pdmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		free(pcontent);
		return false;
	}
	size_t content_len1 = rdlength;
	auto pcontent1 = static_cast<char *>(malloc(content_len1));
	if (NULL == pcontent1) {
		free(pcontent);
		return FALSE;
	}
	if (FALSE == mime_read_content(pdmime, pcontent1, &content_len1)) {
		free(pcontent1);
		free(pcontent);
		return FALSE;
	}
	pbin = apple_util_appledouble_to_macbinary(
			&applefile, pcontent1, content_len1);
	if (NULL == pbin) {
		free(pcontent1);
		free(pcontent);
		return FALSE;
	}
	if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, pbin) != 0) {
		rop_util_free_binary(pbin);
		free(pcontent1);
		free(pcontent);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	free(pcontent1);
	free(pcontent);
	return TRUE;
}

static BOOL oxcmail_parse_macbinary(MIME *pmime,
    ATTACHMENT_CONTENT *pattachment, BOOL b_filename, BOOL b_description,
    EXT_BUFFER_ALLOC alloc, uint16_t *plast_propid, namemap &phash)
{	
	BINARY *pbin;
	BINARY tmp_bin;
	MACBINARY macbin;
	EXT_PULL ext_pull;
	char tmp_buff[64];
	PROPERTY_NAME propname;
	
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t content_len = rdlength;
	auto pcontent = static_cast<char *>(malloc(content_len));
	if (NULL == pcontent) {
		return FALSE;
	}
	if (FALSE == mime_read_content(pmime, pcontent, &content_len)) {
		free(pcontent);
		return FALSE;
	}
	ext_pull.init(pcontent, content_len, alloc, 0);
	if (EXT_ERR_SUCCESS != macbinary_pull_binary(&ext_pull, &macbin)) {
		free(pcontent);
		return FALSE;
	}
	tmp_bin.cb = sizeof(MACBINARY_ENCODING);
	tmp_bin.pb = deconst(MACBINARY_ENCODING);
	if (pattachment->proplist.set(PR_ATTACH_ENCODING, &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	if (FALSE == b_filename) {
		strcpy(tmp_buff, macbin.header.file_name);
		if (FALSE == oxcmail_set_mac_attachname(
			&pattachment->proplist, b_description, tmp_buff)) {
			free(pcontent);
			return FALSE;
		}
	}
	pbin = apple_util_macbinary_to_appledouble(&macbin);
	if (NULL == pbin) {
		free(pcontent);
		return FALSE;
	}
	propname.guid = PSETID_ATTACHMENT;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameAttachmentMacInfo);
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0) {
		rop_util_free_binary(pbin);
		free(pcontent);
		return FALSE;
	}
	if (pattachment->proplist.set(PROP_TAG(PT_BINARY, *plast_propid), pbin) != 0) {
		rop_util_free_binary(pbin);
		free(pcontent);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	(*plast_propid) ++;
	tmp_bin.pc = tmp_buff;
	oxcmail_compose_mac_additional(macbin.header.type,
					macbin.header.creator, &tmp_bin);
	if (pattachment->proplist.set(PR_ATTACH_ADDITIONAL_INFO, &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	tmp_bin.cb = content_len;
	tmp_bin.pc = pcontent;
	if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	free(pcontent);
	return TRUE;
}

static BOOL oxcmail_parse_applesingle(MIME *pmime,
    ATTACHMENT_CONTENT *pattachment, BOOL b_filename, BOOL b_description,
    EXT_BUFFER_ALLOC alloc, uint16_t *plast_propid, namemap &phash)
{
	int i;
	BINARY *pbin;
	BINARY tmp_bin;
	EXT_PULL ext_pull;
	char tmp_buff[256];
	APPLEFILE applefile;
	PROPERTY_NAME propname;
	
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t content_len = rdlength;
	auto pcontent = static_cast<char *>(malloc(content_len));
	if (NULL == pcontent) {
		return FALSE;
	}
	if (FALSE == mime_read_content(pmime, pcontent, &content_len)) {
		free(pcontent);
		return FALSE;
	}
	ext_pull.init(pcontent, content_len, alloc, 0);
	if (EXT_ERR_SUCCESS != applefile_pull_file(&ext_pull, &applefile)) {
		free(pcontent);
		return oxcmail_parse_macbinary(pmime,
			pattachment, b_filename, b_description,
			alloc, plast_propid, phash);
	}
	tmp_bin.cb = sizeof(MACBINARY_ENCODING);
	tmp_bin.pb = deconst(MACBINARY_ENCODING);
	if (pattachment->proplist.set(PR_ATTACH_ENCODING, &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	propname.guid = PSETID_ATTACHMENT;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameAttachmentMacInfo);
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0) {
		free(pcontent);
		return FALSE;
	}
	pbin = apple_util_applesingle_to_appledouble(&applefile);
	if (NULL == pbin) {
		free(pcontent);
		return FALSE;
	}
	if (pattachment->proplist.set(PROP_TAG(PT_BINARY, *plast_propid), pbin) != 0) {
		rop_util_free_binary(pbin);
		free(pcontent);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	(*plast_propid) ++;
	for (i=0; i<applefile.count; i++) {
		if (FALSE == b_filename && AS_REALNAME ==
			applefile.pentries[i].entry_id) {
			auto bv = static_cast<BINARY *>(applefile.pentries[i].pentry);
			memset(tmp_buff, 0, arsizeof(tmp_buff));
			memcpy(tmp_buff, bv->pb, std::min(bv->cb, static_cast<uint32_t>(255)));
			if (FALSE == oxcmail_set_mac_attachname(
				&pattachment->proplist, b_description, tmp_buff)) {
				free(pcontent);
				return FALSE;
			}
		}
		if (AS_FINDERINFO == applefile.pentries[i].entry_id) {
			tmp_bin.cb = 0;
			tmp_bin.pc = tmp_buff;
			auto as = static_cast<ASFINDERINFO *>(applefile.pentries[i].pentry);
			oxcmail_compose_mac_additional(as->finfo.fd_type,
				as->finfo.fd_creator, &tmp_bin);
			if (pattachment->proplist.set(PR_ATTACH_ADDITIONAL_INFO, &tmp_bin) != 0) {
				free(pcontent);
				return FALSE;
			}
		}
	}
	pbin = apple_util_applesingle_to_macbinary(&applefile);
	if (NULL == pbin) {
		free(pcontent);
		return FALSE;
	}
	if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, pbin) != 0) {
		rop_util_free_binary(pbin);
		free(pcontent);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	free(pcontent);
	return TRUE;
}

static void oxcmail_enum_attachment(MIME *pmime, void *pparam)
{
	VCARD vcard;
	MIME *pmime1;
	BOOL b_unifn;
	char *ptoken;
	BINARY tmp_bin;
	BOOL b_filename;
	time_t tmp_time;
	const char *pext;
	uint64_t tmp_int64;
	uint32_t tmp_int32;
	BOOL b_description;
	char extension[16];
	char mode_buff[32];
	char dir_buff[256];
	char tmp_buff[1024];
	char site_buff[256];
	char file_name[512];
	char date_buff[128];
	char mime_charset[32];
	MESSAGE_CONTENT *pmsg;
	char display_name[512];
	ATTACHMENT_CONTENT *pattachment;
	
	pmime1 = NULL;
	auto pmime_enum = static_cast<MIME_ENUM_PARAM *>(pparam);
	if (FALSE == pmime_enum->b_result) {
		return;
	}
	if (pmime == pmime_enum->phtml ||
		pmime == pmime_enum->pplain ||
		pmime == pmime_enum->pcalendar ||
		pmime == pmime_enum->penriched ||
		pmime == pmime_enum->preport) {
		return;
	}
	if (NULL != mime_get_parent(pmime) && 0 == strcasecmp(
		"multipart/appledouble", mime_get_content_type(
		mime_get_parent(pmime)))) {
		return;
	}
	if (MULTIPLE_MIME == mime_get_type(pmime)) {
		if (0 != strcasecmp("multipart/appledouble",
			mime_get_content_type(pmime))) {
			return;
		}
		pmime1 = pmime;
		pmime = mime_get_child(pmime);
		if (NULL == pmime) {
			return;
		}
		if (mime_get_sibling(pmime) == nullptr)
			pmime1 = NULL;
		else
			pmime = mime_get_sibling(pmime);
	}
	pattachment = attachment_content_init();
	if (NULL == pattachment) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (FALSE == attachment_list_append_internal(
		pmime_enum->pmsg->children.pattachments, pattachment)) {
		attachment_content_free(pattachment);
		pmime_enum->b_result = FALSE;
		return;
	}
	const char *newval;
	if (0 == strcasecmp("application/ms-tnef",
		mime_get_content_type(pmime))) {
		newval = "application/octet-stream";
	} else {
		newval = mime_get_content_type(pmime);
	}
	if (pattachment->proplist.set(PR_ATTACH_MIME_TAG, newval) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	b_filename = mime_get_filename(pmime, tmp_buff);
	if (TRUE == b_filename) {
		if (TRUE == mime_string_to_utf8(
			pmime_enum->charset, tmp_buff,
			file_name)) {
			b_unifn = TRUE;
		} else {
			b_unifn = FALSE;
			strcpy(file_name, tmp_buff);
		}
		oxcmail_split_filename(file_name, extension);
		if ('\0' == extension[0]) {
			pext = oxcmail_mime_to_extension(
					mime_get_content_type(pmime));
			if (pext != NULL) {
				sprintf(extension, ".%s", pext);
				HX_strlcat(file_name, extension, sizeof(file_name));
			}
		}
	} else {
		b_unifn = TRUE;
		if ('\0' != extension[0]) {
			pext = oxcmail_mime_to_extension(
					mime_get_content_type(pmime));
			if (NULL != pext) {
				sprintf(extension, ".%s", pext);
				HX_strlcat(file_name, extension, sizeof(file_name));
			} else {
				strcpy(extension, ".dat");
			}
		}
		pmime_enum->attach_id ++;
		sprintf(file_name, "attachment%d%s",
			pmime_enum->attach_id, extension);
	}
	if (extension[0] != '\0' &&
	    pattachment->proplist.set(PR_ATTACH_EXTENSION, extension) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (pattachment->proplist.set(b_unifn ? PR_ATTACH_LONG_FILENAME :
	    PR_ATTACH_LONG_FILENAME_A, file_name) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	b_description = mime_get_field(pmime,
		"Content-Description", tmp_buff, 256);
	if (TRUE == b_description) {
		uint32_t tag;
		if (TRUE == mime_string_to_utf8(
			pmime_enum->charset, tmp_buff,
			display_name)) {
			tag = PR_DISPLAY_NAME;
		} else {
			tag = PR_DISPLAY_NAME_A;
			strcpy(display_name, tmp_buff);
		}
		if (pattachment->proplist.set(tag, display_name) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
	}
	bool b_inline = false;
	if (TRUE == mime_get_field(pmime,
		"Content-Disposition", tmp_buff, 1024)) {
		b_inline = strcmp(tmp_buff, "inline") == 0 || strncasecmp(tmp_buff, "inline;", 7) == 0;
		if (TRUE == oxcmail_get_field_param(tmp_buff,
			"create-date", date_buff, 128)) {
			if (TRUE == parse_rfc822_timestamp(
				date_buff, &tmp_time)) {
				tmp_int64 = rop_util_unix_to_nttime(tmp_time);
				if (pattachment->proplist.set(PR_CREATION_TIME, &tmp_int64) != 0) {
					pmime_enum->b_result = FALSE;
					return;
				}
			}
		}
		if (TRUE == oxcmail_get_field_param(tmp_buff,
			"modification-date", date_buff, 128)) {
			if (TRUE == parse_rfc822_timestamp(
				date_buff, &tmp_time)) {
				tmp_int64 = rop_util_unix_to_nttime(tmp_time);
				if (pattachment->proplist.set(PR_LAST_MODIFICATION_TIME, &tmp_int64) != 0) {
					pmime_enum->b_result = FALSE;
					return;
				}
			}
		}
	}
	if (!pattachment->proplist.has(PR_CREATION_TIME) &&
	    pattachment->proplist.set(PR_CREATION_TIME, &pmime_enum->nttime_stamp) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (!pattachment->proplist.has(PR_LAST_MODIFICATION_TIME) &&
	    pattachment->proplist.set(PR_LAST_MODIFICATION_TIME, &pmime_enum->nttime_stamp) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (TRUE == mime_get_field(pmime, "Content-ID", tmp_buff, 128)) {
		tmp_int32 = strlen(tmp_buff);
		if (tmp_int32 > 0) {
			if ('>' == tmp_buff[tmp_int32 - 1]) {
				tmp_buff[tmp_int32 - 1] = '\0';
			}
			const char *newval;
			if ('<' == tmp_buff[0]) {
				newval = tmp_buff + 1;
			} else {
				newval = tmp_buff;
			}
			uint32_t tag = oxcmail_check_ascii(newval) ?
			                  PR_ATTACH_CONTENT_ID : PR_ATTACH_CONTENT_ID_A;
			if (pattachment->proplist.set(tag, newval) != 0) {
				pmime_enum->b_result = FALSE;
				return;
			}
		}
	}
	if (TRUE == mime_get_field(pmime, "Content-Location", tmp_buff, 1024)) {
		uint32_t tag = oxcmail_check_ascii(tmp_buff) ?
		                  PR_ATTACH_CONTENT_LOCATION :
		                  PR_ATTACH_CONTENT_LOCATION_A;
		if (pattachment->proplist.set(tag, tmp_buff) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
	}
	if (TRUE == mime_get_field(pmime, "Content-Base", tmp_buff, 1024)) {
		uint32_t tag = oxcmail_check_ascii(tmp_buff) ?
		                  PR_ATTACH_CONTENT_BASE : PR_ATTACH_CONTENT_BASE_A;
		if (pattachment->proplist.set(tag, tmp_buff) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
	}
	if (b_inline) {
		if (0 != strcasecmp("image/jpeg",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/jpg",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/pjpeg",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/gif",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/bmp",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/png",
			mime_get_content_type(pmime)) &&
			0 != strcasecmp("image/x-png",
			mime_get_content_type(pmime))) {
			b_inline = false;
		}
	}
	if (b_inline) {
		if (NULL == mime_get_parent(pmime) ||
			(0 != strcasecmp(mime_get_content_type(
			mime_get_parent(pmime)), "multipart/related") &&
			0 != strcasecmp(mime_get_content_type(
			mime_get_parent(pmime)), "multipart/mixed"))) {
			b_inline = false;
		}
	}
	if (b_inline) {
		tmp_int32 = ATT_MHTML_REF;
		if (pattachment->proplist.set(PR_ATTACH_FLAGS, &tmp_int32) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
	}
	/* Content-Type is multipart/appledouble */
	if (NULL != pmime1) {
		tmp_int32 = ATTACH_BY_VALUE;
		if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
		pmime_enum->b_result = oxcmail_parse_appledouble(
			pmime1, pattachment, b_filename, b_description,
			pmime_enum->alloc, &pmime_enum->last_propid,
			pmime_enum->phash);
		return;
	}
	if (0 == strcasecmp("text/directory",  mime_get_content_type(pmime))) {
		auto rdlength = mime_get_length(pmime);
		if (rdlength < 0) {
			printf("%s:mime_get_length:%u: unsuccessful\n", __func__, __LINE__);
			pmime_enum->b_result = false;
			return;
		}
		size_t content_len = rdlength;
		if (content_len < VCARD_MAX_BUFFER_LEN) {
			std::unique_ptr<char[], stdlib_delete> pcontent(static_cast<char *>(malloc(3 * content_len + 2)));
			if (NULL == pcontent) {
				pmime_enum->b_result = FALSE;
				return;
			}
			if (!mime_read_content(pmime, pcontent.get(), &content_len)) {
				pmime_enum->b_result = FALSE;
				return;
			}
			pcontent[content_len] = '\0';
			if (FALSE == oxcmail_get_content_param(
				pmime, "charset", mime_charset, 32)) {
				gx_strlcpy(mime_charset, !utf8_check(pcontent.get()) ?
					pmime_enum->charset : "utf-8", GX_ARRAY_SIZE(mime_charset));
			}
			if (string_to_utf8(mime_charset, pcontent.get(), pcontent.get() + content_len + 1)) {
				if (!utf8_check(pcontent.get() + content_len + 1))
					utf8_filter(pcontent.get() + content_len + 1);
				vcard_init(&vcard);
				if (vcard_retrieve(&vcard, pcontent.get() + content_len + 1) &&
				    (pmsg = oxvcard_import(&vcard, pmime_enum->get_propids)) != nullptr) {
					attachment_content_set_embedded_internal(pattachment, pmsg);
					tmp_int32 = ATTACH_EMBEDDED_MSG;
					if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0)
						pmime_enum->b_result = FALSE;
					vcard_free(&vcard);
					/* parsed successfully */
					return;
				}
				vcard_free(&vcard);
			}
			/* parsing as vcard failed */
			tmp_int32 = ATTACH_BY_VALUE;
			if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0) {
				pmime_enum->b_result = FALSE;
				return;
			}
			tmp_bin.cb = content_len;
			tmp_bin.pc = pcontent.get();
			pmime_enum->b_result = pattachment->proplist.set(PR_ATTACH_DATA_BIN, &tmp_bin) == 0 ? TRUE : false;
			return;
		}
	}
	if (0 == strcasecmp("message/rfc822", mime_get_content_type(pmime)) ||
		(TRUE == b_filename && 0 == strcasecmp(".eml", extension))) {
		auto rdlength = mime_get_length(pmime);
		if (rdlength < 0) {
			printf("%s:mime_get_length:%u: unsuccessful\n", __func__, __LINE__);
			pmime_enum->b_result = false;
			return;
		}
		size_t content_len = rdlength;
		std::unique_ptr<char[], stdlib_delete> pcontent(static_cast<char *>(malloc(content_len)));
		if (NULL == pcontent) {
			pmime_enum->b_result = FALSE;
			return;
		}
		if (!mime_read_content(pmime, pcontent.get(), &content_len)) {
			pmime_enum->b_result = FALSE;
			return;
		}
		MAIL mail(pmime_enum->pmime_pool);
		if (mail.retrieve(pcontent.get(), content_len)) {
			pattachment->proplist.erase(PR_ATTACH_LONG_FILENAME);
			pattachment->proplist.erase(PR_ATTACH_LONG_FILENAME_A);
			pattachment->proplist.erase(PR_ATTACH_EXTENSION);
			pattachment->proplist.erase(PR_ATTACH_EXTENSION_A);
			if (!b_description &&
			    mime_get_field(mail.get_head(), "Subject", tmp_buff, 256) &&
			    mime_string_to_utf8(pmime_enum->charset, tmp_buff, file_name) &&
			    pattachment->proplist.set(PR_DISPLAY_NAME, file_name) != 0) {
				pmime_enum->b_result = FALSE;
				return;
			}
			tmp_int32 = ATTACH_EMBEDDED_MSG;
			if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0) {
				pmime_enum->b_result = FALSE;
				return;
			}
			pmsg = oxcmail_import(pmime_enum->charset,
				pmime_enum->str_zone, &mail,
				pmime_enum->alloc, pmime_enum->get_propids);
			if (NULL == pmsg) {
				pmime_enum->b_result = FALSE;
				return;
			}
			attachment_content_set_embedded_internal(pattachment, pmsg);
			return;
		}
	}
	if (TRUE == b_filename && 0 == strcasecmp(
		"message/external-body", mime_get_content_type(pmime)) &&
		TRUE == oxcmail_get_content_param(pmime, "access-type",
		tmp_buff, 32) && 0 == strcasecmp(tmp_buff, "anon-ftp") &&
		TRUE == oxcmail_get_content_param(
		pmime, "site", site_buff, 256) &&
		TRUE == oxcmail_get_content_param(
		pmime, "directory", dir_buff, 256)) {
		if (FALSE == oxcmail_get_content_param(
			pmime, "mode", mode_buff, 32)) {
			mode_buff[0] = '\0';
		}
		if (0 == strcasecmp(mode_buff, "ascii")) {
			strcpy(mode_buff, ";type=a");
		} else if (0 == strcasecmp(mode_buff, "image")) {
			strcpy(mode_buff, ";type=i");
		}
		tmp_bin.cb = gx_snprintf(tmp_buff, GX_ARRAY_SIZE(tmp_buff), "[InternetShortcut]\r\n"
					"URL=ftp://%s/%s/%s%s", site_buff, dir_buff,
					file_name, mode_buff);
		tmp_bin.pc = mode_buff;
		if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, &tmp_bin) != 0) {
			pmime_enum->b_result = FALSE;
			return;
		}
		ptoken = strrchr(file_name, '.');
		if (NULL != ptoken) {
			strcpy(ptoken + 1, "URL");
		} else {
			strcat(file_name, ".URL");
		}
		uint32_t tag = b_unifn ? PR_ATTACH_LONG_FILENAME : PR_ATTACH_LONG_FILENAME_A;
		if (pattachment->proplist.set(tag, file_name) != 0)
			pmime_enum->b_result = FALSE;
		return;
	}
	tmp_int32 = ATTACH_BY_VALUE;
	if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (0 == strcasecmp("application/mac-binhex40",
		mime_get_content_type(pmime))) {
		pmime_enum->b_result = oxcmail_parse_binhex(
			pmime, pattachment, b_filename, b_description,
			&pmime_enum->last_propid, pmime_enum->phash);
		return;
	} else if (0 == strcasecmp("application/applefile",
		mime_get_content_type(pmime))) {
		if (TRUE == oxcmail_parse_applesingle(
			pmime, pattachment, b_filename, b_description,
			pmime_enum->alloc, &pmime_enum->last_propid,
			pmime_enum->phash)) {
			return;
		}
	}
	if (0 == strncasecmp(mime_get_content_type(pmime), "text/", 5)) {
		if (TRUE == oxcmail_get_content_param(
			pmime, "charset", tmp_buff, 32)) {
			uint32_t tag = oxcmail_check_ascii(tmp_buff) ?
			                  PROP_TAG_TEXTATTACHMENTCHARSET :
			                  PROP_TAG_TEXTATTACHMENTCHARSET_STRING8;
			if (pattachment->proplist.set(tag, tmp_buff) != 0) {
				pmime_enum->b_result = FALSE;
				return;
			}
		}
	}
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length:%u: unsuccessful\n", __func__, __LINE__);
		pmime_enum->b_result = false;
		return;
	}
	size_t content_len = rdlength;
	std::unique_ptr<char[], stdlib_delete> pcontent(static_cast<char *>(malloc(content_len)));
	if (NULL == pcontent) {
		pmime_enum->b_result = FALSE;
		return;
	}
	if (!mime_read_content(pmime, pcontent.get(), &content_len)) {
		pmime_enum->b_result = FALSE;
		return;
	}
	tmp_bin.cb = content_len;
	tmp_bin.pc = pcontent.get();
	pmime_enum->b_result = pattachment->proplist.set(PR_ATTACH_DATA_BIN,
	                       &tmp_bin) == 0 ? TRUE : false;
}

static MESSAGE_CONTENT* oxcmail_parse_tnef(MIME *pmime,
	EXT_BUFFER_ALLOC alloc, GET_PROPIDS get_propids)
{
	void *pcontent;
	MESSAGE_CONTENT *pmsg;
	
	auto rdlength = mime_get_length(pmime);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return nullptr;
	}
	size_t content_len = rdlength;
	pcontent = malloc(content_len);
	if (NULL == pcontent) {
		return NULL;
	}
	if (!mime_read_content(pmime, static_cast<char *>(pcontent), &content_len)) {
		free(pcontent);
		return NULL;
	}
	pmsg = tnef_deserialize(pcontent, content_len, alloc,
			get_propids, oxcmail_username_to_entryid);
	free(pcontent);
	return pmsg;
}

static void oxcmail_replace_propid(TPROPVAL_ARRAY *pproplist,
    std::unordered_map<uint16_t, uint16_t> &phash)
{
	int i;
	uint16_t propid;
	uint32_t proptag;
	
	for (i=0; i<pproplist->count; i++) {
		proptag = pproplist->ppropval[i].proptag;
		propid = PROP_ID(proptag);
		if (!is_nameprop_id(propid))
			continue;
		auto it = phash.find(propid);
		if (it == phash.cend() || it->second == 0) {
			pproplist->erase(proptag);
			i --;
			continue;
		}
		pproplist->ppropval[i].proptag =
			PROP_TAG(PROP_TYPE(pproplist->ppropval[i].proptag), it->second);
	}
}

static BOOL oxcmail_fetch_propname(MESSAGE_CONTENT *pmsg, namemap &phash,
    EXT_BUFFER_ALLOC alloc, GET_PROPIDS get_propids)
{
	PROPID_ARRAY propids;
	PROPID_ARRAY propids1;
	PROPNAME_ARRAY propnames;
	
	propids.count = 0;
	propids.ppropid = static_cast<uint16_t *>(alloc(sizeof(uint16_t) * phash.size()));
	if (NULL == propids.ppropid) {
		return FALSE;
	}
	propnames.count = 0;
	propnames.ppropname = static_cast<PROPERTY_NAME *>(alloc(sizeof(PROPERTY_NAME) * phash.size()));
	if (NULL == propnames.ppropname) {
		return FALSE;
	}
	for (const auto &pair : phash) {
		propids.ppropid[propids.count] = pair.first;
		propnames.ppropname[propnames.count] = pair.second;
		propids.count ++;
		propnames.count ++;
	}
	if (FALSE == get_propids(&propnames, &propids1)) {
		return FALSE;
	}
	std::unordered_map<uint16_t, uint16_t> phash1;
	for (size_t i = 0; i < propids.count; ++i) try {
		phash1.emplace(propids.ppropid[i], propids1.ppropid[i]);
	} catch (const std::bad_alloc &) {
	}
	oxcmail_replace_propid(&pmsg->proplist, phash1);
	if (NULL != pmsg->children.prcpts) {
		for (size_t i = 0; i < pmsg->children.prcpts->count; ++i)
			oxcmail_replace_propid(pmsg->children.prcpts->pparray[i], phash1);
	}
	if (NULL != pmsg->children.pattachments) {
		for (size_t i = 0; i < pmsg->children.pattachments->count; ++i)
			oxcmail_replace_propid(
				&pmsg->children.pattachments->pplist[i]->proplist, phash1);
	}
	return TRUE;
}

static void oxcmail_remove_flag_propties(
	MESSAGE_CONTENT *pmsg, GET_PROPIDS get_propids)
{
	PROPID_ARRAY propids;
	PROPERTY_NAME propname_buff[3];
	PROPNAME_ARRAY propnames;
	
	pmsg->proplist.erase(PROP_TAG_FLAGCOMPLETETIME);
	for (size_t i = 0; i < arsizeof(propname_buff); ++i) {
		propname_buff[i].guid = PSETID_TASK;
		propname_buff[i].kind = MNID_ID;
	}
	propnames.count = 3;
	propnames.ppropname = propname_buff;
	propname_buff[0].lid = PidLidTaskDueDate;
	propname_buff[1].lid = PidLidTaskStartDate;
	propname_buff[2].lid = PidLidTaskDateCompleted;
	if (FALSE == get_propids(&propnames, &propids)) {
		return;
	}
	pmsg->proplist.erase(PROP_TAG(PT_SYSTIME, propids.ppropid[0]));
	pmsg->proplist.erase(PROP_TAG(PT_SYSTIME, propids.ppropid[1]));
	pmsg->proplist.erase(PROP_TAG(PT_SYSTIME, propids.ppropid[2]));
}

static BOOL oxcmail_copy_message_proplist(
	MESSAGE_CONTENT *pmsg, MESSAGE_CONTENT *pmsg1)
{
	int i;
	
	for (i=0; i<pmsg->proplist.count; i++) {
		if (!pmsg1->proplist.has(pmsg->proplist.ppropval[i].proptag) &&
		    pmsg1->proplist.set(pmsg->proplist.ppropval[i]) != 0)
			return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_merge_message_attachments(
	MESSAGE_CONTENT *pmsg, MESSAGE_CONTENT *pmsg1)
{
	if (NULL == pmsg1->children.pattachments) {
		pmsg1->children.pattachments = pmsg->children.pattachments;
		pmsg->children.pattachments = NULL;
		return TRUE;
	}
	while (0 != pmsg->children.pattachments->count) {
		if (FALSE == attachment_list_append_internal(
			pmsg1->children.pattachments,
			pmsg->children.pattachments->pplist[0])) {
			return FALSE;
		}
		pmsg->children.pattachments->count --;
		if (0 == pmsg->children.pattachments->count) {
			return TRUE;
		}
		memmove(pmsg->children.pattachments->pplist,
			pmsg->children.pattachments->pplist + 1,
			sizeof(void*)*pmsg->children.pattachments->count);
	}
	return TRUE;
}

static bool oxcmail_enum_dsn_action_field(const char *tag,
    const char *value, void *pparam)
{
	int severity;
	
	if (0 != strcasecmp("Action", tag)) {
		return true;
	}
	if (0 == strcasecmp("delivered", value)) {
		severity = 0;
	} else if (0 == strcasecmp("expanded", value)) {
		severity = 1;
	} else if (0 == strcasecmp("relayed", value)) {
		severity = 2;
	} else if (0 == strcasecmp("delayed", value)) {
		severity = 3;
	} else if (0 == strcasecmp("failed", value)) {
		severity = 4;
	} else {
		return true;
	}
	if (severity > *(int*)pparam) {
		*(int*)pparam = severity;
	}
	return true;
}

static bool oxcmail_enum_dsn_action_fields(DSN_FIELDS *pfields, void *pparam)
{
	return dsn_enum_fields(pfields, oxcmail_enum_dsn_action_field, pparam);
}

static bool oxcmail_enum_dsn_rcpt_field(const char *tag,
    const char *value, void *pparam)
{
	DSN_FILEDS_INFO *pinfo;
	
	pinfo = (DSN_FILEDS_INFO*)pparam;
	if (0 == strcasecmp(tag, "Final-Recipient") &&
		0 == strncasecmp(value, "rfc822;", 7)) {
		gx_strlcpy(pinfo->final_recipient, value + 7, GX_ARRAY_SIZE(pinfo->final_recipient));
		HX_strrtrim(pinfo->final_recipient);
		HX_strltrim(pinfo->final_recipient);
	} else if (0 == strcasecmp(tag, "Action")) {
		if (0 == strcasecmp("delivered", value)) {
			pinfo->action_severity = 0;
		} else if (0 == strcasecmp("expanded", value)) {
			pinfo->action_severity = 1;
		} else if (0 == strcasecmp("relayed", value)) {
			pinfo->action_severity = 2;
		} else if (0 == strcasecmp("delayed", value)) {
			pinfo->action_severity = 3;
		} else if (0 == strcasecmp("failed", value)) {
			pinfo->action_severity = 4;
		}
	} else if (0 == strcasecmp(tag, "Status")) {
		pinfo->status = value;
	} else if (0 == strcasecmp(tag, "Diagnostic-Code")) {
		pinfo->diagnostic_code = value;
	} else if (0 == strcasecmp(tag, "Remote-MTA")) {
		gx_strlcpy(pinfo->remote_mta, value, GX_ARRAY_SIZE(pinfo->remote_mta));
	} else if (0 == strcasecmp(tag, "X-Supplementary-Info")) {
		pinfo->x_supplementary_info = value;
	} else if (0 == strcasecmp(tag, "X-Display-Name")) {
		pinfo->x_display_name = value;
	}
	return true;
}

static bool oxcmail_enum_dsn_rcpt_fields(DSN_FIELDS *pfields, void *pparam)
{
	int kind;
	int tmp_len;
	char *ptoken1;
	char *ptoken2;
	BINARY tmp_bin;
	char essdn[1280];
	char tmp_buff[1280];
	uint32_t status_code;
	DSN_ENUM_INFO *pinfo;
	uint32_t reason_code;
	DSN_FILEDS_INFO f_info;
	char display_name[512];
	uint32_t diagnostic_code;
	TPROPVAL_ARRAY *pproplist;
	
	pinfo = (DSN_ENUM_INFO*)pparam;
	f_info.final_recipient[0] = '\0';
	f_info.action_severity = -1;
	f_info.remote_mta[0] = '\0';
	f_info.status = NULL;
	f_info.diagnostic_code = NULL;
	f_info.x_supplementary_info = NULL;
	f_info.x_display_name = NULL;
	dsn_enum_fields(pfields, oxcmail_enum_dsn_rcpt_field, &f_info);
	if (f_info.action_severity < pinfo->action_severity ||
		'\0' == f_info.final_recipient[0] || NULL == f_info.status) {
		return true;
	}
	strncpy(tmp_buff, f_info.status, 1024);
	ptoken1 = strchr(tmp_buff, '.');
	if (NULL == ptoken1) {
		return true;
	}
	*ptoken1 = '\0';
	if (1 != strlen(tmp_buff)) {
		return true;
	}
	if ('2' == tmp_buff[0]) {
		kind = 2;
	} else if ('4' == tmp_buff[0]) {
		kind = 4;
	} else if ('5' == tmp_buff[0]) {
		kind = 5;
	} else {
		return true;
	}
	ptoken1 ++;
	ptoken2 = strchr(ptoken1, '.');
	if (NULL == ptoken2) {
		return true;
	}
	*ptoken2 = '\0';
	tmp_len = strlen(ptoken1);
	if (tmp_len < 1 || tmp_len > 3) {
		return true;
	}
	int subject = strtol(ptoken1, nullptr, 0);
	if (subject > 9 || subject < 0) {
		subject = 0;
	}
	ptoken2 ++;
	tmp_len = strlen(ptoken2);
	if (tmp_len < 1 || tmp_len > 3) {
		return true;
	}
	int detail = strtol(ptoken2, nullptr, 0);
	if (detail > 9 || detail < 0) {
		detail = 0;
	}
	pproplist = tpropval_array_init();
	if (NULL == pproplist) {
		return false;
	}
	if (!tarray_set_append_internal(pinfo->prcpts, pproplist)) {
		tpropval_array_free(pproplist);
		return false;
	}
	uint32_t tmp_int32 = MAPI_TO;
	if (pproplist->set(PR_RECIPIENT_TYPE, &tmp_int32) != 0)
		return false;
	if (NULL != f_info.x_display_name) {
		if (strlen(f_info.x_display_name) < 256 &&
			TRUE == mime_string_to_utf8("utf-8",
			f_info.x_display_name, display_name)) {
			if (pproplist->set(PR_DISPLAY_NAME, display_name) != 0)
				return false;
		}
	}
	auto dtypx = DT_MAILUSER;
	if (!oxcmail_username_to_essdn(f_info.final_recipient, essdn, &dtypx)) {
		essdn[0] = '\0';
		dtypx = DT_MAILUSER;
		tmp_bin.cb = snprintf(tmp_buff, arsizeof(tmp_buff), "SMTP:%s",
					f_info.final_recipient) + 1;
		HX_strupper(tmp_buff);
		if (pproplist->set(PR_ADDRTYPE, "SMTP") != 0 ||
		    pproplist->set(PR_EMAIL_ADDRESS, f_info.final_recipient) != 0)
			return false;
	} else {
		tmp_bin.cb = gx_snprintf(tmp_buff, GX_ARRAY_SIZE(tmp_buff), "EX:%s", essdn) + 1;
		if (pproplist->set(PR_ADDRTYPE, "EX") != 0 ||
		    pproplist->set(PR_EMAIL_ADDRESS, essdn) != 0)
			return false;
	}
	if (pproplist->set(PR_SMTP_ADDRESS, f_info.final_recipient) != 0)
		return false;
	tmp_bin.pc = tmp_buff;
	if (pproplist->set(PR_SEARCH_KEY, &tmp_bin) != 0)
		return false;
	tmp_bin.cb = 0;
	tmp_bin.pc = tmp_buff;
	if ('\0' == essdn[0]) {
		if (FALSE == oxcmail_username_to_oneoff(
			f_info.final_recipient, display_name, &tmp_bin)) {
			return false;
		}
	} else {
		if (FALSE == oxcmail_essdn_to_entryid(essdn, &tmp_bin)) {
			return false;
		}
	}
	if (pproplist->set(PR_ENTRYID, &tmp_bin) != 0 ||
	    pproplist->set(PR_RECIPIENT_ENTRYID, &tmp_bin) != 0 ||
	    pproplist->set(PR_RECORD_KEY, &tmp_bin) != 0)
		return false;
	tmp_int32 = dtypx == DT_DISTLIST ? MAPI_DISTLIST : MAPI_MAILUSER;
	if (pproplist->set(PR_OBJECT_TYPE, &tmp_int32) != 0)
		return false;
	tmp_int32 = static_cast<uint32_t>(dtypx);
	if (pproplist->set(PR_DISPLAY_TYPE, &tmp_int32) != 0)
		return false;
	tmp_int32 = 1;
	if (pproplist->set(PROP_TAG_RECIPIENTFLAGS, &tmp_int32) != 0)
		return false;
	if (f_info.remote_mta[0] != '\0' &&
	    pproplist->set(PROP_TAG_REMOTEMESSAGETRANSFERAGENT, f_info.remote_mta) != 0)
		return false;
	if (pproplist->set(PROP_TAG_REPORTTIME, &pinfo->submit_time) != 0)
		return false;
	if (NULL != f_info.x_supplementary_info) {
		if (pproplist->set(PR_SUPPLEMENTARY_INFO, f_info.x_supplementary_info) != 0)
			return false;
	} else {
		if (NULL == f_info.diagnostic_code) {
			snprintf(tmp_buff, 1024, "<%s #%s>",
				f_info.remote_mta, f_info.status);
		} else {
			snprintf(tmp_buff, 1024, "<%s #%s %s>",
				f_info.remote_mta, f_info.status,
				f_info.diagnostic_code);
		}
		if (pproplist->set(PR_SUPPLEMENTARY_INFO, tmp_buff) != 0)
			return false;
	}
	status_code = 100*kind + 10*subject + detail;
	if (pproplist->set(PROP_TAG_NONDELIVERYREPORTSTATUSCODE, &status_code) != 0)
		return false;
	reason_code = 0;
	diagnostic_code = -1;
	switch (subject) {
	case 1:
		switch (detail) {
		case 1:
			diagnostic_code = 35;
			reason_code = 1;
			break;
		case 2:
			diagnostic_code = 48;
			break;
		case 3:
			diagnostic_code = 32;
			break;
		case 4:
			diagnostic_code = 1;
			break;
		case 6:
			diagnostic_code = 40;
			break;
		default:
			diagnostic_code = 0;
		}
		break;
	case 2:
		switch (detail) {
		case 2:
		case 3:
			diagnostic_code = 13;
			break;
		case 4:
			diagnostic_code = 30;
			break;
		default:
			diagnostic_code = 38;
			break;
		}
		break;
	case 3:
		switch (detail) {
		case 2:
			break;
		case 3:
		case 5:
			diagnostic_code = 18;
			break;
		case 4:
			diagnostic_code = 13;
			break;
		default:
			diagnostic_code = 38;
			break;
		}
		break;
	case 4:
		switch (detail) {
		case 0:
		case 4:
			break;
		case 3:
			reason_code = 6;
			break;
		case 6:
		case 8:
			diagnostic_code = 3;
			break;
		case 7:
			diagnostic_code = 5;
			break;
		default:
			diagnostic_code = 2;
			break;
		}
		break;
	case 5:
		switch (detail) {
		case 3:
			diagnostic_code = 16;
			break;
		case 4:
			diagnostic_code = 11;
			break;
		default:
			diagnostic_code = 17;
			break;
		}
		break;
	case 6:
		switch (detail) {
		case 2:
			diagnostic_code = 9;
			break;
		case 3:
			diagnostic_code = 8;
			break;
		case 4:
			diagnostic_code = 25;
			break;
		case 5:
			reason_code = 2;
			break;
		default:
			diagnostic_code = 15;
			break;
			
		}
		break;
	case 7:
		switch (detail) {
		case 1:
			diagnostic_code = 29;
			break;
		case 2:
			diagnostic_code = 28;
			break;
		case 3:
			diagnostic_code = 26;
			break;
		default:
			diagnostic_code = 46;
			break;
		}
		break;
	}
	if (pproplist->set(PROP_TAG_NONDELIVERYREPORTDIAGCODE, &diagnostic_code) != 0 ||
	    pproplist->set(PROP_TAG_NONDELIVERYREPORTREASONCODE, &reason_code) != 0)
		return false;
	return true;
}

static bool oxcmail_enum_dsn_reporting_mta(const char *tag,
    const char *value, void *pparam)
{
	if (strcasecmp(tag, "Reporting-MTA") != 0)
		return true;
	return static_cast<MESSAGE_CONTENT *>(pparam)->proplist.set(PROP_TAG_REPORTINGMESSAGETRANSFERAGENT, value) == 0;
}

static MIME* oxcmail_parse_dsn(MAIL *pmail, MESSAGE_CONTENT *pmsg)
{
	DSN dsn;
	void *pvalue;
	size_t content_len;
	DSN_ENUM_INFO dsn_info;
	char tmp_buff[256*1024];
	
	auto pmime = pmail->get_head();
	pmime = mime_get_child(pmime);
	if (NULL == pmime) {
		return NULL;
	}
	do {
		if (0 == strcasecmp("message/delivery-status",
			mime_get_content_type(pmime))) {
			break;
		}
	} while ((pmime = mime_get_sibling(pmime)) != nullptr);
	if (NULL == pmime) {
		return NULL;
	}
	auto mgl = mime_get_length(pmime);
	if (mgl < 0 || static_cast<size_t>(mgl) > sizeof(tmp_buff))
		return NULL;
	content_len = sizeof(tmp_buff);
	if (FALSE == mime_read_content(pmime, tmp_buff, &content_len)) {
		return NULL;
	}
	dsn_init(&dsn);
	if (!dsn_retrieve(&dsn, tmp_buff, content_len)) {
		dsn_free(&dsn);
		return NULL;
	}
	dsn_info.action_severity = -1;
	dsn_enum_rcpts_fields(&dsn,
		oxcmail_enum_dsn_action_fields,
		&dsn_info.action_severity);
	if (-1 == dsn_info.action_severity) {
		dsn_free(&dsn);
		return NULL;
	}
	dsn_info.prcpts = tarray_set_init();
	if (NULL == dsn_info.prcpts) {
		dsn_free(&dsn);
		return NULL;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_CLIENTSUBMITTIME);
	if (NULL == pvalue) {
		dsn_info.submit_time = rop_util_unix_to_nttime(time(NULL));
	} else {
		dsn_info.submit_time = *(uint64_t*)pvalue;
	}
	if (!dsn_enum_rcpts_fields(&dsn,
	    oxcmail_enum_dsn_rcpt_fields, &dsn_info)) {
		tarray_set_free(dsn_info.prcpts);
		dsn_free(&dsn);
		return NULL;
	}
	message_content_set_rcpts_internal(pmsg, dsn_info.prcpts);
	if (!dsn_enum_fields(dsn_get_message_fileds(&dsn),
	    oxcmail_enum_dsn_reporting_mta, pmsg)) {
		dsn_free(&dsn);
		return NULL;
	}
	switch (dsn_info.action_severity) {
	case 0:
		strcpy(tmp_buff, "REPORT.IPM.Note.DR");
		break;
	case 1:
		strcpy(tmp_buff, "REPORT.IPM.Note.Expanded.DR");
		break;
	case 2:
		strcpy(tmp_buff, "REPORT.IPM.Note.Relayed.DR");
		break;
	case 3:
		strcpy(tmp_buff, "REPORT.IPM.Note.Delayed.DR");
		break;
	case 4:
		strcpy(tmp_buff, "REPORT.IPM.Note.NDR");
		break;
	}
	if (pmsg->proplist.set(PR_MESSAGE_CLASS, tmp_buff) != 0) {
		dsn_free(&dsn);
		return NULL;
	}
	dsn_free(&dsn);
	return pmime;
}

static bool oxcmail_enum_mdn(const char *tag,
	const char *value, void *pparam)
{
	size_t len;
	char *ptoken;
	BINARY tmp_bin;
	char tmp_buff[1024];
	auto mcparam = static_cast<MESSAGE_CONTENT *>(pparam);
	
	if (0 == strcasecmp(tag, "Original-Recipient")) {
		if (strncasecmp(value, "rfc822;", 7) == 0 &&
		    mcparam->proplist.set(PROP_TAG_ORIGINALDISPLAYTO, value + 7) != 0)
			return false;
	} else if (0 == strcasecmp(tag, "Final-Recipient")) {
		if (strncasecmp(value, "rfc822;", 7) == 0 &&
		    !static_cast<MESSAGE_CONTENT *>(pparam)->proplist.has(PROP_TAG_ORIGINALDISPLAYTO))
			return mcparam->proplist.set(PROP_TAG_ORIGINALDISPLAYTO, value + 7) == 0;
	} else if (0 == strcasecmp(tag, "Disposition")) {
		auto ptoken2 = strchr(value, ';');
		if (ptoken2 == nullptr)
			return true;
		++ptoken2;
		gx_strlcpy(tmp_buff, ptoken2, GX_ARRAY_SIZE(tmp_buff));
		HX_strltrim(tmp_buff);
		ptoken = strchr(tmp_buff, '/');
		if (NULL != ptoken) {
			*ptoken = '\0';
		}
		if (0 == strcasecmp(tmp_buff, "displayed") ||
			0 == strcasecmp(tmp_buff, "dispatched") ||
			0 == strcasecmp(tmp_buff, "processed")) {
			strcpy(tmp_buff, "REPORT.IPM.Note.IPNRN");
		} else if (0 == strcasecmp(tmp_buff, "deleted") ||
			0 == strcasecmp(tmp_buff, "denied") ||
			0 == strcasecmp(tmp_buff, "failed")) {
			snprintf(tmp_buff, arsizeof(tmp_buff), "REPORT.IPM.Note.IPNNRN");
		} else {
			return true;
		}
		return mcparam->proplist.set(PR_MESSAGE_CLASS, tmp_buff) == 0 &&
		       mcparam->proplist.set(PROP_TAG_REPORTTEXT, value) == 0;
	} else if (0 == strcasecmp(tag, "X-MSExch-Correlation-Key")) {
		len = strlen(value);
		if (len <= 1024 && 0 == decode64(value, len, tmp_buff, &len)) {
			tmp_bin.pc = tmp_buff;
			tmp_bin.cb = len;
			return mcparam->proplist.set(PR_PARENT_KEY, &tmp_bin) == 0;
		}
	} else if (0 == strcasecmp(tag, "Original-Message-ID")) {
		return mcparam->proplist.set(PROP_TAG_ORIGINALMESSAGEID, value) == 0 &&
		       mcparam->proplist.set(PROP_TAG_INTERNETREFERENCES, value) == 0;
	} else if (0 == strcasecmp(tag, "X-Display-Name")) {
		if (TRUE == mime_string_to_utf8("utf-8", value, tmp_buff)) {
			return mcparam->proplist.set(PR_DISPLAY_NAME, tmp_buff) == 0;
		}
		return mcparam->proplist.set(PR_DISPLAY_NAME_A, value) == 0;
	}
	return true;
}

static MIME* oxcmail_parse_mdn(MAIL *pmail, MESSAGE_CONTENT *pmsg)
{
	DSN dsn;
	void *pvalue;
	size_t content_len;
	char tmp_buff[256*1024];
	
	auto pmime = pmail->get_head();
	if (0 != strcasecmp("message/disposition-notification",
		mime_get_content_type(pmime))) {
		pmime = mime_get_child(pmime);
		if (NULL == pmime) {
			return NULL;
		}
		do {
			if (0 == strcasecmp("message/disposition-notification",
				mime_get_content_type(pmime))) {
				break;
			}
		} while ((pmime = mime_get_sibling(pmime)) != nullptr);
	}
	if (NULL == pmime) {
		return NULL;
	}
	auto mgl = mime_get_length(pmime);
	if (mgl < 0 || static_cast<size_t>(mgl) > sizeof(tmp_buff))
		return NULL;
	content_len = sizeof(tmp_buff);
	if (FALSE == mime_read_content(pmime, tmp_buff, &content_len)) {
		return NULL;
	}
	dsn_init(&dsn);
	if (!dsn_retrieve(&dsn, tmp_buff, content_len)) {
		dsn_free(&dsn);
		return NULL;
	}
	if (!dsn_enum_fields(dsn_get_message_fileds(&dsn),
	    oxcmail_enum_mdn, pmsg)) {
		dsn_free(&dsn);
		return NULL;
	}
	dsn_free(&dsn);
	pvalue = pmsg->proplist.getval(PROP_TAG_CLIENTSUBMITTIME);
	if (pmsg->proplist.set(PROP_TAG_ORIGINALDELIVERYTIME, pvalue) != 0 ||
	    pmsg->proplist.set(PROP_TAG_RECEIPTTIME, pvalue) != 0)
		return NULL;
	for (size_t i = 0; i < pmsg->children.prcpts->count; ++i)
		if (pmsg->children.prcpts->pparray[i]->set(PROP_TAG_REPORTTIME, pvalue) != 0)
			return NULL;
	return pmime;
}

static BOOL oxcmail_parse_encrypted(MIME *phead, uint16_t *plast_propid,
    namemap &phash, MESSAGE_CONTENT *pmsg)
{
	char tmp_buff[1024];
	PROPERTY_NAME propname;
	
	if (FALSE == mime_get_field(phead,
		"Content-Type", tmp_buff, 1024)) {
		return FALSE;
	}
	propname.guid = PS_INTERNET_HEADERS;
	propname.kind = MNID_STRING;
	propname.pname = deconst("Content-Type");
	if (namemap_add(phash, *plast_propid, std::move(propname)) != 0 ||
	    pmsg->proplist.set(PROP_TAG(PT_UNICODE, *plast_propid), tmp_buff) != 0)
		return FALSE;
	(*plast_propid) ++;
	return TRUE;
}

static BOOL oxcmail_parse_smime_message(
	MAIL *pmail, MESSAGE_CONTENT *pmsg)
{
	size_t offset;
	BINARY tmp_bin;
	uint32_t tmp_int32;
	const char *content_type;
	ATTACHMENT_CONTENT *pattachment;
	
	auto phead = pmail->get_head();
	if (NULL == phead) {
		return FALSE;
	}
	auto rdlength = mime_get_length(phead);
	if (rdlength < 0) {
		printf("%s:mime_get_length: unsuccessful\n", __func__);
		return false;
	}
	size_t content_len = rdlength;
	auto pcontent = static_cast<char *>(malloc(content_len + 1024));
	if (NULL == pcontent) {
		return FALSE;
	}
	content_type = mime_get_content_type(phead);
	if (0 == strcasecmp(content_type, "multipart/signed")) {
		memcpy(pcontent, "Content-Type: ", 14);
		offset = 14;
		if (FALSE == mime_get_field(phead, "Content-Type",
			pcontent + offset, 1024 - offset)) {
			free(pcontent);
			return FALSE;
		}
		offset += strlen(pcontent + offset);
		memcpy(pcontent + offset, "\r\n\r\n", 4);
		offset += 4;
		if (FALSE == mime_read_content(phead,
			pcontent + offset, &content_len)) {
			free(pcontent);
			return FALSE;
		}
		offset += content_len;
	} else {
		if (FALSE == mime_read_content(phead,
			pcontent, &content_len)) {
			free(pcontent);
			return FALSE;
		}
		offset = content_len;
	}
	pattachment = attachment_content_init();
	if (NULL == pattachment) {
		free(pcontent);
		return FALSE;
	}
	if (FALSE == attachment_list_append_internal(
		pmsg->children.pattachments, pattachment)) {
		attachment_content_free(pattachment);
		free(pcontent);
		return FALSE;
	}
	tmp_bin.cb = offset;
	tmp_bin.pc = pcontent;
	if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, &tmp_bin) != 0) {
		free(pcontent);
		return FALSE;
	}
	free(pcontent);
	tmp_int32 = ATTACH_BY_VALUE;
	if (pattachment->proplist.set(PR_ATTACH_METHOD, &tmp_int32) != 0 ||
	    pattachment->proplist.set(PR_ATTACH_MIME_TAG, content_type) != 0 ||
	    pattachment->proplist.set(PR_ATTACH_EXTENSION, ".p7m") != 0 ||
	    pattachment->proplist.set(PR_ATTACH_FILENAME, "SMIME.p7m") != 0 ||
	    pattachment->proplist.set(PR_ATTACH_LONG_FILENAME, "SMIME.p7m") != 0 ||
	    pattachment->proplist.set(PR_DISPLAY_NAME, "SMIME.p7m") != 0)
		return FALSE;
	return TRUE;
}

static BOOL oxcmail_try_assign_propval(TPROPVAL_ARRAY *pproplist,
	uint32_t proptag1, uint32_t proptag2)
{
	void *pvalue;
	
	if (pproplist->has(proptag1))
		return TRUE;
	pvalue = pproplist->getval(proptag2);
	if (NULL == pvalue) {
		return TRUE;
	}
	return pproplist->set(proptag1, pvalue) == 0 ? TRUE : false;
}

static bool atx_is_hidden(const TPROPVAL_ARRAY &props)
{
	auto v = props.getval(PR_ATTACH_FLAGS);
	if (v != nullptr && (*static_cast<uint32_t *>(v) & ATT_MHTML_REF))
		return true;
	v = props.getval(PR_ATTACHMENT_HIDDEN);
	if (v != nullptr && *static_cast<uint8_t *>(v) != 0)
		return true;
	return false;
}

static bool atxlist_all_hidden(const ATTACHMENT_LIST *atl)
{
	if (atl == nullptr || atl->count == 0)
		return false;
	for (size_t i = 0; i < atl->count; ++i)
		if (!atx_is_hidden(atl->pplist[i]->proplist))
			return false;
	return true;
}

static inline bool tnef_vfy_get_field(MIME *head, char *buf, size_t z)
{
#ifdef VERIFY_TNEF_CORRELATOR
	return mime_get_field(head, "X-MS-TNEF-Correlator", buf, z);
#else
	return true;
#endif
}

static inline bool tnef_vfy_check_key(MESSAGE_CONTENT *msg, const char *xtnefcorrel)
{
#ifdef VERIFY_TNEF_CORRELATOR
	auto key = msg->proplist.get<BINARY>(PR_TNEF_CORRELATION_KEY);
	return key != nullptr && strncmp(key->pb, xtnefcorrel, key->cb) == 0);
#else
	return true;
#endif
}

MESSAGE_CONTENT* oxcmail_import(const char *charset,
	const char *str_zone, MAIL *pmail,
	EXT_BUFFER_ALLOC alloc, GET_PROPIDS get_propids)
{
	int i;
	ICAL ical;
	MIME *pmime;
	MIME *pmime1;
	BOOL b_smime;
	void *pvalue;
	char *pcontent;
	uint8_t tmp_byte;
	BINARY *phtml_bin;
	TARRAY_SET *prcpts;
	uint32_t tmp_int32;
	char tmp_buff[256];
	BOOL b_alternative;
	PROPID_ARRAY propids;
	const char *encoding;
	char mime_charset[64];
	MESSAGE_CONTENT *pmsg;
	PROPERTY_NAME propname;
	PROPNAME_ARRAY propnames;
	char default_charset[64];
	namemap phash;
	MIME_ENUM_PARAM mime_enum{phash};
	FIELD_ENUM_PARAM field_param{phash};
	ATTACHMENT_LIST *pattachments;
	
	b_smime = FALSE;
	pmsg = message_content_init();
	if (NULL == pmsg) {
		return NULL;
	}
	/* set default message class */
	if (pmsg->proplist.set(PR_MESSAGE_CLASS, "IPM.Note") != 0) {
		message_content_free(pmsg);
		return NULL;
	}
	prcpts = tarray_set_init();
	if (NULL == prcpts) {
		message_content_free(pmsg);
		return NULL;
	}
	message_content_set_rcpts_internal(pmsg, prcpts);
	if (!pmail->get_charset(default_charset))
		gx_strlcpy(default_charset, charset, GX_ARRAY_SIZE(default_charset));
	field_param.alloc = alloc;
	field_param.pmail = pmail;
	field_param.pmsg = pmsg;
	field_param.charset = default_charset;
	field_param.last_propid = 0x8000;
	field_param.b_flag_del = FALSE;
	auto phead = pmail->get_head();
	if (NULL == phead) {
		message_content_free(pmsg);
		return NULL;
	}
	field_param.b_classified = mime_get_field(phead,
		"X-Microsoft-Classified", tmp_buff, 16);
	if (FALSE == mime_enum_field(phead,
		oxcmail_enum_mail_head, &field_param)) {
		message_content_free(pmsg);
		return NULL;
	}
	if (!pmsg->proplist.has(PR_SENDER_NAME) &&
	    !pmsg->proplist.has(PROP_TAG_SENDERSMTPADDRESS)) {
		if (!oxcmail_try_assign_propval(&pmsg->proplist, PR_SENDER_NAME, PR_SENT_REPRESENTING_NAME) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PROP_TAG_SENDERSMTPADDRESS, PR_SENT_REPRESENTING_SMTP_ADDRESS) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENDER_ADDRTYPE, PR_SENT_REPRESENTING_ADDRTYPE) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENDER_EMAIL_ADDRESS, PR_SENT_REPRESENTING_EMAIL_ADDRESS) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENDER_SEARCH_KEY, PR_SENT_REPRESENTING_SEARCH_KEY) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENDER_ENTRYID, PR_SENT_REPRESENTING_ENTRYID)) {
			message_content_free(pmsg);
			return NULL;
		}
	} else if (!pmsg->proplist.has(PR_SENT_REPRESENTING_NAME) &&
	    !pmsg->proplist.has(PR_SENT_REPRESENTING_SMTP_ADDRESS)) {
		if (!oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_NAME, PR_SENDER_NAME) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_SMTP_ADDRESS, PROP_TAG_SENDERSMTPADDRESS) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_ADDRTYPE, PR_SENDER_ADDRTYPE) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_EMAIL_ADDRESS, PR_SENDER_EMAIL_ADDRESS) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_SEARCH_KEY, PR_SENDER_SEARCH_KEY) ||
		    !oxcmail_try_assign_propval(&pmsg->proplist, PR_SENT_REPRESENTING_ENTRYID, PR_SENDER_ENTRYID)) {
			message_content_free(pmsg);
			return NULL;
		}	
	}
	if (!pmsg->proplist.has(PR_IMPORTANCE)) {
		tmp_int32 = IMPORTANCE_NORMAL;
		if (pmsg->proplist.set(PR_IMPORTANCE, &tmp_int32) != 0) {
			message_content_free(pmsg);
			return NULL;
		}
	}
	if (!pmsg->proplist.has(PR_SENSITIVITY)) {
		tmp_int32 = SENSITIVITY_NONE;
		if (pmsg->proplist.set(PR_SENSITIVITY, &tmp_int32) != 0) {
			message_content_free(pmsg);
			return NULL;
		}
	}
	if (FALSE == oxcmail_parse_transport_message_header(
		phead, &pmsg->proplist)) {
		message_content_free(pmsg);
		return NULL;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_CLIENTSUBMITTIME);
	if (NULL == pvalue) {
		mime_enum.nttime_stamp = rop_util_unix_to_nttime(time(NULL));
		if (pmsg->proplist.set(PROP_TAG_CLIENTSUBMITTIME,
		    &mime_enum.nttime_stamp) != 0) {
			message_content_free(pmsg);
			return NULL;
		}
	} else {
		mime_enum.nttime_stamp = *(uint64_t*)pvalue;
	}
	if (pmsg->proplist.set(PR_CREATION_TIME, &mime_enum.nttime_stamp) != 0 ||
	    pmsg->proplist.set(PR_LAST_MODIFICATION_TIME, &mime_enum.nttime_stamp) != 0) {
		message_content_free(pmsg);
		return NULL;
	}

	if (strcasecmp("application/ms-tnef", mime_get_content_type(phead)) == 0 &&
	    tnef_vfy_get_field(phead, tmp_buff, arsizeof(tmp_buff))) {
	auto pmsg1 = oxcmail_parse_tnef(phead, alloc, get_propids);
	if (pmsg1 != nullptr) {
		auto cl_1 = make_scope_exit([&]() { message_content_free(pmsg1); });
		if (tnef_vfy_check_key(pmsg1, tmp_buff)) {
			if (FALSE == oxcmail_fetch_propname(
				pmsg, phash, alloc, get_propids)) {
				message_content_free(pmsg);
				return NULL;
			}
			if (FALSE == oxcmail_copy_message_proplist(
				pmsg, pmsg1)) {
				message_content_free(pmsg);
				return NULL;
			}
			prcpts = pmsg1->children.prcpts;
			pmsg1->children.prcpts =
				pmsg->children.prcpts;
			pmsg->children.prcpts = prcpts;
			message_content_free(pmsg);
			if (TRUE == field_param.b_flag_del) {
				oxcmail_remove_flag_propties(
				pmsg1, get_propids);
			}
			cl_1.release();
			return pmsg1;
		}
	}
	}
	if (0 == strcasecmp("multipart/report",
		mime_get_content_type(phead)) &&
		TRUE == oxcmail_get_content_param(
		phead, "report-type", tmp_buff, 128) &&
		0 == strcasecmp("delivery-status", tmp_buff)) {
		mime_enum.preport = oxcmail_parse_dsn(pmail, pmsg);
	}
	if ((0 == strcasecmp("multipart/report",
		mime_get_content_type(phead)) &&
		TRUE == oxcmail_get_content_param(
		phead, "report-type", tmp_buff, 128) &&
		0 == strcasecmp("disposition-notification", tmp_buff)) ||
		0 == strcasecmp("message/disposition-notification",
		mime_get_content_type(phead))) {
		mime_enum.preport = oxcmail_parse_mdn(pmail, pmsg);
	}		
	if (0 == strcasecmp("multipart/mixed", mime_get_content_type(phead))) {
		if (mime_get_children_num(phead) == 2 &&
		    (pmime = mime_get_child(phead)) != nullptr &&
		    (pmime1 = mime_get_sibling(pmime)) != nullptr &&
		    strcasecmp("text/plain", mime_get_content_type(pmime)) == 0 &&
		    strcasecmp("application/ms-tnef", mime_get_content_type(pmime1)) == 0 &&
		    tnef_vfy_get_field(phead, tmp_buff, arsizeof(tmp_buff))) {
		auto pmsg1 = oxcmail_parse_tnef(pmime1, alloc, get_propids);
		if (pmsg1 != nullptr) {
			auto cl_1 = make_scope_exit([&]() { message_content_free(pmsg1); });
			if (tnef_vfy_check_key(pmsg1, tmp_buff)) {
				if (FALSE == oxcmail_parse_message_body(
					default_charset, pmime, &pmsg->proplist)
					|| FALSE == oxcmail_fetch_propname(
					pmsg, phash, alloc, get_propids)) {
					message_content_free(pmsg);
					return NULL;
				}
				if (FALSE == oxcmail_copy_message_proplist(
					pmsg, pmsg1)) {
					message_content_free(pmsg);
					return NULL;
				}
				prcpts = pmsg1->children.prcpts;
				pmsg1->children.prcpts =
					pmsg->children.prcpts;
				pmsg->children.prcpts = prcpts;
				message_content_free(pmsg);
				if (TRUE == field_param.b_flag_del) {
					oxcmail_remove_flag_propties(
					pmsg1, get_propids);
				}
				cl_1.release();
				return pmsg1;
			}
		}
		}
	} else if (0 == strcasecmp("multipart/signed",
		mime_get_content_type(phead))) {
		if (pmsg->proplist.set(PR_MESSAGE_CLASS, "IPM.Note.SMIME.MultipartSigned") != 0) {
			message_content_free(pmsg);
			return NULL;
		}
		b_smime = TRUE;
	} else if (0 == strcasecmp("application/pkcs7-mime",
		mime_get_content_type(phead)) ||
		0 == strcasecmp("application/x-pkcs7-mime",
		mime_get_content_type(phead))) {
		if (pmsg->proplist.set(PR_MESSAGE_CLASS, "IPM.Note.SMIME") != 0 ||
		    !oxcmail_parse_encrypted(phead, &field_param.last_propid, phash, pmsg)) {
			message_content_free(pmsg);
			return NULL;
		}
		b_smime = TRUE;
	}
	mime_enum.b_result = TRUE;
	mime_enum.attach_id = 0;
	mime_enum.charset = default_charset;
	mime_enum.str_zone = str_zone;
	mime_enum.get_propids = get_propids;
	mime_enum.alloc = alloc;
	mime_enum.pmime_pool = pmail->pmime_pool;
	mime_enum.pmsg = pmsg;
	mime_enum.phash = phash;
	pmime = phead;
	for (i=0; i<MAXIMUM_SEARCHING_DEPTH; i++) {
		pmime1 = mime_get_child(pmime);
		if (NULL == pmime1) {
			break;
		}
		pmime = pmime1;
	}
	b_alternative = FALSE;
	pmime1 = mime_get_parent(pmime);
	if (NULL != pmime1 && 0 == strcasecmp(
		"multipart/alternative",
		mime_get_content_type(pmime1))) {
		b_alternative = TRUE;
	}
	do {
		if (0 == strcasecmp("text/plain",
			mime_get_content_type(pmime))) {
			if (NULL == mime_enum.pplain) {
				mime_enum.pplain = pmime;
			}
		}
		if (0 == strcasecmp("text/html",
			mime_get_content_type(pmime))) {
			if (NULL == mime_enum.phtml) {
				mime_enum.phtml = pmime;
			}
		}
		if (0 == strcasecmp("text/enriched",
			mime_get_content_type(pmime))) {
			if (NULL == mime_enum.penriched) {
				mime_enum.penriched = pmime;
			}
		}
		if (0 == strcasecmp("text/calendar",
			mime_get_content_type(pmime))) {
			if (NULL == mime_enum.pcalendar) {
				mime_enum.pcalendar = pmime;
			}
		}
		if (TRUE == b_alternative &&
			MULTIPLE_MIME == mime_get_type(pmime)) {
			pmime1 = mime_get_child(pmime);
			while (NULL != pmime1) {
				if (0 == strcasecmp("text/plain",
					mime_get_content_type(pmime1))) {
					if (NULL == mime_enum.pplain) {
						mime_enum.pplain = pmime1;
					}
				}
				if (0 == strcasecmp("text/html",
					mime_get_content_type(pmime1))) {
					if (NULL == mime_enum.phtml) {
						mime_enum.phtml = pmime1;
					}
				}
				if (0 == strcasecmp("text/enriched",
					mime_get_content_type(pmime1))) {
					if (NULL == mime_enum.penriched) {
						mime_enum.penriched = pmime1;
					}
				}
				if (0 == strcasecmp("text/calendar",
					mime_get_content_type(pmime1))) {
					if (NULL == mime_enum.pcalendar) {
						mime_enum.pcalendar = pmime1;
					}
				}
				pmime1 = mime_get_sibling(pmime1);
			}
		}
	} while (b_alternative && (pmime = mime_get_sibling(pmime)) != nullptr);
	
	if (NULL != mime_enum.pplain) {
		if (FALSE == oxcmail_parse_message_body(default_charset,
			mime_enum.pplain, &pmsg->proplist)) {
			message_content_free(pmsg);
			return NULL;
		}
	}
	if (NULL != mime_enum.phtml) {
		if (FALSE == oxcmail_parse_message_body(default_charset,
			mime_enum.phtml, &pmsg->proplist)) {
			message_content_free(pmsg);
			return NULL;
		}
	} else if (NULL != mime_enum.penriched) {
		if (FALSE == oxcmail_parse_message_body(default_charset,
			mime_enum.penriched, &pmsg->proplist)) {
			message_content_free(pmsg);
			return NULL;
		}
	}
	size_t content_len = 0;
	MESSAGE_CONTENT *pmsg1 = nullptr; /* ical */
	if (NULL != mime_enum.pcalendar) {
		auto rdlength = mime_get_length(mime_enum.pcalendar);
		if (rdlength < 0) {
			printf("%s:mime_get_length: unsuccessful\n", __func__);
			message_content_free(pmsg);
			return nullptr;
		}
		content_len = rdlength;
		pcontent = static_cast<char *>(malloc(3 * content_len + 2));
		if (NULL == pcontent) {
			message_content_free(pmsg);
			return NULL;
		}
		if (FALSE == mime_read_content(mime_enum.pcalendar,
			pcontent, &content_len)) {
			free(pcontent);
			message_content_free(pmsg);
			return NULL;
		}
		pcontent[content_len] = '\0';
		if (FALSE == oxcmail_get_content_param(
			mime_enum.pcalendar, "charset",
			mime_charset, 32)) {
			if (FALSE == utf8_check(pcontent)) {
				strcpy(mime_charset, default_charset);
			} else {
				strcpy(mime_charset, "utf-8");
			}
		}
		if (FALSE == string_to_utf8(
			mime_charset, pcontent,
			pcontent + content_len + 1)) {
			mime_enum.pcalendar = NULL;
		} else {
			if (FALSE == utf8_check(pcontent + content_len + 1)) {
				utf8_filter(pcontent + content_len + 1);
			}
			if (ical_init(&ical) < 0) {
				free(pcontent);
				message_content_free(pmsg);
				return nullptr;
			}
			if (!ical_retrieve(&ical, pcontent + content_len + 1) ||
				NULL == (pmsg1 = oxcical_import(
				str_zone, &ical, alloc, get_propids,
				oxcmail_username_to_entryid))) {
				mime_enum.pcalendar = NULL;
			}
		}
		free(pcontent);
	}
	
	pattachments = attachment_list_init();
	if (NULL == pattachments) {
		message_content_free(pmsg);
		if (NULL != mime_enum.pcalendar) {
			message_content_free(pmsg1);
		}
		return NULL;
	}
	message_content_set_attachments_internal(pmsg, pattachments);
	if (TRUE == b_smime) {
		if (FALSE == oxcmail_parse_smime_message(pmail, pmsg)) {
			message_content_free(pmsg);
			return NULL;
		}
	} else {
		mime_enum.last_propid = field_param.last_propid;
		pmail->enum_mime(oxcmail_enum_attachment, &mime_enum);
		if (FALSE == mime_enum.b_result) {
			message_content_free(pmsg);
			if (NULL != mime_enum.pcalendar) {
				message_content_free(pmsg1);
			}
			return NULL;
		}
	}
	if (FALSE == oxcmail_fetch_propname(
		pmsg, phash, alloc, get_propids)) {
		message_content_free(pmsg);
		if (NULL != mime_enum.pcalendar) {
			message_content_free(pmsg1);
		}
		return NULL;
	}
	if (NULL != mime_enum.pcalendar) {
		if (!pmsg1->proplist.has(PR_MESSAGE_CLASS)) {
			/* multiple calendar objects in attachment list */
			if (NULL != pmsg1->children.pattachments) {
				if (FALSE == oxcmail_merge_message_attachments(
					pmsg1, pmsg)) {
					message_content_free(pmsg);
					message_content_free(pmsg1);
					return NULL;
				}
			}
			message_content_free(pmsg1);
		} else {
			if (FALSE == oxcmail_copy_message_proplist(pmsg, pmsg1) ||
				FALSE == oxcmail_merge_message_attachments(pmsg, pmsg1)) {
				message_content_free(pmsg);
				message_content_free(pmsg1);
				return NULL;
			}
			message_content_free(pmsg);
			pmsg = pmsg1;
			/* calendar message object can not be displayed
				correctly without PidTagRtfCompressed convert
				PidTagHtml to PidTagRtfCompressed */
			phtml_bin = pmsg->proplist.get<BINARY>(PROP_TAG_HTML);
			if (NULL != phtml_bin) {
				pvalue = pmsg->proplist.getval(PR_INTERNET_CPID);
				if (NULL == pvalue) {
					tmp_int32 = 65001;
				} else {
					tmp_int32 = *(uint32_t*)pvalue;
				}
				char *rtfout = nullptr;
				if (html_to_rtf(phtml_bin->pv, phtml_bin->cb, tmp_int32,
				    &rtfout, &content_len)) {
					pvalue = rtfcp_compress(rtfout, content_len);
					free(rtfout);
					if (pvalue != nullptr) {
						pmsg->proplist.set(PROP_TAG_RTFCOMPRESSED, pvalue);
						rop_util_free_binary(static_cast<BINARY *>(pvalue));
					}
				}
			}
		}
	}
	if (!pmsg->proplist.has(PR_BODY_W) && !pmsg->proplist.has(PR_BODY_A)) {
		phtml_bin = pmsg->proplist.get<BINARY>(PROP_TAG_HTML);
		if (NULL != phtml_bin) {
			pvalue = pmsg->proplist.getval(PR_INTERNET_CPID);
			if (NULL == pvalue) {
				tmp_int32 = 65001;
			} else {
				tmp_int32 = *(uint32_t*)pvalue;
			}
			std::string plainbuf;
			auto ret = html_to_plain(phtml_bin->pc, phtml_bin->cb, plainbuf);
			if (ret < 0) {
				message_content_free(pmsg);
				return NULL;
			}
			if (ret == 65001) {
				pmsg->proplist.set(PR_BODY_W, plainbuf.data());
			} else {
				pvalue = alloc(3 * plainbuf.size() + 1);
				if (pvalue == nullptr) {
					message_content_free(pmsg);
					return NULL;
				}
				encoding = oxcmail_cpid_to_charset(tmp_int32);
				if (NULL == encoding) {
					encoding = "windows-1252";
				}
				if (string_to_utf8(encoding, plainbuf.c_str(), static_cast<char *>(pvalue)) &&
				    utf8_check(static_cast<char *>(pvalue)))
					pmsg->proplist.set(PR_BODY_W, pvalue);
			}
		}
	}
	if (!pmsg->proplist.has(PROP_TAG_HTML)) {
		pvalue = pmsg->proplist.getval(PR_BODY);
		if (NULL != pvalue) {
			phtml_bin = static_cast<BINARY *>(alloc(sizeof(BINARY)));
			if (NULL == phtml_bin) {
				message_content_free(pmsg);
				return NULL;
			}
			phtml_bin->pc = plain_to_html(static_cast<char *>(pvalue));
			if (phtml_bin->pc == nullptr) {
				message_content_free(pmsg);
				return NULL;
			}
			phtml_bin->cb = strlen(phtml_bin->pc);
			pmsg->proplist.set(PROP_TAG_HTML, phtml_bin);
			tmp_int32 = 65001;
			pmsg->proplist.set(PR_INTERNET_CPID, &tmp_int32);
		}
	}
	if (atxlist_all_hidden(pmsg->children.pattachments)) {
		/*
		 * You know the data model sucks when you cannot determine
		 * all-hidden in O(1) without creating redundant information
		 * like this property.
		 */
		propname.guid = PSETID_COMMON;
		propname.kind = MNID_ID;
		propname.lid = PidLidSmartNoAttach;
		propnames.count = 1;
		propnames.ppropname = &propname;
		if (FALSE == get_propids(&propnames, &propids)) {
			message_content_free(pmsg);
			return nullptr;
		}
		tmp_byte = 1;
		if (pmsg->proplist.set(PROP_TAG(PT_BOOLEAN, propids.ppropid[0]), &tmp_byte) != 0) {
			message_content_free(pmsg);
			return nullptr;
		}
	}
	if (TRUE == field_param.b_flag_del) {
		oxcmail_remove_flag_propties(pmsg, get_propids);
	}
	return pmsg;
}

static size_t oxcmail_encode_mime_string(const char *charset,
    const char *pstring, char *pout_string, size_t max_length)
{
	size_t offset;
	size_t base64_len;
	auto alloc_size = worst_encoding_overhead(strlen(pstring)) + 1;
	std::unique_ptr<char[]> tmp_buff;
	try {
		tmp_buff = std::make_unique<char[]>(alloc_size);
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1539: ENOMEM\n");
		return 0;
	}
	
	if (!oxcmail_check_ascii(pstring) ||
		TRUE == oxcmail_check_crlf(pstring)) {
		if (string_from_utf8(charset, pstring, tmp_buff.get(), alloc_size)) {
			auto string_len = strlen(tmp_buff.get());
			offset = std::max(0, gx_snprintf(pout_string,
				max_length, "=?%s?B?", charset));
			if (encode64(tmp_buff.get(), string_len, pout_string + offset,
			    max_length - offset, &base64_len) != 0)
				return 0;
		} else {
			auto string_len = strlen(pstring);
			offset = std::max(0, gx_snprintf(pout_string,
				max_length, "=?utf-8?B?"));
			if (encode64(pstring, string_len, pout_string + offset,
			    max_length - offset, &base64_len) != 0)
				return 0;
		}
		offset += base64_len;
		if (offset + 3 >= max_length) {
			return 0;
		}
		memcpy(pout_string + offset, "?=", 3);
		return offset + 2;
	} else {
		auto string_len = strlen(pstring);
		if (string_len >= max_length) {
			return 0;
		}
		memcpy(pout_string, pstring, string_len + 1);
		return string_len;
	}
}

static BOOL oxcmail_get_smtp_address(const TPROPVAL_ARRAY *pproplist,
    EXT_BUFFER_ALLOC alloc, uint32_t proptag1, uint32_t proptag2,
    uint32_t proptag3, uint32_t proptag4, char *username, size_t ulen)
{
	auto pvalue = pproplist->getval(proptag1);
	if (NULL == pvalue) {
		pvalue = pproplist->getval(proptag2);
		if (NULL == pvalue) {
 FIND_ENTRYID:
			pvalue = pproplist->getval(proptag4);
			if (NULL == pvalue) {
				return FALSE;
			}
			return oxcmail_entryid_to_username(static_cast<BINARY *>(pvalue), alloc, username, ulen);
		} else {
			if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
				pvalue = pproplist->getval(proptag3);
			} else if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
				pvalue = pproplist->getval(proptag3);
				if (NULL != pvalue) {
					if (oxcmail_essdn_to_username(static_cast<char *>(pvalue), username, ulen))
						return TRUE;	
				}
			} else {
				pvalue = NULL;
			}
			if (NULL == pvalue) {
				goto FIND_ENTRYID;
			}
		}
	}
	gx_strlcpy(username, static_cast<char *>(pvalue), ulen);
	return TRUE;
}

static BOOL oxcmail_export_addresses(const char *charset, TARRAY_SET *prcpts,
    uint32_t rcpt_type, EXT_BUFFER_ALLOC alloc, char *field, size_t fdsize)
{
	size_t offset = 0;
	char username[UADDR_SIZE];
	
	for (size_t i = 0; i < prcpts->count; ++i) {
		auto prcpt = prcpts->pparray[i];
		auto pvalue = prcpt->get<uint32_t>(PR_RECIPIENT_TYPE);
		if (pvalue == nullptr || *pvalue != rcpt_type)
			continue;
		if (0 != offset) {
			memcpy(field + offset, ",\r\n\t", 4);
			offset += 4;
			if (offset >= fdsize)
				return FALSE;
		}
		auto pdisplay_name = prcpt->get<char>(PR_DISPLAY_NAME);
		if (NULL != pdisplay_name) {
			field[offset] = '"';
			offset ++;
			if (offset >= fdsize)
				return FALSE;
			auto tmp_len = oxcmail_encode_mime_string(
				charset, pdisplay_name, field + offset,
			               fdsize - offset);
			if (0 == tmp_len) {
				return FALSE;
			}
			offset += tmp_len;
			field[offset] = '"';
			offset ++;
			if (offset >= fdsize)
				return FALSE;
		}
		if (oxcmail_get_smtp_address(prcpt, alloc, PR_SMTP_ADDRESS,
		    PR_ADDRTYPE, PR_EMAIL_ADDRESS, PR_ENTRYID,
		    username, GX_ARRAY_SIZE(username))) {
			if (NULL != pdisplay_name) {
				offset += std::max(0, gx_snprintf(field + offset,
				          fdsize - offset,
						" <%s>", username));
			} else {
				offset += std::max(0, gx_snprintf(field,
				          fdsize - offset,
					"<%s>", username));
			}
		}
	}
	if (0 == offset || offset >= fdsize)
		return FALSE;
	return TRUE;
}

static BOOL oxcmail_export_reply_to(const MESSAGE_CONTENT *pmsg,
	const char *charset, EXT_BUFFER_ALLOC alloc, char *field)
{
	EXT_PULL ext_pull;
	ONEOFF_ARRAY address_array;
	
	auto pbin = pmsg->proplist.get<BINARY>(PROP_TAG_REPLYRECIPIENTENTRIES);
	if (NULL == pbin) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, pbin->cb, alloc, 0);
	if (ext_pull.g_oneoff_a(&address_array) != EXT_ERR_SUCCESS)
		return FALSE;
	auto pstrings = pmsg->proplist.get<STRING_ARRAY>(PROP_TAG_REPLYRECIPIENTNAMES);
	if (NULL != pstrings && pstrings->count !=
		address_array.count) {
		pstrings = NULL;
	}
	size_t offset = 0;
	for (size_t i = 0; i < address_array.count; ++i) {
		if (0 != offset) {
			memcpy(field + offset, ", ", 2);
			offset += 2;
			if (offset >= MIME_FIELD_LEN) {
				return FALSE;
			}
		}
		if (NULL != pstrings) {
			field[offset] = '"';
			offset ++;
			if (offset >= MIME_FIELD_LEN) {
				return FALSE;
			}
			auto tmp_len = oxcmail_encode_mime_string(charset,
					pstrings->ppstr[i], field + offset,
					MIME_FIELD_LEN - offset);
			if (0 == tmp_len) {
				return FALSE;
			}
			offset += tmp_len;
			field[offset] = '"';
			offset ++;
			if (offset >= MIME_FIELD_LEN) {
				return FALSE;
			}
		}
		if (0 != strcasecmp("SMTP", 
			address_array.pentry_id[i].paddress_type)) {
			return FALSE;
		}
		if (NULL != pstrings) {
			offset += std::max(0, gx_snprintf(field, MIME_FIELD_LEN - offset,
				" <%s>", address_array.pentry_id[i].pmail_address));
		} else {
			offset += std::max(0, gx_snprintf(field, MIME_FIELD_LEN - offset,
				"<%s>", address_array.pentry_id[i].pmail_address));
		}
	}
	if (0 == offset || offset >= MIME_FIELD_LEN) {
		return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_export_address(const MESSAGE_CONTENT *pmsg,
	EXT_BUFFER_ALLOC alloc, uint32_t proptag1, uint32_t proptag2,
	uint32_t proptag3, uint32_t proptag4, uint32_t proptag5,
    const char *charset, char *field, size_t fdsize)
{
	int offset;
	char address[UADDR_SIZE];
	
	offset = 0;
	auto pvalue = pmsg->proplist.get<char>(proptag1);
	if (NULL != pvalue) {
		if (strlen(pvalue) >= GX_ARRAY_SIZE(address)) {
			goto EXPORT_ADDRESS;
		}
		field[offset] = '"';
		offset ++;
		offset += oxcmail_encode_mime_string(charset,
		          pvalue, field + offset, fdsize - offset);
		field[offset] = '"';
		offset ++;
		field[offset] = '\0';
	}
 EXPORT_ADDRESS:
	if (oxcmail_get_smtp_address(&pmsg->proplist, alloc, proptag4, proptag2,
	    proptag3, proptag5, address, GX_ARRAY_SIZE(address))) {
		if (0 == offset) {
			offset = gx_snprintf(field, fdsize, "<%s>", address);
		} else {
			offset += gx_snprintf(field + offset,
			          fdsize - offset, " <%s>", address);
		}
	}
	if (0 == offset) {
		return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_export_content_class(
	const char *pmessage_class, char *field)
{
	if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Fax")) {
		strcpy(field, "fax");
	} else if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Fax.CA")) {
		strcpy(field, "fax-ca");
	} else if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Missed.Voice")) {
		strcpy(field, "missedcall");
	} else if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Conversation.Voice")) {
		strcpy(field, "voice-uc");
	} else if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Voicemail.UM.CA")) {
		strcpy(field, "voice-ca");
	} else if (0 == strcasecmp(pmessage_class,
		"IPM.Note.Microsoft.Voicemail.UM")) {
		strcpy(field, "voice");
	} else if (0 == strncasecmp(pmessage_class,
		"IPM.Note.Custom.", 16)) {
		snprintf(field, 1024,
			"urn:content-class:custom.%s",
			pmessage_class + 16);
	} else {
		return FALSE;
	}
	return TRUE;
}

static int oxcmail_get_mail_type(const char *pmessage_class)
{
	int tmp_len;
	
	tmp_len = strlen(pmessage_class);
	if (0 == strcasecmp( pmessage_class,
		"IPM.Note.SMIME.MultipartSigned")) {
		return MAIL_TYPE_SIGNED;
	}
	if (0 == strncasecmp(pmessage_class, "IPM.InfoPathForm.",
		17) && 0 == strcasecmp(pmessage_class + tmp_len - 22,
		".SMIME.MultipartSigned")) {
		return MAIL_TYPE_SIGNED;
	}
	if (0 == strcasecmp(pmessage_class, "IPM.Note.SMIME")) {
		return MAIL_TYPE_ENCRYPTED;
	}
	if (0 == strncasecmp(pmessage_class, "IPM.InfoPathForm.",
		17) && 0 == strcasecmp(pmessage_class + tmp_len - 6,
		".SMIME")) {
		return MAIL_TYPE_ENCRYPTED;
	}
	if (0 == strcasecmp(pmessage_class, "IPM.Note") ||
		0 == strncasecmp(pmessage_class, "IPM.Note.", 9) ||
		0 == strncasecmp(pmessage_class, "IPM.InfoPathForm.", 17)) {
		return MAIL_TYPE_NORMAL;
	}
	if (0 == strncasecmp(pmessage_class, "REPORT.", 7) &&
		(0 == strcasecmp(pmessage_class + tmp_len - 3, ".DR") ||
		0 == strcasecmp(pmessage_class + tmp_len - 12, ".Expanded.DR") ||
		0 == strcasecmp(pmessage_class + tmp_len - 11, ".Relayed.DR") ||
		0 == strcasecmp(pmessage_class + tmp_len - 11, ".Delayed.DR") ||
		0 == strcasecmp(pmessage_class + tmp_len - 4, ".NDR"))) {
		return MAIL_TYPE_DSN;
	}
	if (0 == strncasecmp(pmessage_class, "REPORT.", 7) &&
		(0 == strcasecmp(pmessage_class + tmp_len - 6, ".IPNRN") ||
		0 == strcasecmp(pmessage_class + tmp_len - 7, ".IPNNRN"))) {
		return MAIL_TYPE_MDN;
	}
	if (0 == strcasecmp(pmessage_class, "IPM.Appointment") ||
		0 == strcasecmp(pmessage_class, "IPM.Schedule.Meeting.Request") ||
		0 == strcasecmp(pmessage_class, "IPM.Schedule.Meeting.Resp.Pos") ||
		0 == strcasecmp(pmessage_class, "IPM.Schedule.Meeting.Resp.Tent") ||
		0 == strcasecmp(pmessage_class, "IPM.Schedule.Meeting.Resp.Neg") ||
		0 == strcasecmp(pmessage_class, "IPM.Schedule.Meeting.Canceled")) {
		return MAIL_TYPE_CALENDAR;
	}
	return MAIL_TYPE_TNEF;
}

static BOOL oxcmail_load_mime_skeleton(const MESSAGE_CONTENT *pmsg,
    const char *pcharset, BOOL b_tnef, int body_type, MIME_SKELETON *pskeleton)
{
	int i;
	char *pbuff;
	BINARY *prtf;
	ATTACHMENT_CONTENT *pattachment;
	memset(pskeleton, 0, sizeof(MIME_SKELETON));
	pskeleton->charset = pcharset;
	pskeleton->pmessage_class = pmsg->proplist.get<char>(PR_MESSAGE_CLASS);
	if (NULL == pskeleton->pmessage_class) {
		pskeleton->pmessage_class = pmsg->proplist.get<char>(PR_MESSAGE_CLASS_A);
	}
	if (NULL == pskeleton->pmessage_class) {
		debug_info("[oxcmail]: missing message class for exporting");
		return FALSE;
	}
	pskeleton->mail_type = oxcmail_get_mail_type(
						pskeleton->pmessage_class);
	if (MAIL_TYPE_SIGNED == pskeleton->mail_type ||
		MAIL_TYPE_ENCRYPTED == pskeleton->mail_type) {
		if (TRUE == b_tnef) {
			b_tnef = FALSE;
		}
	}
	if (TRUE == b_tnef) {
		pskeleton->mail_type = MAIL_TYPE_TNEF;
	}
	pskeleton->body_type = body_type;
	pskeleton->pplain = pmsg->proplist.get<char>(PR_BODY);
	if (MAIL_TYPE_ENCRYPTED == pskeleton->mail_type ||
		MAIL_TYPE_ENCRYPTED == pskeleton->mail_type ||
		MAIL_TYPE_TNEF == pskeleton->mail_type) {
		/* do nothing */
	} else {
		auto pvalue = pmsg->proplist.get<uint32_t>(PROP_TAG_NATIVEBODY);
		if (NULL != pvalue && NATIVE_BODY_RTF == *pvalue &&
		    ((pvalue = pmsg->proplist.get<uint32_t>(PROP_TAG_RTFINSYNC)) == nullptr ||
		    *pvalue == 0)) {
 FIND_RTF:
			prtf = pmsg->proplist.get<BINARY>(PROP_TAG_RTFCOMPRESSED);
			if (NULL != prtf) {
				ssize_t unc_size = rtfcp_uncompressed_size(prtf);
				pbuff = nullptr;
				if (unc_size >= 0) {
					pbuff = static_cast<char *>(malloc(unc_size));
					if (pbuff == nullptr)
						return false;
				}
				size_t rtf_len = unc_size;
				if (unc_size >= 0 && rtfcp_uncompress(prtf, pbuff, &rtf_len)) {
					pskeleton->pattachments = attachment_list_init();
					if (NULL == pskeleton->pattachments) {
						free(pbuff);
						return FALSE;
					}
					size_t tmp_len = SIZE_MAX;
					char *htmlout = nullptr;
					if (rtf_to_html(pbuff, rtf_len, pcharset, &htmlout,
					    &tmp_len, pskeleton->pattachments)) {
						pskeleton->rtf_bin.pv = htmlout;
						free(pbuff);
						pskeleton->rtf_bin.cb = tmp_len;
						pskeleton->phtml = &pskeleton->rtf_bin;
						if (0 == pskeleton->pattachments->count) {
							attachment_list_free(pskeleton->pattachments);
							pskeleton->pattachments = NULL;
						} else {
							pskeleton->b_inline = TRUE;
						}
					} else {
						attachment_list_free(pskeleton->pattachments);
						pskeleton->pattachments = NULL;
						free(pbuff);
						pskeleton->mail_type = MAIL_TYPE_TNEF;
					}
				} else {
					free(pbuff);
					pskeleton->mail_type = MAIL_TYPE_TNEF;
				}
			}
		} else {
			pskeleton->phtml = pmsg->proplist.get<BINARY>(PROP_TAG_HTML);
			if (NULL == pskeleton->phtml) {
				goto FIND_RTF;
			}
		}
	}
	if (NULL == pmsg->children.pattachments) {
		return TRUE;
	}
	for (i=0; i<pmsg->children.pattachments->count; i++) {
		pattachment = pmsg->children.pattachments->pplist[i];
		if (NULL != pattachment->pembedded) {
			pskeleton->b_attachment = TRUE;
			continue;
		}
		auto pvalue = pattachment->proplist.get<uint32_t>(PR_ATTACH_FLAGS);
		if (pvalue != nullptr && (*pvalue & ATT_MHTML_REF)) {
			if (pattachment->proplist.has(PR_ATTACH_CONTENT_ID) ||
			    pattachment->proplist.has(PR_ATTACH_CONTENT_LOCATION)) {
				pskeleton->b_inline = TRUE;
				continue;
			}
		}
		pskeleton->b_attachment = TRUE;
	}
	return TRUE;
}

static void oxcmail_free_mime_skeleton(MIME_SKELETON *pskeleton)
{
	if (NULL != pskeleton->pattachments) {
		attachment_list_free(pskeleton->pattachments);
	}
	if (NULL != pskeleton->rtf_bin.pb) {
		free(pskeleton->rtf_bin.pb);
	}
}

static BOOL oxcmail_export_mail_head(const MESSAGE_CONTENT *pmsg,
	MIME_SKELETON *pskeleton, EXT_BUFFER_ALLOC alloc,
	GET_PROPIDS get_propids, GET_PROPNAME get_propname,
	MIME *phead)
{
	size_t tmp_len = 0;
	time_t tmp_time;
	uint16_t propid;
	uint32_t proptag;
	size_t base64_len;
	struct tm time_buff;
	PROPID_ARRAY propids;
	PROPERTY_NAME propname;
	PROPERTY_NAME *ppropname;
	PROPNAME_ARRAY propnames;
	char tmp_buff[MIME_FIELD_LEN];
	char tmp_field[MIME_FIELD_LEN];
	
	
	if (FALSE == mime_set_field(phead, "MIME-Version", "1.0")) {
		return FALSE;
	}
	
	auto pvalue = pmsg->proplist.getval(PROP_TAG_SENDERSMTPADDRESS);
	auto pvalue1 = pmsg->proplist.getval(PR_SENT_REPRESENTING_SMTP_ADDRESS);
	if (NULL != pvalue && NULL != pvalue1) {
		if (strcasecmp(static_cast<char *>(pvalue), static_cast<char *>(pvalue1)) != 0) {
			oxcmail_export_address(pmsg, alloc, PR_SENDER_NAME,
				PR_SENDER_ADDRTYPE, PR_SENDER_EMAIL_ADDRESS,
				PROP_TAG_SENDERSMTPADDRESS, PR_SENDER_ENTRYID,
				pskeleton->charset, tmp_field,
				GX_ARRAY_SIZE(tmp_field));
			if (FALSE == mime_set_field(phead,
				"Sender", tmp_field)) {
				return FALSE;
			}
		}
	} else {
		pvalue = pmsg->proplist.getval(PR_SENDER_ADDRTYPE);
		pvalue1 = pmsg->proplist.getval(PR_SENT_REPRESENTING_ADDRTYPE);
		if (NULL != pvalue && NULL != pvalue1 &&
		    strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0 &&
		    strcasecmp(static_cast<char *>(pvalue1), "SMTP") == 0) {
			pvalue = pmsg->proplist.getval(PR_SENDER_EMAIL_ADDRESS);
			pvalue1 = pmsg->proplist.getval(PR_SENT_REPRESENTING_EMAIL_ADDRESS);
			if (NULL != pvalue && NULL != pvalue1) {
				if (strcasecmp(static_cast<char *>(pvalue), static_cast<char *>(pvalue1)) != 0) {
					oxcmail_export_address(pmsg, alloc,
						PR_SENDER_NAME,
						PR_SENDER_ADDRTYPE,
						PR_SENDER_EMAIL_ADDRESS,
						PROP_TAG_SENDERSMTPADDRESS,
						PR_SENDER_ENTRYID,
						pskeleton->charset, tmp_field,
						GX_ARRAY_SIZE(tmp_field));
					if (FALSE == mime_set_field(phead,
						"Sender", tmp_field)) {
						return FALSE;
					}
				}
			}
		}
	}
	if (oxcmail_export_address(pmsg, alloc, PR_SENT_REPRESENTING_NAME,
	    PR_SENT_REPRESENTING_ADDRTYPE, PR_SENT_REPRESENTING_EMAIL_ADDRESS,
	    PR_SENT_REPRESENTING_SMTP_ADDRESS, PR_SENT_REPRESENTING_ENTRYID,
	    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
		if (FALSE == mime_set_field(phead,
			"From", tmp_field)) {
			return FALSE;
		}
	} else {
		if (oxcmail_export_address(pmsg, alloc, PR_SENDER_NAME,
		    PR_SENDER_ADDRTYPE, PR_SENDER_EMAIL_ADDRESS,
		    PROP_TAG_SENDERSMTPADDRESS, PR_SENDER_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
			if (FALSE == mime_set_field(phead,
				"Sender", tmp_field)) {
				return FALSE;
			}	
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_ORIGINATORDELIVERYREPORTREQUESTED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		if (TRUE == oxcmail_export_address(pmsg, alloc,
			PROP_TAG_READRECEIPTNAME,
			PROP_TAG_READRECEIPTADDRESSTYPE,
			PROP_TAG_READRECEIPTEMAILADDRESS,
			PROP_TAG_READRECEIPTSMTPADDRESS,
			PR_READ_RECEIPT_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field)) ||

		    oxcmail_export_address(pmsg, alloc, PR_SENDER_NAME,
		    PR_SENDER_ADDRTYPE, PR_SENDER_EMAIL_ADDRESS,
		    PROP_TAG_SENDERSMTPADDRESS, PR_SENDER_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field)) ||

		    oxcmail_export_address(pmsg, alloc, PR_SENT_REPRESENTING_NAME,
		    PR_SENT_REPRESENTING_ADDRTYPE, PR_SENT_REPRESENTING_EMAIL_ADDRESS,
		    PR_SENT_REPRESENTING_SMTP_ADDRESS, PR_SENT_REPRESENTING_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
			if (FALSE == mime_set_field(phead,
				"Return-Receipt-To", tmp_field)) {
				return FALSE;
			}	
		}
	}
	
	pvalue = pmsg->proplist.getval(PR_READ_RECEIPT_REQUESTED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		if (TRUE == oxcmail_export_address(pmsg, alloc,
			PROP_TAG_READRECEIPTNAME,
			PROP_TAG_READRECEIPTADDRESSTYPE,
			PROP_TAG_READRECEIPTEMAILADDRESS,
			PROP_TAG_READRECEIPTSMTPADDRESS,
			PR_READ_RECEIPT_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field)) ||

		    oxcmail_export_address(pmsg, alloc, PR_SENT_REPRESENTING_NAME,
		    PR_SENT_REPRESENTING_ADDRTYPE, PR_SENT_REPRESENTING_EMAIL_ADDRESS,
		    PR_SENT_REPRESENTING_SMTP_ADDRESS, PR_SENT_REPRESENTING_ENTRYID,
		    pskeleton->charset, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
			if (FALSE == mime_set_field(phead,
				"Disposition-Notification-To",
				tmp_field)) {
				return FALSE;
			}
		}
	}
	
	if (TRUE == oxcmail_export_reply_to(pmsg,
		pskeleton->charset, alloc, tmp_field)) {
		if (FALSE == mime_set_field(phead,
			"Reply-To", tmp_field)) {
			return FALSE;
		}
	}
	
	if (NULL == pmsg->children.prcpts) {
		goto EXPORT_CONTENT_CLASS;
	}
	if (oxcmail_export_addresses(pskeleton->charset, pmsg->children.prcpts,
	    MAPI_TO, alloc, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
		if (FALSE == mime_set_field(
			phead, "To", tmp_field)) {
			return FALSE;
		}
	}
	if (oxcmail_export_addresses(pskeleton->charset, pmsg->children.prcpts,
	    MAPI_CC, alloc, tmp_field, GX_ARRAY_SIZE(tmp_field))) {
		if (FALSE == mime_set_field(
			phead, "Cc", tmp_field)) {
			return FALSE;
		}
	}
	
	if (0 == strncasecmp(pskeleton->pmessage_class,
		"IPM.Schedule.Meeting.", 21) ||
		0 == strcasecmp(pskeleton->pmessage_class,
		"IPM.Task") || 0 == strncasecmp(
		pskeleton->pmessage_class, "IPM.Task.", 9)) {
		if (oxcmail_export_addresses(pskeleton->charset,
		    pmsg->children.prcpts, MAPI_BCC, alloc,
		    tmp_field, GX_ARRAY_SIZE(tmp_field))) {
			if (FALSE == mime_set_field(
				phead, "Bcc", tmp_field)) {
				return FALSE;
			}
		}
	}
	
 EXPORT_CONTENT_CLASS:
	if (TRUE == oxcmail_export_content_class(
		pskeleton->pmessage_class, tmp_field)) {
		if (FALSE == mime_set_field(phead,
			"Content-Class", tmp_field)) {
			return FALSE;
		}
	} else if (0 == strncasecmp(
		pskeleton->pmessage_class,
		"IPM.InfoPathForm.", 17)) {
		propname.guid = PSETID_COMMON;
		propname.kind = MNID_ID;
		propname.lid = PidLidInfoPathFromName;
		propnames.count = 1;
		propnames.ppropname = &propname;
		if (FALSE == get_propids(&propnames, &propids)) {
			return FALSE;
		}
		propid = propids.ppropid[0];
		proptag = PROP_TAG(PT_UNICODE, propid);
		pvalue = pmsg->proplist.getval(proptag);
		if (NULL != pvalue) {
			pvalue1 = strrchr(static_cast<char *>(pvalue), '.');
			if (NULL != pvalue1) {
				pvalue = static_cast<char *>(pvalue1) + 1;
			}
			snprintf(tmp_field, 1024, "InfoPathForm.%s",
			         static_cast<const char *>(pvalue));
			if (FALSE == mime_set_field(phead,
				"Content-Class", tmp_field)) {
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PR_SENDER_TELEPHONE_NUMBER);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-CallingTelephoneNumber",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_VOICEMESSAGEDURATION);
	if (NULL != pvalue) {
		snprintf(tmp_field, arsizeof(tmp_field), "%d", *(uint32_t*)pvalue);
		if (FALSE == mime_set_field(phead,
			"X-VoiceMessageDuration", tmp_field)) {
			return FALSE;
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_VOICEMESSAGESENDERNAME);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-VoiceMessageSenderName",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_FAXNUMBEROFPAGES);
	if (NULL != pvalue) {
		snprintf(tmp_field, arsizeof(tmp_field), "%u", *(uint32_t*)pvalue);
		if (FALSE == mime_set_field(phead,
			"X-FaxNumverOfPages", tmp_field)) {
			return FALSE;
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_VOICEMESSAGEATTACHMENTORDER);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-AttachmentOrder",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_CALLID);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-CallID",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	
	pvalue = pmsg->proplist.getval(PR_IMPORTANCE);
	if (NULL != pvalue) {
		switch (*(uint32_t*)pvalue) {
		case IMPORTANCE_LOW:
			if (FALSE == mime_set_field(phead,
				"Importance", "Low")) {
				return FALSE;
			}
			break;
		case IMPORTANCE_NORMAL:
			if (FALSE == mime_set_field(phead,
				"Importance", "Normal")) {
				return FALSE;
			}
			break;
		case IMPORTANCE_HIGH:
			if (FALSE == mime_set_field(phead,
				"Importance", "High")) {
				return FALSE;
			}
			break;
		}
	}
	
	pvalue = pmsg->proplist.getval(PR_SENSITIVITY);
	if (NULL != pvalue) {
		switch (*(uint32_t*)pvalue) {
		case SENSITIVITY_NONE:
			if (FALSE == mime_set_field(phead,
				"Sensitivity", "Normal")) {
				return FALSE;
			}
			break;
		case SENSITIVITY_PERSONAL:
			if (FALSE == mime_set_field(phead,
				"Sensitivity", "Personal")) {
				return FALSE;
			}
			break;
		case SENSITIVITY_PRIVATE:
			if (FALSE == mime_set_field(phead,
				"Sensitivity", "Private")) {
				return FALSE;
			}
			break;
		case SENSITIVITY_COMPANY_CONFIDENTIAL:
			if (FALSE == mime_set_field(
				phead, "Sensitivity",
				"Company-Confidential")) {
				return FALSE;
			}
			break;
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_CLIENTSUBMITTIME);
	if (NULL == pvalue) {
		time(&tmp_time);
	} else {
		tmp_time = rop_util_nttime_to_unix(*(uint64_t*)pvalue);
	}
	strftime(tmp_field, 128, "%a, %d %b %Y %H:%M:%S %z",
					localtime_r(&tmp_time, &time_buff));
	if (FALSE == mime_set_field(phead, "Date", tmp_field)) {
		return FALSE;
	}
	
	pvalue = pmsg->proplist.getval(PR_SUBJECT_PREFIX);
	pvalue1 = pmsg->proplist.getval(PR_NORMALIZED_SUBJECT);
	if (NULL != pvalue && NULL != pvalue1) {
		snprintf(tmp_buff, MIME_FIELD_LEN, "%s%s",
		         static_cast<const char *>(pvalue),
		         static_cast<const char *>(pvalue1));
		if (oxcmail_encode_mime_string(pskeleton->charset,
			tmp_buff, tmp_field, sizeof(tmp_field)) > 0) {
			if (FALSE == mime_set_field(phead,
				"Subject", tmp_field)) {
				return FALSE;
			}
		}
	} else {
		pvalue = pmsg->proplist.getval(PR_SUBJECT);
		if (pvalue != nullptr && oxcmail_encode_mime_string(pskeleton->charset,
		    static_cast<char *>(pvalue), tmp_field, sizeof(tmp_field)) > 0) {
			if (FALSE == mime_set_field(
				phead, "Subject", tmp_field)) {
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_CONVERSATIONTOPIC);
	if (NULL != pvalue) {
		if (oxcmail_encode_mime_string(pskeleton->charset,
		    static_cast<char *>(pvalue), tmp_field, sizeof(tmp_field)) > 0) {
			if (FALSE == mime_set_field(phead,
				"Thread-Topic", tmp_field)) {
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_CONVERSATIONINDEX);
	if (NULL != pvalue) {
		auto bv = static_cast<BINARY *>(pvalue);
		if (encode64(bv->pb, bv->cb, tmp_field, 1024, &base64_len) == 0) {
			if (FALSE == mime_set_field(phead,
				"Thread-Index", tmp_field)) {
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_INTERNETMESSAGEID);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "Message-ID",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_INTERNETREFERENCES);
	if (NULL != pvalue) {
		if (FALSE == mime_set_field(phead,
			"References", tmp_field)) {
			return FALSE;
		}
	}
	propname.guid = PS_PUBLIC_STRINGS;
	propname.kind = MNID_STRING;
	propname.pname = deconst(PidNameKeywords);
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_MV_UNICODE, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue) {
		tmp_len = 0;
		auto sa = static_cast<STRING_ARRAY *>(pvalue);
		for (size_t i = 0; i < sa->count; ++i) {
			if (0 != tmp_len) {
				memcpy(tmp_field, " ,", 2);
				tmp_len += 2;
			}
			if (tmp_len >= MIME_FIELD_LEN) {
				break;
			}
			tmp_len += oxcmail_encode_mime_string(pskeleton->charset,
					sa->ppstr[i],
					tmp_field + tmp_len, MIME_FIELD_LEN - tmp_len);
		}
		if (tmp_len > 0 && tmp_len < MIME_FIELD_LEN) {
			if (FALSE == mime_set_field(phead, "Keywords", tmp_field)) {
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_INREPLYTOID);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "In-Reply-To",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_LISTHELP);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "List-Help",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_LISTSUBSCRIBE);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "List-Subscribe",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_LISTUNSUBSCRIBE);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "List-Unsubscribe",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	pvalue = pmsg->proplist.getval(PR_MESSAGE_LOCALE_ID);
	if (NULL != pvalue) {
		pvalue = deconst(oxcmail_lcid_to_ltag(*static_cast<uint32_t *>(pvalue)));
		if (NULL != pvalue) {
			if (!mime_set_field(phead, "Content-Language",
			    static_cast<char *>(pvalue)))
				return FALSE;
		}
	}
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassified;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_BOOLEAN, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		if (FALSE == mime_set_field(phead,
			"X-Microsoft-Classified", "true")) {
			return FALSE;
		}
	}
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassificationKeep;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_BOOLEAN, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		if (FALSE == mime_set_field(phead,
			"X-Microsoft-ClassKeep", "true")) {
			return FALSE;
		}
	}
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassification;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_UNICODE, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-Microsoft-Classification",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassificationDescription;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_UNICODE, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-Microsoft-ClassDesc",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidClassificationGuid;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_UNICODE, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-Microsoft-ClassID",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	
	if ((NULL != pmsg->children.pattachments &&
		pmsg->children.pattachments->count) > 0 ||
		(NULL != pskeleton->pattachments &&
		pskeleton->pattachments->count > 0)) {
		if (FALSE == mime_set_field(phead,
			"X-MS-Has-Attach", "yes")) {
			return FALSE;
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_AUTORESPONSESUPPRESS);
	if (NULL != pvalue && 0 != *(uint32_t*)pvalue) {
		if (0xFFFFFFFF == *(uint32_t*)pvalue) {
			if (FALSE == mime_set_field(phead,
				"X-Auto-Response-Suppress", "ALL")) {
				return FALSE;
			}
		} else {
			tmp_len = 0;
			if (*(uint32_t*)pvalue & 0x00000001) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "DR");
				tmp_len += 2;
			}
			if (*(uint32_t*)pvalue & 0x00000002) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "NDR");
				tmp_len += 3;
			}
			if (*(uint32_t*)pvalue & 0x00000004) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "RN");
				tmp_len += 2;
			}
			if (*(uint32_t*)pvalue & 0x00000008) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "NRN");
				tmp_len += 3;
			}
			if (*(uint32_t*)pvalue & 0x00000010) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "OOF");
				tmp_len += 3;
			}
			if (*(uint32_t*)pvalue & 0x00000020) {
				if (0 != tmp_len) {
					strcpy(tmp_field + tmp_len, ", ");
					tmp_len += 2;
				}
				strcpy(tmp_field + tmp_len, "AutoReply");
				tmp_len += 9;
			}
		}
		if (0 != tmp_len) {
			if (FALSE == mime_set_field(phead,
				"X-Auto-Response-Suppress", tmp_field)) {
				return FALSE;
			}
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_AUTOFORWARDED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		if (FALSE == mime_set_field(phead,
			"X-MS-Exchange-Organization-AutoForwarded", "true")) {
			return FALSE;
		}
	}
	
	pvalue = pmsg->proplist.getval(PR_SENDER_ID_STATUS);
	if (NULL != pvalue) {
		switch (*(uint32_t*)pvalue) {
		case 1:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"Neutral")) {
				return FALSE;
			}
			break;
		case 2:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"Pass")) {
				return FALSE;
			}
			break;
		case 3:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"Fail")) {
				return FALSE;
			}
			break;
		case 4:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"SoftFail")) {
				return FALSE;
			}
			break;
		case 5:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"None")) {
				return FALSE;
			}
			break;
		case 6:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"TempError")) {
				return FALSE;
			}
			break;
		case 7:
			if (FALSE == mime_set_field(phead,
				"X-MS-Exchange-Organization-SenderIdResult",
				"PermError")) {
				return FALSE;
			}
			break;
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_PURPORTEDSENDERDOMAIN);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "X-MS-Exchange-Organization-PRD",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_CONTENTFILTERSPAMCONFIDENCELEVEL);
	if (NULL != pvalue) {
		snprintf(tmp_field, arsizeof(tmp_field), "%d", *(int32_t*)pvalue);
		if (FALSE == mime_set_field(phead,
			"X-MS-Exchange-Organization-SCL", tmp_field)) {
			return FALSE;
		}
	}
	
	propname.guid = PSETID_COMMON;
	propname.kind = MNID_ID;
	propname.lid = PidLidFlagRequest;
	propnames.count = 1;
	propnames.ppropname = &propname;
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	propid = propids.ppropid[0];
	proptag = PROP_TAG(PT_UNICODE, propid);
	pvalue = pmsg->proplist.getval(proptag);
	if (NULL != pvalue && '\0' != *(char*)pvalue) {
		if (!mime_set_field(phead, "X-Message-Flag",
		    static_cast<char *>(pvalue)))
			return FALSE;
		pvalue = pmsg->proplist.getval(PROP_TAG_REPLYTIME);
		if (NULL != pvalue) {
			tmp_time = rop_util_nttime_to_unix(*(uint64_t*)pvalue);
			strftime(tmp_field, 128, "%a, %d %b %Y %H:%M:%S %z",
							localtime_r(&tmp_time, &time_buff));
			if (FALSE == mime_set_field(phead,
				"Reply-By", tmp_field)) {
				return FALSE;
			}
		}
	}
	
	if (MAIL_TYPE_TNEF == pskeleton->mail_type) {
		pvalue = pmsg->proplist.getval(PR_TNEF_CORRELATION_KEY);
		if (NULL == pvalue) {
			pvalue = pmsg->proplist.getval(PROP_TAG_INTERNETMESSAGEID);
			if (NULL != pvalue) {
				strncpy(tmp_field, static_cast<char *>(pvalue), 1024);
			} else {
				tmp_field[0] = '\0';
			}
		} else {
			auto bv = static_cast<BINARY *>(pvalue);
			if (bv->cb >= 1024) {
				tmp_field[0] = '\0';
			} else {
				memcpy(tmp_field, bv->pb, bv->cb);
				tmp_field[bv->cb] = '\0';
			}
		}
		if (FALSE == mime_set_field(phead,
			"X-MS-TNEF-Correlator", tmp_field)) {
			return FALSE;
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_BODYCONTENTID);
	if (NULL != pvalue) {
		snprintf(tmp_buff, sizeof(tmp_buff), "<%s>",
		         static_cast<const char *>(pvalue));
		if (FALSE == mime_set_field(phead,
			"Content-ID", tmp_buff)) {
			return FALSE;
		}
	}
	
	pvalue = pmsg->proplist.getval(PROP_TAG_BODYCONTENTLOCATION);
	if (NULL != pvalue) {
		if (!mime_set_field(phead, "Content-Location",
		    static_cast<char *>(pvalue)))
			return FALSE;
	}
	
	mime_set_field(phead, "X-Mailer", "gromox-oxcmail " PACKAGE_VERSION);
	auto guid = PS_INTERNET_HEADERS;
	for (size_t i = 0; i < pmsg->proplist.count; ++i) {
		propid = PROP_ID(pmsg->proplist.ppropval[i].proptag);
		if (!is_nameprop_id(propid))
			continue;
		if (FALSE == get_propname(propid, &ppropname)) {
			return FALSE;
		}
		if (ppropname->guid != guid)
			continue;
		if (ppropname->kind != MNID_STRING ||
		    strcasecmp(ppropname->pname, "Content-Type") == 0)
			continue;
		if (!mime_set_field(phead, ppropname->pname,
		    static_cast<char *>(pmsg->proplist.ppropval[i].pvalue)))
			return FALSE;
	}
	return TRUE;
}

static BOOL oxcmail_export_dsn(const MESSAGE_CONTENT *pmsg,
	const char *charset, const char *pmessage_class,
	EXT_BUFFER_ALLOC alloc, char *pdsn_content,
	int max_length)
{
	DSN dsn;
	int tmp_len;
	void *pvalue;
	char action[16];
	TARRAY_SET *prcpts;
	char tmp_buff[1024];
	DSN_FIELDS *pdsn_fields;
	static constexpr const char status_strings1[][6] =
		{"5.4.0", "5.1.0", "5.6.5", "5.6.5", "5.2.0", "5.3.0", "4.4.3"};
	static constexpr const char status_strings2[][6] =
		{"5.1.0", "5.1.4", "4.4.5", "4.4.6", "5.1.0", "4.4.7",
		"5.3.0", "5.6.0", "5.6.3", "5.6.2", "5.6.0", "5.5.4",
		"5.6.0", "5.3.4", "5.6.0", "5.6.1", "5.5.3", "5.5.5",
		"5.3.3", "5.6.2", "5.6.0", "5.6.0", "4.6.4", "4.6.4",
		"4.6.4", "4.6.4", "5.7.3", "4.4.6", "5.7.2", "5.7.1",
		"5.2.4", "5.6.1", "5.1.3", "5.1.0", "5.1.3", "5.1.1",
		"5.1.0", "5.3.0", "5.3.0", "5.1.0", "5.1.6", "5.1.0",
		"5.1.0", "5.1.6", "5.0.0", "5.0.0", "5.7.0", "5.0.0",
		"5.1.2"};
	
	dsn_init(&dsn);
	pdsn_fields = dsn_get_message_fileds(&dsn);
	pvalue = /* (TPROPVAL_ARRAY*)& */ pmsg->proplist.getval(PROP_TAG_REPORTINGMESSAGETRANSFERAGENT);
	if (NULL == pvalue) {
		strcpy(tmp_buff, "dns; ");
		gethostname(tmp_buff + 5, sizeof(tmp_buff) - 5);
		tmp_buff[arsizeof(tmp_buff)-1] = '\0';
		if (!dsn_append_field(pdsn_fields, "Reporting-MTA", tmp_buff)) {
			dsn_free(&dsn);
			return FALSE;
		}
	} else {
		if (!dsn_append_field(pdsn_fields, "Reporting-MTA",
		    static_cast<char *>(pvalue))) {
			dsn_free(&dsn);
			return FALSE;
		}
	}
	
	tmp_len = strlen(pmessage_class);
	if (0 == strcasecmp(pmessage_class
		+ tmp_len - 3, ".DR")) {
		strcpy(action, "delivered");
	} else if (0 == strcasecmp(pmessage_class
		+ tmp_len - 12, ".Expanded.DR")) {
		strcpy(action, "expanded");
	} else if (0 == strcasecmp(pmessage_class
		+ tmp_len - 11, ".Relayed.DR")) {
		strcpy(action, "relayed");
	} else if (0 == strcasecmp(pmessage_class
		+ tmp_len - 11, ".Delayed.DR")) {
		strcpy(action, "delayed");
	} else if (0 == strcasecmp(pmessage_class
		+ tmp_len - 4, ".NDR")) {
		strcpy(action, "failed");
	}
	if (NULL == pmsg->children.prcpts) {
		goto SERIALIZE_DSN;
	}
	prcpts = pmsg->children.prcpts;
	for (size_t i = 0; i < prcpts->count; ++i) {
		pdsn_fields = dsn_new_rcpt_fields(&dsn);
		if (NULL == pdsn_fields) {
			dsn_free(&dsn);
			return FALSE;
		}
		strcpy(tmp_buff, "rfc822;");
		if (!oxcmail_get_smtp_address(prcpts->pparray[i], alloc,
		    PR_SMTP_ADDRESS, PR_ADDRTYPE,
		    PR_EMAIL_ADDRESS, PR_ENTRYID, tmp_buff + 7,
		    GX_ARRAY_SIZE(tmp_buff) - 7)) {
			dsn_free(&dsn);
			return FALSE;
		}
		if (!dsn_append_field(pdsn_fields, "Final-Recipient", tmp_buff) ||
		    !dsn_append_field(pdsn_fields, "Action", action)) {
			dsn_free(&dsn);
			return FALSE;
		}
		pvalue = prcpts->pparray[i]->getval(PROP_TAG_NONDELIVERYREPORTDIAGCODE);
		if (NULL != pvalue) {
			if (0xFFFFFFFF == *(uint32_t*)pvalue) {
				pvalue = prcpts->pparray[i]->getval(PROP_TAG_NONDELIVERYREPORTREASONCODE);
				if (NULL != pvalue) {
					if (*(uint32_t*)pvalue > 6) {
						strcpy(tmp_buff, "5.4.0");
					} else {
						strcpy(tmp_buff,
							status_strings1[*(uint32_t*)pvalue]);
					}
					if (!dsn_append_field(pdsn_fields, "Status", tmp_buff)) {
						dsn_free(&dsn);
						return FALSE;
					}
				}
			} else {
				pvalue = prcpts->pparray[i]->getval(PROP_TAG_NONDELIVERYREPORTREASONCODE);
				if (NULL != pvalue) {
					if (*(uint32_t*)pvalue > 48) {
						strcpy(tmp_buff, "5.0.0");
					} else {
						strcpy(tmp_buff,
							status_strings2[*(uint32_t*)pvalue]);
					}
					if (!dsn_append_field(pdsn_fields, "Status", tmp_buff)) {
						dsn_free(&dsn);
						return FALSE;
					}
				}
			}
		}
		pvalue = prcpts->pparray[i]->getval(PROP_TAG_REMOTEMESSAGETRANSFERAGENT);
		if (NULL != pvalue) {
			if (!dsn_append_field(pdsn_fields, "Remote-MTA",
			    static_cast<char *>(pvalue))) {
				dsn_free(&dsn);
				return FALSE;
			}
		}
		pvalue = prcpts->pparray[i]->getval(PR_SUPPLEMENTARY_INFO);
		if (NULL != pvalue) {
			if (!dsn_append_field(pdsn_fields, "X-Supplementary-Info",
			    static_cast<char *>(pvalue))) {
				dsn_free(&dsn);
				return FALSE;
			}
		}
		pvalue = prcpts->pparray[i]->getval(PR_DISPLAY_NAME);
		if (NULL != pvalue) {
			if (oxcmail_encode_mime_string(charset,
			    static_cast<char *>(pvalue), tmp_buff, GX_ARRAY_SIZE(tmp_buff)) > 0) {
				if (!dsn_append_field(pdsn_fields, "X-Display-Name", tmp_buff)) {
					dsn_free(&dsn);
					return FALSE;
				}
			}
		}
	}
 SERIALIZE_DSN:
	if (!dsn_serialize(&dsn, pdsn_content, max_length)) {
		dsn_free(&dsn);
		return FALSE;
	}
	dsn_free(&dsn);
	return TRUE;
}

static BOOL oxcmail_export_mdn(const MESSAGE_CONTENT *pmsg,
	const char *charset, const char *pmessage_class,
	EXT_BUFFER_ALLOC alloc, char *pmdn_content,
	int max_length)
{
	DSN dsn;
	int tmp_len;
	size_t base64_len;
	char tmp_buff[1024];
	char tmp_address[UADDR_SIZE];
	DSN_FIELDS *pdsn_fields;
	
	tmp_address[0] = '\0';
	auto pvalue = pmsg->proplist.getval(PROP_TAG_SENDERSMTPADDRESS);
	auto pdisplay_name = pmsg->proplist.get<const char>(PR_SENDER_NAME);
	if (NULL != pvalue) {
		gx_strlcpy(tmp_address, static_cast<char *>(pvalue), GX_ARRAY_SIZE(tmp_address));
	} else {
		pvalue = pmsg->proplist.getval(PR_SENDER_ADDRTYPE);
		if (pvalue != nullptr &&
		    strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			pvalue = pmsg->proplist.getval(PR_SENDER_EMAIL_ADDRESS);
			if (NULL != pvalue) {
				gx_strlcpy(tmp_address, static_cast<char *>(pvalue), GX_ARRAY_SIZE(tmp_address));
			}
		}
	}
	if ('\0' != tmp_address[0]) {
		goto EXPORT_MDN_CONTENT;
	}
	pvalue = pmsg->proplist.getval(PR_SENT_REPRESENTING_SMTP_ADDRESS);
	pdisplay_name = pmsg->proplist.get<char>(PR_SENT_REPRESENTING_NAME);
	if (NULL != pvalue) {
		gx_strlcpy(tmp_address, static_cast<char *>(pvalue), GX_ARRAY_SIZE(tmp_address));
	} else {
		pvalue = pmsg->proplist.getval(PR_SENT_REPRESENTING_ADDRTYPE);
		if (pvalue != nullptr &&
		    strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			pvalue = pmsg->proplist.getval(PR_SENT_REPRESENTING_EMAIL_ADDRESS);
			if (NULL != pvalue) {
				gx_strlcpy(tmp_address, static_cast<char *>(pvalue), GX_ARRAY_SIZE(tmp_address));
			}
		}
	}
 EXPORT_MDN_CONTENT:
	dsn_init(&dsn);
	pdsn_fields = dsn_get_message_fileds(&dsn);
	snprintf(tmp_buff, arsizeof(tmp_buff), "rfc822;%s", tmp_address);
	if (!dsn_append_field(pdsn_fields, "Final-Recipient", tmp_buff)) {
		dsn_free(&dsn);
		return FALSE;
	}
	tmp_len = strlen(pmessage_class);
	if (0 == strcasecmp(pmessage_class + tmp_len - 6, ".IPNRN")) {
		strcpy(tmp_buff, "manual-action/MDN-sent-automatically; displayed");
	} else {
		strcpy(tmp_buff, "manual-action/MDN-sent-automatically; deleted");
	}
	if (!dsn_append_field(pdsn_fields, "Disposition", tmp_buff)) {
		dsn_free(&dsn);
		return FALSE;
	}
	pvalue = pmsg->proplist.getval(PR_PARENT_KEY);
	if (NULL != pvalue) {
		auto bv = static_cast<BINARY *>(pvalue);
		if (encode64(bv->pb, bv->cb, tmp_buff, 1024, &base64_len) == 0) {
			tmp_buff[base64_len] = '\0';
			if (!dsn_append_field(pdsn_fields, "X-MSExch-Correlation-Key", tmp_buff)) {
				dsn_free(&dsn);
				return FALSE;
			}
		}
	}
	pvalue = pmsg->proplist.getval(PROP_TAG_ORIGINALMESSAGEID);
	if (NULL != pvalue) {
		if (!dsn_append_field(pdsn_fields, "Original-Message-ID",
		    static_cast<char *>(pvalue))) {
			dsn_free(&dsn);
			return FALSE;
		}
	}
	if (NULL != pdisplay_name) {
		if (oxcmail_encode_mime_string(charset, pdisplay_name,
		    tmp_buff, GX_ARRAY_SIZE(tmp_buff)) > 0) {
			if (!dsn_append_field(pdsn_fields, "X-Display-Name", tmp_buff)) {
				dsn_free(&dsn);
				return FALSE;
			}
		}
	}
	if (!dsn_serialize(&dsn, pmdn_content, max_length)) {
		dsn_free(&dsn);
		return FALSE;
	}
	dsn_free(&dsn);
	return TRUE;
}

static BOOL oxcmail_export_appledouble(MAIL *pmail,
	BOOL b_inline, ATTACHMENT_CONTENT *pattachment,
	MIME_SKELETON *pskeleton, EXT_BUFFER_ALLOC alloc,
	GET_PROPIDS get_propids, MIME *pmime)
{
	int tmp_len;
	const void *pvalue;
	MACBINARY macbin;
	uint32_t proptag;
	EXT_PULL ext_pull;
	char tmp_field[1024];
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	PROPERTY_NAME propname_buff[2];
	
	auto pbin = pattachment->proplist.get<BINARY>(PR_ATTACH_DATA_BIN);
	if (NULL == pbin) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, pbin->cb, alloc, 0);
	if (EXT_ERR_SUCCESS != macbinary_pull_binary(
		&ext_pull, &macbin)) {
		return FALSE;
	}
	propnames.count = 2;
	propnames.ppropname = propname_buff;
	propname_buff[0].guid = PSETID_ATTACHMENT;
	propname_buff[0].kind = MNID_STRING;
	propname_buff[0].pname = deconst(PidNameAttachmentMacInfo);
	propname_buff[1].guid = PSETID_ATTACHMENT;
	propname_buff[1].kind = MNID_STRING;
	propname_buff[1].pname = deconst(PidNameAttachmentMacContentType);
	if (FALSE == get_propids(&propnames, &propids)) {
		return FALSE;
	}
	proptag = PROP_TAG(PT_BINARY, propids.ppropid[0]);
	pbin = pattachment->proplist.get<BINARY>(proptag);
	proptag = PROP_TAG(PT_UNICODE, propids.ppropid[1]);
	pvalue = pattachment->proplist.getval(proptag);
	if (NULL == pvalue) {
		pvalue = "application/octet-stream";
	} else {
		auto pvs = static_cast<const char *>(pvalue);
		if (strcasecmp(pvs, "message/rfc822") == 0 ||
		    strcasecmp(pvs, "application/applefile") == 0 ||
		    strcasecmp(pvs, "application/mac-binhex40") == 0 ||
		    strncasecmp(pvs, "multipart/", 10) == 0)
			pvalue = "application/octet-stream";
	}
	if (FALSE == mime_set_content_type(
		pmime, "multipart/appledouble")) {
		return FALSE;
	}
	auto pmime1 = pmail->add_child(pmime, MIME_ADD_LAST);
	if (NULL == pmime1) {
		return FALSE;
	}
	if (FALSE == mime_set_content_type(
		pmime1, "application/applefile")) {
		return FALSE;
	}
	auto pmime2 = pmail->add_child(pmime, MIME_ADD_LAST);
	if (NULL == pmime2) {
		return FALSE;
	}
	if (!mime_set_content_type(pmime2, static_cast<const char *>(pvalue)))
		return FALSE;
	if (NULL == pbin) {
		pbin = apple_util_macbinary_to_appledouble(&macbin);
		if (NULL == pbin) {
			return FALSE;
		}
		if (FALSE == mime_write_content(pmime1, pbin->pc,
			pbin->cb, MIME_ENCODING_BASE64)) {
			rop_util_free_binary(pbin);
			return FALSE;
		}
		rop_util_free_binary(pbin);
	} else {
		if (FALSE == mime_write_content(pmime1, pbin->pc,
			pbin->cb, MIME_ENCODING_BASE64)) {
			return FALSE;
		}
	}
	pvalue = pattachment->proplist.getval(PR_ATTACH_LONG_FILENAME);
	if (NULL == pvalue) {
		pvalue = pattachment->proplist.getval(PR_ATTACH_FILENAME);
	}
	if (NULL != pvalue) {
		tmp_field[0] = '"';
		tmp_len = oxcmail_encode_mime_string(pskeleton->charset,
		          static_cast<const char *>(pvalue), tmp_field + 1, 512);
		if (tmp_len > 0) {
			memcpy(tmp_field + 1 + tmp_len, "\"", 2);
			if (FALSE == mime_set_content_param(
				pmime2, "name", tmp_field)) {
				return FALSE;
			}
		}
		if (FALSE == b_inline) {
			strcpy(tmp_field, "attachment; filename=\"");
			tmp_len = 22;
		} else {
			strcpy(tmp_field, "inline; filename=\"");
			tmp_len = 18;
		}
		tmp_len += oxcmail_encode_mime_string(pskeleton->charset,
		           static_cast<const char *>(pvalue), tmp_field + tmp_len,
		           1024 - tmp_len);
		memcpy(tmp_field + tmp_len, "\"", 2);
		if (FALSE == mime_set_field(pmime2,
			"Content-Disposition", tmp_field)) {
			return FALSE;
		}
	}
	pvalue = pattachment->proplist.getval(PR_DISPLAY_NAME);
	if (NULL != pvalue) {
		tmp_len = oxcmail_encode_mime_string(pskeleton->charset,
		          static_cast<const char *>(pvalue), tmp_field, 1024);
		if (tmp_len > 0) {
			if (FALSE == mime_set_field(pmime2,
				"Content-Description", tmp_field)) {
				return FALSE;
			}
		}
	}
	return mime_write_content(pmime2, reinterpret_cast<const char *>(macbin.pdata),
			macbin.header.data_len, MIME_ENCODING_BASE64);
}

static BOOL oxcmail_export_attachment(ATTACHMENT_CONTENT *pattachment,
    BOOL b_inline, MIME_SKELETON *pskeleton, EXT_BUFFER_ALLOC alloc,
    GET_PROPIDS get_propids, GET_PROPNAME get_propname,
    std::shared_ptr<MIME_POOL> ppool, MIME *pmime)
{
	void *ptr;
	BOOL b_tnef;
	int tmp_len;
	VCARD vcard;
	BOOL b_vcard;
	size_t offset;
	time_t tmp_time;
	struct tm time_buff;
	char tmp_field[1024];
	const char *pfile_name = nullptr;
	LIB_BUFFER *pallocator;
	
	b_vcard = FALSE;
	if (NULL != pattachment->pembedded) {
		/*
		 * "Send as business card" makes Outlook (2019) generate a MAPI
		 * attachment object, representing a file which contains a
		 * vCard 2.0 header and which is marked as text/vcard. oxcmail
		 * just repacks that into the MIME framing.
		 *
		 * "Send as Outlook contact" makes Outlook produce a MAPI-based
		 * attachment. oxcmail will convert this to vCard 4.0 and mark
		 * it as text/directory.
		 */
		auto pvalue = pattachment->pembedded->proplist.getval(PR_MESSAGE_CLASS);
		if (pvalue != nullptr &&
		    strcasecmp(static_cast<char *>(pvalue), "IPM.Contact") == 0)
			b_vcard = TRUE;
	}
	
	if (NULL == pattachment->pembedded) {
		auto pcontent_type = pattachment->proplist.get<const char>(PR_ATTACH_MIME_TAG);
		pfile_name = pattachment->proplist.get<char>(PR_ATTACH_LONG_FILENAME);
		if (NULL == pfile_name) {
			pfile_name = pattachment->proplist.get<char>(PR_ATTACH_FILENAME);
		}
		if (NULL == pcontent_type) {
			auto pvalue = pattachment->proplist.getval(PR_ATTACH_EXTENSION);
			if (NULL != pvalue) {
				pcontent_type = oxcmail_extension_to_mime(static_cast<char *>(pvalue) + 1);
			}
			if (NULL == pcontent_type) {
				pcontent_type = "application/octet-stream";
			}
		}
		if (0 == strncasecmp(pcontent_type, "multipart/", 10)) {
			pcontent_type = "application/octet-stream";
		}
		if (FALSE == mime_set_content_type(pmime, pcontent_type)) {
			return FALSE;
		}
		if (NULL != pfile_name) {
			tmp_field[0] = '"';
			tmp_len = oxcmail_encode_mime_string(
				pskeleton->charset,	pfile_name,
				tmp_field + 1, 512);
			if (tmp_len > 0) {
				memcpy(tmp_field + 1 + tmp_len, "\"", 2);
				if (FALSE == mime_set_content_param(
					pmime, "name", tmp_field)) {
					return FALSE;
				}
			}
		}
	} else if (b_vcard) {
		pfile_name = pattachment->proplist.get<char>(PR_ATTACH_LONG_FILENAME);
		if (NULL == pfile_name) {
			pfile_name = pattachment->proplist.get<char>(PR_ATTACH_FILENAME);
		}
		if (FALSE == mime_set_content_type(
			pmime, "text/directory")) {
			return FALSE;
		}
		if (FALSE == mime_set_content_param(
			pmime, "charset", "\"utf-8\"") ||
			FALSE == mime_set_content_param(
			pmime, "profile", "vCard")) {
			return FALSE;
		}
	} else {
		if (FALSE == mime_set_content_type(
			pmime, "message/rfc822")) {
			return FALSE;
		}
	}
	
	auto pvalue = pattachment->proplist.getval(PR_DISPLAY_NAME);
	if (NULL != pvalue) {
		tmp_len = oxcmail_encode_mime_string(pskeleton->charset,
		          static_cast<char *>(pvalue), tmp_field, 1024);
		if (tmp_len > 0) {
			if (FALSE == mime_set_field(pmime,
				"Content-Description", tmp_field)) {
				return FALSE;
			}
		}
	}
	
	auto pctime = pattachment->proplist.get<uint64_t>(PR_CREATION_TIME);
	auto pmtime = pattachment->proplist.get<uint64_t>(PR_LAST_MODIFICATION_TIME);
	if (TRUE == b_inline) {
		strcpy(tmp_field, "inline; ");
		tmp_len = 8;
	} else {
		strcpy(tmp_field, "attachment; ");
		tmp_len = 12;
	}
	if (NULL != pfile_name) {
		memcpy(tmp_field + tmp_len, "filename=\"", 10);
		tmp_len += 10;
		tmp_len += oxcmail_encode_mime_string(pskeleton->charset,
				pfile_name, tmp_field + tmp_len, 1024 - tmp_len);
		memcpy(tmp_field + tmp_len, "\";\r\n\t", 5);
		tmp_len += 5;
	}
	time(&tmp_time);
	if (NULL != pctime) {
		tmp_time = rop_util_nttime_to_unix(*pctime);
	}
	gmtime_r(&tmp_time, &time_buff);
	tmp_len += strftime(tmp_field + tmp_len, 1024 - tmp_len,
		"creation-date=\"%a, %d %b %Y %H:%M:%S GMT\";\r\n\t",
		&time_buff);
	if (NULL != pmtime) {
		tmp_time = rop_util_nttime_to_unix(*pmtime);
	}
	gmtime_r(&tmp_time, &time_buff);
	tmp_len += strftime(tmp_field + tmp_len, 1024 - tmp_len,
		"modification-date=\"%a, %d %b %Y %H:%M:%S GMT\"",
		&time_buff);
	tmp_field[tmp_len] = '\0';
	if (FALSE == mime_set_field(pmime,
		"Content-Disposition", tmp_field)) {
		return FALSE;
	}
	
	pvalue = pattachment->proplist.getval(PR_ATTACH_CONTENT_ID);
	if (NULL != pvalue) {
		snprintf(tmp_field, sizeof(tmp_field), "<%s>",
		         static_cast<const char *>(pvalue));
		if (FALSE == mime_set_field(pmime,
			"Content-ID", tmp_field)) {
			return FALSE;
		}
	}
	pvalue = pattachment->proplist.getval(PR_ATTACH_CONTENT_LOCATION);
	if (pvalue != nullptr && !mime_set_field(pmime, "Content-Location",
	    static_cast<char *>(pvalue)))
		return FALSE;
	pvalue = pattachment->proplist.getval(PR_ATTACH_CONTENT_BASE);
	if (pvalue != nullptr && !mime_set_field(pmime, "Content-Base",
	    static_cast<char *>(pvalue)))
		return FALSE;
	
	if (b_vcard && oxvcard_export(pattachment->pembedded, &vcard, get_propids)) {
		std::unique_ptr<char[], stdlib_delete> pbuff(static_cast<char *>(malloc(VCARD_MAX_BUFFER_LEN)));
		if (pbuff != nullptr && vcard_serialize(&vcard, pbuff.get(),
		    VCARD_MAX_BUFFER_LEN)) {
			if (!mime_write_content(pmime, pbuff.get(), strlen(pbuff.get()), MIME_ENCODING_BASE64)) {
				vcard_free(&vcard);
				return FALSE;
			}
			vcard_free(&vcard);
			return TRUE;
		}
		vcard_free(&vcard);
	}
	
	if (NULL != pattachment->pembedded) {
		if (MAIL_TYPE_TNEF == pskeleton->mail_type) {
			b_tnef = TRUE;
		} else {
			b_tnef = FALSE;
		}
		MAIL imail;
		if (FALSE == oxcmail_export(pattachment->pembedded,
			b_tnef, pskeleton->body_type, ppool, &imail,
			alloc, get_propids, get_propname)) {
			return FALSE;
		}
		auto mail_len = imail.get_length();
		if (mail_len < 0)
			return false;
		pallocator = lib_buffer_init(STREAM_ALLOC_SIZE,
				mail_len / STREAM_BLOCK_SIZE + 1, FALSE);
		if (pallocator == nullptr)
			return FALSE;
		auto cl_0 = make_scope_exit([&]() { lib_buffer_free(pallocator); });
		STREAM tmp_stream(pallocator);
		if (!imail.serialize(&tmp_stream)) {
			return FALSE;
		}
		imail.clear();
		std::unique_ptr<char[], stdlib_delete> pbuff(static_cast<char *>(malloc(mail_len + 128)));
		if (NULL == pbuff) {
			return FALSE;
		}
				
		offset = 0;
		unsigned int size = STREAM_BLOCK_SIZE;
		while ((ptr = tmp_stream.get_read_buf(&size)) != nullptr) {
			memcpy(pbuff.get() + offset, ptr, size);
			offset += size;
			size = STREAM_BLOCK_SIZE;
		}
		tmp_stream.clear();
		return mime_write_content(pmime, pbuff.get(), mail_len, MIME_ENCODING_NONE);
	}
	pvalue = pattachment->proplist.getval(PR_ATTACH_DATA_BIN);
	auto bv = static_cast<BINARY *>(pvalue);
	if (bv != nullptr && bv->cb != 0)
		if (!mime_write_content(pmime, bv->pc, bv->cb, MIME_ENCODING_BASE64))
			return FALSE;
	return TRUE;
}

BOOL oxcmail_export(const MESSAGE_CONTENT *pmsg, BOOL b_tnef, int body_type,
    std::shared_ptr<MIME_POOL> ppool, MAIL *pmail, EXT_BUFFER_ALLOC alloc,
    GET_PROPIDS get_propids, GET_PROPNAME get_propname)
{
	int i;
	ICAL ical;
	MIME *phtml;
	MIME *pmime;
	MIME *pplain;
	MIME *pmixed;
	BOOL b_inline;
	MIME *prelated;
	MIME *pcalendar;
	char tmp_method[32];
	char tmp_charset[32];
	const char *pcharset;
	MIME_FIELD mime_field;
	MIME_SKELETON mime_skeleton;
	ATTACHMENT_CONTENT *pattachment;
	
	*pmail = MAIL(ppool);
	auto pvalue = pmsg->proplist.getval(PR_INTERNET_CPID);
	if (NULL == pvalue || 1200 == *(uint32_t*)pvalue) {
		pcharset = "utf-8";
	} else {
		pcharset = oxcmail_cpid_to_charset(*(uint32_t*)pvalue);
		if (NULL == pcharset) {
			pcharset = "utf-8";
		}
	}
	if (!oxcmail_load_mime_skeleton(pmsg, pcharset, b_tnef,
	    body_type, &mime_skeleton))
		return FALSE;
	auto phead = pmail->add_head();
	if (NULL == phead) {
		goto EXPORT_FAILURE;
	}
	pmime = phead;
	pplain = NULL;
	phtml = NULL;
	pmixed = NULL;
	prelated = NULL;
	pcalendar = NULL;
	switch (mime_skeleton.mail_type) {
	case MAIL_TYPE_DSN:
	case MAIL_TYPE_MDN:
	case MAIL_TYPE_NORMAL:
	case MAIL_TYPE_CALENDAR:
		if (MAIL_TYPE_DSN == mime_skeleton.mail_type) {
			pmixed = pmime;
			if (!mime_set_content_type(pmime, "multipart/report") ||
			    !mime_set_content_param(pmime, "report-type", "delivery-status") ||
			    (pmime = pmail->add_child(pmime, MIME_ADD_LAST)) == nullptr)
				goto EXPORT_FAILURE;
		} else if (MAIL_TYPE_MDN == mime_skeleton.mail_type) {
			pmixed = pmime;
			if (!mime_set_content_type(pmime, "multipart/report") ||
			    !mime_set_content_param(pmime, "report-type", "disposition-notification") ||
			    (pmime = pmail->add_child(pmime, MIME_ADD_LAST)) == nullptr)
				goto EXPORT_FAILURE;
		} else {
			if (TRUE == mime_skeleton.b_attachment) {
				pmixed = pmime;
				if (!mime_set_content_type(pmime, "multipart/mixed") ||
				    (pmime = pmail->add_child(pmime, MIME_ADD_LAST)) == nullptr)
					goto EXPORT_FAILURE;
			}
		}
		if (TRUE == mime_skeleton.b_inline) {
			prelated = pmime;
			if (!mime_set_content_type(pmime, "multipart/related") ||
			    (pmime = pmail->add_child(pmime, MIME_ADD_LAST)) == nullptr)
				goto EXPORT_FAILURE;
		}
		if (OXCMAIL_BODY_PLAIN_AND_HTML == mime_skeleton.body_type &&
			NULL != mime_skeleton.pplain && NULL != mime_skeleton.phtml) {
			if (FALSE == mime_set_content_type(
				pmime, "multipart/alternative")) {
				goto EXPORT_FAILURE;
			}
			pplain = pmail->add_child(pmime, MIME_ADD_LAST);
			phtml = pmail->add_child(pmime, MIME_ADD_LAST);
			if (NULL == pplain || FALSE == mime_set_content_type(
				pplain, "text/plain") || NULL == phtml ||
				FALSE == mime_set_content_type(phtml, "text/html")) {
				goto EXPORT_FAILURE;
			}
			if (MAIL_TYPE_CALENDAR == mime_skeleton.mail_type) {
				pcalendar = pmail->add_child(pmime, MIME_ADD_LAST);
				if (NULL == pcalendar || FALSE == mime_set_content_type(
					pcalendar, "text/calendar")) {
					goto EXPORT_FAILURE;
				}
			}
		} else if (OXCMAIL_BODY_PLAIN_ONLY == mime_skeleton.body_type
			&& NULL != mime_skeleton.pplain) {
 PLAIN_ONLY:
			if (MAIL_TYPE_CALENDAR != mime_skeleton.mail_type) {
				if (FALSE ==  mime_set_content_type(
					pmime, "text/plain")) {
					goto EXPORT_FAILURE;
				}
				pplain = pmime;
			} else {
				if (FALSE == mime_set_content_type(
					pmime, "multipart/alternative")) {
					goto EXPORT_FAILURE;
				}
				pplain = pmail->add_child(pmime, MIME_ADD_LAST);
				pcalendar = pmail->add_child(pmime, MIME_ADD_LAST);
				if (NULL == pplain || FALSE == mime_set_content_type(
					pplain, "text/plain") || NULL == pcalendar ||
					FALSE == mime_set_content_type(pcalendar,
					"text/calendar")) {
					goto EXPORT_FAILURE;
				}
			}
		} else if (OXCMAIL_BODY_HTML_ONLY == mime_skeleton.body_type
			&& NULL != mime_skeleton.phtml) {
 HTML_ONLY:
			if (MAIL_TYPE_CALENDAR != mime_skeleton.mail_type) {
				if (FALSE ==  mime_set_content_type(
					pmime, "text/html")) {
					goto EXPORT_FAILURE;
				}
				phtml = pmime;
			} else {
				if (FALSE == mime_set_content_type(
					pmime, "multipart/alternative")) {
					goto EXPORT_FAILURE;
				}
				phtml = pmail->add_child(pmime, MIME_ADD_LAST);
				pcalendar = pmail->add_child(pmime, MIME_ADD_LAST);
				if (NULL == phtml || FALSE == mime_set_content_type(
					phtml, "text/html") || NULL == pcalendar ||
					FALSE == mime_set_content_type(pcalendar,
					"text/calendar")) {
					goto EXPORT_FAILURE;
				}
			}
		} else if (NULL != mime_skeleton.phtml) {
			mime_skeleton.body_type = OXCMAIL_BODY_HTML_ONLY;
			goto HTML_ONLY;
		} else {
			mime_skeleton.body_type = OXCMAIL_BODY_PLAIN_ONLY;
			goto PLAIN_ONLY;
		}
		break;
	case MAIL_TYPE_TNEF:
		if (FALSE == mime_set_content_type(
			pmime, "multipart/mixed")) {
			goto EXPORT_FAILURE;
		}
		if ((pplain = pmail->add_child(pmime, MIME_ADD_LAST)) == nullptr ||
		    !mime_set_content_type(pplain, "text/plain"))
			goto EXPORT_FAILURE;
		break;
	}
	
	if (!oxcmail_export_mail_head(pmsg, &mime_skeleton, alloc,
	    get_propids, get_propname, phead))
		goto EXPORT_FAILURE;
	
	if (MAIL_TYPE_ENCRYPTED == mime_skeleton.mail_type) {
		if (FALSE == mime_set_content_type(
			pmime, "application/pkcs7-mime")) {
			goto EXPORT_FAILURE;
		}
		if (NULL == pmsg->children.pattachments ||
			1 != pmsg->children.pattachments->count) {
			goto EXPORT_FAILURE;
		}
		auto pbin = pmsg->children.pattachments->pplist[0]->proplist.get<BINARY>(PR_ATTACH_DATA_BIN);
		if (NULL == pbin) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_write_content(pmime, pbin->pc,
			pbin->cb, MIME_ENCODING_BASE64)) {
			goto EXPORT_FAILURE;
		}
		return TRUE;
	} else if (MAIL_TYPE_SIGNED == mime_skeleton.mail_type) {
		/* make fake "Content-Type" to avoid produce boundary string */
		mime_set_content_type(pmime, "fake-part/signed");
		if (NULL == pmsg->children.pattachments ||
			1 != pmsg->children.pattachments->count) {
			goto EXPORT_FAILURE;
		}
		auto pbin = pmsg->children.pattachments->pplist[0]->proplist.get<BINARY>(PR_ATTACH_DATA_BIN);
		if (NULL == pbin || 0 == pbin->cb) {
			goto EXPORT_FAILURE;
		}
		auto tmp_len = parse_mime_field(pbin->pc, pbin->cb, &mime_field);
		if (0 == tmp_len || 0 != strncmp(pbin->pc + tmp_len,
			"\r\n", 2) || 12 != mime_field.field_name_len
			|| 0 != strncasecmp( mime_field.field_name,
			"Content-Type", 12) || mime_field.field_value_len > 1024
			|| mime_field.field_value_len < 16 || 0 != strncasecmp(
			mime_field.field_value, "multipart/signed", 16)) {
			goto EXPORT_FAILURE;
		}
		memcpy(mime_field.field_value, "fake-part/signed", 16);
		mime_field.field_value[mime_field.field_value_len] = '\0';
		if (FALSE == mime_set_field(pmime, "Content-Type",
			mime_field.field_value)) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_write_content(pmime, pbin->pc + tmp_len + 2,
			pbin->cb - tmp_len - 2, MIME_ENCODING_NONE)) {
			return FALSE;
		}
		/* replace "Content-Type" back to the real one */
		strcpy(pmime->content_type, "multipart/signed");
		return TRUE;
	}
	
	if (NULL != pplain) {
		if (NULL == mime_skeleton.pplain ||
			'\0' == mime_skeleton.pplain[0]) {
			if (FALSE == mime_write_content(pplain,
				"\r\n", 2, MIME_ENCODING_BASE64)) {
				goto EXPORT_FAILURE;
			}
		} else {
			auto alloc_size = worst_encoding_overhead(strlen(mime_skeleton.pplain)) + 1;
			std::unique_ptr<char[]> pbuff;
			try {
				pbuff = std::make_unique<char[]>(alloc_size);
			} catch (const std::bad_alloc &) {
				fprintf(stderr, "E-1508: ENOMEM\n");
				goto EXPORT_FAILURE;
			}
			if (!string_from_utf8(mime_skeleton.charset,
			    mime_skeleton.pplain, pbuff.get(), alloc_size)) {
				pbuff.reset();
				if (FALSE == mime_write_content(
					pplain, mime_skeleton.pplain,
					strlen(mime_skeleton.pplain),
					MIME_ENCODING_BASE64)) {
					goto EXPORT_FAILURE;
				}
				strcpy(tmp_charset, "\"utf-8\"");
			} else {
				if (!mime_write_content(pplain, pbuff.get(),
				    strlen(pbuff.get()), MIME_ENCODING_BASE64))
					goto EXPORT_FAILURE;
				snprintf(tmp_charset, arsizeof(tmp_charset), "\"%s\"", mime_skeleton.charset);
			}
			if (FALSE == mime_set_content_param(
				pplain, "charset", tmp_charset)) {
				goto EXPORT_FAILURE;
			}
		}
	}
	
	if (MAIL_TYPE_TNEF == mime_skeleton.mail_type) {
		pmime = pmail->add_child(pmime, MIME_ADD_LAST);
		BINARY *pbin = nullptr;
		if (NULL == pmime || FALSE == mime_set_content_type(pmime,
			"application/ms-tnef") || NULL == (pbin =
			tnef_serialize(pmsg, alloc, get_propname))) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_write_content(pmime, pbin->pc,
			pbin->cb, MIME_ENCODING_BASE64)) {
			rop_util_free_binary(pbin);
			goto EXPORT_FAILURE;
		}
		rop_util_free_binary(pbin);
		if (FALSE == mime_set_content_param(
			pmime, "name", "\"winmail.dat\"") ||
			FALSE == mime_set_field(pmime, "Content-Disposition",
			"attachment; filename=\"winmail.dat\"")) {
			goto EXPORT_FAILURE;
		}
		oxcmail_free_mime_skeleton(&mime_skeleton);
		return TRUE;
	}
	
	if (NULL != phtml) {
		if (FALSE == mime_write_content(phtml, mime_skeleton.phtml->pc,
			mime_skeleton.phtml->cb,
			MIME_ENCODING_BASE64)) {
			goto EXPORT_FAILURE;
		}
		snprintf(tmp_charset, arsizeof(tmp_charset), "\"%s\"", mime_skeleton.charset);
		if (FALSE == mime_set_content_param(
			phtml, "charset", tmp_charset)) {
			goto EXPORT_FAILURE;
		}
	}
	
	if (NULL != pcalendar) {
		char tmp_buff[1024*1024];
		
		if (ical_init(&ical) < 0)
			goto EXPORT_FAILURE;
		if (FALSE == oxcical_export(pmsg, &ical, alloc,
		    get_propids, oxcmail_entryid_to_username,
		    oxcmail_essdn_to_username, oxcmail_lcid_to_ltag))
			goto EXPORT_FAILURE;
		tmp_method[0] = '\0';
		auto piline = ical.get_line("METHOD");
		if (NULL != piline) {
			pvalue = deconst(piline->get_first_subvalue());
			if (NULL != pvalue) {
				gx_strlcpy(tmp_method, static_cast<char *>(pvalue), GX_ARRAY_SIZE(tmp_method));
			}
		}
		if (!ical_serialize(&ical, tmp_buff, sizeof(tmp_buff)))
			goto EXPORT_FAILURE;
		if (FALSE == mime_write_content(pcalendar, tmp_buff,
			strlen(tmp_buff), MIME_ENCODING_BASE64)) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_set_content_param(
			pcalendar, "charset", "\"utf-8\"")) {
			goto EXPORT_FAILURE;
		}
		if ('\0' != tmp_method[0]) {
			mime_set_content_param(pcalendar, "method", tmp_method);
		}
	}
	
	if (MAIL_TYPE_DSN == mime_skeleton.mail_type) {
		char tmp_buff[1024*1024];
		
		pmime = pmail->add_child(phead, MIME_ADD_LAST);
		if (NULL == pmime) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_set_content_type(pmime,
			"message/delivery-status")) {
			goto EXPORT_FAILURE;
		}
		if (!oxcmail_export_dsn(pmsg, mime_skeleton.charset,
		    mime_skeleton.pmessage_class, alloc, tmp_buff, sizeof(tmp_buff)))
			goto EXPORT_FAILURE;
		if (FALSE == mime_write_content(pmime, tmp_buff,
			strlen(tmp_buff), MIME_ENCODING_NONE)) {
			goto EXPORT_FAILURE;
		}
	} else if (MAIL_TYPE_MDN == mime_skeleton.mail_type) {
		char tmp_buff[1024*1024];
		
		pmime = pmail->add_child(phead, MIME_ADD_LAST);
		if (NULL == pmime) {
			goto EXPORT_FAILURE;
		}
		if (FALSE == mime_set_content_type(pmime,
			"message/disposition-notification")) {
			goto EXPORT_FAILURE;
		}
		if (!oxcmail_export_mdn(pmsg, mime_skeleton.charset,
		    mime_skeleton.pmessage_class, alloc, tmp_buff,
		    arsizeof(tmp_buff)))
			goto EXPORT_FAILURE;
		if (FALSE == mime_write_content(pmime, tmp_buff,
			strlen(tmp_buff), MIME_ENCODING_NONE)) {
			goto EXPORT_FAILURE;
		}
	}
	
	if (NULL != mime_skeleton.pattachments) {
		for (i=0; i<mime_skeleton.pattachments->count; i++) {
			pmime = pmail->add_child(prelated, MIME_ADD_LAST);
			if (NULL == pmime) {
				goto EXPORT_FAILURE;
			}
			if (FALSE == oxcmail_export_attachment(
				mime_skeleton.pattachments->pplist[i],
				TRUE, &mime_skeleton, alloc, get_propids,
				get_propname, NULL, pmime)) {
				goto EXPORT_FAILURE;
			}
		}
	}
	
	if (NULL == pmsg->children.pattachments) {
		oxcmail_free_mime_skeleton(&mime_skeleton);
		return TRUE;
	}
	for (i=0; i<pmsg->children.pattachments->count; i++) {
		pattachment = pmsg->children.pattachments->pplist[i];
		if (NULL != pattachment->pembedded) {
			pvalue = pattachment->pembedded->proplist.getval(PR_MESSAGE_CLASS);
			if (pvalue != nullptr && strcasecmp(static_cast<char *>(pvalue),
			    "IPM.OLE.CLASS.{00061055-0000-0000-C000-000000000046}") == 0)
				continue;
		}
		if (NULL == pattachment->pembedded &&
		    (pvalue = pattachment->proplist.getval(PR_ATTACH_FLAGS)) != nullptr &&
		    (*static_cast<uint32_t *>(pvalue) & ATT_MHTML_REF) &&
		    (pattachment->proplist.has(PR_ATTACH_CONTENT_ID) ||
		    pattachment->proplist.has(PR_ATTACH_CONTENT_LOCATION))) {
			b_inline = TRUE;
			pmime = pmail->add_child(prelated, MIME_ADD_LAST);
		} else {
			b_inline = FALSE;
			pmime = pmail->add_child(pmixed, MIME_ADD_LAST);
		}
		if (NULL == pmime) {
			goto EXPORT_FAILURE;
		}
		if (NULL == pattachment->pembedded &&
		    (pvalue = pattachment->proplist.getval(PR_ATTACH_METHOD)) != nullptr &&
		    *static_cast<uint32_t *>(pvalue) == ATTACH_BY_VALUE &&
		    (pvalue = pattachment->proplist.getval(PR_ATTACH_ENCODING)) != nullptr &&
		    static_cast<BINARY *>(pvalue)->cb == sizeof(MACBINARY_ENCODING) &&
		    memcmp(static_cast<BINARY *>(pvalue)->pb, MACBINARY_ENCODING, sizeof(MACBINARY_ENCODING)) == 0) {
			if (TRUE == oxcmail_export_appledouble(pmail,
				b_inline, pattachment, &mime_skeleton,
				alloc, get_propids, pmime)) {
				continue;
			}
		}
		if (FALSE == oxcmail_export_attachment(pattachment,
			b_inline, &mime_skeleton, alloc, get_propids,
			get_propname, ppool, pmime)) {
			goto EXPORT_FAILURE;
		}
	}
	oxcmail_free_mime_skeleton(&mime_skeleton);
	return TRUE;
 EXPORT_FAILURE:
	oxcmail_free_mime_skeleton(&mime_skeleton);
	return FALSE;
}
