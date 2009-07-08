#ifndef _LIGHTTPD_RADIX_H_
#define _LIGHTTPD_RADIX_H_

#include <lighttpd/settings.h>

/*
 * Radix tree with 32bit key, lookup, insert and remove in O(32).
 * This is where the bit magic happens.
 */

typedef struct liRadixNode32 liRadixNode32;
struct liRadixNode32 {
	guint32 key;
	guint32 mask;
	gpointer data;
	liRadixNode32 *parent;
	liRadixNode32 *right;
	liRadixNode32 *left;
};

typedef struct liRadixTree32 liRadixTree32;
struct liRadixTree32 {
	liRadixNode32 **root;
	guint32 size;
	guint32 root_width;
	guint32 root_mask;
};


LI_API liRadixTree32 *radixtree32_new(guint32 root_width);
LI_API guint32 radixtree32_free(liRadixTree32 *tree);

LI_API void radixtree32_insert(liRadixTree32 *tree, guint32 key, guint32 mask, gpointer data);
LI_API gboolean radixtree32_remove(liRadixTree32 *tree, guint32 key, guint32 mask);

/* lookup tree node (best match) */
LI_API liRadixNode32 *radixtree32_lookup_node(liRadixTree32 *tree, guint32 key);

/* lookup data pointer (best match) */
LI_API gpointer radixtree32_lookup(liRadixTree32 *tree, guint32 key);
/* lookup data pointer (exact match) */
LI_API gpointer radixtree32_lookup_exact(liRadixTree32 *tree, guint32 key);

/*
typedef struct liRadixNode128 liRadixNode128;
struct liRadixNode128 {
	guint32 key[4];
	guint32 mask[3];
	gpointer data;
	liRadixNode128 *parent;
	liRadixNode128 *right;
	liRadixNode128 *left;
};


struct liRadixTree128 {
	liRadixNode128 **root;
	guint64 size;
	guint32 root_width;
	guint32 root_mask;
}

LI_api liRadixTree128 *radixtree128_new(guint32 root_width);
LI_API guint radixtree128_free(liRadixTree128 *tree);

LI_API void radixtree128_insert(liRadixTree128 *tree, guint32 *key, guint32 *mask, gpointer data);
LI_API gboolean radixtree128_remove(liRadixTree128 *tree, guint32 *key, guint32 *mask);
LI_API gpointer radixtree128_lookup(liRadixTree128 *tree, guint32 *key);
*/
#endif
