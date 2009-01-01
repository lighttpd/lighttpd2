#ifndef _LIGHTTPD_ENVIRONMENT_H_
#define _LIGHTTPD_ENVIRONMENT_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct environment {
	GHashTable *table;
};

LI_API void environment_init(environment *env);
LI_API void environment_reset(environment *env);
LI_API void environment_clear(environment *env);

/* overwrite previous value */
LI_API void environment_set(environment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);
/* do not overwrite */
LI_API void environment_insert(environment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);
LI_API void environment_remove(environment *env, const gchar *key, size_t keylen);
LI_API GString* environment_get(environment *env, const gchar *key, size_t keylen);

#endif
