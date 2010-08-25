#ifndef _LIGHTTPD_TASKLET_H_
#define _LIGHTTPD_TASKLET_H_

#include <lighttpd/settings.h>

typedef struct liTaskletPool liTaskletPool;

typedef void (*liTaskletFinishedCB)(gpointer data);
typedef void (*liTaskletRunCB)(gpointer data);

/* if threads = 0: all run callbacks are executed immediately in li_tasklet_push (but finished_cb is delayed)
 * if threads < 0: a shared GThreadPool is used
 * if threads > 0: a exclusive GThreadPool is used with the specified numbers of threads
 */

/* we do not keep the loop alive! */
LI_API liTaskletPool* li_tasklet_pool_new(struct ev_loop *loop, gint threads);

/* blocks until all tasks are done; calls all finished callbacks;
 * you are allowed to call this from finish callbacks, but not more than once!
 */
LI_API void li_tasklet_pool_free(liTaskletPool *pool);

/* this may stop the old pool and wait for all jobs to be finished; doesn't call finished callbacks */
LI_API void li_tasklet_pool_set_threads(liTaskletPool *pool, gint threads);

LI_API gint li_tasklet_pool_get_threads(liTaskletPool *pool);

/* the finished callback is executed in the same thread context as the pool lives in;
 *   it will either be called from li_tasklet_pool_free or the ev-loop handler,
 *   never from li_tasklet_push
 * all tasklets will be executed, you can *not* cancel them!
 */
LI_API void li_tasklet_push(liTaskletPool *pool, liTaskletRunCB run, liTaskletFinishedCB finished, gpointer data);

#endif
