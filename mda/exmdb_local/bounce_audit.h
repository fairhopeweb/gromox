#pragma once
#include <gromox/common_types.hpp>
#include <ctime>

enum {
    BOUNCE_AUDIT_INTERVAL = 0,
	BOUNCE_AUDIT_CAPABILITY
};

void bounce_audit_init(int audit_num, int audit_interval);
int bounce_audit_set_param(int type, int value);
int bounce_audit_get_param(int type);
BOOL bounce_audit_check(const char *audit_string);
