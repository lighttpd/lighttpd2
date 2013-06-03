#ifndef _LIGHTTPD_FETCH_H_
#define _LIGHTTPD_FETCH_H_

#include <lighttpd/events.h>

/* API to "fetch" data async. entries are revalidated on every lookup; revalidation should only check
 * against a simple TTL. revalidation can trigger refresh, which doesn't invalidate the current entry,
 * but starts a new lookup to check whether an update is needed.
 *
 * If a lookup fails ("key not found") it still keeps an entry with a NULL data,
 * which is also revalidated on lookup (you should use a TTL for negative hits too).
 */


typedef struct liFetchDatabase liFetchDatabase;
typedef struct liFetchCallbacks liFetchCallbacks;
typedef struct liFetchEntry liFetchEntry;
typedef struct liFetchWait liFetchWait;

typedef void (*liFetchWakeupCB)(gpointer wakeup_data);

struct liFetchEntry {
	GString *key;
	gpointer data; /* read-only after li_fetch_entry_ready() */

	gpointer backend_data;
};

struct liFetchCallbacks {
	/* key is in entry->key. set entry->data (and entry->backend_data if needed).
	 * call li_fetch_entry_ready(entry) if done. if entry->data == NULL it means "not found"
	 * entry->data/backend_data could already contain data from a refresh with li_fetch_entry_refresh_skip().
	 */
	void (*lookup)(liFetchDatabase* db, gpointer data, liFetchEntry *entry);

	/* called on every lookup, should do a very simple check to verify the entry is still valid.
	 * DON'T MODIFY entry->data! return FALSE to trigger a new lookup, return TRUE if entry is still valid.
	 * you can trigger a refresh before returing TRUE with li_fetch_entry_refresh(entry)
	 */
	gboolean (*revalidate)(liFetchDatabase* db, gpointer data, liFetchEntry *entry);

	/* check whether entry should be updated in background. put new data in new_entry.
	 * call li_fetch_entry_refresh_skip() if old entry is still good (new entry will be deleted)
	 * call li_fetch_entry_refresh_ready() if old entry should be replaced.
	 */
	void (*refresh)(liFetchDatabase* db, gpointer data, liFetchEntry *cur_entry, liFetchEntry *new_entry);

	/* optional */
	void (*free_entry)(gpointer data, liFetchEntry *entry);

	/* optional. only called after all entries are freed */
	void (*free_db)(gpointer data);
};

/***********************************************/
/*                generic API                  */
/***********************************************/
LI_API void li_fetch_database_acquire(liFetchDatabase* db);
LI_API void li_fetch_database_release(liFetchDatabase* db);

LI_API void li_fetch_entry_acquire(liFetchEntry *entry);
LI_API void li_fetch_entry_release(liFetchEntry *entry);

/* "management" API */
LI_API void li_fetch_invalidate(liFetchDatabase* db, GString *key);

/***********************************************/
/*              API for backends               */
/***********************************************/
LI_API liFetchDatabase* li_fetch_database_new(const liFetchCallbacks *callbacks, gpointer data, guint cache_size, guint neg_cache_size);

/* mark entry as ready to be used. call after lookup() is done */
LI_API void li_fetch_entry_ready(liFetchEntry *entry);

/* trigger a refresh in the background while the entry is still valid. while a refresh is already in progress further refreshes are ignored */
LI_API void li_fetch_entry_refresh(liFetchEntry *entry);
/* li_fetch_entry_refresh_skip can trigger a new lookup if old entry got invalid */
LI_API void li_fetch_entry_refresh_skip(liFetchEntry *new_entry);
LI_API void li_fetch_entry_refresh_ready(liFetchEntry *new_entry);

/***********************************************/
/*              API for frontend               */
/***********************************************/

LI_API gboolean li_fetch_entry_revalidate(liFetchEntry *entry);

/* result == NULL: waiting, jobref will be triggered if ready (call _get again).
 * result != NULL:
 *   call li_fetch_entry_release if you're done with it
 *   result->data == NULL: not found
 */
LI_API liFetchEntry* li_fetch_get(liFetchDatabase* db, GString *key, liJobRef *jobref, liFetchWait **wait);

/* result == NULL: waiting, wakeup(wakeup_data) will be called if ready (call _get again, can get called in any thread context)
 * result != NULL:
 *   call li_fetch_entry_release if you're done with it
 *   result->data == NULL: not found
 */
LI_API liFetchEntry* li_fetch_get2(liFetchDatabase* db, GString *key, liFetchWakeupCB wakeup, gpointer wakeup_data, liFetchWait **wait);

LI_API void li_fetch_cancel(liFetchWait **wait);

#endif
