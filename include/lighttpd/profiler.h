#ifndef _LIGHTTPD_PROFILER_H_
#define _LIGHTTPD_PROFILER_H_

extern gboolean li_profiler_enabled; /* read only */

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
LI_API void li_profiler_dump(gint minsize); /* dumps memory statistics to file specified in LI_PROFILE_MEM env var */
LI_API void li_profiler_hashtable_insert(const gpointer addr, gsize size); /* registers an allocated block with the profiler */
LI_API void li_profiler_hashtable_remove(const gpointer addr); /* unregisters an allocated block with the profiler */

#endif
