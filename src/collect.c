
#include <lighttpd/base.h>

struct collect_job;
typedef struct collect_job collect_job;
struct collect_job {
	enum { COLLECT_FUNC, COLLECT_CB } type;
	collect_info *ci;
};

static collect_info* collect_info_new(worker *ctx, CollectFunc func, gpointer fdata, CollectFree free_fdata, CollectCallback cb, gpointer cbdata) {
	collect_info *ci = g_slice_new(collect_info);
	ci->wrk = ctx;
	ci->counter = ctx->srv->worker_count;
	ci->stopped = FALSE;
	ci->func = func;
	ci->fdata = fdata;
	ci->free_fdata = free_fdata;
	ci->cb = cb;
	ci->cbdata = cbdata;
	ci->results = g_ptr_array_sized_new(ctx->srv->worker_count);
	g_ptr_array_set_size(ci->results, ctx->srv->worker_count);
	return ci;
}

static void collect_info_free(collect_info *ci) {
	g_ptr_array_free(ci->results, TRUE);
	g_slice_free(collect_info, ci);
}

static void collect_insert_callback(worker *ctx, collect_info *ci) {
	if (ctx == ci->wrk) {
		/* we are in the destiation context */
		ci->cb(ci->cbdata, ci->fdata, ci->results, !ci->stopped);
		collect_info_free(ci);
	} else {
		worker *wrk = ci->wrk;
		collect_job *j = g_slice_new(collect_job);
		j->type = COLLECT_CB;
		j->ci = ci;
		g_async_queue_push(wrk->collect_queue, j);
		ev_async_send(wrk->loop, &wrk->collect_watcher);
	}
}

static void collect_send_result(worker *ctx, collect_info *ci) {
	if (!g_atomic_int_dec_and_test(&ci->counter)) return; /* not all workers done yet */
	if (g_atomic_int_get(&ctx->srv->exiting)) {
		/* cleanup state, just call the callback with complete = FALSE */
		ci->cb(ci->cbdata, ci->fdata, ci->results, FALSE);
		collect_info_free(ci);
	} else {
		/* no worker is freed yet */
		collect_insert_callback(ctx, ci);
	}
}

static void collect_insert_func(worker *ctx, collect_info *ci) {
	guint i;
	server *srv = ctx->srv;
	for (i = 0; i < srv->worker_count; i++) {
		worker *wrk;
		wrk = g_array_index(srv->workers, worker*, i);
		if (wrk == ctx) {
			/* we are in the destiation context */
			g_ptr_array_index(ci->results, wrk->ndx) = ci->func(wrk, ci->fdata);
			collect_send_result(wrk, ci);
		} else {
			collect_job *j = g_slice_new(collect_job);
			j->type = COLLECT_FUNC;
			j->ci = ci;
			g_async_queue_push(wrk->collect_queue, j);
			ev_async_send(wrk->loop, &wrk->collect_watcher);
		}
	}
}

collect_info* collect_start(worker *ctx, CollectFunc func, gpointer fdata, CollectFree free_fdata, CollectCallback cb, gpointer cbdata) {
	collect_info *ci = collect_info_new(ctx, func, fdata, free_fdata, cb, cbdata);
	collect_insert_func(ctx, ci);
	return ci;
}

void collect_break(collect_info* ci) {
	ci->stopped = TRUE;
}

void collect_watcher_cb(struct ev_loop *loop, ev_async *w, int revents) {
	worker *wrk = (worker*) w->data;
	collect_job *j;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (j = (collect_job*) g_async_queue_try_pop(wrk->collect_queue))) {
		collect_info *ci = j->ci;
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

