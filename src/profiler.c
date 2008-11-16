
/*
 * lighty memory profiler
 * counts how many times malloc/realloc/free have been called and the amounts of bytes allocated/freed
 * TODO: move hashtable to utils.c, optimize hashtable? implementation is very basic
 */


#include <lighttpd/base.h>
#include <lighttpd/profiler.h>

#define PROFILER_HASHTABLE_SIZE 1024


struct profiler_entry {
	gpointer addr;
	gsize len;
	struct profiler_entry *next;
};
typedef struct  profiler_entry profiler_entry;

static profiler_mem stats_mem = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static GMutex *profiler_mutex = NULL;
static gboolean profiler_enabled = FALSE;
static profiler_entry *free_list = NULL;


static struct {
	profiler_entry **nodes;
} profiler_hashtable;

static void profiler_hashtable_init() {
	profiler_hashtable.nodes = calloc(1, sizeof(profiler_entry*) * PROFILER_HASHTABLE_SIZE);
}

static guint profiler_hash_addr(gpointer addr) {
	guint h = (gsize) addr;
	h = (h >> 3) * 2654435761; /* ~ golden ratio of 2^32, shift 3 because of 8 byte boundary alignment (use 2 for 4 byte boundary) */
	//printf("hashing addr 0x%zx: %u ([%u])\n", (gsize)addr, h, h % PROFILER_HASHTABLE_SIZE);
	return h % PROFILER_HASHTABLE_SIZE;
}

static profiler_entry *profiler_hashtable_find(gpointer addr) {
	guint h = profiler_hash_addr(addr);

	for (profiler_entry *e = profiler_hashtable.nodes[h]; e != NULL; e = e->next) {
		if (e->addr == addr)
			return e;
	}
	assert(NULL);
	return NULL;
}

static void profiler_hashtable_insert(gpointer addr, gsize len) {
	profiler_entry *e = free_list;
	free_list = free_list->next ? free_list->next : calloc(1, sizeof(profiler_entry));

	e->addr = addr;
	e->len = len;
	e->next = NULL;

	guint h = profiler_hash_addr(addr);

	if (profiler_hashtable.nodes[h] == NULL) {
		profiler_hashtable.nodes[h] = e;
		return;
	}

	for (profiler_entry *ec = profiler_hashtable.nodes[h];; ec = ec->next) {
		if (ec->next == NULL) {
			ec->next = e;
			return;
		}
	}
}

static void profiler_hashtable_remove(gpointer addr) {
	guint h = profiler_hash_addr(addr);
	profiler_entry *prev = profiler_hashtable.nodes[h];

	if (!prev)
		return;

	if (prev->addr == addr) {
		if (prev->next)
			profiler_hashtable.nodes[h] = prev->next;
		else
			profiler_hashtable.nodes[h] = NULL;
		free(prev);
		return;
	}

	for (profiler_entry *e = prev->next; e != NULL; e = e->next) {
		if (e->addr == addr) {
			prev->next = e->next;
			e->next = free_list;
			free_list = e;
			return;
		}
		prev = e;
	}
}

static gpointer profiler_try_malloc(gsize n_bytes) {
	/* we alloc sizeof(gsize) bytes more to hold n_bytes */
	gsize *p;

	p = malloc(n_bytes);

	if (p) {
		g_mutex_lock(profiler_mutex);
		profiler_hashtable_insert(p, n_bytes);
		stats_mem.alloc_times++;
		stats_mem.alloc_bytes += n_bytes;
		stats_mem.inuse_bytes += n_bytes;
		g_mutex_unlock(profiler_mutex);
	}

	return p;
}

static gpointer profiler_malloc(gsize n_bytes) {
	gpointer p = profiler_try_malloc(n_bytes);

	assert(p);

	return p;
}

static gpointer profiler_try_realloc(gpointer mem, gsize n_bytes) {
	gsize l;
	gsize *p = mem;

	if (!mem) {
		p = malloc(n_bytes);
		g_mutex_lock(profiler_mutex);
		stats_mem.alloc_times++;
		g_mutex_unlock(profiler_mutex);
		l = 0;
	}
	else {
		p = realloc(p, n_bytes);
		g_mutex_lock(profiler_mutex);
		profiler_entry *e = profiler_hashtable_find(mem);
		l = e->len;
		profiler_hashtable_remove(mem);
		g_mutex_unlock(profiler_mutex);
	}

	if (p) {
		g_mutex_lock(profiler_mutex);
		profiler_hashtable_insert(p, n_bytes);
		stats_mem.realloc_times++;
		stats_mem.realloc_bytes += n_bytes;
		stats_mem.inuse_bytes += n_bytes - l;
		g_mutex_unlock(profiler_mutex);
	}

	return p;
}

static gpointer profiler_realloc(gpointer mem, gsize n_bytes) {
	gpointer p = profiler_try_realloc(mem, n_bytes);

	assert(p);

	return p;
}

static gpointer profiler_calloc(gsize n_blocks, gsize n_bytes) {
	/* we alloc sizeof(gsize) bytes more to hold n_blocks*n_bytes */
	gsize *p;

	gsize l = n_blocks * n_bytes;

	p = calloc(1, l);

	if (p) {
		g_mutex_lock(profiler_mutex);
		profiler_hashtable_insert(p, l);
		stats_mem.calloc_times++;
		stats_mem.calloc_bytes += l;
		stats_mem.inuse_bytes += l;
		g_mutex_unlock(profiler_mutex);
	}

	assert(p);

	return p;
}

static void profiler_free(gpointer mem) {
	gsize *p = mem;

	assert(p);
	g_mutex_lock(profiler_mutex);
	profiler_entry *e = profiler_hashtable_find(mem);
	stats_mem.free_times++;
	stats_mem.free_bytes += e->len;
	stats_mem.inuse_bytes -= e->len;
	profiler_hashtable_remove(mem);
	g_mutex_unlock(profiler_mutex);
	g_mutex_free(profiler_mutex);
	free(p);
}

/* public functions */
void profiler_enable() {
	GMemVTable t;

	if (profiler_enabled)
		return;

	profiler_mutex = g_mutex_new();

	profiler_enabled = TRUE;

	profiler_hashtable_init();
	/* prealloc 50 hashtable entries */
	free_list = calloc(1, sizeof(profiler_entry));
	for (guint i = 0; i < 49; i++) {
		profiler_entry *e = calloc(1, sizeof(profiler_entry));
		e->next = free_list;
		free_list = e;
	}

	t.malloc = profiler_malloc;
	t.realloc = profiler_realloc;
	t.free = profiler_free;

	t.calloc = profiler_calloc;
	t.try_malloc = profiler_try_malloc;
	t.try_realloc = profiler_try_realloc;

	g_mem_set_vtable(&t);
}

void profiler_finish() {
	for (profiler_entry *e = free_list; e != NULL;) {
		profiler_entry *prev = e;
		e = e->next;
		free(prev);
	}
}

void profiler_dump() {
	if (!profiler_enabled)
		return;

	g_mutex_lock(profiler_mutex);
	profiler_mem s = stats_mem;
	g_mutex_unlock(profiler_mutex);

	g_print("--- memory profiler stats ---\n");
	g_print("malloc(): called %" G_GUINT64_FORMAT " times, %" G_GUINT64_FORMAT " bytes total\n", s.alloc_times, s.alloc_bytes);
	g_print("calloc(): called %" G_GUINT64_FORMAT " times, %" G_GUINT64_FORMAT " bytes total\n", s.calloc_times, s.calloc_bytes);
	g_print("realloc(): called %" G_GUINT64_FORMAT " times, %" G_GUINT64_FORMAT " bytes total\n", s.realloc_times, s.realloc_bytes);
	g_print("free(): called %" G_GUINT64_FORMAT " times, %" G_GUINT64_FORMAT " bytes total\n", s.free_times, s.free_bytes);
	g_print("memory remaining: %" G_GUINT64_FORMAT " bytes, %" G_GUINT64_FORMAT " calls to free()\n", s.inuse_bytes, s.alloc_times + s.calloc_times - s.free_times);
}

void profiler_dump_table() {
	for (guint i = 0; i < PROFILER_HASHTABLE_SIZE; i++) {
		guint n = 1;
		for (profiler_entry *e = profiler_hashtable.nodes[i]; e; e = e->next) {
			g_print("profiler entry #%u/%u: addr 0x%zx, size %zu bytes\n", i+1, n, (gsize)e->addr, e->len);
			n++;
		}
	}
}
