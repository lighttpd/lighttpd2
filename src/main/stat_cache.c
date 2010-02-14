#include <lighttpd/base.h>
#include <sys/stat.h>
#include <fcntl.h>


static void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents);
static void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents);
static gpointer stat_cache_thread(gpointer data);
static void stat_cache_entry_free(liStatCacheEntry *sce);

void li_stat_cache_new(liWorker *wrk, gdouble ttl) {
	liStatCache *sc;
	GError *err;

	/* ttl default 10s */
	if (ttl < 1)
		ttl = 10.0;

	sc = g_slice_new0(liStatCache);
	sc->ttl = ttl;
	sc->entries = g_hash_table_new_full((GHashFunc)g_string_hash, (GEqualFunc)g_string_equal, NULL, NULL);
	sc->dirlists = g_hash_table_new_full((GHashFunc)g_string_hash, (GEqualFunc)g_string_equal, NULL, NULL);
	sc->job_queue_in = g_async_queue_new();
	sc->job_queue_out = g_async_queue_new();

	li_waitqueue_init(&sc->delete_queue, wrk->loop, stat_cache_delete_cb, ttl, sc);

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

void li_stat_cache_free(liStatCache *sc) {
	liStatCacheEntry *sce;
	liWaitQueueElem *wqe;

	/* wake up thread */
	sce = g_slice_new0(liStatCacheEntry);
	g_async_queue_push(sc->job_queue_out, sce);
	g_thread_join(sc->thread);
	g_slice_free(liStatCacheEntry, sce);

	li_waitqueue_stop(&sc->delete_queue);
	while (NULL != (wqe = li_waitqueue_pop_force(&sc->delete_queue))) {
		sce = wqe->data;
		if (sce->cached) {
			if (sce->type == STAT_CACHE_ENTRY_SINGLE)
				g_hash_table_remove(sc->entries, sce->data.path);
			else
				g_hash_table_remove(sc->dirlists, sce->data.path);
			sce->cached = FALSE;
		}
		stat_cache_entry_free(sce);
	}

	li_ev_safe_ref_and_stop(ev_async_stop, sc->delete_queue.loop, &sc->job_watcher);

	g_async_queue_unref(sc->job_queue_in);
	g_async_queue_unref(sc->job_queue_out);
	g_hash_table_destroy(sc->entries);
	g_hash_table_destroy(sc->dirlists);
	g_slice_free(liStatCache, sc);
}

static void stat_cache_delete_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liStatCache *sc = (liStatCache*) w->data;
	liStatCacheEntry *sce;
	liWaitQueueElem *wqe;

	UNUSED(loop);
	UNUSED(revents);

	while ((wqe = li_waitqueue_pop(&sc->delete_queue)) != NULL) {
		/* stat cache entry TTL over */
		sce = wqe->data;

		if (sce->cached) {
			if (sce->type == STAT_CACHE_ENTRY_SINGLE)
				g_hash_table_remove(sc->entries, sce->data.path);
			else
				g_hash_table_remove(sc->dirlists, sce->data.path);

			sce->cached = FALSE;
		}

		if (sce->refcount) {
			/* if there are still vrequests using this entry just requeue it */
			li_waitqueue_push(&sc->delete_queue, wqe);
		} else {
			/* no more vrequests using this entry, finally free it */
			stat_cache_entry_free(sce);
		}
	}

	li_waitqueue_update(&sc->delete_queue);
}

/* called whenever an async stat job has finished */
static void stat_cache_job_cb(struct ev_loop *loop, ev_async *w, int revents) {
	guint i;
	liStatCacheEntry *sce;
	liStatCache *sc = ((liWorker*)w->data)->stat_cache;
	liVRequest *vr;

	UNUSED(loop);
	UNUSED(revents);

	while ((sce = g_async_queue_try_pop(sc->job_queue_in)) != NULL) {
		if (sce->data.failed)
			sc->errors++;

		/* queue pending vrequests */
		for (i = 0; i < sce->vrequests->len; i++) {
			vr = g_ptr_array_index(sce->vrequests, i);
			li_vrequest_joblist_append(vr);
		}

		g_ptr_array_set_size(sce->vrequests, 0);
		sce->refcount--;
	}
}

static void stat_cache_entry_free(liStatCacheEntry *sce) {
	guint i;

	assert(sce->vrequests->len == 0);
	assert(sce->refcount == 0);

	g_string_free(sce->data.path, TRUE);
	g_ptr_array_free(sce->vrequests, TRUE);

	if (NULL != sce->dirlist) {
		for (i = 0; i < sce->dirlist->len; i++) {
			g_string_free(g_array_index(sce->dirlist, liStatCacheEntryData, i).path, TRUE);
		}

		g_array_free(sce->dirlist, TRUE);
	}

	g_slice_free(liStatCacheEntry, sce);
}


static gpointer stat_cache_thread(gpointer data) {
	liStatCache *sc = data;
	liStatCacheEntry *sce;

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
			liStatCacheEntryData sced;
			GString *str;

			dirp = opendir(sce->data.path->str);
			if (dirp == NULL) {
				sce->data.failed = TRUE;
				sce->data.err = errno;
			} else {
				size = li_dirent_buf_size(dirp);
				assert(size != (gsize)-1);
				entry = g_slice_alloc(size);

				sce->dirlist = g_array_sized_new(FALSE, FALSE, sizeof(liStatCacheEntryData), 32);

				str = g_string_sized_new(sce->data.path->len + 64);
				g_string_append_len(str, GSTR_LEN(sce->data.path));

				while ((error = readdir_r(dirp, entry, &result)) == 0 && result != NULL) {
					/* hide "." and ".." */
					if (result->d_name[0] == '.' && (result->d_name[1] == '\0' ||
						(result->d_name[1] == '.' && result->d_name[2] == '\0'))) {
						continue;
					}

					sced.path = g_string_sized_new(63);
					g_string_assign(sced.path, result->d_name);

					g_string_truncate(str, sce->data.path->len);
					/* make sure the path ends with / (or whatever) */
					if (!sce->data.path->len || sce->data.path->str[sce->data.path->len-1] != G_DIR_SEPARATOR)
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

static liStatCacheEntry *stat_cache_entry_new(GString *path) {
	liStatCacheEntry *sce;

	sce = g_slice_new0(liStatCacheEntry);
	sce->data.path = g_string_new_len(GSTR_LEN(path));
	sce->vrequests = g_ptr_array_sized_new(8);
	sce->state = STAT_CACHE_ENTRY_WAITING;
	sce->queue_elem.data = sce;
	sce->refcount = 1;
	sce->cached = TRUE;

	return sce;
}

liHandlerResult li_stat_cache_get_dirlist(liVRequest *vr, GString *path, liStatCacheEntry **result) {
	liStatCache *sc;
	liStatCacheEntry *sce;
	guint i;

	sc = vr->wrk->stat_cache;
	sce = g_hash_table_lookup(sc->dirlists, path);

	if (sce) {
		/* cache hit, check state */
		if (g_atomic_int_get(&sce->state) == STAT_CACHE_ENTRY_WAITING) {
			/* already waiting for it? */
			for (i = 0; i < vr->stat_cache_entries->len; i++) {
				if (g_ptr_array_index(vr->stat_cache_entries, i) == sce)
					return LI_HANDLER_WAIT_FOR_EVENT;
			}
			li_stat_cache_entry_acquire(vr, sce);
			return LI_HANDLER_WAIT_FOR_EVENT;
		}

		sc->hits++;
		*result = sce;
		for (i = 0; i < vr->stat_cache_entries->len; i++) {
			if (g_ptr_array_index(vr->stat_cache_entries, i) == sce)
				return LI_HANDLER_GO_ON;
		}
		li_stat_cache_entry_acquire(vr, sce);
		return LI_HANDLER_GO_ON;
	} else {
		/* cache miss, allocate new entry */
		sce = stat_cache_entry_new(path);
		sce->type = STAT_CACHE_ENTRY_DIR;
		li_stat_cache_entry_acquire(vr, sce);
		li_waitqueue_push(&sc->delete_queue, &sce->queue_elem);
		g_hash_table_insert(sc->dirlists, sce->data.path, sce);
		g_async_queue_push(sc->job_queue_out, sce);
		sc->misses++;
		return LI_HANDLER_WAIT_FOR_EVENT;
	}
}

static liHandlerResult stat_cache_get(liVRequest *vr, GString *path, struct stat *st, int *err, int *fd, gboolean async) {
	liStatCache *sc;
	liStatCacheEntry *sce;
	guint i;

	sc = vr->wrk->stat_cache;
	sce = g_hash_table_lookup(sc->entries, path);

	if (sce) {
		/* cache hit, check state */
		if (g_atomic_int_get(&sce->state) == STAT_CACHE_ENTRY_WAITING) {
			if (async) {
				sce = NULL;
				goto callstat;
			}

			/* already waiting for it? */
			for (i = 0; i < vr->stat_cache_entries->len; i++) {
				if (g_ptr_array_index(vr->stat_cache_entries, i) == sce) {
					return LI_HANDLER_WAIT_FOR_EVENT;
				}
			}
			li_stat_cache_entry_acquire(vr, sce);
			return LI_HANDLER_WAIT_FOR_EVENT;
		}

		sc->hits++;
	} else if (async) {
		/* cache miss, allocate new entry */
		sce = stat_cache_entry_new(path);
		sce->type = STAT_CACHE_ENTRY_SINGLE;
		li_stat_cache_entry_acquire(vr, sce);
		li_waitqueue_push(&sc->delete_queue, &sce->queue_elem);
		g_hash_table_insert(sc->entries, sce->data.path, sce);
		g_async_queue_push(sc->job_queue_out, sce);
		sc->misses++;
		return LI_HANDLER_WAIT_FOR_EVENT;
	}

callstat:
	if (fd) {
		/* open + fstat */
		while (-1 == (*fd = open(path->str, O_RDONLY))) {
			if (errno == EINTR)
				continue;

			*err = errno;
			return LI_HANDLER_ERROR;
		}
		if (-1 == fstat(*fd, st)) {
			*err = errno;
			close(*fd);
			*fd = -1;
			return LI_HANDLER_ERROR;
		}
	} else {
		/* stat */
		if (-1 == stat(path->str, st)) {
			*err = errno;
			return LI_HANDLER_ERROR;
		}
	}

	return LI_HANDLER_GO_ON;
}

liHandlerResult li_stat_cache_get(liVRequest *vr, GString *path, struct stat *st, int *err, int *fd) {
	return stat_cache_get(vr, path, &st, &err, &fd, TRUE);
}

/* doesn't return HANDLER_WAIT_FOR_EVENT, blocks instead of async lookup */
liHandlerResult li_stat_cache_get_sync(liVRequest *vr, GString *path, struct stat *st, int *err, int *fd) {
	return stat_cache_get(vr, path, &st, &err, &fd, FALSE);
}


void li_stat_cache_entry_acquire(liVRequest *vr, liStatCacheEntry *sce) {
	sce->refcount++;
	g_ptr_array_add(vr->stat_cache_entries, sce);
	g_ptr_array_add(sce->vrequests, vr);
}

void li_stat_cache_entry_release(liVRequest *vr, liStatCacheEntry *sce) {
	sce->refcount--;
	g_ptr_array_remove_fast(sce->vrequests, vr);
	g_ptr_array_remove_fast(vr->stat_cache_entries, sce);
}
