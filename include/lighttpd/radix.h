#ifndef _LIGHTTPD_RADIX_H_
#define _LIGHTTPD_RADIX_H_

/*
 * Radix tree with 32bit key, lookup, insert and remove in O(32).
 * This is where the bit magic happens.
 */

struct RadixNode32 {
	guint32 key;
	guint32 mask;
	gpointer data;
	struct RadixNode32 *parent;
	struct RadixNode32 *right;
	struct RadixNode32 *left;
};

typedef struct RadixNode32 RadixNode32;

struct RadixTree32 {
	RadixNode32 **root;
	guint32 size;
	guint32 root_width;
	guint32 root_mask;
};

typedef struct RadixTree32 RadixTree32;

LI_API RadixTree32 *radixtree32_new(guint32 root_width);
LI_API guint32 radixtree32_free(RadixTree32 *tree);

LI_API void radixtree32_insert(RadixTree32 *tree, guint32 key, guint32 mask, gpointer data);
LI_API gboolean radixtree32_remove(RadixTree32 *tree, guint32 key, guint32 mask);

/* lookup tree node (best match) */
LI_API RadixNode32 *radixtree32_lookup_node(RadixTree32 *tree, guint32 key);

/* lookup data pointer (best match) */
LI_API gpointer radixtree32_lookup(RadixTree32 *tree, guint32 key);
/* lookup data pointer (exact match) */
LI_API gpointer radixtree32_lookup_exact(RadixTree32 *tree, guint32 key);

/*
struct RadixNode128 {
	guint32 key[4];
	guint32 mask[3];
	gpointer data;
	struct RadixNode128 *parent;
	struct RadixNode128 *right;
	struct RadixNode128 *left;
};

typedef struct RadixNode128 RadixNode128;

struct RadixTree128 {
	RadixNode128 **root;
	guint64 size;
	guint32 root_width;
	guint32 root_mask;
}

LI_api RadixTree128 *radixtree128_new(guint32 root_width);
LI_API guint radixtree128_free(RadixTree128 *tree);

LI_API void radixtree128_insert(RadixTree128 *tree, guint32 *key, guint32 *mask, gpointer data);
LI_API gboolean radixtree128_remove(RadixTree128 *tree, guint32 *key, guint32 *mask);
LI_API gpointer radixtree128_lookup(RadixTree128 *tree, guint32 *key);
*/
#endif
