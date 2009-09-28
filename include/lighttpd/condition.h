#ifndef _LIGHTTPD_CONDITION_H_
#define _LIGHTTPD_CONDITION_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/**
 * possible compare ops in the configfile parser
 */
typedef enum {
/* everything */
	LI_CONFIG_COND_EQ,      /** == */
	LI_CONFIG_COND_NE,      /** != */

/* only with string */
	LI_CONFIG_COND_PREFIX,  /** =^ */
	LI_CONFIG_COND_NOPREFIX,/** !^ */
	LI_CONFIG_COND_SUFFIX,  /** =$ */
	LI_CONFIG_COND_NOSUFFIX,/** !$ */

	LI_CONFIG_COND_MATCH,   /** =~ */
	LI_CONFIG_COND_NOMATCH, /** !~ */

	LI_CONFIG_COND_IP,
	LI_CONFIG_COND_NOTIP,

/* only with int */
	LI_CONFIG_COND_GT,      /** > */
	LI_CONFIG_COND_GE,      /** >= */
	LI_CONFIG_COND_LT,      /** < */
	LI_CONFIG_COND_LE       /** <= */
} liCompOperator;

/**
 * possible fields to match against
 */
typedef enum {
	LI_COMP_REQUEST_LOCALIP,
	LI_COMP_REQUEST_REMOTEIP,
	LI_COMP_REQUEST_PATH,
	LI_COMP_REQUEST_HOST,
	LI_COMP_REQUEST_SCHEME,
	LI_COMP_REQUEST_QUERY_STRING,
	LI_COMP_REQUEST_METHOD,
	LI_COMP_REQUEST_CONTENT_LENGTH,
	LI_COMP_PHYSICAL_PATH,
	LI_COMP_PHYSICAL_EXISTS,
	LI_COMP_PHYSICAL_SIZE,
	LI_COMP_PHYSICAL_ISDIR,
	LI_COMP_PHYSICAL_ISFILE,
	LI_COMP_RESPONSE_STATUS,

/* needs a key */
	LI_COMP_REQUEST_HEADER,         /**< needs lowercase key, enforced by li_condition_lvalue_new */
	LI_COMP_RESPONSE_HEADER,         /**< needs lowercase key, enforced by li_condition_lvalue_new */

	LI_COMP_UNKNOWN
} liCondLValue;

#define LI_COND_LVALUE_FIRST_WITH_KEY LI_COMP_REQUEST_HEADER
#define LI_COND_LVALUE_END            (LI_COMP_UNKNOWN)

struct liConditionLValue {
	int refcount;
	liCondLValue type;

	GString *key;
};

typedef enum {
	LI_COND_VALUE_BOOL,
	LI_COND_VALUE_NUMBER,
	LI_COND_VALUE_STRING,
	LI_COND_VALUE_REGEXP,
	LI_COND_VALUE_SOCKET_IPV4,  /** only match ip/netmask */
	LI_COND_VALUE_SOCKET_IPV6   /** only match ip/netmask */
} liCondRValue;

struct liConditionRValue {
	liCondRValue type;

	gboolean b;
	GString *string;
	GRegex *regex;
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

#include <lighttpd/base.h>

struct liCondition {
	int refcount;

	liCompOperator op;
	liConditionLValue *lvalue;
	liConditionRValue rvalue;
};

/* lvalue */
LI_API liConditionLValue* li_condition_lvalue_new(liCondLValue type, GString *key);
LI_API void li_condition_lvalue_acquire(liConditionLValue *lvalue);
LI_API void li_condition_lvalue_release(liConditionLValue *lvalue);

LI_API liCondition* li_condition_new_bool(liServer *srv, liConditionLValue *lvalue, gboolean b);
LI_API liCondition* li_condition_new_string(liServer *srv, liCompOperator op, liConditionLValue *lvalue, GString *str);
LI_API liCondition* li_condition_new_int(liServer *srv, liCompOperator op, liConditionLValue *lvalue, gint64 i);

LI_API void li_condition_acquire(liCondition *c);
LI_API void li_condition_release(liServer *srv, liCondition* c);

LI_API const char* li_comp_op_to_string(liCompOperator op);
LI_API const char* li_cond_lvalue_to_string(liCondLValue t);
LI_API liCondLValue li_cond_lvalue_from_string(const gchar *str, guint len);

LI_API liHandlerResult li_condition_check(liVRequest *vr, liCondition *cond, gboolean *result);

#endif
