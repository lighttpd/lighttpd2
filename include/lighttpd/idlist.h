#ifndef _LIGHTTPD_IDLIST_H_
#define _LIGHTTPD_IDLIST_H_

#include <lighttpd/settings.h>

typedef struct liIDList liIDList;

struct liIDList {
	/* used ids are marked with a "1" in the bitvector (represented as array of gulong) */
	GArray *bitvector;

	/* all ids are in the range [0, max_ids[, i.e. 0 <= id < max_ids
	 * although the type is guint, it has to fit in a gint too, as we
	 * use gint for the ids in the interface, so we can use -1 as a special value.
	 */
	guint max_ids;

	/* if all ids in [0, used_ids-1] are used, next_free_id is -1
	 * if not, then all available ids are >= next_free_id,
	 * so we can start at next_free_id for searching the next free id
	 */
	gint next_free_id;
	guint used_ids;
};

/* create new idlist; the parameter max_ids is "signed" on purpose */
LI_API liIDList* li_idlist_new(gint max_ids);

/* free idlist */
LI_API void li_idlist_free(liIDList *l);

/* request new id; return -1 if no id is available, valid ids are always > 0 */
LI_API gint li_idlist_get(liIDList *l);

/* check whether an id is in use and can be "_put" */
LI_API gboolean li_idlist_is_used(liIDList *l, gint id);

/* release id. never release an id more than once! */
LI_API void li_idlist_put(liIDList *l, gint id);

#endif
