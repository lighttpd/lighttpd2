#ifndef _LIGHTTPD_ENVIRONMENT_H_
#define _LIGHTTPD_ENVIRONMENT_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct environment {
	GHashTable *table;
};

/* read only duplicate of a real environment: use it to remember which
   env vars you already sent (mod_fastcgi) */
struct environment_dup {
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


/* create (data) read only copy of a environment; don't modify the real environment
   while using the duplicate */
LI_API environment_dup* environment_make_dup(environment *env);
LI_API void environment_dup_free(environment_dup *envdup);
/* remove an entry (this is allowed - it doesn't modify anything in the original environment);
   you must not modify the returned GString */
LI_API GString* environment_dup_pop(environment_dup *envdup, const gchar *key, size_t keylen);


#endif
