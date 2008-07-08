struct config_parser_data_t;
typedef struct config_parser_data_t config_parser_data_t;

struct config_parser_filestack_entry_t;
typedef struct config_parser_filestack_entry_t config_parser_filestack_entry_t;

/* loads a file into memory and parses it */
gboolean config_parser_file(const gchar *path);
/* launched a command through the shell and parses the stdout it returns */
gboolean config_parser_shell(const gchar *command);
/* parses a buffer pointed to by the previously allocated config_parser_data struct */
gboolean config_parser_buffer();

struct config_parser_data_t {
	/* information of currently parsed file */
	gchar *filename;
	gchar *ptr;
	gsize len;
	gsize line;
	int stacksize;

	/* ragel vars */
	int cs;
	int *stack;
	int top;
	char *p, *pe, *eof;

	/* markers to start of current data */
	gchar *mark;
	gchar *mark_var;

	/* current value */
	enum { CONFP_BOOL, CONFP_INT, CONFP_STR, CONFP_LIST, CONFP_HASH } val_type;
	GString *val_str;
	gint64 val_int;
	gboolean val_bool;

	/* name of current variable */
	GString *varname;
};
