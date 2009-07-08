#ifndef _LIGHTTPD_COLLECT_H_
#define _LIGHTTPD_COLLECT_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* executes a function in each worker context */

/** CollectFunc: the type of functions to execute in each workers context
  *   - wrk: the current worker
  *   - fdata: optional user data
  * the return value will be placed in the GArray
  */
typedef gpointer (*liCollectFuncCB)(liWorker *wrk, gpointer fdata);

/** CollectCallback: the type of functions to call after a function was called in each workers context
  *   - cbdata: optional callback data
  *     depending on the data you should only use it when complete == TRUE
  *   - fdata : the data the CollectFunc got (this data must be valid until cb is called)
  *   - result: the return values
  *   - complete: determines if cbdata is still valid
  *     if this is FALSE, it may be called from another context than collect_start was called
  */
typedef void (*liCollectCB)(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete);

typedef struct liCollectInfo liCollectInfo;

/** collect_start returns NULL if the callback was called directly (e.g. for only one worker and ctx = wrk) */
LI_API liCollectInfo* collect_start(liWorker *ctx, liCollectFuncCB func, gpointer fdata, liCollectCB cb, gpointer cbdata);
LI_API void collect_break(liCollectInfo* ci); /** this will result in complete == FALSE in the callback; call it if cbdata gets invalid */

/* internal functions */
LI_API void collect_watcher_cb(struct ev_loop *loop, ev_async *w, int revents);

#endif
