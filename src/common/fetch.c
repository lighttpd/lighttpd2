#include <lighttpd/fetch.h>

struct liFetchDatabase {
	guint refcount, internal_refcount;
	GMutex *lock;
	GHashTable *cache; /* GString -> liFetchEntryP. key is in entry->public.key */
	GQueue lru_queue, lru_negative_queue;

	gint cache_size, neg_cache_size;
	const liFetchCallbacks *callbacks;
	gpointer data;
};

typedef struct liFetchEntryP liFetchEntryP;

typedef enum {
	LI_ENTRY_LOOKUP,
	LI_ENTRY_VALID,
	LI_ENTRY_REFRESH_OLD,
	LI_ENTRY_REFRESH_INVALID, /* got invalid while refreshing */
	LI_ENTRY_REFRESH_NEW,
	LI_ENTRY_INVALID
} liFetchEntryState;

struct liFetchEntryP {
	gint refcount;
	liFetchDatabase *db; /* keeps "internal" reference */
	liFetchEntry public;

	gint state; /* atomic access, write only with db->lock */
	GQueue wait_queue; /* <liFetchWaitElement links> */
	liFetchEntryP *refreshing;
	GList lru_link; /* only in LI_ENTRY_VALID state */
};

/* struct liFetchWait is not defined; liFetchWait* is used as liFetchEntryP* */

typedef struct liFetchWaitElement liFetchWaitElement;
struct liFetchWaitElement {
	liFetchWakeupCB wakeup;
	GList wait_link;
};

static GQueue entry_extract_wait_queue(liFetchEntryP *pentry) {
	GQueue q = pentry->wait_queue;
	pentry->wait_queue.head = pentry->wait_queue.tail = NULL;
	pentry->wait_queue.length = 0;
	return q;
}

static void wakeup_jobs(GQueue *wait_queue) {
	GList *wait_link;

	while (NULL != (wait_link = g_queue_pop_head_link(wait_queue))) {
		liFetchWaitElement *elem = LI_CONTAINER_OF(wait_link, liFetchWaitElement, wait_link);
		elem->wakeup(elem->wait_link.data);
		g_slice_free(liFetchWaitElement, elem);
	}
}

static gboolean wakeup_has_job(GQueue *wait_queue, liFetchWakeupCB wakeup, gpointer wakeup_data) {
	GList *wait_link;

	for (wait_link = wait_queue->head; NULL != wait_link; wait_link = wait_link->next) {
		liFetchWaitElement *elem = LI_CONTAINER_OF(wait_link, liFetchWaitElement, wait_link);
		if (wakeup == elem->wakeup && wakeup_data == elem->wait_link.data) return TRUE;
	}

	return FALSE;
}

static void wakeup_add_job(GQueue *wait_queue, liFetchWakeupCB wakeup, gpointer wakeup_data) {
	liFetchWaitElement *elem = g_slice_new0(liFetchWaitElement);
	elem->wait_link.data = wakeup_data;
	elem->wakeup = wakeup;
	g_queue_push_tail_link(wait_queue, &elem->wait_link);
}

static void remove_from_lru(liFetchEntryP *pentry) {
	g_queue_unlink(
		NULL != pentry->public.data ? &pentry->db->lru_queue : &pentry->db->lru_negative_queue,
		&pentry->lru_link);
}

static void append_to_lru(liFetchEntryP *pentry) {
	liFetchDatabase *db = pentry->db;
	GQueue *q = NULL != pentry->public.data ? &db->lru_queue : &db->lru_negative_queue;
	guint limit = NULL != pentry->public.data ? db->cache_size : db->neg_cache_size;

	assert(LI_ENTRY_VALID == pentry->state);
	g_queue_push_tail_link(q, &pentry->lru_link);
	while (q->length > limit) {
		GList *lru_link  =g_queue_peek_head_link(q);
		liFetchEntryP *purge_entry = LI_CONTAINER_OF(lru_link, liFetchEntryP, lru_link);
		g_hash_table_remove(db->cache, purge_entry->public.key);
	}
}

static void fetch_db_int_acquire(liFetchDatabase* db) {
	assert(g_atomic_int_get(&db->internal_refcount) > 0);
	g_atomic_int_inc(&db->internal_refcount);
}

static void fetch_db_int_release(liFetchDatabase* db) {
	assert(g_atomic_int_get(&db->internal_refcount) > 0);
	if (g_atomic_int_dec_and_test(&db->internal_refcount)) {
		db->internal_refcount = 1;
		assert(NULL == db->cache);

		if (NULL != db->callbacks->free_db) db->callbacks->free_db(db->data);

		g_mutex_free(db->lock);
		db->lock = NULL;
		db->data = NULL;
		db->callbacks = NULL;
		assert(1 == db->internal_refcount);
		g_slice_free(liFetchDatabase, db);
	}
}

void li_fetch_database_acquire(liFetchDatabase* db) {
	assert(g_atomic_int_get(&db->refcount) > 0);
	assert(g_atomic_int_get(&db->internal_refcount) > 0);
	g_atomic_int_inc(&db->refcount);
}

void li_fetch_database_release(liFetchDatabase* db) {
	assert(g_atomic_int_get(&db->refcount) > 0);
	assert(g_atomic_int_get(&db->internal_refcount) > 0);
	if (g_atomic_int_dec_and_test(&db->refcount)) {
		GHashTable *cache = db->cache;
		db->refcount = 1;

		g_mutex_lock(db->lock);

			assert(NULL != cache);
			db->cache = NULL;
			g_hash_table_destroy(cache);

		g_mutex_unlock(db->lock);

		assert(1 == db->refcount);
		db->refcount = 0;
		fetch_db_int_release(db);
	}
}

void li_fetch_entry_acquire(liFetchEntry *entry) {
	liFetchEntryP *pentry = LI_CONTAINER_OF(entry, liFetchEntryP, public);
	assert(g_atomic_int_get(&pentry->refcount) > 0);
	g_atomic_int_inc(&pentry->refcount);
}

void li_fetch_entry_release(liFetchEntry *entry) {
	liFetchEntryP *pentry;
	if (NULL == entry) return;

	pentry = LI_CONTAINER_OF(entry, liFetchEntryP, public);
	assert(g_atomic_int_get(&pentry->refcount) > 0);
	if (g_atomic_int_dec_and_test(&pentry->refcount)) {
		liFetchEntryState state = g_atomic_int_get(&pentry->state);
		liFetchDatabase *db = pentry->db;
		pentry->refcount = 1;

		if (NULL != db->callbacks->free_entry) db->callbacks->free_entry(db->data, entry);
		pentry->public.data = pentry->public.backend_data = NULL;
		g_string_free(pentry->public.key, TRUE);
		pentry->public.key = NULL;

		assert(LI_ENTRY_INVALID == state);

		pentry->db = NULL;
		fetch_db_int_release(db);
		assert(1 == pentry->refcount);
		pentry->refcount = 0;
		g_slice_free(liFetchEntryP, pentry);
	}
}

void li_fetch_invalidate(liFetchDatabase* db, GString *key) {
	fetch_db_int_acquire(db);
	g_mutex_lock(db->lock);

	{
		liFetchEntryP *pentry = g_hash_table_lookup(db->cache, key);
		liFetchEntryState state;

		if (NULL == pentry) goto out;

		state = g_atomic_int_get(&pentry->state);
		assert(LI_ENTRY_REFRESH_NEW != state && LI_ENTRY_INVALID != state); /* this is never in the cache */
		switch (state) {
		case LI_ENTRY_LOOKUP:
			goto out;
		case LI_ENTRY_VALID:
			/* hashtable destroy callback will mark as invalid */
			break;
		case LI_ENTRY_REFRESH_OLD:
			g_atomic_int_set(&pentry->state, LI_ENTRY_REFRESH_INVALID);
			goto out;
		case LI_ENTRY_REFRESH_INVALID:
			goto out;
		case LI_ENTRY_REFRESH_NEW: /* not reachable */
			break;
		case LI_ENTRY_INVALID: /* not reachable */
			break;
		}

		g_hash_table_remove(db->cache, key);
	}

out:
	g_mutex_unlock(db->lock);
	fetch_db_int_release(db);
}

/* db is already locked */
static void cache_delete_data_cb(gpointer data) {
	liFetchEntryP *pentry = data;
	liFetchEntryState state = g_atomic_int_get(&pentry->state);
	GQueue wait_queue = G_QUEUE_INIT;
	liFetchEntryP *new_entry = NULL;

	assert(LI_ENTRY_REFRESH_NEW != state && LI_ENTRY_INVALID != state); /* this is never in the cache */

	switch (state) {
	case LI_ENTRY_LOOKUP:
		wait_queue = entry_extract_wait_queue(pentry);
		break;
	case LI_ENTRY_VALID:
		remove_from_lru(pentry);
		break;
	case LI_ENTRY_REFRESH_OLD:
		new_entry = pentry->refreshing;
		pentry->refreshing = NULL;
		break;
	case LI_ENTRY_REFRESH_INVALID:
		new_entry = pentry->refreshing;
		pentry->refreshing = NULL;
		wait_queue = pentry->wait_queue;
		g_queue_init(&pentry->wait_queue);
		break;
	case LI_ENTRY_REFRESH_NEW: /* not reachable */
		break;
	case LI_ENTRY_INVALID: /* not reachable */
		break;
	}
	g_atomic_int_set(&pentry->state, LI_ENTRY_INVALID);

	if (NULL != new_entry) {
		assert(pentry == new_entry->refreshing);
		assert(LI_ENTRY_REFRESH_NEW == g_atomic_int_get(&new_entry->state));
		new_entry->refreshing = NULL;
		g_atomic_int_set(&new_entry->state, LI_ENTRY_INVALID);
		li_fetch_entry_release(&pentry->public);
		li_fetch_entry_release(&new_entry->public);
	}

	wakeup_jobs(&wait_queue);

	li_fetch_entry_release(&pentry->public);
}

liFetchDatabase* li_fetch_database_new(const liFetchCallbacks *callbacks, gpointer data, guint cache_size, guint neg_cache_size) {
	liFetchDatabase *db = g_slice_new0(liFetchDatabase);

	db->refcount = 1;
	db->internal_refcount = 1; /* db->refcount > 0 keeps internal ref */
	db->lock = g_mutex_new();
	db->cache = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, NULL, cache_delete_data_cb);
	db->cache_size = cache_size;
	db->neg_cache_size = neg_cache_size;
	db->callbacks = callbacks;
	db->data = data;

	return db;
}

void li_fetch_entry_ready(liFetchEntry *entry) {
	/* releases entry reference */
	liFetchEntryP *pentry = LI_CONTAINER_OF(entry, liFetchEntryP, public);
	liFetchDatabase *db = pentry->db;
	GQueue wait_queue = G_QUEUE_INIT;

	g_mutex_lock(db->lock);

	{
		liFetchEntryState state = g_atomic_int_get(&pentry->state);
		wait_queue = entry_extract_wait_queue(pentry);

		if (LI_ENTRY_INVALID == state) goto out;

		assert(LI_ENTRY_LOOKUP == state);

		g_atomic_int_set(&pentry->state, LI_ENTRY_VALID);
		append_to_lru(pentry);
	}

out:
	g_mutex_unlock(db->lock);

	li_fetch_entry_release(entry);

	wakeup_jobs(&wait_queue);
}

void li_fetch_entry_refresh(liFetchEntry *entry) {
	liFetchEntryP *pentry = LI_CONTAINER_OF(entry, liFetchEntryP, public);
	liFetchDatabase *db = pentry->db;
	liFetchEntryP *new_entry = NULL;

	li_fetch_entry_acquire(entry);
	g_mutex_lock(db->lock);

	{
		liFetchEntryState state = g_atomic_int_get(&pentry->state);

		if (NULL == db->cache) goto out; /* cache already destroyed, nothing to refresh */

		switch (state) {
		case LI_ENTRY_LOOKUP:
			goto out;
		case LI_ENTRY_VALID:
			remove_from_lru(pentry);
			break;
		case LI_ENTRY_REFRESH_OLD:
			goto out;
		case LI_ENTRY_REFRESH_INVALID:
			goto out;
		case LI_ENTRY_REFRESH_NEW:
			goto out;
		case LI_ENTRY_INVALID:
			goto out;
		}

		fetch_db_int_acquire(db);
		new_entry = g_slice_new0(liFetchEntryP);
		new_entry->db = db;
		new_entry->state = LI_ENTRY_REFRESH_NEW;
		new_entry->refcount = 2; /* one in pentry->refreshing, the other for the callback */
		new_entry->public.key = g_string_new_len(GSTR_LEN(entry->key));
		new_entry->public.data = new_entry->public.backend_data = NULL;

		g_atomic_int_set(&pentry->state, LI_ENTRY_REFRESH_OLD);

		li_fetch_entry_acquire(entry);
		new_entry->refreshing = pentry;
		pentry->refreshing = new_entry;
	}

out:
	g_mutex_unlock(db->lock);

	if (NULL != new_entry) {
		db->callbacks->refresh(db, db->data, &pentry->public, &new_entry->public);
	}

	li_fetch_entry_release(entry);
}

void li_fetch_entry_refresh_skip(liFetchEntry *new_entry) {
	/* releases new_entry reference */
	liFetchEntryP *pnew_entry = LI_CONTAINER_OF(new_entry, liFetchEntryP, public);
	liFetchDatabase *db = pnew_entry->db;
	liFetchEntryP *pentry;
	gboolean new_lookup = FALSE;

	g_mutex_lock(db->lock);

	{
		liFetchEntryState state = g_atomic_int_get(&pnew_entry->state);
		g_atomic_int_set(&pnew_entry->state, LI_ENTRY_INVALID);

		if (LI_ENTRY_INVALID == state) goto out;

		assert(LI_ENTRY_REFRESH_NEW == state);

		pentry = pnew_entry->refreshing;
		pnew_entry->refreshing = NULL;
		assert(pnew_entry == pentry->refreshing);

		state = g_atomic_int_get(&pentry->state);
		assert(LI_ENTRY_REFRESH_OLD == state || LI_ENTRY_REFRESH_INVALID == state);
		if (LI_ENTRY_REFRESH_OLD == state) {
			g_atomic_int_set(&pentry->state, LI_ENTRY_VALID);
			append_to_lru(pentry);
		} else { /* LI_ENTRY_REFRESH_INVALID */
			if (pnew_entry->wait_queue.length > 0) {
				/* someone could be waiting. trigger a new lookup */
				g_atomic_int_set(&pnew_entry->state, LI_ENTRY_LOOKUP);
				new_lookup = TRUE;
				/* use new entry, as waiting entries are using it */
				g_hash_table_replace(db->cache, pnew_entry->public.key, pnew_entry);
			} else {
				/* no one waiting, just remove the cache entry */
				g_hash_table_remove(db->cache, pentry->public.key);
			}
		}

		li_fetch_entry_release(&pnew_entry->public);
		li_fetch_entry_release(&pentry->public);
	}

out:
	g_mutex_unlock(db->lock);

	if (new_lookup) {
		db->callbacks->lookup(db, db->data, &pnew_entry->public);
	} else {
		li_fetch_entry_release(&pnew_entry->public);
	}
}

void li_fetch_entry_refresh_ready(liFetchEntry *new_entry) {
	/* releases new_entry reference */
	liFetchEntryP *pnew_entry = LI_CONTAINER_OF(new_entry, liFetchEntryP, public);
	liFetchDatabase *db = pnew_entry->db;
	liFetchEntryP *pentry;

	GQueue wait_queue = G_QUEUE_INIT;

	g_mutex_lock(db->lock);

	{
		liFetchEntryState state = g_atomic_int_get(&pnew_entry->state);

		if (LI_ENTRY_INVALID == state) goto out;

		assert(LI_ENTRY_REFRESH_NEW == state);

		pentry = pnew_entry->refreshing;
		pnew_entry->refreshing = NULL;
		assert(pnew_entry == pentry->refreshing);

		state = g_atomic_int_get(&pentry->state);
		assert(LI_ENTRY_REFRESH_OLD == state || LI_ENTRY_REFRESH_INVALID == state);

		wait_queue = entry_extract_wait_queue(pnew_entry);

		li_fetch_entry_release(&pentry->public);
		/* not releasing &pnew_entry->public - insert into cache instead */

		g_atomic_int_set(&pentry->state, LI_ENTRY_INVALID);
		g_atomic_int_set(&pnew_entry->state, LI_ENTRY_VALID);
		g_hash_table_replace(db->cache, pentry->public.key, pentry);
		append_to_lru(pnew_entry);
		/* old entry wasn't in LRU anymore as it was in a REFRESH, not in a VALID state */
	}

out:
	g_mutex_unlock(db->lock);

	li_fetch_entry_release(&pnew_entry->public);

	wakeup_jobs(&wait_queue);
}

gboolean li_fetch_entry_revalidate(liFetchEntry *entry) {
	liFetchEntryP *pentry = LI_CONTAINER_OF(entry, liFetchEntryP, public);
	liFetchDatabase *db = pentry->db;
	liFetchEntryState state = g_atomic_int_get(&pentry->state);

	assert(LI_ENTRY_REFRESH_NEW != state);
	switch (state) {
	case LI_ENTRY_LOOKUP:
		return FALSE;
	case LI_ENTRY_VALID:
		break;
	case LI_ENTRY_REFRESH_OLD:
		break;
	case LI_ENTRY_REFRESH_INVALID:
		return FALSE;
	case LI_ENTRY_REFRESH_NEW:
		return FALSE;
	case LI_ENTRY_INVALID:
		return FALSE;
	}

	return db->callbacks->revalidate(db, db->data, entry);
}

static void wakeup_jobref(gpointer data) {
	liJobRef *jobref = (liJobRef*) data;
	li_job_async(jobref);
	li_job_ref_release(jobref);
}

liFetchEntry* li_fetch_get(liFetchDatabase* db, GString *key, liJobRef *jobref, liFetchWait **wait) {
	liFetchEntry *entry;

	if (NULL == *wait) {
		li_job_ref_acquire(jobref);
	}

	entry = li_fetch_get2(db, key, wakeup_jobref, jobref, wait);

	if (NULL == *wait) {
		li_job_ref_release(jobref);
	}

	return entry;
}

liFetchEntry* li_fetch_get2(liFetchDatabase* db, GString *key, liFetchWakeupCB wakeup, gpointer wakeup_data, liFetchWait **wait) {
	liFetchEntryP *pentry, *pentry_new = NULL;
	liFetchEntryP *result = NULL;
	liFetchEntryState state;

	assert(NULL != wakeup);

	g_mutex_lock(db->lock);

	if (*wait != NULL) {
		pentry = (liFetchEntryP*) *wait;

		if (wakeup_has_job(&pentry->wait_queue, wakeup, wakeup_data)) goto out; /* not notified yet */

		/* notified, don't care about state - it was good once */
		*wait = NULL;
		result = pentry;
		goto out;
	}

	pentry = g_hash_table_lookup(db->cache, key);
	if (NULL != pentry) {
		li_fetch_entry_acquire(&pentry->public);
		g_mutex_unlock(db->lock);
			/* unlock for revalidation and refresh */
			if (li_fetch_entry_revalidate(&pentry->public)) {
				/* db already unlocked */
				return &pentry->public;
			}
			li_fetch_entry_refresh(&pentry->public);
		g_mutex_lock(db->lock);

		pentry_new = pentry;

		state = g_atomic_int_get(&pentry->state);
		assert(LI_ENTRY_REFRESH_NEW != state);
		switch (state) {
		case LI_ENTRY_LOOKUP:
			/* lookup in progress, just register wakeup */
			break;
		case LI_ENTRY_VALID:
			/* refresh already done - fresh entry, return */
			result = pentry;
			goto out;
		case LI_ENTRY_REFRESH_OLD:
			/* refresh in progress, but data is now invalid. register wakeup */
			g_atomic_int_set(&pentry->state, LI_ENTRY_REFRESH_INVALID);
			pentry_new = pentry->refreshing; /* wait on refresh "new" entry */
			li_fetch_entry_acquire(&pentry_new->public);
			li_fetch_entry_release(&pentry->public);
			break;
		case LI_ENTRY_REFRESH_INVALID:
			/* refresh in progress, already invalid. just register wakeup */
			pentry_new = pentry->refreshing; /* wait on refresh "new" entry */
			li_fetch_entry_acquire(&pentry_new->public);
			li_fetch_entry_release(&pentry->public);
			break;
		case LI_ENTRY_REFRESH_NEW: /* not reachable */
			break;
		case LI_ENTRY_INVALID:
			/* entry got removed from cache, can't add wakeup anymore */
			/* if there is an entry in the cache it is fresh enough,
			 * otherwise do a new lookup */
			li_fetch_entry_release(&pentry->public);
			result = g_hash_table_lookup(db->cache, key);
			if (NULL != result) {
				li_fetch_entry_acquire(&pentry_new->public);
				goto out;
			}
			goto create_new_entry;
		}

		wakeup_add_job(&pentry_new->wait_queue, wakeup, wakeup_data);

		*wait = (liFetchWait*) pentry_new;
	} else {
create_new_entry:
		/* create new entry */
		fetch_db_int_acquire(db);
		pentry = g_slice_new0(liFetchEntryP);
		pentry->db = db;
		pentry->state = LI_ENTRY_LOOKUP;
		pentry->refcount = 3; /* one for us, one for cache, one for the callback */
		pentry->public.key = g_string_new_len(GSTR_LEN(key));
		pentry->public.data = pentry->public.backend_data = NULL;

		g_hash_table_replace(db->cache, pentry->public.key, pentry);

		g_mutex_unlock(db->lock);
			/* unlock for lookup */
			db->callbacks->lookup(db, db->data, &pentry->public);

			state = g_atomic_int_get(&pentry->state);
			if (LI_ENTRY_LOOKUP != state) return &pentry->public;
		g_mutex_lock(db->lock);

		/* do we need to register wakeup? */

		/* read again */
		state = g_atomic_int_get(&pentry->state);
		if (LI_ENTRY_LOOKUP != state) {
			/* catch especially LI_ENTRY_INVALID which doesn't have a wait_queue anymore.
			 * any entry is fine here, we just asked for the lookup! - don't care how "valid" it is */
			result = pentry;
			goto out;
		}

		wakeup_add_job(&pentry->wait_queue, wakeup, wakeup_data);

		*wait = (liFetchWait*) pentry;
	}

out:
	g_mutex_unlock(db->lock);

	return (NULL != result) ? &result->public : NULL;
}

void li_fetch_cancel(liFetchWait **wait) {
	liFetchEntryP* pentry = (liFetchEntryP*) *wait;
	if (NULL != pentry) li_fetch_entry_release(&pentry->public);
}
