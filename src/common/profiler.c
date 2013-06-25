
/*
 * lighty memory profiler
 * prints a backtrace for every object not free()d at exit()
 *
 */


#include <lighttpd/settings.h>
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
#define PROFILER_STACKFRAMES 36

typedef struct profiler_block profiler_block;
struct profiler_block {
	gpointer addr;
	gsize size;
	profiler_block *next;
	void *stackframes[PROFILER_STACKFRAMES];
	gint stackframes_num;
};

typedef struct profiler_stackframe profiler_stackframe;
struct profiler_stackframe {
	gpointer addr;
	gsize size;
	guint blocks;
	gchar *symbol;
	profiler_stackframe *next;
	profiler_stackframe *children;
};


static void profiler_free(gpointer addr);

static GStaticMutex profiler_mutex = G_STATIC_MUTEX_INIT;
static profiler_block *block_free_list = NULL;
static profiler_block **profiler_hashtable = NULL;
static gint profiler_output_fd = 0;
static gpointer profiler_heap_base = NULL;

gboolean li_profiler_enabled = FALSE;


static guint profiler_hash(gpointer addr) {
	return ((uintptr_t)addr * 2654435761); /* ~ golden ratio of 2^32 */
}

static profiler_block *profiler_block_new(void) {
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

static gpointer profiler_try_malloc(gsize n_bytes) {
	gsize *p;

	p = malloc(n_bytes);

	if (p) {
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		li_profiler_hashtable_insert(p, n_bytes);
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
		li_profiler_hashtable_remove(mem);
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		li_profiler_hashtable_insert(p, n_bytes);
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
#if defined(LIGHTY_OS_MACOSX)
		n_bytes = malloc_size(p);
#elif defined(LIGHTY_OS_LINUX)
		n_bytes = malloc_usable_size(p);
#endif
		li_profiler_hashtable_insert(p, n_bytes);
	}

	assert(p);

	return p;
}

static void profiler_free(gpointer mem) {
	assert(mem);
	li_profiler_hashtable_remove(mem);
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
			abort();
		}

		written += res;
		len -= res;
	}
}

#ifdef HAVE_EXECINFO_H
static void profiler_dump_frame(guint level, profiler_stackframe *frame, gsize minsize) {
	gchar str[1024];
	gint len;
	gboolean swapped;
	profiler_stackframe *f;

	/* sort this tree level according to total allocated size. yes, bubblesort. */
	do {
		profiler_stackframe *f1, *f2;

		swapped = FALSE;

		for (f1 = frame, f2 = frame->next; f1 != NULL && f2 != NULL; f1 = f2, f2 = f2->next) {
			if (f2->size > f1->size) {
				profiler_stackframe tmp = *f1;

				f1->addr = f2->addr;
				f1->blocks = f2->blocks;
				f1->size = f2->size;
				f1->children = f2->children;
				f1->symbol = f2->symbol;

				f2->addr = tmp.addr;
				f2->blocks = tmp.blocks;
				f2->size = tmp.size;
				f2->children = tmp.children;
				f2->symbol = tmp.symbol;

				swapped = TRUE;
			}
		}
	} while (swapped);

	while (frame) {
		if (frame->size >= minsize) {
			/* indention */
			memset(str, ' ', level*4);
			profiler_write(str, level*4);
			len = sprintf(str,
				"%"G_GSIZE_FORMAT" %s in %u blocks @ %p %s\n",
				(frame->size > 1024) ? frame->size / 1024 : frame->size,
				(frame->size > 1024) ? "kilobytes" : "bytes",
				frame->blocks,
				frame->addr, frame->symbol
			);
			profiler_write(str, len);
		}

		if (frame->children)
			profiler_dump_frame(level+1, frame->children, minsize);

		f = frame->next;
		free(frame->symbol);
		free(frame);
		frame = f;
	}
}
#endif

/* public functions */
void li_profiler_enable(gchar *output_path) {
	GMemVTable t;

	/* force allocation of mutex data; newer glib versions allocate extra data */
	g_static_mutex_lock(&profiler_mutex);
	g_static_mutex_unlock(&profiler_mutex);

	profiler_heap_base = sbrk(0);

	if (g_str_equal(output_path, "stdout")) {
		profiler_output_fd = STDOUT_FILENO;
	} else if (g_str_equal(output_path, "stderr")) {
		profiler_output_fd = STDERR_FILENO;
	} else {
		profiler_output_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (-1 == profiler_output_fd) {
			fputs("error opening profiler output file\n", stderr);
			fflush(stderr);
			abort();
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

	li_profiler_enabled = TRUE;
}

void li_profiler_finish(void) {
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


void li_profiler_dump(gint minsize) {
	profiler_stackframe *tree_cur, *frame;
	gchar **symbols;
	gint i, j, len;
	profiler_block *block;
	struct stat st;
	gchar str[1024];
	gsize total_size = 0;
	guint total_blocks = 0;
	profiler_stackframe *tree = calloc(1, sizeof(profiler_stackframe));

	g_static_mutex_lock(&profiler_mutex);

	fstat(profiler_output_fd, &st);
	lseek(profiler_output_fd, st.st_size, SEEK_SET);

	len = sprintf(str, "--------------- memory profiler dump @ %ju ---------------\n", time(NULL));
	profiler_write(str, len);

	for (i = 0; i < PROFILER_HASHTABLE_SIZE; i++) {
		for (block = profiler_hashtable[i]; block != NULL; block = block->next) {
			total_size += block->size;
			total_blocks++;

#ifdef HAVE_EXECINFO_H
			/* resolve all symbols */
			symbols = backtrace_symbols(block->stackframes, block->stackframes_num);

			tree_cur = tree;

			for (j = block->stackframes_num-1; j >= 0; j--) {
				for (frame = tree_cur->children; frame != NULL; frame = frame->next) {
					if (block->stackframes[j] == frame->addr) {
						frame->blocks++;
						frame->size += block->size;
						break;
					}
				}

				if (!frame) {
					frame = malloc(sizeof(profiler_stackframe));
					frame->addr = block->stackframes[j];
					frame->blocks = 1;
					frame->size = block->size;
					frame->symbol = strdup(symbols[j]);
					frame->children = NULL;
					frame->next = tree_cur->children;
					tree_cur->children = frame;
				}

				tree_cur = frame;
			}

			free(symbols);
#endif

		}
	}

#ifdef HAVE_EXECINFO_H
	profiler_dump_frame(0, tree->children, minsize);
#endif

	len = sprintf(str,
		"--------------- memory profiler summary ---------------\n"
		"total blocks: %u\n"
		"total size:   %"G_GSIZE_FORMAT" %s\n"
		"heap base / break / size: %p / %p / %"G_GSIZE_FORMAT"\n",
		total_blocks,
		(total_size > 1024) ? total_size / 1024 : total_size,
		(total_size > 1024) ? "kilobytes" : "bytes",
		profiler_heap_base, sbrk(0), (guintptr)sbrk(0) - (guintptr)profiler_heap_base
	);
	profiler_write(str, len);

	len = sprintf(str, "--------------- memory profiler dump end ---------------\n");

	free(tree);

	profiler_write(str, len);

	g_static_mutex_unlock(&profiler_mutex);
}

void li_profiler_hashtable_insert(const gpointer addr, gsize size) {
	profiler_block *block;
	guint hash;

	g_static_mutex_lock(&profiler_mutex);

	hash = profiler_hash(addr);

	block = profiler_block_new();
	block->addr = addr;
	block->size = size;
#ifdef HAVE_EXECINFO_H
	block->stackframes_num = backtrace(block->stackframes, PROFILER_STACKFRAMES);
#endif

	block->next = profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE];
	profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE] = block;

	g_static_mutex_unlock(&profiler_mutex);
}

void li_profiler_hashtable_remove(const gpointer addr) {
	profiler_block *block, *block_prev;
	guint hash;

	g_static_mutex_lock(&profiler_mutex);

	hash = profiler_hash(addr);

	block = profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE];

	if (block->addr == addr) {
		profiler_hashtable[hash % PROFILER_HASHTABLE_SIZE] = block->next;
		profiler_block_free(block);
		g_static_mutex_unlock(&profiler_mutex);
		return;
	}

	block_prev = block;
	for (block = block->next; block != NULL; block = block->next) {
		if (block->addr == addr) {
			block_prev->next = block->next;
			profiler_block_free(block);
			g_static_mutex_unlock(&profiler_mutex);
			return;
		}
		block_prev = block;
	}

	g_static_mutex_unlock(&profiler_mutex);
}
