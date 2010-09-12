
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <assert.h>

#define MIME_COUNT_CHILDREN(x)	(x->cmin == 0 ? 0 : ((guint)x->cmax - x->cmin + 1))
#define MIME_MARK_NODE(x)		((gpointer)((uintptr_t)x | 1))
#define MIME_UNMARK_NODE(x)		((gpointer)((uintptr_t)x & (~1)))
#define MIME_IS_NODE(x)			(1 == ((uintptr_t)x & 1))

LI_API liMimetypeNode *li_mimetype_node_new(void) {
	return g_slice_new0(liMimetypeNode);
}

LI_API void li_mimetype_node_free(liMimetypeNode *node) {
	guint i;
	gpointer ptr;

	if (node->mimetype)
		g_string_free(node->mimetype, TRUE);

	for (i = 0; i < MIME_COUNT_CHILDREN(node); i++) {
		ptr = node->children[i];

		if (NULL == ptr)
			continue;

		if (!MIME_IS_NODE(ptr)) {
			g_string_free(ptr, TRUE);
		} else {
			li_mimetype_node_free(MIME_UNMARK_NODE(ptr));
		}
	}

	if (node->children)
		g_free(node->children);

	g_slice_free(liMimetypeNode, node);
}

LI_API void li_mimetype_insert(liMimetypeNode *node, GString *suffix, GString *mimetype, guint depth) {
	guchar c, cdiff;
	gpointer ptr;
	liMimetypeNode *next_node;
	assert(!MIME_IS_NODE(mimetype));

	/* start of suffix reached */
	if (depth == suffix->len) {
		if (node->mimetype)
			g_string_free(node->mimetype, TRUE);

		node->mimetype = mimetype;
		return;
	}

	c = (guchar) suffix->str[suffix->len - depth - 1];
	assert(c != '\0');

	if (NULL == node->children) {
		node->cmin = node->cmax = c;
		node->children = g_malloc(sizeof(gpointer));
		node->children[0] = mimetype;
		return;
	} else if (c < node->cmin) {
		cdiff = node->cmin - c; /* how much space we need in front */
		node->children = g_realloc(node->children, sizeof(gpointer) * (MIME_COUNT_CHILDREN(node) + cdiff)); /* make room for more children */
		memmove(&node->children[(guint)cdiff], node->children, sizeof(gpointer) * MIME_COUNT_CHILDREN(node)); /* move existing children to the back */
		memset(node->children, 0, cdiff * sizeof(gpointer));
		node->cmin = c;
	} else if (c > node->cmax) {
		cdiff = c - node->cmax;
		node->children = g_realloc(node->children, sizeof(gpointer) * (MIME_COUNT_CHILDREN(node) + cdiff)); /* make room for more children */
		memset(&node->children[MIME_COUNT_CHILDREN(node)], 0, cdiff * sizeof(gpointer));
		node->cmax = c;
	}

	ptr = node->children[c - node->cmin];

	/* slot not used yet, just point to mimetype */
	if (ptr == NULL) {
		node->children[c - node->cmin] = mimetype;
		return;
	}

	/* slot contains another node */
	if (MIME_IS_NODE(ptr)) {
		next_node = MIME_UNMARK_NODE(ptr);
	} else {
		/* slot contains a mimetype, split into node */
		next_node = g_slice_new(liMimetypeNode);
		next_node->mimetype = ptr;
		next_node->cmax = next_node->cmin = 0;
		next_node->children = NULL;
		node->children[c - node->cmin] = MIME_MARK_NODE(next_node);
	}

	li_mimetype_insert(next_node, suffix, mimetype, depth+1);
}

LI_API GString *li_mimetype_get(liVRequest *vr, GString *filename) {
	/* search in mime_types option for the longest suffix match */
	GString *mimetype;
	liMimetypeNode *node;
	guchar *c, *s;
	gpointer ptr;

	if (!vr || !filename || !filename->len)
		return NULL;

	node = CORE_OPTIONPTR(LI_CORE_OPTION_MIME_TYPES).ptr;

	mimetype = node->mimetype;

	for (s = (guchar*) filename->str, c = (guchar*) filename->str + filename->len; c-- > s; ) {
		if (*c < node->cmin || *c > node->cmax)
			return mimetype;

		ptr = node->children[*c - node->cmin];
		if (NULL == ptr) {
			return mimetype;
		}

		if (!MIME_IS_NODE(ptr)) {
			return ptr;
		}

		node = MIME_UNMARK_NODE(ptr);

		if (NULL != node->mimetype) {
			mimetype = node->mimetype;
		}
	}

	return mimetype;
}
