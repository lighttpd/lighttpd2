#ifndef _LIGHTTPD_CONFIGPARSER_H_
#define _LIGHTTPD_CONFIGPARSER_H_

#include <lighttpd/base.h>

typedef struct liConfigParserContext liConfigParserContext;

typedef enum {
	LI_CFG_PARSER_CAST_NONE,
	LI_CFG_PARSER_CAST_INT,
	LI_CFG_PARSER_CAST_STR
} liCastType;

struct liConfigParserContext {
	/* ragel vars */
	int cs;
	int *stack;
	int top;
	int stacksize; /* not really used by ragel but need to remember it */
	char *p, *pe, *eof;

	gchar *mark;
	gboolean in_setup_block;


	gboolean condition_with_key;
	gboolean condition_nonbool;
	gboolean condition_negated;

	liCompOperator op;
	gchar value_op;

	liCastType cast;

	GHashTable *action_blocks; /* foo { } */
	GHashTable *uservars; /* var.foo */

	GQueue *action_list_stack; /* first entry is current action list */
	GQueue *option_stack; /* stack of liValue* */
	GQueue *condition_stack; /* stack of condition* */

	/* information about currenty parsed file */
	gchar *filename;
	gchar *ptr; /* pointer to the data */
	gsize len;
	gsize line; /* holds current line */
};

/* returns a new config parser stack with the first context in it */
LI_API GList* li_config_parser_init(liServer *srv);
LI_API void li_config_parser_finish(liServer *srv, GList *ctx_stack, gboolean free_all);

/* loads a file into memory and parses it */
LI_API gboolean li_config_parser_file(liServer *srv, GList *ctx_stack, const gchar *path);

#endif
