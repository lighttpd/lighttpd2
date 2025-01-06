#ifndef _LIGHTTPD_JOBQUEUE_H_
#define _LIGHTTPD_JOBQUEUE_H_

#ifndef _LIGHTTPD_EVENTS_H_
#error Include lighttpd/events.h instead
#endif

typedef struct liJob liJob;
typedef struct liJobRef liJobRef;
typedef struct liJobQueue liJobQueue;

typedef void (*liJobCB)(liJob *job);

/* All data here is private; use the functions to interact with the job-queue */

struct liJob {
	/* prevent running callback in a loop (delay if job generation == queue generation) */
	guint generation;
	GList link;
	liJobCB callback;
	liJobRef *ref;
};

struct liJobRef {
	gint refcount;
	liJob *job;
	liJobQueue *queue;
};

struct liJobQueue {
	guint generation;

	liEventPrepare prepare_watcher;

	GQueue queue;
	liEventTimer queue_watcher;

	GAsyncQueue *async_queue;
	liEventAsync async_queue_watcher;
};

LI_API void li_job_queue_init(liJobQueue *jq, liEventLoop *loop);
LI_API void li_job_queue_clear(liJobQueue *jq); /* runs until all jobs are done */

LI_API void li_job_init(liJob *job, liJobCB callback);
LI_API void li_job_reset(liJob *job);
/* remove job from queue if active and detach existing references, but doesn't reset loop detection */
LI_API void li_job_stop(liJob *job);
LI_API void li_job_clear(liJob *job);

/* marks the job for later execution */
LI_API void li_job_later(liJobQueue *jq, liJob *job);
LI_API void li_job_later_ref(liJobRef *jobref); /* NOT thread-safe! */
/* if the job didn't run in this generation yet, run it now; otherwise mark it for later execution */
LI_API void li_job_now(liJobQueue *jq, liJob *job);
LI_API void li_job_now_ref(liJobRef *jobref); /* NOT thread-safe! */

LI_API void li_job_async(liJobRef *jobref);
/* marks the job for later execution; this is the only threadsafe way to push a job to the queue */

LI_API liJobRef* li_job_ref(liJobQueue *jq, liJob *job);
LI_API void li_job_ref_release(liJobRef *jobref);
LI_API void li_job_ref_acquire(liJobRef *jobref);

#endif
