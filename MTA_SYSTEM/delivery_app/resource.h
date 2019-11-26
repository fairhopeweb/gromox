#ifndef _H_RESOURCE_
#define _H_RESOURCE_
#include "common_types.h"
#include "string_table.h"

extern void resource_init(const char *cfg_filename);
extern void resource_free(void);
extern int resource_run(void);
extern int resource_stop(void);
extern BOOL resource_save(void);
BOOL resource_get_integer(int key, int* value);

const char* resource_get_string(int key);

BOOL resource_set_integer(int key, int value);
extern BOOL resource_set_string(int key, const char *value);

#endif /* _H_RESOURCE_ */
