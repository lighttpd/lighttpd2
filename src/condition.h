#ifndef _LIGHTTPD_CONDITION_H_
#define _LIGHTTPD_CONDITION_H_

#include "settings.h"

struct condition_rvalue;
typedef struct condition_rvalue condition_rvalue;

struct condition_lvalue;
typedef struct condition_lvalue condition_lvalue;

struct condition;
typedef struct condition condition;

/**
 * possible compare ops in the configfile parser
 */
typedef enum {
/* everything */
	CONFIG_COND_EQ,      /** == */
	CONFIG_COND_NE,      /** != */

/* only with string */
	CONFIG_COND_PREFIX,  /** =^ */
	CONFIG_COND_NOPREFIX,/** !^ */
	CONFIG_COND_SUFFIX,  /** =$ */
	CONFIG_COND_NOSUFFIX,/** !$ */

/* only usable with pcre */
	CONFIG_COND_MATCH,   /** =~ */
	CONFIG_COND_NOMATCH, /** !~ */

	CONFIG_COND_IP,
	CONFIG_COND_NOTIP,

/* only with int */
	CONFIG_COND_GT,      /** > */
	CONFIG_COND_GE,      /** >= */
	CONFIG_COND_LT,      /** < */
	CONFIG_COND_LE       /** <= */
} comp_operator_t;

/**
 * possible fields to match against
 */
typedef enum {
	COMP_REQUEST_LOCALIP,
	COMP_REQUEST_REMOTEIP,
	COMP_REQUEST_PATH,
	COMP_REQUEST_HOST,
	COMP_REQUEST_SCHEME,
	COMP_REQUEST_QUERY_STRING,
	COMP_REQUEST_METHOD,
	COMP_REQUEST_CONTENT_LENGTH,
	COMP_PHYSICAL_PATH,
	COMP_PHYSICAL_PATH_EXISTS,
	COMP_PHYSICAL_SIZE,

/* needs a key */
	COMP_REQUEST_HEADER          /**< needs lowercase key, enforced by condition_lvalue_new */
} cond_lvalue_t;

#define COND_LVALUE_FIRST_WITH_KEY COMP_REQUEST_HEADER
#define COND_LVALUE_END            (1+COMP_REQUEST_HEADER)

struct condition_lvalue {
	int refcount;
	cond_lvalue_t type;

	GString *key;
};

typedef enum {
	COND_VALUE_INT,
	COND_VALUE_STRING,
#ifdef HAVE_PCRE_H
	COND_VALUE_REGEXP,
#endif
	COND_VALUE_SOCKET_IPV4,  /** only match ip/netmask */
	COND_VALUE_SOCKET_IPV6   /** only match ip/netmask */
} cond_rvalue_t;

struct condition_rvalue {
	cond_rvalue_t type;

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
	struct {
		guint8 addr[16];
		guint network;
	} ipv6;
};

#include "base.h"

struct condition {
	int refcount;

	comp_operator_t op;
	condition_lvalue *lvalue;
	condition_rvalue rvalue;
};

/* lvalue */
LI_API condition_lvalue* condition_lvalue_new(cond_lvalue_t type, GString *key);
LI_API void condition_lvalue_acquire(condition_lvalue *lvalue);
LI_API void condition_lvalue_release(condition_lvalue *lvalue);

LI_API condition* condition_new_string(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str);
LI_API condition* condition_new_int(server *srv, comp_operator_t op, condition_lvalue *lvalue, gint64 i);

LI_API void condition_acquire(condition *c);
LI_API void condition_release(server *srv, condition* c);

LI_API const char* comp_op_to_string(comp_operator_t op);
LI_API const char* cond_lvalue_to_string(cond_lvalue_t t);

LI_API gboolean condition_check(server *srv, connection *con, condition *cond);

/* parser */
/** parse an IPv4 (if netmask is not NULL with cidr netmask) */
LI_API gboolean parse_ipv4(const char *str, guint32 *ip, guint32 *netmask);
/** parse an IPv6 (if network is not NULL with cidr network) */
LI_API gboolean parse_ipv6(const char *str, guint8 *ip, guint *network);
LI_API GString* ipv6_tostring(const guint8 ip[16]);

#endif
