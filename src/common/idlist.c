
#include <lighttpd/idlist.h>

#define UL_BITS (sizeof(gulong) * 8)

/* There are often no explicit bit shifts used in this code. This is on purpose, the
 * code looks much cleaner without them, the correct constant for *, / and % is easier to calculate
 * as constant (UL_BITS) and the compiler should know how to optimize the operations; as UL_BITS is hopefully
 * of the form 2^n this should result in bit shifts in the executable code.
 */

liIDList* li_idlist_new(gint max_ids) {
	liIDList *l = g_slice_new0(liIDList);
	g_assert(max_ids > 0);
	l->bitvector = g_array_new(FALSE, TRUE, sizeof(gulong));
	l->max_ids = max_ids;
	l->next_free_id = -1;
	l->used_ids = 0;
	return l;
}

void li_idlist_free(liIDList *l) {
	if (!l) return;
	g_array_free(l->bitvector, TRUE);
	g_slice_free(liIDList, l);
}

static void mark_bit(GArray *a, gint id) {
	guint ndx = id / UL_BITS, bndx = id % UL_BITS;
	gulong bmask = 1ul << bndx;
	g_assert(id >= 0 && ndx < a->len);

	g_assert(0 == (g_array_index(a, gulong, ndx) & (bmask))); /* bit mustn't be set */
	g_array_index(a, gulong, ndx) |= (bmask);
}

static void clear_bit(GArray *a, gint id) {
	guint ndx = id / UL_BITS, bndx = id % UL_BITS;
	gulong bmask = 1ul << bndx;
	g_assert(id >= 0 && ndx < a->len);

	g_assert(0 != (g_array_index(a, gulong, ndx) & (bmask))); /* bit must be set */
	g_array_index(a, gulong, ndx) &= ~(bmask);
}

static void idlist_reserve(GArray *a, guint id) {
	guint ndx = id / UL_BITS;
	if (ndx >= a->len) g_array_set_size(a, ndx+1);
}

gint li_idlist_get(liIDList *l) {
	guint fndx, ndx;
	gint newid, bndx;
	gulong u = -1;
	GArray *a = l->bitvector;
	if (l->used_ids >= l->max_ids) return -1;

	if (l->next_free_id < 0) { /* all ids in use */
		newid = l->used_ids++;
		idlist_reserve(a, newid);
		mark_bit(a, newid);
		return newid;
	}

	/* search for an array entry which doesn't have all bits set (i.e. != (gulong) -1)
	 * start with the entry of next_free_id, all below are in use anyway
	 */
	fndx = l->next_free_id / UL_BITS;
	for (ndx = fndx; ndx < a->len && ((gulong) -1 == (u = g_array_index(a, gulong, ndx))); ndx++) ;

	if (ndx == a->len) { /* again: all ids are in use */
		l->next_free_id = -1;
		newid = l->used_ids++;
		idlist_reserve(a, newid);
		mark_bit(a, newid);
		return newid;
	}

	/* array entry != -1, search for free bit */
	if (fndx == ndx) bndx = (l->next_free_id / UL_BITS) - 1;
	else bndx = -1;
	bndx = g_bit_nth_lsf(~u, bndx);

	/* no free bit found; should never happen as u != -1 and next_free_id should be correct, i.e. all bits <= the bit start index should be set */
	g_assert(bndx != -1);

	newid = ndx * UL_BITS + bndx;
	if (newid == (gint) l->used_ids) {
		l->next_free_id = -1;
	} else {
		l->next_free_id = newid+1;
	}

	l->used_ids++;
	mark_bit(a, newid);

	return newid;
}

gboolean li_idlist_is_used(liIDList *l, gint id) {
	GArray *a = l->bitvector;
	guint ndx = id / UL_BITS, bndx = id % UL_BITS;
	gulong bmask = 1ul << bndx;
	if (id < 0 || ndx >= a->len) return FALSE;

	return (0 != (g_array_index(a, gulong, ndx) & (bmask)));
}

void li_idlist_put(liIDList *l, gint id) {
	clear_bit(l->bitvector, id);

	l->used_ids--;
	if ((l->next_free_id < 0 && (guint) id < l->used_ids) || id < l->next_free_id) l->next_free_id = id;
}
