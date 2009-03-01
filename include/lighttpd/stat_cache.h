/*
 * stat cache - speeding up stat()s
 *
 * The basic idea behind the stat cache is to reduce calls to stat() which might be slow due to disk io (some ms).
 * Each worker thread has its own cache so no locking contention between threads happens which could be slow.
 * This means that there will be more stat() calls than there would be with only one shared cache but since there
 * should be mostly hits in most cases (few items requested frequently) it will outweight the locking contention.
 * To prevent the stat() from blocking all other requests of that worker, we hand it over to another thread.
 *
 * Entries are removed after 10 seconds (adjustable through stat_cache.ttl setup)
 *
 * TODO:
 *     - stat_cache.ttl setup
 *     - create ETAGs
 *     - get content type from xattr
 *     - add support for inotify (linux). TTL for entries can be increased to 60s
 *
 * Technical details:
 * If a stat is requested, the following procedure takes place:
 * - a cache lookup is performed
 *     - in case of a cache HIT:
 *         - if state is FINISHED and entry is fresh then return entry
 *         - if state is FINISHED but entry old then reset entry, create new job and return NULL
 *         - if state is WAITING then add vrequest to entry and return NULL (looks like a cache miss)
 *     - in case of a cache MISS:
 *         - a new entry is allocated and inserted into the cache, state is set to WAITING
 *         - the entry is inserted into the delete queue
 *         - a new job is created and NULL returned
 *
 * In the delete queue callback we check if no vrequests are working on that entry. If yes, we free it. If not then we requeue it.
 * Locking only happens in two cases: 1) a new job is send to the stat thread 2) the stat thread sends the info back to the worker.
 *
 */

#ifndef _LIGHTTPD_STAT_CACHE_H_
#define _LIGHTTPD_STAT_CACHE_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct stat_cache_entry {
	GString *path;
	GString *etag;
	GString *content_type;
	struct stat st;
	ev_tstamp ts;                  /* timestamp the entry was created (not when the stat() was done) */
	gint err;
	gboolean failed;
	enum {
		STAT_CACHE_ENTRY_WAITING,  /* waiting for stat thread to do the work, no info available */
		STAT_CACHE_ENTRY_FINISHED, /* stat() done, info available */
	} state;
	GPtrArray *vrequests;          /* vrequests waiting for this info */
	guint refcount;
	waitqueue_elem queue_elem;     /* queue element for the delete_queue */
	gboolean in_cache;
};

struct stat_cache {
	GHashTable *entries;
	GAsyncQueue *job_queue_out;    /* elements waiting for stat */
	GAsyncQueue *job_queue_in;     /* elements with finished stat */
	waitqueue delete_queue;
	GThread *thread;
	ev_async job_watcher;
	gdouble ttl;

	guint64 hits;
	guint64 misses;
	guint64 errors;
};

void stat_cache_new(worker *wrk, gdouble ttl);
void stat_cache_free(stat_cache *sc);
void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents);
void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents);
gpointer stat_cache_thread(gpointer data);

void stat_cache_entry_free(stat_cache_entry *sce);

/*
 gets a stat_cache_entry for a specified path
 returns NULL in case of a cache MISS and you should return HANDLER_WAIT_FOR_EVENT
*/
LI_API stat_cache_entry *stat_cache_entry_get(vrequest *vr, GString *path);

/* release a stat_cache_entry so it can be cleaned up */
LI_API void stat_cache_entry_release(vrequest *vr);

#endif