#pragma once
#include "pop3_parser.h"

/* enumeration for the return value of pop3_parser_dispatch_cmd */
enum{
    DISPATCH_CONTINUE,
    DISPATCH_SHOULD_CLOSE,
    DISPATCH_DATA,
	DISPATCH_LIST
};

#ifdef __cplusplus
extern "C" {
#endif

int pop3_cmd_handler_capa(const char *cmd_line, int line_length,
	POP3_CONTEXT *pcontext);

int pop3_cmd_handler_stls(const char *cmd_line, int line_length,
	POP3_CONTEXT *pcontext);

int pop3_cmd_handler_user(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_pass(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_stat(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_uidl(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_list(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_retr(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_rset(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_noop(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_dele(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_top(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_quit(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

int pop3_cmd_handler_else(const char* cmd_line, int line_length,
    POP3_CONTEXT *pcontext);

#ifdef __cplusplus
} /* extern "C" */
#endif
