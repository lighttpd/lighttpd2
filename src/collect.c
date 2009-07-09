
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
		/* we are in the destiation context */
		ci->cb(ci->cbdata, ci->fdata, ci->results, !ci->stopped);
		collect_info_free(ci);
		return TRUE;
	} else {
		liWorker *wrk = ci->wrk;
		collect_job *j = g_slice_new(collect_job);
		j->type = COLLECT_CB;
		j->ci = ci;
		g_async_queue_push(wrk->collect_queue, j);
		ev_async_send(wrk->loop, &wrk->collect_watcher);
	}
	return FALSE;
}

/* returns true if callback was called directly */
static gboolean collect_send_result(liWorker *ctx, liCollectInfo *ci) {
	if (!g_atomic_int_dec_and_test(&ci->counter)) return FALSE; /* not all workers done yet */
	if (g_atomic_int_get(&ctx->srv->exiting)) {
		/* cleanup state, just call the callback with complete = FALSE */
		ci->cb(ci->cbdata, ci->fdata, ci->results, FALSE);
		collect_info_free(ci);
		return TRUE;
	} else {
		/* no worker is freed yet */
		return collect_insert_callback(ctx, ci);
	}
}

/* returns true if callback was called directly */
static gboolean collect_insert_func(liWorker *ctx, liCollectInfo *ci) {
	guint i;
	liServer *srv = ctx->srv;
	for (i = 0; i < srv->worker_count; i++) {
		liWorker *wrk;
		wrk = g_array_index(srv->workers, liWorker*, i);
		if (wrk == ctx) {
			/* we are in the destiation context */
			g_ptr_array_index(ci->results, wrk->ndx) = ci->func(wrk, ci->fdata);
			if (collect_send_result(wrk, ci)) return TRUE;
		} else {
			collect_job *j = g_slice_new(collect_job);
			j->type = COLLECT_FUNC;
			j->ci = ci;
			g_async_queue_push(wrk->collect_queue, j);
			ev_async_send(wrk->loop, &wrk->collect_watcher);
		}
	}
	return FALSE;
}

liCollectInfo* li_collect_start(liWorker *ctx, liCollectFuncCB func, gpointer fdata, liCollectCB cb, gpointer cbdata) {
	liCollectInfo *ci = collect_info_new(ctx, func, fdata, cb, cbdata);
	if (collect_insert_func(ctx, ci)) return NULL; /* collect info is invalid now */
	return ci;
}

void li_collect_break(liCollectInfo* ci) {
	ci->stopped = TRUE;
}

void li_collect_watcher_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	collect_job *j;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (j = (collect_job*) g_async_queue_try_pop(wrk->collect_queue))) {
		liCollectInfo *ci = j->ci;
		switch (j->type) {
		case COLLECT_FUNC:
			g_ptr_array_index(ci->results, wrk->ndx) = ci->func(wrk, ci->fdata);
			collect_send_result(wrk, ci);
			break;
		case COLLECT_CB:
			ci->cb(ci->cbdata, ci->fdata, ci->results, !ci->stopped);
			collect_info_free(ci);
			break;
		}
		g_slice_free(collect_job, j);
	}
}

