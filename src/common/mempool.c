
#include <lighttpd/mempool.h>

/*
 * available #defines:
 * - MEMPOOL_MALLOC
 *   just use malloc, "disable" mempool
 * - MEMPOOL_SAFE_MUTEX
 *   use g_mutex in mempool to synchronize access in magazines;
 *   by default we use spinlocks (busy wait), as we assume that memory
 *   objects stay in one thread by default
 * - MP_SEARCH_BITVECTOR
 *   search in bitvector of a magazine for free chunks
 */

#define MP_SEARCH_BITVECTOR


#ifdef MEMPOOL_MALLOC

mempool_ptr mempool_alloc(gsize size) {
	mempool_ptr ptr = { NULL, g_malloc(size) };
	return ptr;
}

void mempool_free(mempool_ptr ptr, gsize size) {
	UNUSED(size);
	if (!ptr.data) return;
	g_free(ptr.data);
}

void mempool_cleanup() {
}

gsize mempool_align_page_size(gsize size) {
	return size;
}

#else /* MP_MALLOC */

/*
 * mempool:
 *  - allocate memory for a chunk; each thread has up to 2 magazines for each chunk size we use (page-aligned);
 *    each magazine has one mmap-area from which chunks are allocated
 *  - if MAP_ANON is not available (for mmap) use malloc instead to allocate the magazine area; as the size of
 *    these areas exceeds 1MB perhaps the default malloc() uses a sane fallback... (instead of brk()).
 *  - if a magazine is full, the thread allocates a new one; magazines aren't reused
 *  - if MP_SEARCH_BITVECTOR is defined, we search for free chunks in the bitvector;
 *    if not, we don't even reuse chunks in a "active" magazine, unless it is the last one we allocated from it
 *  - needed characteristics are:
 *    + fast for few sizes (all page-aligned)
 *    + allow complex interface (don't want to replace malloc/free)
 *    + assume memory is going to be released soon again
 *    + avoid fragmentation with other allocators (like 1024*malloc(16kb); malloc(1byte); 1024*free(16kb) - 16mb "garbage")
 */

# define UL_BITS (sizeof(gulong) * 8)

# define MP_MAX_ALLOC_SIZE (8*1024*1024)
# define MP_MIN_ALLOC_COUNT 8
# define MP_MAX_ALLOC_COUNT 256
# define MP_MAX_MAGAZINES 2

# define MP_BIT_VECTOR_SIZE ((MP_MAX_ALLOC_COUNT + UL_BITS - 1)/UL_BITS)

# if 0
#  define mp_assert(x) g_assert(x)
# else
#  define mp_assert(x) do { (void) 0; } while (0)
# endif


# ifdef MEMPOOL_SAFE_MUTEX
/* use GMutex */
typedef GMutex* mp_lock;
#  define MP_LOCK_NEW() g_mutex_new()
#  define MP_LOCK_FREE(lock) g_mutex_free(lock)
#  define MP_LOCK(lock) g_mutex_lock(lock)
#  define MP_TRYLOCK(lock) g_mutex_trylock(lock)
#  define MP_UNLOCK(lock) g_mutex_unlock(lock)
# else
/* use spinlocks */
typedef gint mp_lock;
#  define MP_LOCK_NEW() (1)
#  define MP_LOCK_FREE(lock) do { (void) 0; } while (0)
#  define MP_LOCK(lock) do { (void) 0; } while (!g_atomic_int_compare_and_exchange(&lock, 1, 0))
#  define MP_TRYLOCK(lock) (g_atomic_int_compare_and_exchange(&lock, 1, 0))
#  define MP_UNLOCK(lock) (g_atomic_int_set(&lock, 1))
# endif


typedef struct mp_pools mp_pools;
typedef struct mp_pool mp_pool;
typedef struct mp_magazine mp_magazine;

struct mp_pool {
	guint32 chunksize;

	/* if magazines[i+1] != NULL => magazines[i] != NULL - only the "head" entries are not NULL */
	/* so we can stop searching if an entry is NULL */
	mp_magazine *magazines[MP_MAX_MAGAZINES];

	GList pools_list; /* list element for the mp_pools.queue */
};

struct mp_magazine {
	gint refcount; /* one ref from pool + one per allocated chunk */
	void *data; /* pointer to mmap area */
	guint32 chunksize;
	guint32 used, count;
#  ifndef MP_SEARCH_BITVECTOR
	guint32 next;
#  endif
	gulong bv_used[MP_BIT_VECTOR_SIZE];

	mp_lock mutex;
};

/* one queue of pools per thread */
struct mp_pools {
	/* one pool per chunksize; queue is sorted ASC by chunksize */
	GQueue queue;
};

static void mp_pools_free(gpointer _pools);

static GPrivate *thread_pools = NULL;
static gboolean mp_initialized = 0;
static gsize mp_pagesize = 0;

static GStaticMutex mp_init_mutex = G_STATIC_MUTEX_INIT;

static void mempool_init() {
	g_static_mutex_lock (&mp_init_mutex);
	if (!mp_initialized) {
		mp_pagesize = sysconf(_SC_PAGE_SIZE);
		if (0 == mp_pagesize) mp_pagesize = 4*1024; /* 4kb default */

		if (!g_thread_supported()) g_thread_init (NULL);

		thread_pools = g_private_new(mp_pools_free);

		mp_initialized = TRUE;
	}
	g_static_mutex_unlock (&mp_init_mutex);
}

/* only call if you know that mempool_init was called */
static inline gsize mp_align_size(gsize size) {
	/* assume pagesize is 2^n */
	size = (size + mp_pagesize - 1) & ~(mp_pagesize-1);
	return size;
}

gsize mempool_align_page_size(gsize size) {
	if (G_UNLIKELY(!mp_initialized)) {
		mempool_init();
	}
	return mp_align_size(size);
}

static inline void* mp_alloc_page(gsize size) {
	void *ptr;
# ifdef MAP_ANON
	if (G_UNLIKELY(MAP_FAILED == (ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)))) {
		g_error ("%s: failed to allocate %"G_GSIZE_FORMAT" bytes with mmap", G_STRLOC, size);
	}
# else
	if (G_UNLIKELY(NULL == (ptr = malloc(size)))) {
		g_error ("%s: failed to allocate %"G_GSIZE_FORMAT" bytes", G_STRLOC, size);
	}
# endif
	return ptr;
}

static inline void mp_free_page(const void *ptr, gsize size) {
	if (!ptr) return;
# ifdef MAP_ANON
	munmap((void*) ptr, size);
# else
	free(ptr);
# endif
}

/* how many chunks in an arey for a chunk size? */
static gsize mp_chunks_for_size(gsize size) {
	gsize chunks = MP_MAX_ALLOC_SIZE/size;
	if (chunks > MP_MAX_ALLOC_COUNT) chunks = MP_MAX_ALLOC_COUNT;
	return chunks;
}

static mp_magazine* mp_mag_new(mp_pool *pool) {
	mp_magazine *mag = g_slice_new0(mp_magazine);
	mag->refcount = 1;
	mag->chunksize = pool->chunksize;
	mag->used = 0;
# ifndef MP_SEARCH_BITVECTOR
	mag->next = 0;
# endif
	mag->count = mp_chunks_for_size(mag->chunksize);
	mag->mutex = MP_LOCK_NEW();

	return mag;
}

/* do NOT call with lock held */
static inline void mp_mag_release(mp_magazine *mag) {
	if (!mag) return;
	assert(g_atomic_int_get(&mag->refcount) > 0);
	if (g_atomic_int_dec_and_test(&mag->refcount)) {
		MP_LOCK_FREE(mag->mutex);
		g_slice_free(mp_magazine, mag);
	}
}

static inline void mp_mag_acquire(mp_magazine *mag) {
	assert(g_atomic_int_get(&mag->refcount) > 0);
	g_atomic_int_inc(&mag->refcount);
}

static inline guint find_free_bit(gulong l) {
	guint idx;
	guint8 b;

	mp_assert(~0ul != l);

	for (idx = 0; ((guint8)~0u) == (b = (guint8)(l >> idx)); idx += 8) ;
	for ( ; (b & 1); b >>= 1, idx++ ) ;

	return idx;
}

/* call with lock held */
static inline void* mp_mag_alloc(mp_magazine *mag) {
	gulong *bv = mag->bv_used;
# ifndef MP_SEARCH_BITVECTOR
	guint id = mag->next, bndx = id % UL_BITS, ndx = id / UL_BITS;
# else
	guint id, ndx;
# endif

	mp_assert(mag->used < mag->count);
	mp_assert(mag->next < mag->count);

	if (NULL == mag->data) {
		mag->data = mp_alloc_page(mag->count * mag->chunksize);
	}

# ifndef MP_SEARCH_BITVECTOR
	bv[ndx] |= (1ul << bndx);
	mag->next++;
# else
	for (ndx = 0; ndx < MP_BIT_VECTOR_SIZE && (bv[ndx] == ~0ul); ndx++) ;
	mp_assert(ndx < MP_BIT_VECTOR_SIZE);

	id = find_free_bit(bv[ndx]);
	/* id = g_bit_nth_lsf(~bv[ndx], -1); */
	bv[ndx] |= (1ul << id);
	id += UL_BITS * ndx;
# endif

	mag->used++;

	return (void*) (((intptr_t)mag->data) + (id * mag->chunksize));
}

/* call with lock held; update ref counter after releasing lock! */
static inline void mp_mag_free(mp_magazine *mag, void *ptr) {
	guint id = (((intptr_t) ptr) - ((intptr_t) mag->data)) / mag->chunksize;
	guint ndx = id / UL_BITS, bndx = id % UL_BITS;
	gulong bmask = 1ul << bndx;

	mp_assert(ndx < MP_BIT_VECTOR_SIZE);
	g_assert(0 != (mag->bv_used[ndx] & bmask)); /* check for double free */

	mag->bv_used[ndx] &= ~bmask;
	mag->used--;

# ifndef MP_SEARCH_BITVECTOR
	if (id == mag->next - 1) mag->next--; /* if chunk was just allocated, "undo" it */
# endif

	if (G_UNLIKELY(0 == mag->used)) {
		mp_free_page(mag->data, mag->count * mag->chunksize);
		mag->data = NULL;
# ifndef MP_SEARCH_BITVECTOR
		mag->next = 0;
# endif
	}
}

static mp_pool* mp_pool_new(gsize size) {
	mp_pool *pool = g_slice_new0(mp_pool);
	pool->chunksize = size;
	pool->pools_list.data = pool;
	pool->magazines[0] = mp_mag_new(pool);

	return pool;
}

static void mp_pool_free(mp_pool *pool) {
	guint i;
	mp_magazine *mag;
	if (!pool) return;

	for (i = 0; i < MP_MAX_MAGAZINES; i++) {
		mag = pool->magazines[i];
		pool->magazines[i] = NULL;
		mp_mag_release(mag);
	}

	g_slice_free(mp_pool, pool);
}

static void _queue_insert_before(GQueue *queue, GList *sibling, GList *item) {
	item->prev = sibling->prev;
	item->next = sibling;
	sibling->prev = item;
	if (NULL == item->prev) {
		queue->head = item;
	} else {
		item->prev->next = item;
	}
	queue->length++;
}

static void mp_pools_free(gpointer _pools) {
	mp_pools *pools = _pools;
	mp_pool *pool;
	GList *iter;

	while (NULL != (iter = g_queue_pop_head_link(&pools->queue))) {
		pool = iter->data;

		mp_pool_free(pool);
	}

	g_slice_free(mp_pools, pools);
}

static inline mp_pool* mp_pools_get(gsize size) {
	GList *iter;
	mp_pools *pools;
	mp_pool *pool;

	pools = g_private_get(thread_pools);
	if (G_UNLIKELY(!pools)) {
		pools = g_slice_new0(mp_pools);
		g_private_set(thread_pools, pools);
	}

	for (iter = pools->queue.head; iter; iter = iter->next) {
		pool = iter->data;
		if (G_LIKELY(pool->chunksize == size)) {
			goto done;
		} else if (G_UNLIKELY(pool->chunksize > size)) {
			pool = mp_pool_new(size);
			_queue_insert_before(&pools->queue, iter, &pool->pools_list);
			goto done;
		}
	}

	pool = mp_pool_new(size);
	g_queue_push_tail_link(&pools->queue, &pool->pools_list);

done:
	return pool;
}

mempool_ptr mempool_alloc(gsize size) {
	mempool_ptr ptr = { NULL, NULL };
	mp_pool *pool;
	mp_magazine *mag;
	guint i;

	if (G_UNLIKELY(!mp_initialized)) {
		mempool_init();
	}

	size = mp_align_size(size);

	/* mp_alloc_page fallback */
	if (G_UNLIKELY(size > MP_MAX_ALLOC_SIZE/MP_MIN_ALLOC_COUNT)) {
		if (G_UNLIKELY(NULL == (ptr.data = mp_alloc_page(size)))) {
			g_error ("%s: failed to allocate %"G_GSIZE_FORMAT" bytes", G_STRLOC, size);
		}
		return ptr;
	}

	pool = mp_pools_get(size);

	/* Try to lock a unlocked magazine if possible; creating new magazines is allowed
	 * (new ones can't be locked as only the current thread knows this magazine)
	 * Spinlock the first magazine if first strategy failed */
	if (G_LIKELY(pool->magazines[0])) {
		/* at least one magazine available */
		for (i = 0; i < MP_MAX_MAGAZINES; i++) {
			mag = pool->magazines[i];
			if (!mag) break;
			if (G_LIKELY(MP_TRYLOCK(mag->mutex))) {
				goto found_mag;
			}
		}
		i = 0;
		mag = pool->magazines[0];
		MP_LOCK(mag->mutex);
	} else {
		/* no magazine - just create one */
		i = 0;
		mag = pool->magazines[0] = mp_mag_new(pool);
		MP_LOCK(mag->mutex);
	}
found_mag:

	ptr.priv_data = mag;
	ptr.data = mp_mag_alloc(mag);
	mp_mag_acquire(mag); /* keep track of chunk count */

# ifndef MP_SEARCH_BITVECTOR
	if (G_UNLIKELY(mag->next == mag->count)) {
# else
	if (G_UNLIKELY(mag->used == mag->count)) {
# endif
		/* full magazine; remove from pool */
		guint j;

		/* replace entry with last entry != NULL */
		for (j = i+1; j < MP_MAX_MAGAZINES && pool->magazines[j]; j++) ;
		if (j < MP_MAX_MAGAZINES) {
			pool->magazines[i] = pool->magazines[j];
			pool->magazines[j] = NULL;
		} else {
			pool->magazines[i] = NULL;
		}

		MP_UNLOCK(mag->mutex);
		mp_mag_release(mag); /* keep track of pool -> magazine ref; release always after unlock! */
	} else {
		MP_UNLOCK(mag->mutex);
	}

	return ptr;
}

void mempool_free(mempool_ptr ptr, gsize size) {
	mp_magazine *mag;
	if (!ptr.data) return;

	size = mp_align_size(size);

	/* mp_alloc_page fallback */
	if (G_UNLIKELY(size > MP_MAX_ALLOC_SIZE/MP_MIN_ALLOC_COUNT)) {
		mp_free_page(ptr.data, size);
		return;
	}

	mp_assert(ptr.priv_data);
	mag = ptr.priv_data;
	MP_LOCK(mag->mutex);
	mp_mag_free(mag, ptr.data);
	MP_UNLOCK(mag->mutex);

	mp_mag_release(mag); /* keep track of chunk count; release always after unlock! */
}

void mempool_cleanup() {
	/* "Force" thread-private cleanup */
	mp_pools *pools;

	if (G_UNLIKELY(!mp_initialized)) {
		mempool_init();
	}

	pools = g_private_get(thread_pools);

	if (pools) {
		g_private_set(thread_pools, NULL);
		mp_pools_free(pools);
	}
}

#endif /* !MP_MALLOC */
