#ifndef _LIGHTTPD_CONDITION_H_
#define _LIGHTTPD_CONDITION_H_

/**
 * possible compare ops in the configfile parser
 */
typedef enum {
/* everything */
	CONFIG_COND_EQ,      /** == */
	CONFIG_COND_NE,      /** != */

/* only with strings (including socket name) */
	CONFIG_COND_MATCH,   /** =~ */
	CONFIG_COND_NOMATCH, /** !~ */

/* only with int */
	CONFIG_COND_GT,      /** > */
	CONFIG_COND_GE,      /** >= */
	CONFIG_COND_LT,      /** < */
	CONFIG_COND_LE       /** <= */
} config_cond_t;

/**
 * possible fields to match against
 */
typedef enum {
	COMP_SERVER_SOCKET,
	COMP_REQUEST_PATH,
	COMP_REQUEST_HOST,
	COMP_REQUEST_REFERER,
	COMP_REQUEST_USER_AGENT,
	COMP_REQUEST_COOKIE,
	COMP_REQUEST_SCHEME,
	COMP_REQUEST_REMOTE_IP,
	COMP_REQUEST_QUERY_STRING,
	COMP_REQUEST_METHOD,
	COMP_REQUEST_CONTENT_LENGTH,
	COMP_PHYSICAL_PATH,
	COMP_PHYSICAL_PATH_EXISTS,
	COMP_PHYSICAL_SIZE
} comp_key_t;

typedef enum {
	COND_VALUE_INT,
	COND_VALUE_STRING,
	COND_VALUE_SOCKET_IPV4,  /** only match ip/netmask */
	COND_VALUE_SOCKET_IPV6   /** only match ip/netmask */
} cond_value_t;

struct condition;
typedef struct condition condition;

#include "base.h"

struct condition {
	int refcount;

	config_cond_t cond;
	comp_key_t comp;

	/* index into connection conditional caching table, -1 if uncached */
	int cache_index;

	cond_value_t value_type;
	union {
		GString *string;
#ifdef HAVE_PCRE_H
		struct {
			pcre   *regex;
			pcre_extra *regex_study;
		};
#endif
		gint64 i;
		struct {
			guint32 addr;
			guint32 networkmask;
		} ipv4;
#ifdef HAVE_IPV6
		struct {
			guint8 addr[16];
			guint network;
		} ipv6;
#endif
		sock_addr addr;
	} value;
};

LI_API condition* condition_new_string(server *srv, config_cond_t cond, comp_key_t comp, GString *str);
LI_API condition* condition_new_int(server *srv, config_cond_t cond, comp_key_t comp, gint i);

LI_API condition* condition_new_string_uncached(server *srv, config_cond_t cond, comp_key_t comp, GString *str);
LI_API condition* condition_new_int_uncached(server *srv, config_cond_t cond, comp_key_t comp, gint i);

LI_API void condition_release(condition* c);

LI_API const char* config_cond_to_string(config_cond_t cond);
LI_API const char* comp_key_to_string(comp_key_t comp);


LI_API gboolean condition_check(server *srv, connection *con, condition *cond);

#endif
