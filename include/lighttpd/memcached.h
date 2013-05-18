#ifndef _LIGHTTPD_MEMCACHED_H_
#define _LIGHTTPD_MEMCACHED_H_

#include <lighttpd/settings.h>
#include <lighttpd/events.h>
#include <lighttpd/buffer.h>

typedef struct liMemcachedCon liMemcachedCon;
typedef struct liMemcachedItem liMemcachedItem;
typedef struct liMemcachedRequest liMemcachedRequest;
typedef enum {
	LI_MEMCACHED_OK, /* STORED, VALUE, DELETED */
	LI_MEMCACHED_NOT_STORED,
	LI_MEMCACHED_EXISTS,
	LI_MEMCACHED_NOT_FOUND,
	LI_MEMCACHED_RESULT_ERROR /* some error occured */
} liMemcachedResult;

typedef void (*liMemcachedCB)(liMemcachedRequest *request, liMemcachedResult result, liMemcachedItem *item, GError **err);

struct liMemcachedItem {
	GString *key;
	guint32 flags;
	li_tstamp ttl;
	guint64 cas;
	liBuffer *data;
};

struct liMemcachedRequest {
	liMemcachedCB callback;
	gpointer cb_data;
};

/* error handling */
#define LI_MEMCACHED_ERROR li_memcached_error_quark()
LI_API GQuark li_memcached_error_quark();

typedef enum {
	LI_MEMCACHED_CONNECTION,
	LI_MEMCACHED_BAD_KEY,
	LI_MEMCACHED_DISABLED, /* disabled right now */
	LI_MEMCACHED_UNKNOWN = 0xff
} liMemcachedError;

LI_API liMemcachedCon* li_memcached_con_new(liEventLoop *loop, liSocketAddress addr);
LI_API void li_memcached_con_acquire(liMemcachedCon* con);
LI_API void li_memcached_con_release(liMemcachedCon* con); /* thread-safe */

/* these functions are not thread-safe, i.e. must be called in the same context as "loop" from li_memcached_con_new */
LI_API liMemcachedRequest* li_memcached_get(liMemcachedCon *con, GString *key, liMemcachedCB callback, gpointer cb_data, GError **err);
LI_API liMemcachedRequest* li_memcached_set(liMemcachedCon *con, GString *key, guint32 flags, li_tstamp ttl, liBuffer *data, liMemcachedCB callback, gpointer cb_data, GError **err);

/* if length(key) <= 250 and all chars x: 0x20 < x < 0x7f the key
 * remains untouched; otherwise it gets replaced with its sha1hex hash
 * so in most cases the key stays readable, and we have a good fallback
 */
LI_API void li_memcached_mutate_key(GString *key);
LI_API gboolean li_memcached_is_key_valid(GString *key);

#endif
