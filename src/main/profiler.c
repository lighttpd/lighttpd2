
/*
 * lighty memory profiler
 * prints a backtrace for every object not free()d at exit()
 *
 */


#include <lighttpd/base.h>
#include <lighttpd/profiler.h>
#include <fcntl.h>

#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#if defined(LIGHTY_OS_MACOSX)
# include <malloc/malloc.h>
#elif defined(LIGHTY_OS_LINUX)
# include <malloc.h>
#endif

#define PROFILER_HASHTABLE_SIZE 65521


typedef struct profiler_block profiler_block;
struct profiler_block {
	gpointer addr;
	gsize size;
	profiler_block *next;
	void *stackframes[12];
	gint stackframes_num;
};


static void profiler_free(gpointer addr);

static GStaticMutex profiler_mutex = G_STATIC_MUTEX_INIT;
static profiler_block *block_free_list = NULL;
static profiler_block **profiler_hashtable = NULL;
static gint profiler_output_fd = 0;


static guint profiler_hash(gpointer addr) {
	return ((guintptr)addr * 2654435761); /* ~ golden ratio of 2^32 */
}

static profiler_block *profiler_block_new() {
	profiler_block *block;

	if (!block_free_list) {
		/* page_free_list exhausted */
		block = malloc(sizeof(profiler_block));
	} else {
		block = block_free_list;
		block_free_list = block_free_list->next;
	}

	block->addr = NULL;
	block->size = 0;
	block->next = NULL;
	block->stackframes_num = 0;

	return block;
}

static void profiler_block_free(profiler_block *block) {
	/* push onto free list */
	block->next = block_free_list;
	block_free_list = block;
}

static void profiler_hashtable_insert(gpointer addr, gsize size) {
	profiler_block *block;
	guint hash;

	hash = profiler_hash(addr);

	block = profiler_block_new();
	block->addr = addr;
	block->size = size;
#ifdef HAVE_EXECINFO_H
	block->stackframes_num = backtrace(block->stackframes, 12);
#endif

	block->next = profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE];
	profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE] = block;
}

static void profiler_hashtable_remove(gpointer addr) {
	profiler_block *block, *block_prev;
	guint hash;

	hash = profiler_hash(addr);

	block = profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE];

	if (block->addr == addr) {
		profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE] = block->next;
		profiler_block_free(block);
		return;
	}

	block_prev = block;
	for (block = block->next; block != NULL; block = block->next) {
		if (block->addr == addr) {
			block_prev->next = block->next;
			profiler_block_free(block);
			return;
		}
		block_prev = block;
	}
}

static gpointer profiler_try_malloc(gsize n_bytes) {
	gsize *p;

	p = malloc(n_bytes);

	if (p) {
		g_static_mutex_lock(&profiler_mutex);
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		profiler_hashtable_insert(p, n_bytes);
		g_static_mutex_unlock(&profiler_mutex);
	}

	return p;
}

static gpointer profiler_malloc(gsize n_bytes) {
	gpointer p = profiler_try_malloc(n_bytes);

	assert(p);

	return p;
}

static gpointer profiler_try_realloc(gpointer mem, gsize n_bytes) {
	gsize *p;

	if (!mem)
		return profiler_try_malloc(n_bytes);

	if (!n_bytes) {
		profiler_free(mem);
		return NULL;
	}

	p = realloc(mem, n_bytes);

	if (p) {
		g_static_mutex_lock(&profiler_mutex);
		profiler_hashtable_remove(mem);
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		profiler_hashtable_insert(p, n_bytes);
		g_static_mutex_unlock(&profiler_mutex);
	}

	return p;
}

static gpointer profiler_realloc(gpointer mem, gsize n_bytes) {
	gpointer p = profiler_try_realloc(mem, n_bytes);

	assert(p);

	return p;
}

static gpointer profiler_calloc(gsize n_blocks, gsize n_bytes) {
	gsize *p;
	gsize size = n_blocks * n_bytes;

	p = calloc(1, size);

	if (p) {
		g_static_mutex_lock(&profiler_mutex);
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		profiler_hashtable_insert(p, n_bytes);
		g_static_mutex_unlock(&profiler_mutex);
	}

	assert(p);

	return p;
}

static void profiler_free(gpointer mem) {
	assert(mem);

	g_static_mutex_lock(&profiler_mutex);
	profiler_hashtable_remove(mem);
	g_static_mutex_unlock(&profiler_mutex);

	free(mem);
}

static void profiler_write(gchar *str, gint len) {
	gint res;
	gint written = 0;

	while (len) {
		res = write(profiler_output_fd, str + written, len);
		if (-1 == res) {
			fputs("error writing to profiler output file\n", stderr);
			fflush(stderr);
			exit(1);
		}

		written += res;
		len -= res;
	}
}

/* public functions */
void li_profiler_enable(gchar *output_path) {
	GMemVTable t;

	if (g_str_equal(output_path, "stdout")) {
		profiler_output_fd = STDOUT_FILENO;
	} else if (g_str_equal(output_path, "stderr")) {
		profiler_output_fd = STDERR_FILENO;
	} else {
		profiler_output_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC);
		if (-1 == profiler_output_fd) {
			fputs("error opening profiler output file\n", stderr);
			fflush(stderr);
			exit(1);
		}
	}

	block_free_list = profiler_block_new();
	profiler_hashtable = calloc(sizeof(profiler_block), PROFILER_HASHTABLE_SIZE);

	t.malloc = profiler_malloc;
	t.realloc = profiler_realloc;
	t.free = profiler_free;

	t.calloc = profiler_calloc;
	t.try_malloc = profiler_try_malloc;
	t.try_realloc = profiler_try_realloc;

	g_mem_set_vtable(&t);
}

void li_profiler_finish() {
	guint i;
	profiler_block *block, *block_tmp;

	for (i = 0; i < PROFILER_HASHTABLE_SIZE; i++) {
		for (block = profiler_hashtable[i]; block != NULL;) {
			block_tmp = block->next;
			free(block);
			block = block_tmp;
		}
	}

	for (block = block_free_list; block != NULL;) {
		block_tmp = block->next;
		free(block);
		block = block_tmp;
	}

	free(profiler_hashtable);
}

void li_profiler_dump() {
	profiler_block *block;
	guint i;
	gint len;
	gchar str[1024];
	gsize leaked_size = 0;
	guint leaked_num = 0;

	g_static_mutex_lock(&profiler_mutex);

	len = sprintf(str, "--------------- memory profiler dump @ %ju ---------------\n", time(NULL));
	profiler_write(str, len);

	for (i = 0; i < PROFILER_HASHTABLE_SIZE; i++) {
		for (block = profiler_hashtable[i]; block != NULL; block = block->next) {
			leaked_num++;
			leaked_size += block->size;
			len = sprintf(str, "--------------- unfreed block of %"G_GSIZE_FORMAT" bytes @ %p ---------------\n", block->size, block->addr);
			profiler_write(str, len);
#ifdef HAVE_EXECINFO_H
			backtrace_symbols_fd(block->stackframes, block->stackframes_num, profiler_output_fd);
#endif
		}
	}

	len = sprintf(str,
		"--------------- memory profiler stats ---------------\n"
		"leaked objects:\t\t%u\n"
		"leaked bytes:\t\t%"G_GSIZE_FORMAT" %s\n",
		leaked_num,
		(leaked_size > 1024) ? leaked_size / 1024 : leaked_size,
		(leaked_size > 1024) ? "kilobytes" : "bytes"
	);
	profiler_write(str, len);

	len = sprintf(str, "--------------- memory profiler dump end ---------------\n");
	profiler_write(str, len);

	g_static_mutex_unlock(&profiler_mutex);

}

void li_profiler_dump_table() {

}
