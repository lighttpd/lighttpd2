#include <lighttpd/base.h>


static void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents);
static void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents);
static gpointer stat_cache_thread(gpointer data);
static void stat_cache_entry_free(stat_cache_entry *sce);
static stat_cache_entry *stat_cache_get_internal(vrequest *vr, GString *path, gboolean dir);

void stat_cache_new(worker *wrk, gdouble ttl) {
	stat_cache *sc;
	GError *err;

	/* ttl default 10s */
	if (ttl < 1)
		ttl = 10.0;

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
	stat_cache_entry *dummy;

	/* wake up thread */
	dummy = g_slice_new0(stat_cache_entry);
	g_async_queue_push(sc->job_queue_out, dummy);
	g_thread_join(sc->thread);
	g_slice_free(stat_cache_entry, dummy);

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

static void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents) {
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
				g_hash_table_remove(sc->entries, sce->data.path);
			stat_cache_entry_free(sce);
		}
	}

	waitqueue_update(&sc->delete_queue);
}

static void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents) {
	guint i;
	stat_cache_entry *sce;
	stat_cache *sc = ((worker*)w->data)->stat_cache;
	vrequest *vr;

	UNUSED(loop);
	UNUSED(revents);

	while ((sce = g_async_queue_try_pop(sc->job_queue_in)) != NULL) {
		if (sce->data.failed)
			sc->errors++;

		for (i = 0; i < sce->vrequests->len; i++) {
			vr = g_ptr_array_index(sce->vrequests, i);
			vrequest_joblist_append(vr);
		}

		g_ptr_array_set_size(sce->vrequests, 0);
	}
}

static void stat_cache_entry_free(stat_cache_entry *sce) {
	guint i;

	assert(sce->vrequests->len == 0);
	assert(sce->refcount == 0);

	g_string_free(sce->data.path, TRUE);
	g_ptr_array_free(sce->vrequests, TRUE);

	if (sce->type == STAT_CACHE_ENTRY_DIR) {
		for (i = 0; i < sce->dirlist->len; i++) {
			g_string_free(g_array_index(sce->dirlist, stat_cache_entry_data, i).path, TRUE);
		}

		g_array_free(sce->dirlist, TRUE);
	}

	g_slice_free(stat_cache_entry, sce);
}


static gpointer stat_cache_thread(gpointer data) {
	stat_cache *sc = data;
	stat_cache_entry *sce;

	while (TRUE) {
		sce = g_async_queue_pop(sc->job_queue_out);

		/* stat cache entry with path == NULL indicates server stop */
		if (!sce->data.path)
			break;

		if (stat(sce->data.path->str, &sce->data.st) == -1) {
			sce->data.failed = TRUE;
			sce->data.err = errno;
		} else {
			sce->data.failed = FALSE;
		}
		if (!sce->data.failed && sce->type == STAT_CACHE_ENTRY_DIR) {
			/* dirlisting */
			DIR *dirp;
			gsize size;
			struct dirent *entry;
			struct dirent *result;
			gint error;
			stat_cache_entry_data sced;
			GString *str;

			dirp = opendir(sce->data.path->str);
			if (dirp == NULL) {
				sce->data.failed = TRUE;
				sce->data.err = errno;
			} else {
				size = dirent_buf_size(dirp);
				assert(size != (gsize)-1);
				entry = g_slice_alloc(size);

				str = g_string_sized_new(sce->data.path->len + 32);
				g_string_append_len(str, GSTR_LEN(sce->data.path));

				while ((error = readdir_r(dirp, entry, &result)) == 0 && result != NULL) {
					/* hide "." and ".." */
					if (result->d_name[0] == '.' && (result->d_name[1] == '\0' ||
						(result->d_name[1] == '.' && result->d_name[2] == '\0'))) {
						continue;
					}

					sced.path = g_string_sized_new(32);
					g_string_assign(sced.path, result->d_name);

					g_string_truncate(str, sce->data.path->len);
					/* make sure the path ends with / (or whatever) */
					if (sce->data.path->str[sce->data.path->len-1] != G_DIR_SEPARATOR)
						g_string_append_c(str, G_DIR_SEPARATOR);
					g_string_append_len(str, GSTR_LEN(sced.path));

					if (stat(str->str, &sced.st) == -1) {
						sced.failed = TRUE;
						sced.err = errno;
					} else {
						sced.failed = FALSE;
					}

					g_array_append_val(sce->dirlist, sced);
				}

				if (error) {
					sce->data.failed = TRUE;
					sce->data.err = error;
				}

				g_string_free(str, TRUE);
				g_slice_free1(size, entry);
				closedir(dirp);
			}
		}

		g_atomic_int_set(&sce->state, STAT_CACHE_ENTRY_FINISHED);
		g_async_queue_push(sc->job_queue_in, sce);
		ev_async_send(sc->delete_queue.loop, &sc->job_watcher);
	}

	return NULL;
}

static stat_cache_entry *stat_cache_get_internal(vrequest *vr, GString *path, gboolean dir) {
	stat_cache *sc;
	stat_cache_entry *sce;

	sc = vr->con->wrk->stat_cache;

	/* lookup entry in cache */
	sce = g_hash_table_lookup(sc->entries, path);

	if (sce) {
		/* cache hit, check state */
		if (g_atomic_int_get(&sce->state) == STAT_CACHE_ENTRY_FINISHED) {
			/* stat info available, check if it is fresh */
			if (!((sce->type == STAT_CACHE_ENTRY_DIR) ^ dir) && sce->ts >= (CUR_TS(vr->con->wrk) - (ev_tstamp)sc->ttl)) {
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
					if (sce->type == STAT_CACHE_ENTRY_DIR) {
						guint i;
	
						/* free old entries */
						for (i = 0; i < sce->dirlist->len; i++) {
							g_string_free(g_array_index(sce->dirlist, stat_cache_entry_data, i).path, TRUE);
						}

						if (!dir) {
							g_array_free(sce->dirlist, TRUE);
							sce->type = STAT_CACHE_ENTRY_SINGLE;
						} else {
							g_array_set_size(sce->dirlist, 0);
						}
					} else {
						/* single file */
						if (dir) {
							sce->dirlist = g_array_sized_new(FALSE, FALSE, sizeof(stat_cache_entry_data), 32);
							sce->type = STAT_CACHE_ENTRY_DIR;
						}
					}
				} else {
					/* there are still vrequests using this entry, replace with a new one */
					sce->in_cache = FALSE;
					sce = g_slice_new0(stat_cache_entry);
					sce->data.path = g_string_new_len(GSTR_LEN(path));
					sce->vrequests = g_ptr_array_sized_new(8);
					sce->in_cache = TRUE;
					sce->queue_elem.data = sce;
					g_hash_table_replace(sc->entries, sce->data.path, sce);
					if (dir) {
						sce->type = STAT_CACHE_ENTRY_DIR;
						sce->dirlist = g_array_sized_new(FALSE, FALSE, sizeof(stat_cache_entry_data), 32);
					} else {
						sce->type = STAT_CACHE_ENTRY_SINGLE;
					}
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
		sce->data.path = g_string_new_len(GSTR_LEN(path));
		sce->vrequests = g_ptr_array_sized_new(8);
		sce->ts = CUR_TS(vr->con->wrk);
		sce->state = STAT_CACHE_ENTRY_WAITING;
		sce->in_cache = TRUE;
		sce->queue_elem.data = sce;
		vr->stat_cache_entry = sce;
		g_ptr_array_add(sce->vrequests, vr);
		sce->refcount = 1;
		sc->misses++;

		if (dir) {
			sce->type = STAT_CACHE_ENTRY_DIR;
			sce->dirlist = g_array_sized_new(FALSE, FALSE, sizeof(stat_cache_entry_data), 32);
		} else {
			sce->type = STAT_CACHE_ENTRY_SINGLE;
		}

		waitqueue_push(&sc->delete_queue, &sce->queue_elem);
		g_hash_table_insert(sc->entries, sce->data.path, sce);
		g_async_queue_push(sc->job_queue_out, sce);

		return NULL;
	}
}

stat_cache_entry *stat_cache_get(vrequest *vr, GString *path) {
	return stat_cache_get_internal(vr, path, FALSE);
}

stat_cache_entry *stat_cache_get_dir(vrequest *vr, GString *path) {
	return stat_cache_get_internal(vr, path, TRUE);
}

void stat_cache_entry_release(vrequest *vr) {
	vr->stat_cache_entry->refcount--;
	vr->stat_cache_entry = NULL;
}
