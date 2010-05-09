#ifndef _LIGHTTPD_PROFILER_H_
#define _LIGHTTPD_PROFILER_H_

typedef struct liProfilerMem liProfilerMem;

struct liProfilerMem {
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

LI_API void li_profiler_enable(gchar *output_path); /* enables the profiler */
LI_API void li_profiler_finish();
LI_API void li_profiler_dump(); /* dumps memory statistics to stdout */
LI_API void li_profiler_dump_table();

#endif
