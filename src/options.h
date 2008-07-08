#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

typedef enum { OPTION_NONE, OPTION_BOOLEAN, OPTION_INT, OPTION_STRING, OPTION_LIST, OPTION_HASH } option_type;

struct option;
typedef struct option option;

struct option_mark;
typedef struct option_mark option_mark;

#include "base.h"

/* registered options */
GArray *options;
/* hash table to speed up lookup of options by name */
GHashTable *options_hash;

struct option {
	option_type type;
	union {
		gboolean opt_bool;
		gint opt_int;
		GString *opt_string;
		/* array of option */
		GArray *opt_list;
		/* hash GString => option */
		GHashTable *opt_hash;
	} value;
};

struct option_set {
	guint ndx;
	option opt;
};

LI_API option* option_new_bool(gboolean val);
LI_API option* option_new_int(gint val);
LI_API option* option_new_string(GString *val);
LI_API option* option_new_list();
LI_API option* option_new_hash();
LI_API void option_free(option* opt);

/* registers an option */
LI_API gboolean option_register(GString *name, option *opt);
/* unregisters an option */
LI_API gboolean option_unregister(GString *name);
/* retrieves the index of a previously registered option. returns TRUE if option was found, FALSE otherwise */
LI_API gboolean option_index(GString *name, guint *index);

#endif
