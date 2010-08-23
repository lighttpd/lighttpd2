#ifndef _LIGHTTPD_RADIX_H_
#define _LIGHTTPD_RADIX_H_

#include <lighttpd/settings.h>

typedef struct liRadixTree liRadixTree;

LI_API liRadixTree* li_radixtree_new(void);
LI_API void li_radixtree_free(liRadixTree *tree, GFunc free_func, gpointer free_userdata);

LI_API gpointer li_radixtree_insert(liRadixTree *tree, const void *key, guint32 bits, gpointer data); /* returns old data after overwrite */
LI_API gpointer li_radixtree_remove(liRadixTree *tree, const void *key, guint32 bits); /* returns data from removed node */
LI_API gpointer li_radixtree_lookup(liRadixTree *tree, const void *key, guint32 bits); /* longest matching prefix */
LI_API gpointer li_radixtree_lookup_exact(liRadixTree *tree, const void *key, guint32 bits);

LI_API void li_radixtree_foreach(liRadixTree *tree, GFunc func, gpointer userdata);

#endif
