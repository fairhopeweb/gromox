// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/svc_common.h>
#include "str_filter.h"
#include <gromox/config_file.hpp>
#include <gromox/util.hpp>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <sys/types.h>

using namespace std::string_literals;
using namespace gromox;

DECLARE_SVC_API();

static BOOL svc_str_filter(int reason, void **ppdata)
{
	char list_path[256], temp_buff[128];
	int audit_max, audit_interval, audit_times;
	int temp_list_size, growing_num;
	BOOL case_sensitive;
	
	switch(reason) {
	case PLUGIN_INIT: {
		LINK_SVC_API(ppdata);
		std::string plugname = get_plugin_name();
		auto pos = plugname.find('.');
		if (pos != plugname.npos)
			plugname.erase(pos);
		auto cfg_path = plugname + ".cfg";
		if (FALSE == register_talk(str_filter_console_talk)) {
			printf("[%s]: failed to register console talk\n", plugname.c_str());
			return FALSE;
		}
		auto pfile = config_file_initd(cfg_path.c_str(), get_config_path());
		if (NULL == pfile) {
			printf("[%s]: config_file_initd %s: %s\n",
			       plugname.c_str(), cfg_path.c_str(), strerror(errno));
			return FALSE;
		}
		auto str_value = pfile->get_value("IS_CASE_SENSITIVE");
		if (NULL == str_value) {
			case_sensitive = FALSE;
			printf("[%s]: case-insensitive\n", plugname.c_str());
		} else {
			if (0 == strcasecmp(str_value, "FALSE")) {
				case_sensitive=FALSE;
				printf("[%s]: case-insensitive\n", plugname.c_str());
			} else if (0 == strcasecmp(str_value, "TRUE")) {
				case_sensitive=TRUE;
				printf("[%s]: case-sensitive\n", plugname.c_str());
			} else {
				case_sensitive = FALSE;
				printf("[%s]: case-insensitive\n", plugname.c_str());
			}
		}				
		str_value = pfile->get_value("AUDIT_MAX_NUM");
		audit_max = str_value != nullptr ? strtol(str_value, nullptr, 0) : 0;
		if (audit_max < 0)
			audit_max = 0;
		printf("[%s]: audit capacity is %d\n", plugname.c_str(), audit_max);
		str_value = pfile->get_value("AUDIT_INTERVAL");
		if (NULL == str_value) {
			audit_interval = 60;
		} else {
			audit_interval = atoitvl(str_value);
			if (audit_interval <= 0)
				audit_interval = 60;
		}
		itvltoa(audit_interval, temp_buff);
		printf("[%s]: audit interval is %s\n", plugname.c_str(), temp_buff);
		str_value = pfile->get_value("AUDIT_TIMES");
		audit_times = str_value != nullptr ? strtol(str_value, nullptr, 0) : 10;
		if (audit_times <= 0)
			audit_times = 10;
		printf("[%s]: audit times is %d\n", plugname.c_str(), audit_times);
		str_value = pfile->get_value("TEMP_LIST_SIZE");
		temp_list_size = str_value != nullptr ? strtol(str_value, nullptr, 0) : 0;
		if (temp_list_size < 0)
			temp_list_size = 0;
		printf("[%s]: temporary list capacity is %d\n", plugname.c_str(),
			temp_list_size);
		str_value = pfile->get_value("GREY_GROWING_NUM");
		growing_num = str_value != nullptr ? strtol(str_value, nullptr, 0) : 0;
		if (growing_num < 0)
			growing_num = 0;
		printf("[%s]: grey list growing number is %d\n", plugname.c_str(),
			growing_num);
		str_value = pfile->get_value("JUDGE_SERVICE_NAME");
		std::string judge_name = str_value != nullptr ? str_value : plugname.c_str() + "_judge"s;
		str_value = pfile->get_value("ADD_SERVICE_NAME");
		std::string add_name = str_value != nullptr ? str_value : plugname.c_str() + "_add"s;
		str_value = pfile->get_value("QUERY_SERVICE_NAME");
		std::string query_name = str_value != nullptr ? str_value : plugname.c_str() + "_query"s;
		snprintf(list_path, GX_ARRAY_SIZE(list_path), "%s.txt", plugname.c_str());
		str_filter_init(plugname.c_str(), case_sensitive, audit_max,
		   audit_interval, audit_times, temp_list_size, list_path, growing_num);
		if (0 != str_filter_run()) {
			printf("[%s]: failed to run the module\n", plugname.c_str());
			return FALSE;
		}
		if (judge_name.size() > 0 && !register_service(judge_name.c_str(), str_filter_judge)) {
			printf("[%s]: failed to register \"%s\" service\n",
			       plugname.c_str(), judge_name.c_str());
			return FALSE;
		}
		if (query_name.size() > 0 && !register_service(query_name.c_str(), str_filter_query)) {
			printf("[%s]: failed to register \"%s\" service\n",
			       plugname.c_str(), query_name.c_str());
			return FALSE;
		}
		if (add_name.size() > 0 && !register_service(add_name.c_str(), str_filter_add_string_into_temp_list)) {
			printf("[%s]: failed to register \"%s\" service\n",
			       plugname.c_str(), add_name.c_str());
			return FALSE;
		}
		return TRUE;
	}
	case PLUGIN_FREE:
		str_filter_stop();
		str_filter_free();
		return TRUE;
	}
	return TRUE;
}
SVC_ENTRY(svc_str_filter);
