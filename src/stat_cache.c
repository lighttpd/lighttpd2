#include <lighttpd/base.h>


void stat_cache_new(worker *wrk, gdouble ttl) {
	stat_cache *sc;
	GError *err;

	sc = g_slice_new0(stat_cache);
	sc->ttl = ttl;
	sc->entries = g_hash_table_new_full((GHashFunc)g_string_hash, (GEqualFunc)g_string_equal, NULL, NULL);
	sc->job_queue_in = g_async_queue_new();
	sc->job_queue_out = g_async_queue_new();

	waitqueue_init(&sc->delete_queue, wrk->loop, stat_cache_delete_cb, ttl, sc);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	ev_init(&sc->job_watcher, stat_cache_job_cb);
	sc->job_watcher.data = wrk;
	ev_async_start(wrk->loop, &sc->job_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */
	wrk->stat_cache = sc;

	sc->thread = g_thread_create(stat_cache_thread, sc, TRUE, &err);

	if (!sc->thread) {
		/* failed to create thread */
		assert(0);
	}
}

void stat_cache_free(stat_cache *sc) {
	GHashTableIter iter;
	gpointer k, v;

	/* wake up thread */
	g_async_queue_push(sc->job_queue_out, g_slice_new0(stat_cache_entry));
	g_thread_join(sc->thread);

	ev_async_stop(sc->delete_queue.loop, &sc->job_watcher);

	/* clear cache */
	g_hash_table_iter_init(&iter, sc->entries);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		stat_cache_entry_free(v);
	}

	waitqueue_stop(&sc->delete_queue);
	g_async_queue_unref(sc->job_queue_in);
	g_async_queue_unref(sc->job_queue_out);
	g_hash_table_destroy(sc->entries);
	g_slice_free(stat_cache, sc);
}

void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	stat_cache *sc = (stat_cache*) w->data;
	stat_cache_entry *sce;
	waitqueue_elem *wqe;

	UNUSED(loop);
	UNUSED(revents);

	while ((wqe = waitqueue_pop(&sc->delete_queue)) != NULL) {
		/* stat cache entry TTL over */
		sce = wqe->data;
		if (sce->refcount) {
			/* if there are still vrequests using this entry just requeue it */
			waitqueue_push(&sc->delete_queue, wqe);
		} else {
			/* no more vrequests using this entry, finally free it */
			if (sce->in_cache)
				g_hash_table_remove(sc->entries, sce->path);
			stat_cache_entry_free(sce);
		}
	}

	waitqueue_update(&sc->delete_queue);
}

void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents) {
	guint i;
	stat_cache_entry *sce;
	stat_cache *sc = ((worker*)w->data)->stat_cache;
	vrequest *vr;

	UNUSED(loop);
	UNUSED(revents);

	while ((sce = g_async_queue_try_pop(sc->job_queue_in)) != NULL) {
		if (sce->failed)
			sc->errors++;

		for (i = 0; i < sce->vrequests->len; i++) {
			vr = g_ptr_array_index(sce->vrequests, i);
			vrequest_joblist_append(vr);
		}

		g_ptr_array_set_size(sce->vrequests, 0);
	}
}

void stat_cache_entry_free(stat_cache_entry *sce) {
	assert(sce->vrequests->len == 0);
	assert(sce->refcount == 0);
	g_string_free(sce->path, TRUE);
	g_ptr_array_free(sce->vrequests, TRUE);
	g_slice_free(stat_cache_entry, sce);
}


stat_cache_entry *stat_cache_entry_get(vrequest *vr, GString *path) {
	stat_cache *sc;
	stat_cache_entry *sce;

	sc = vr->con->wrk->stat_cache;

	/* lookup entry in cache */
	sce = g_hash_table_lookup(sc->entries, path);

	if (sce) {
		/* cache hit, check state */
		if (g_atomic_int_get(&sce->state) == STAT_CACHE_ENTRY_FINISHED) {
			/* stat info available, check if it is fresh */
			if (sce->ts >= (CUR_TS(vr->con->wrk) - (ev_tstamp)sc->ttl)) {
				/* entry fresh */
				if (!vr->stat_cache_entry) {
					sc->hits++;
					vr->stat_cache_entry = sce;
					sce->refcount++;
				}
				return sce;
			} else {
				/* entry old */
				if (sce->refcount == 0) {
					/* no vrequests working on the entry, reuse it */
				} else {
					/* there are still vrequests using this entry, replace with a new one */
					sce->in_cache = FALSE;
					sce = g_slice_new0(stat_cache_entry);
					sce->path = g_string_new_len(GSTR_LEN(path));
					sce->vrequests = g_ptr_array_sized_new(8);
					sce->in_cache = TRUE;
					sce->queue_elem.data = sce;
					g_hash_table_replace(sc->entries, sce->path, sce);
				}

				sce->ts = CUR_TS(vr->con->wrk);
				vr->stat_cache_entry = sce;
				g_ptr_array_add(sce->vrequests, vr);
				sce->refcount++;
				waitqueue_push(&sc->delete_queue, &sce->queue_elem);
				sce->state = STAT_CACHE_ENTRY_WAITING;
				g_async_queue_push(sc->job_queue_out, sce);
				sc->misses++;
				return NULL;
			}
		} else {
			/* stat info not available (state is STAT_CACHE_ENTRY_WAITING) */
			vr->stat_cache_entry = sce;
			g_ptr_array_add(sce->vrequests, vr);
			sce->refcount++;
			sc->misses++;
			return NULL;
		}
	} else {
		/* cache miss, allocate new entry */
		sce = g_slice_new0(stat_cache_entry);
		sce->path = g_string_new_len(GSTR_LEN(path));
		sce->vrequests = g_ptr_array_sized_new(8);
		sce->ts = CUR_TS(vr->con->wrk);
		sce->state = STAT_CACHE_ENTRY_WAITING;
		sce->in_cache = TRUE;
		sce->queue_elem.data = sce;
		vr->stat_cache_entry = sce;
		g_ptr_array_add(sce->vrequests, vr);
		sce->refcount = 1;
		waitqueue_push(&sc->delete_queue, &sce->queue_elem);
		g_hash_table_insert(sc->entries, sce->path, sce);
		g_async_queue_push(sc->job_queue_out, sce);
		sc->misses++;
		return NULL;
	}
}

void stat_cache_entry_release(vrequest *vr) {
	vr->stat_cache_entry->refcount--;
	vr->stat_cache_entry = NULL;
}


gpointer stat_cache_thread(gpointer data) {
	stat_cache *sc = data;
	stat_cache_entry *sce;

	while (TRUE) {
		sce = g_async_queue_pop(sc->job_queue_out);

		/* stat cache entry with path == NULL indicates server stop */
		if (!sce->path)
			break;

		if (stat(sce->path->str, &sce->st) == -1) {
			sce->failed = TRUE;
			sce->err = errno;
		} else
			sce->failed = FALSE;

		g_atomic_int_set(&sce->state, STAT_CACHE_ENTRY_FINISHED);
		g_async_queue_push(sc->job_queue_in, sce);
		ev_async_send(sc->delete_queue.loop, &sc->job_watcher);
	}

	return NULL;
}