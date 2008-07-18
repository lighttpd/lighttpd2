#include <stdio.h>
#include <glib.h>
#include <assert.h>
#include <string.h>

#include "config_parser.h"

#if 1
	#define _printf(fmt, ...) printf(fmt, __VA_ARGS__)
#else
	#define _printf(fmt, ...) /* */
#endif

/** config parser state machine **/

%%{

	machine config_parser;

	variable p cpd->p;
	variable pe cpd->pe;
	variable eof cpd->eof;

	access cpd->;

	# actions
	action mark { cpd->mark = fpc; }
	action mark_var { cpd->mark_var = fpc; }

	prepush {
		_printf("current stacksize: %d, top: %d\n", cpd->stacksize, cpd->top);
		/* increase stacksize if necessary */
		if (cpd->stacksize == cpd->top)
		{
			cpd->stack = g_realloc(cpd->stack, sizeof(int) * (cpd->stacksize + 8));
			cpd->stacksize += 8;
		}
	}

	action boolean {
		cpd->val_type = CONFP_BOOL;
		if (*cpd->mark == 't')
			cpd->val_bool = TRUE;
		else
			cpd->val_bool = FALSE;
		_printf("got boolean %s in line %zd of %s\n", cpd->val_bool ? "true" : "false", cpd->line, cpd->filename);
	}
	action string {
		g_string_truncate(cpd->val_str, 0);
		g_string_append_len(cpd->val_str, cpd->mark + 1, fpc - cpd->mark - 2);
		cpd->val_type = CONFP_STR;
		_printf("got string: \"%s\" in line %zd of %s\n", cpd->val_str->str, cpd->line, cpd->filename);
	}
	action integer
	{
		gchar *c;
		cpd->val_int = 0;
		for (c=cpd->mark; c<fpc; c++)
			cpd->val_int = cpd->val_int * 10 + *c - 48;
		cpd->val_type = CONFP_INT;
		_printf("got integer: %zd in line %zd of %s\n", cpd->val_int, cpd->line, cpd->filename);
	}

	action comment { _printf("got comment in line %zd of %s\n", cpd->line-1, cpd->filename); }
	action value { }
	action valuepair { _printf("got valuepair in line %zd of %s\n", cpd->line, cpd->filename); }
	action line { cpd->line++; }
	action lineWin { cpd->line--; }

	action varname {
		g_string_truncate(cpd->varname, 0);
		g_string_append_len(cpd->varname, cpd->mark_var, fpc - cpd->mark_var);
		_printf("got varname: \"%s\" in line %zd of %s\n", cpd->varname->str, cpd->line, cpd->filename);
	}

	action operator {
		if ((fpc - cpd->mark) == 1) {
			/* 1 char operator: < or > */
			cpd->operator = (*fpc == '<') ? CONFIG_COND_LT : CONFIG_COND_GT;
		}
		else {
			/* 2 char operator */
			char frst = *cpd->mark, scnd = *(fpc-1);
			_printf(" [%c %c] ", frst, scnd);
			if (frst == '<' && scnd == '=') {
				cpd->operator = CONFIG_COND_LE;
			}
			else if (frst == '>' && scnd == '=') {
				cpd->operator = CONFIG_COND_GE;
			}
			else if (frst == '!' && scnd == '=') {
				cpd->operator = CONFIG_COND_NE;
			}
			else if (frst == '=' && scnd == '=') {
				cpd->operator = CONFIG_COND_EQ;
			}
			else if (frst == '=' && scnd == '~') {
				cpd->operator = CONFIG_COND_MATCH;
			}
			else if (frst == '!' && scnd == '~') {
				cpd->operator = CONFIG_COND_NOMATCH;
			}
		}

		_printf("got operator: %s", "");
		for (char* c=cpd->mark; c < fpc; c++)
			_printf("%c", (int)*c);
		_printf(" (%d) in line %zd of %s\n", cpd->operator, cpd->line, cpd->filename);
	}
	action assignment { _printf("got assignment for var %s in line %zd of %s\n", cpd->varname->str, cpd->line, cpd->filename); }
	action function {
		if (g_str_equal(cpd->varname->str, "include") || g_str_equal(cpd->varname->str, "include_shell"))
			break;
		_printf("got function call to %s in line %zd of %s\n", cpd->varname->str, cpd->line, cpd->filename);
	}
	action condition { _printf("got condition for var %s in line %zd of %s\n", cpd->varname->str, cpd->line, cpd->filename); }
	action fooblock { _printf("got fooblock in line %zd of %s\n", cpd->line, cpd->filename); }

	action list { _printf("list\n"); }
	action list_start { /*_printf("list start in line %d\n", line);*/ fcall listscanner; }
	action list_end {
		/*_printf("list end in line %d\n", line);*/
		cpd->val_type = CONFP_LIST;
		fret;
	}

	action hash_start { fcall hashscanner; }
	action hash_end {
		_printf("hash ended in line %zd of %s\n", cpd->line, cpd->filename);
		fret;
	}

	action block_start { /*_printf("block start in line %d\n", line);*/ fcall blockscanner; }
	action block_end { /*_printf("block end in line %d\n", line);*/ fret; }

	action incl {
		_printf("including file %s in line %zd of %s\n", cpd->val_str->str, cpd->line, cpd->filename);
		if (!config_parser_file(srv, cpd_stack, cpd->val_str->str))
			return FALSE;
	}
	action incl_shell {
		if (!config_parser_shell(srv, cpd_stack, cpd->val_str->str))
			return FALSE;
	}

	action done { _printf("done\n"); }

	# tokens
	boolean = ( 'true' | 'false' ) %boolean;
	integer = ( 0 | ( [1-9] [0-9]* ) ) %integer;
	string = ( '"' (any-'"')* '"' ) %string;
	ws = ( ' ' | '\t' );

	lineUnix = ( '\n' ) %line;
	lineMac = ( '\r' ) %line;
	lineWin = ( '\r\n' ) %lineWin;
	line = ( lineUnix | lineMac | lineWin );

	noise = ( ws | line );

	comment = ( '#' (any - line)* line ) %comment;

	value = ( boolean | integer | string ) >mark %value;
	valuepair = ( string ws* '=>' ws* value ) %valuepair;

	list = ( '(' ) >list_start;
	listscanner := ( noise* ((value|valuepair|list) (noise* ',' noise* (value|valuepair|list))*)? noise* ')' >list_end );

	hash = ( '[' ) >hash_start;
	hashelem = ( string >mark %{_printf("hash key \"%s\" ", cpd->val_str->str);} noise* '=>' noise* (value|list|hash) %{_printf("value %s on line %zd of %s\n",
		cpd->val_type == CONFP_BOOL ? "bool" : (cpd->val_type == CONFP_INT ? "int" : (cpd->val_type == CONFP_STR ? "str" : "list or hash")),
	cpd->line, cpd->filename);} );
	hashscanner := ( noise* (hashelem (noise* ',' noise* hashelem)*)? noise* ']' >hash_end );

	varname = ( alpha ( alnum | [_.\-] )* ) >mark_var %varname;

	operator = ( '==' | '!=' | '=~' | '!~' | '<' | '<=' | '>' | '>=' ) >mark %operator;

	assignment = ( varname ws* '=' ws* (value|valuepair|list|hash) ';' ) %assignment;

	function = ( varname (ws+ (value|valuepair|list|hash))? ';' ) %function;

	statement = ( assignment | function );

	incl = ( 'include' ws+ string ';' ) %incl;
	incl_shell = ( 'include_shell' ws+ string ';' ) %incl_shell;

	block = ( '{' >block_start );

	condition_var = (
		'con.ip' | 'req.host' | 'req.path' | 'req.agent' | 'req.scheme' | 'serv.socket' |
		'req.cookie' | 'req.query' | 'req.method' | 'req.length' | 'req.referer' |
		'phys.path' | 'phys.exists' | 'phys.size' | 'resp.size' | 'resp.status'
	) >mark_var;

	condition = ( condition_var %varname ws* operator ws* value >condition noise* block );

	blockscanner := ( (noise | comment | statement | condition | incl | incl_shell )* '}' >block_end );

	fooblock = ( varname ws+ varname ws* block  ) %fooblock;

	main := ( noise | comment | statement | condition | fooblock | incl | incl_shell )* '\00';
}%%

%% write data;

config_parser_data_t *config_parser_data_new() {
	config_parser_data_t *cpd;

	cpd = g_slice_new0(config_parser_data_t);

	cpd->line = 1;

	/* allocate stack of 8 items. sufficient for most configs, will grow when needed */
	cpd->stack = (int*) g_malloc(sizeof(int) * 8);
	cpd->stacksize = 8;


	cpd->val_str = g_string_sized_new(0);
	cpd->varname = g_string_sized_new(0);

	return cpd;
}

void config_parser_data_free(config_parser_data_t *cpd)
{
	g_string_free(cpd->val_str, TRUE);
	g_string_free(cpd->varname, TRUE);

	g_free(cpd->stack);

	g_slice_free(config_parser_data_t, cpd);
}

gboolean config_parser_file(server *srv, GList **cpd_stack, const gchar *path) {
	gboolean res;
	config_parser_data_t *cpd;
	GError *err = NULL;

	cpd = config_parser_data_new();
	cpd->filename = (gchar*) path;

	if (!g_file_get_contents(path, &cpd->ptr, &cpd->len, &err))
	{
		/* could not read file */
		/* TODO: die("could not read config file. reason: \"%s\" (%d)\n", err->message, err->code); */
		_printf("could not read config file \"%s\". reason: \"%s\" (%d)\n", path, err->message, err->code);
		config_parser_data_free(cpd);
		g_error_free(err);
		return FALSE;
	}

	/* push on stack */
	*cpd_stack = g_list_prepend(*cpd_stack, cpd);

	res = config_parser_buffer(srv, cpd_stack);

	/* pop from stack */
	*cpd_stack = g_list_delete_link(*cpd_stack, *cpd_stack);

	/* have to free the buffer on our own */
	g_free(cpd->ptr);
	config_parser_data_free(cpd);

	return res;
}

gboolean config_parser_shell(server *srv, GList **cpd_stack, const gchar *command)
{
	gboolean res;
	gchar* _stdout;
	gchar* _stderr;
	gint status;
	config_parser_data_t *cpd;
	GError *err = NULL;

	cpd = config_parser_data_new();
	cpd->filename = (gchar*) command;

	if (!g_spawn_command_line_sync(command, &_stdout, &_stderr, &status, &err))
	{
		_printf("error launching shell command \"%s\": %s (%d)\n", command, err->message, err->code);
		config_parser_data_free(cpd);
		g_error_free(err);
		return FALSE;
	}

	if (status != 0)
	{
		_printf("shell command \"%s\" exited with status %d\n", command, status);
		_printf("%s\n----\n%s\n", _stdout, _stderr);
		g_free(_stdout);
		g_free(_stderr);
		config_parser_data_free(cpd);
		return FALSE;
	}

	cpd->len = strlen(_stdout);
	cpd->ptr = _stdout;

	_printf("included shell output from \"%s\" (%zu bytes):\n%s\n", command, cpd->len, _stdout);

	*cpd_stack = g_list_prepend(*cpd_stack, cpd);
	res = config_parser_buffer(srv, cpd_stack);
	*cpd_stack = g_list_delete_link(*cpd_stack, *cpd_stack);

	g_free(_stdout);
	g_free(_stderr);
	config_parser_data_free(cpd);

	return res;
}

gboolean config_parser_buffer(server *srv, GList **cpd_stack)
{
	config_parser_data_t *cpd;

	/* get top of stack */
	cpd = (*cpd_stack)->data;

	cpd->p = cpd->ptr;
	cpd->pe = cpd->ptr + cpd->len + 1; /* marks the end of the data to scan (+1 because of trailing \0 char) */

	%% write init;

	%% write exec;

	if (cpd->cs == config_parser_error || cpd->cs == config_parser_first_final)
	{
		/* parse error */
		printf("parse error in line %zd of \"%s\" at character %c (0x%.2x)\n", cpd->line, cpd->filename, *cpd->p, *cpd->p);
		return FALSE;
	}

	return TRUE;
}
