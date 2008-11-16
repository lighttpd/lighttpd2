#ifndef _LIGHTTPD_COLLECT_H_
#define _LIGHTTPD_COLLECT_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* executes a function in each worker context */

/** CollectFunc: the type of functions to execute in each workers context
  *   - wrk: the current worker
  *   - fdata: optional user data
  *     this data either must be persistent data, i.e. not connection related,
  *     or you have to specify a CollectFree func for it
  * the return value will be placed in the GArray
  */
typedef gpointer (*CollectFunc)(worker *wrk, gpointer fdata);

/** CollectCallback: the type of functions to call after a function was called in each workers context
  *   - cbdata: optional callback data
  *     depending on the data you should only use it when complete == TRUE
  *   - fdata : the data the CollectFunc got
  *   - result: the return values
  *   - complete: determines if the function was called in every context or was cancelled
  *     if this is FALSE, it may be called from another context than collect_start was called
  */
typedef void (*CollectCallback)(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete);

typedef void (*CollectFree)(gpointer data);

struct collect_info;
typedef struct collect_info collect_info;

/* internal structure */
struct collect_info {
	worker *wrk;
	gint counter;
	gboolean stopped;

	CollectFunc func;
	gpointer fdata;
	CollectFree free_fdata;

	CollectCallback cb;
	gpointer cbdata;

	GPtrArray *results;
};

LI_API collect_info* collect_start(worker *ctx, CollectFunc func, gpointer fdata, CollectFree free_fdata, CollectCallback cb, gpointer cbdata);
LI_API void collect_break(collect_info* ci); /** this will result in complete == FALSE in the callback */

/* internal functions */
LI_API void collect_watcher_cb(struct ev_loop *loop, ev_async *w, int revents);

#endif
