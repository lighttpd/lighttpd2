#ifndef _LIGHTTPD_CONFIGPARSER_H_
#define _LIGHTTPD_CONFIGPARSER_H_


#include "condition.h"

struct config_parser_context_t;
typedef struct config_parser_context_t config_parser_context_t;


/* returns a new config parser stack with the first context in it */
GList *config_parser_init(server *srv);
void config_parser_finish(server *srv, GList *ctx_stack);

/* loads a file into memory and parses it */
gboolean config_parser_file(server *srv, GList *ctx_stack, const gchar *path);
/* launched a command through the shell and parses the stdout it returns */
gboolean config_parser_shell(server *srv,GList *ctx_stack, const gchar *command);
/* parses a buffer pointed to by the previously allocated config_parser_data struct */
gboolean config_parser_buffer(server *srv, GList *ctx_stack);

config_parser_context_t *config_parser_context_new(server *srv, GList *ctx_stack);
void config_parser_context_free(server *srv, config_parser_context_t *ctx, gboolean free_queues);

struct config_parser_context_t {
	/* ragel vars */
	int cs;
	int *stack;
	int top;
	int stacksize; /* not really used by ragel but need to remember it */
	char *p, *pe, *eof;

	gchar *mark;
	gboolean in_setup_block;
	gboolean condition_with_key;

	comp_operator_t op;
	gchar value_op;


	GHashTable *action_blocks; /* foo { } */
	GHashTable *uservars; /* var.foo */

	GQueue *action_list_stack; /* first entry is current action list */
	GQueue *option_stack; /* stack of option* */
	GQueue *condition_stack; /* stack of condition* */

	/* information about currenty parsed file */
	gchar *filename;
	gchar *ptr; /* pointer to the data */
	gsize len;
	gsize line; /* holds current line */
};

#endif
