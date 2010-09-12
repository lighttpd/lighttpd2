#ifndef _LIGHTTPD_MIMETYPE_H_
#define _LIGHTTPD_MIMETYPE_H_

struct liMimetypeNode {
	guchar cmin;
	guchar cmax;
	gpointer *children; /* array of either liMimetypeNode* or GString* */
	GString *mimetype;
};
typedef struct liMimetypeNode liMimetypeNode;

LI_API liMimetypeNode *li_mimetype_node_new(void);
LI_API void li_mimetype_node_free(liMimetypeNode *node);
LI_API void li_mimetype_insert(liMimetypeNode *node, GString *suffix, GString *mimetype, guint depth);

/* looks up the mimetype for a filename by comparing suffixes. longest match is returned. do not free the result */
LI_API GString *li_mimetype_get(liVRequest *vr, GString *filename);

#endif
