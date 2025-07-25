
#include <lighttpd/base.h>

/* internal structure */
struct liCollectInfo {
	liWorker *wrk;
	gint counter;
	gboolean stopped;

	liCollectFuncCB func;
	gpointer fdata;

	liCollectCB cb;
	gpointer cbdata;

	GPtrArray *results;
};

typedef struct collect_job collect_job;
struct collect_job {
	enum { COLLECT_FUNC, COLLECT_CB } type;
	liCollectInfo *ci;
};

static liCollectInfo* collect_info_new(liWorker *ctx, liCollectFuncCB func, gpointer fdata, liCollectCB cb, gpointer cbdata) {
	liCollectInfo *ci = g_slice_new(liCollectInfo);
	ci->wrk = ctx;
	ci->counter = ctx->srv->worker_count;
	ci->stopped = FALSE;
	ci->func = func;
	ci->fdata = fdata;
	ci->cb = cb;
	ci->cbdata = cbdata;
	ci->results = g_ptr_array_sized_new(ctx->srv->worker_count);
	g_ptr_array_set_size(ci->results, ctx->srv->worker_count);
	return ci;
}

static void collect_info_free(liCollectInfo *ci) {
	g_ptr_array_free(ci->results, TRUE);
	g_slice_free(liCollectInfo, ci);
}

/* returns true if callback was called directly */
static gboolean collect_insert_callback(liWorker *ctx, liCollectInfo *ci) {
	if (ctx == ci->wrk) {
		/* we are in the destination context */
		ci->cb(ctx, ci->cbdata, ci->fdata, ci->results, !ci->stopped);
		collect_info_free(ci);
		return TRUE;
	} else {
		liWorker *wrk = ci->wrk;
		collect_job *j = g_slice_new(collect_job);
		j->type = COLLECT_CB;
		j->ci = ci;
		g_async_queue_push(wrk->collect_queue, j);
		li_event_async_send(&wrk->collect_watcher);
	}
	return FALSE;
}

/* returns true if callback was called directly */
static gboolean collect_send_result(liWorker *ctx, liCollectInfo *ci) {
	if (!g_atomic_int_dec_and_test(&ci->counter)) return FALSE; /* not all workers done yet */
	if (g_atomic_int_get(&ctx->srv->exiting)) {
		/* cleanup state, just call the callback with complete = FALSE */
		ci->cb(ctx, ci->cbdata, ci->fdata, ci->results, FALSE);
		collect_info_free(ci);
		return TRUE;
	} else {
		/* no worker is freed yet */
		return collect_insert_callback(ctx, ci);
	}
}

/* returns true if callback was called directly */
static gboolean collect_insert_func(liServer *srv, liWorker *ctx, liCollectInfo *ci) {
	guint i;
	for (i = 0; i < srv->worker_count; i++) {
		liWorker *wrk;
		wrk = g_array_index(srv->workers, liWorker*, i);
		if (wrk == ctx) {
			/* we are in the destination context */
			g_ptr_array_index(ci->results, wrk->ndx) = ci->func(wrk, ci->fdata);
			if (collect_send_result(wrk, ci)) return TRUE;
		} else {
			collect_job *j = g_slice_new(collect_job);
			j->type = COLLECT_FUNC;
			j->ci = ci;
			g_async_queue_push(wrk->collect_queue, j);
			li_event_async_send(&wrk->collect_watcher);
		}
	}
	return FALSE;
}

liCollectInfo* li_collect_start(liWorker *ctx, liCollectFuncCB func, gpointer fdata, liCollectCB cb, gpointer cbdata) {
	liCollectInfo *ci = collect_info_new(ctx, func, fdata, cb, cbdata);
	if (collect_insert_func(ctx->srv, ctx, ci)) return NULL; /* collect info is invalid now */
	return ci;
}

liCollectInfo* li_collect_start_global(liServer *srv, liCollectFuncCB func, gpointer fdata, liCollectCB cb, gpointer cbdata) {
	liCollectInfo *ci = collect_info_new(srv->main_worker, func, fdata, cb, cbdata);
	if (collect_insert_func(srv, NULL, ci)) return NULL; /* collect info is invalid now */
	return ci;
}


void li_collect_break(liCollectInfo* ci) {
	ci->stopped = TRUE;
}

void li_collect_watcher_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, collect_watcher);
	collect_job *j;
	UNUSED(events);

	while (NULL != (j = (collect_job*) g_async_queue_try_pop(wrk->collect_queue))) {
		liCollectInfo *ci = j->ci;
		switch (j->type) {
		case COLLECT_FUNC:
			g_ptr_array_index(ci->results, wrk->ndx) = ci->func(wrk, ci->fdata);
			collect_send_result(wrk, ci);
			break;
		case COLLECT_CB:
			ci->cb(wrk, ci->cbdata, ci->fdata, ci->results, !ci->stopped);
			collect_info_free(ci);
			break;
		}
		g_slice_free(collect_job, j);
	}
}

