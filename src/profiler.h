#ifndef _LIGHTTPD_PROFILER_H_
#define _LIGHTTPD_PROFILER_H_

struct profiler_mem;
typedef struct profiler_mem profiler_mem;

struct profiler_mem {
	guint64 inuse_bytes;
	guint64 alloc_times;
	guint64 alloc_bytes;
	guint64 calloc_times;
	guint64 calloc_bytes;
	guint64 realloc_times;
	guint64 realloc_bytes;
	guint64 free_times;
	guint64 free_bytes;
};

void profiler_enable(); /* enables the profiler */
void profiler_finish();
void profiler_dump(); /* dumps memory statistics to stdout */
void profiler_dump_table();
#endif
