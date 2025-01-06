
#include <lighttpd/events.h>
#include <lighttpd/utils.h>

#define INC_GEN(jq) do { jq->generation++; if (0 == jq->generation) jq->generation = 1; } while (0)

static void job_queue_run(liJobQueue* jq, int loops) {
	int i;

	for (i = 0; i < loops; i++) {
		GQueue *q = &jq->queue;
		GList *l;
		liJob *job;
		guint todo = q->length;

		INC_GEN(jq);

		if (0 == todo) return;

		while ((todo-- > 0) && (NULL != (l = g_queue_pop_head_link(q)))) {
			job = LI_CONTAINER_OF(l, liJob, link);
			job->generation = jq->generation;
			job->link.data = NULL;

			job->callback(job);
		}
	}

	if (jq->queue.length > 0) {
		/* make sure we will run again soon */
		li_event_timer_once(&jq->queue_watcher, 0);
	}
}

static void job_queue_prepare_cb(liEventBase *watcher, int events) {
	liJobQueue* jq = LI_CONTAINER_OF(li_event_prepare_from(watcher), liJobQueue, prepare_watcher);
	UNUSED(events);

	job_queue_run(jq, 3);
}

static void job_queue_watcher_cb(liEventBase *watcher, int events) {
	UNUSED(watcher);
	UNUSED(events);

	/* just keep loop alive, run jobs in prepare */
}

/* run jobs for async queued jobs */
static void job_async_queue_cb(liEventBase *watcher, int events) {
	liJobQueue* jq = LI_CONTAINER_OF(li_event_async_from(watcher), liJobQueue, async_queue_watcher);
	GAsyncQueue *q = jq->async_queue;
	liJobRef *jobref;
	UNUSED(events);

	while (NULL != (jobref = g_async_queue_try_pop(q))) {
		li_job_now_ref(jobref);
		li_job_ref_release(jobref);
	}
}


void li_job_queue_init(liJobQueue* jq, liEventLoop *loop) {
	li_event_prepare_init(loop, "jobqueue", &jq->prepare_watcher, job_queue_prepare_cb);
	li_event_async_init(loop, "jobqueue", &jq->async_queue_watcher, job_async_queue_cb);
	li_event_timer_init(loop, "jobqueue", &jq->queue_watcher, job_queue_watcher_cb);

	/* job queue */
	g_queue_init(&jq->queue);
	jq->async_queue = g_async_queue_new();
}

void li_job_queue_clear(liJobQueue *jq) {
	if (NULL == jq->async_queue) return;

	while (jq->queue.length > 0 || g_async_queue_length(jq->async_queue) > 0) {
		liJobRef *jobref;

		while (NULL != (jobref = g_async_queue_try_pop(jq->async_queue))) {
			li_job_now_ref(jobref);
			li_job_ref_release(jobref);
		}

		job_queue_run(jq, 1);
	}

	g_async_queue_unref(jq->async_queue);
	jq->async_queue = NULL;

	li_event_clear(&jq->async_queue_watcher);
	li_event_clear(&jq->prepare_watcher);
	li_event_clear(&jq->queue_watcher);
}

void li_job_init(liJob *job, liJobCB callback) {
	job->generation = 0;
	job->link.prev = job->link.next = job->link.data = 0;
	job->callback = callback;
	job->ref = 0;
}

void li_job_reset(liJob *job) {
	li_job_stop(job);
	job->generation = 0;
}

void li_job_stop(liJob *job) {
	if (NULL != job->link.data) {
		liJobQueue *jq = job->link.data;

		g_queue_unlink(&jq->queue, &job->link);
		job->link.data = NULL;
	}

	if (NULL != job->ref) {
		/* keep it if refcount == 1, as we are the only reference then */
		if (1 < g_atomic_int_get(&job->ref->refcount)) {
			job->ref->job = NULL;
			li_job_ref_release(job->ref);
			job->ref = NULL;
		}
	}
}

void li_job_clear(liJob *job) {
	if (NULL != job->link.data) {
		liJobQueue *jq = job->link.data;

		g_queue_unlink(&jq->queue, &job->link);
		job->link.data = NULL;
	}

	job->generation = 0;
	if (NULL != job->ref) {
		job->ref->job = NULL;
		li_job_ref_release(job->ref);
		job->ref = NULL;
	}

	job->callback = NULL;
}

void li_job_later(liJobQueue *jq, liJob *job) {
	if (NULL != job->link.data) return; /* already queued */

	job->link.data = jq;
	g_queue_push_tail_link(&jq->queue, &job->link);
}

void li_job_later_ref(liJobRef *jobref) {
	liJob *job = jobref->job;

	if (NULL != job) li_job_later(jobref->queue, job);
}

void li_job_now(liJobQueue *jq, liJob *job) {
	if (job->generation != jq->generation) {
		job->generation = jq->generation;

		/* unqueue if queued */
		if (NULL != job->link.data) {
			LI_FORCE_ASSERT(jq == job->link.data);
			g_queue_unlink(&jq->queue, &job->link);
			job->link.data = NULL;
		}

		job->callback(job);
	} else {
		li_job_later(jq, job);
	}
}

void li_job_now_ref(liJobRef *jobref) {
	liJob *job = jobref->job;

	if (NULL != job) li_job_now(jobref->queue, job);
}

void li_job_async(liJobRef *jobref) {
	liJobQueue *jq = jobref->queue;
	GAsyncQueue *const q = jq->async_queue;
	if (NULL == q) return;
	li_job_ref_acquire(jobref);
	g_async_queue_push(q, jobref);
	li_event_async_send(&jq->async_queue_watcher);
}

liJobRef* li_job_ref(liJobQueue *jq, liJob *job) {
	liJobRef *ref = job->ref;

	if (NULL != ref) {
		li_job_ref_acquire(ref);
		return ref;
	}

	ref = g_slice_new0(liJobRef);
	ref->refcount = 2; /* job->ref + returned ref */
	ref->job = job;
	ref->queue = jq;
	job->ref = ref;

	return ref;
}

void li_job_ref_release(liJobRef *jobref) {
	g_assert(jobref->refcount > 0);
	if (g_atomic_int_dec_and_test(&jobref->refcount)) {
		g_slice_free(liJobRef, jobref);
	}
}

void li_job_ref_acquire(liJobRef *jobref) {
	g_assert(jobref->refcount > 0);
	g_atomic_int_inc(&jobref->refcount);
}
