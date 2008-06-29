#ifndef _LIGHTTPD_CONDITION_H_
#define _LIGHTTPD_CONDITION_H_

/**
 * possible compare ops in the configfile parser
 */
typedef enum {
	CONFIG_COND_EQ,      /** == */
	CONFIG_COND_MATCH,   /** =~ */
	CONFIG_COND_NE,      /** != */
	CONFIG_COND_NOMATCH, /** !~ */
	CONFIG_COND_GT,      /** > */
	CONFIG_COND_GE,      /** >= */
	CONFIG_COND_LT,      /** < */
	CONFIG_COND_LE       /** <= */
} config_cond_t;

/**
 * possible fields to match against
 */
typedef enum {
	COMP_UNSET,
	COMP_SERVER_SOCKET,
	COMP_HTTP_PATH,
	COMP_HTTP_HOST,
	COMP_HTTP_REFERER,
	COMP_HTTP_USER_AGENT,
	COMP_HTTP_COOKIE,
	COMP_HTTP_SCHEME,
	COMP_HTTP_REMOTE_IP,
	COMP_HTTP_QUERY_STRING,
	COMP_HTTP_REQUEST_METHOD,
	COMP_PHYSICAL_PATH,
	COMP_PHYSICAL_PATH_EXISTS,

	COMP_LAST_ELEMENT
} comp_key_t;

struct condition;
typedef struct condition condition;

#include "base.h"

struct condition {
	int refcount;

	config_cond_t cond;
	comp_key_t comp;

	/* index into connection conditional caching table, -1 if uncached */
	int cache_index;

	union {
		GString *string;
#ifdef HAVE_PCRE_H
		struct {
			pcre   *regex;
			pcre_extra *regex_study;
		};
#endif
		gint i;
	} value;
};

LI_API condition* condition_new_string(server *srv, config_cond_t cond, comp_key_t comp, GString *str);
LI_API condition* condition_new_int(server *srv, config_cond_t cond, comp_key_t comp, gint i);

LI_API condition* condition_new_string_uncached(server *srv, config_cond_t cond, comp_key_t comp, GString *str);
LI_API condition* condition_new_int_uncached(server *srv, config_cond_t cond, comp_key_t comp, gint i);

LI_API void condition_release(condition* c);

LI_API const char* config_cond_to_string(config_cond_t cond);
LI_API const char* comp_key_to_string(comp_key_t comp);

#endif
