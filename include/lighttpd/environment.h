#ifndef _LIGHTTPD_ENVIRONMENT_H_
#define _LIGHTTPD_ENVIRONMENT_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef struct liEnvironment liEnvironment;

typedef struct liEnvironmentDup liEnvironmentDup;

struct liEnvironment {
	GHashTable *table;
};

/* read only duplicate of a real environment: use it to remember which
   env vars you already sent (mod_fastcgi) */
struct liEnvironmentDup {
	GHashTable *table;
};

LI_API void li_environment_init(liEnvironment *env); /* create table */
LI_API void li_environment_reset(liEnvironment *env); /* remove all entries */
LI_API void li_environment_clear(liEnvironment *env); /* destroy table */

/* overwrite previous value */
LI_API void li_environment_set(liEnvironment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);
/* do not overwrite */
LI_API void li_environment_insert(liEnvironment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);
LI_API void li_environment_remove(liEnvironment *env, const gchar *key, size_t keylen);
LI_API GString* li_environment_get(liEnvironment *env, const gchar *key, size_t keylen);


/* create (data) read only copy of a environment; don't modify the real environment
   while using the duplicate */
LI_API liEnvironmentDup* li_environment_make_dup(liEnvironment *env);
LI_API void li_environment_dup_free(liEnvironmentDup *envdup);
/* remove an entry (this is allowed - it doesn't modify anything in the original environment);
   you must not modify the returned GString */
LI_API GString* li_environment_dup_pop(liEnvironmentDup *envdup, const gchar *key, size_t keylen);

typedef void (*liAddEnvironmentCB)(gpointer param, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);
/* calls callback for various CGI environment variables to add; if the variable is also present
   in envdup, the value from envdup is used instead for the callback and it is popped from envdup.
   Also adds all remaining values from envdup via callback, and then frees envdup. */
LI_API void li_environment_dup2cgi(liVRequest *vr, liEnvironmentDup *envdup, liAddEnvironmentCB callback, gpointer param);

#endif
