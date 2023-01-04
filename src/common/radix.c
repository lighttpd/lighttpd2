
#include <lighttpd/radix.h>
#include <lighttpd/utils.h>

/* internal data is saved in "host"-order; search from high to low bit */
typedef guint32 rdxBase;
#define HTON_RDX(x) ((rdxBase) (htonl(x)))

#define RDXBITS (sizeof(rdxBase)*8)

/* 1^(width) 0^(RDXBITS-width): "1..10..0" */
#define RDX_MASK(width) ( width ? ~(  (((rdxBase)1) << (RDXBITS - width)) - 1  ) : 0 )

#define RDX_BIT(bit) (((rdxBase)1) << (RDXBITS - 1 - (bit)))

#define INPUT_SIZE(bits) ( bits ? (bits+RDXBITS-1) / RDXBITS : 1 )
#define INPUT_CHARS(bits) ( (bits+7) / 8 )

typedef struct liRadixNode liRadixNode;
struct liRadixNode{
	rdxBase key;
	guint32 width;
	gpointer data;
	liRadixNode *right; /* "1" bit */
	liRadixNode *left; /* "0" bit */
};

struct liRadixTree {
	liRadixNode *zero;
};

liRadixTree* li_radixtree_new(void) {
	liRadixTree *tree;

	tree = g_slice_new0(liRadixTree);

	return tree;
}

/* node != NULL */
static void li_radixtree_free_node(liRadixNode *node, GFunc free_func, gpointer free_userdata) {
	if (node->right) {
		li_radixtree_free_node(node->right, free_func, free_userdata);
	}
	if (node->left) {
		li_radixtree_free_node(node->left, free_func, free_userdata);
	}

	if (free_func && node->data) free_func(node->data, free_userdata);

	g_slice_free(liRadixNode, node);
}

void li_radixtree_free(liRadixTree *tree, GFunc free_func, gpointer free_userdata) {
	if (tree->zero) li_radixtree_free_node(tree->zero, free_func, free_userdata);

	g_slice_free(liRadixTree, tree);
}

static void rdx_get_input(rdxBase *dest, const void *key, guint32 bits) {
	guint32 entries = INPUT_SIZE(bits), chars = INPUT_CHARS(bits), padding = entries*sizeof(rdxBase) - chars, i;

	memcpy(dest, key, chars);
	memset(((char*)dest) + chars, 0, padding);

	for (i = 0; i < entries; i++) {
		dest[i] = HTON_RDX(dest[i]);
	}
}

gpointer li_radixtree_insert(liRadixTree *tree, const void *key, guint32 bits, gpointer data) {
	liRadixNode *node, **nodeloc;
	rdxBase input[INPUT_SIZE(bits)], current;
	guint32 pos = 0;
	rdx_get_input(input, key, bits);

	if (!data) return NULL;

	pos = 0;
	current = input[0];

	nodeloc = &tree->zero;

	while (NULL != (node = *nodeloc)) {
		rdxBase mask = RDX_MASK(node->width);

		if (node->width > bits || (current & mask) != node->key) { /* prefix longer than key or key doesn't match */
			/* split node */
			liRadixNode *newnode;
			guint32 width = (node->width > bits) ? bits : node->width;
			LI_FORCE_ASSERT(width <= RDXBITS);
			mask = RDX_MASK(width);
			while ((current & mask) != (node->key & mask)) {
				width--;
				mask <<= 1;
			}
			LI_FORCE_ASSERT(width <= RDXBITS-1);
			newnode = g_slice_new0(liRadixNode);
			newnode->width = width;
			newnode->key = current & mask;
			if (node->key & RDX_BIT(width)) { /* current may not have a "next" bit */
				newnode->right = node;
				*nodeloc = newnode;
				nodeloc = &newnode->left;
			} else {
				newnode->left = node;
				*nodeloc = newnode;
				nodeloc = &newnode->right;
			}
			if (width == bits) {
				newnode->data = data;
				return NULL;
			} else {
				/* NULL == *nodeloc */
				break;
			}
		}

		if (node->width == bits) { /* exact match */
			gpointer olddata = node->data;
			node->data = data;
			return olddata;
		}

		if (mask & 0x1) {
			/* next "layer" */
			current = input[++pos];
			bits -= RDXBITS;
			nodeloc = (current & RDX_BIT(0)) ? &node->right : &node->left;
		} else {
			nodeloc = (current & RDX_BIT(node->width)) ? &node->right : &node->left;
		}
	}

	while (bits > RDXBITS) {
		node = g_slice_new0(liRadixNode);
		node->width = RDXBITS;
		node->key = current;
		*nodeloc = node;

		/* next "layer" */
		current = input[++pos];
		bits -= RDXBITS;
		nodeloc = (current & RDX_BIT(0)) ? &node->right : &node->left;
	}

	node = g_slice_new0(liRadixNode);
	node->width = bits;
	node->key = current & RDX_MASK(bits);
	node->data = data;
	*nodeloc = node;

	return NULL;
}

/* *nodeptr == node is the only pointer to node from the parent! */
static void node_compact(liRadixNode **nodeptr, liRadixNode *node) {
	liRadixNode *child;

	/* can't remove nodes with data: */
	if (NULL != node->data) return;

	if (NULL == node->left && NULL == node->right) {
		/* delete node */
		*nodeptr = NULL;
		g_slice_free(liRadixNode, node);
		return;
	}
	/* else: at least one child */

	/* children in the same layer? */
	if (node->width != RDXBITS) {
		/* exactly one child? (we have at least one!) */
		if (NULL == node->left) {
			child = node->right;
		} else if (NULL == node->right) {
			child = node->left;
		} else {
			return; /* two children */
		}

		/* replace our own node with child */
		*nodeptr = child;
		g_slice_free(liRadixNode, node);
		return;
	}
}

/* *nodeptr == node is the only pointer to the node from the parent! */
static gpointer radixtree_remove(liRadixNode **nodeptr, rdxBase *input, guint32 bits) {
	liRadixNode **nextnode;
	gpointer data;
	rdxBase current, mask;
	liRadixNode *node = *nodeptr;

	if (!node) return NULL;

	current = *input;
	mask = RDX_MASK(node->width);

	if (node->width > bits) return NULL; /* prefix longer than key */

	if ((current & mask) != node->key) return NULL; /* doesn't match */

	if (node->width == bits) { /* exact match */
		data = node->data;
		node->data = NULL;
		node_compact(nodeptr, node);
		return data;
	}

	if (mask & 0x1) {
		/* next "layer" */
		input++;
		bits -= RDXBITS;
		nextnode = (current & RDX_BIT(0)) ? &node->right : &node->left;
	} else {
		nextnode = (current & RDX_BIT(node->width)) ? &node->right : &node->left;
	}

	data = radixtree_remove(nextnode, input, bits);

	if (data == NULL) return NULL; /* nothing deleted */

	node_compact(nodeptr, node);

	return data;
}

gpointer li_radixtree_remove(liRadixTree *tree, const void *key, guint32 bits) {
	rdxBase input[INPUT_SIZE(bits)];
	gpointer data;
	rdx_get_input(input, key, bits);

	data = radixtree_remove(&tree->zero, input, bits);

	return data;
}

gpointer li_radixtree_lookup(liRadixTree *tree, const void *key, guint32 bits) { /* longest matching prefix */
	liRadixNode *node;
	rdxBase input[INPUT_SIZE(bits)], current;
	guint32 pos = 0;
	gpointer data = NULL;

	rdx_get_input(input, key, bits);

	pos = 0;
	current = input[0];

	node = tree->zero;

	while (node) {
		rdxBase mask = RDX_MASK(node->width);

		if (node->width > bits) break; /* prefix longer than key */

		if ((current & mask) != node->key) break; /* doesn't match */

		if (node->data) data = node->data; /* longest matching prefix */

		if (node->width == bits) break; /* "end of key" */

		if (mask & 0x1) {
			/* next "layer" */
			current = input[++pos];
			bits -= RDXBITS;
			node = (current & RDX_BIT(0)) ? node->right : node->left;
		} else {
			node = (current & RDX_BIT(node->width)) ? node->right : node->left;
		}
	}

	return data;
}


gpointer li_radixtree_lookup_exact(liRadixTree *tree, const void *key, guint32 bits) {
	liRadixNode *node;
	rdxBase input[INPUT_SIZE(bits)], current;
	guint32 pos = 0;

	rdx_get_input(input, key, bits);

	pos = 0;
	current = input[0];

	node = tree->zero;

	while (node) {
		rdxBase mask = RDX_MASK(node->width);

		if (node->width > bits) break; /* prefix longer than key */

		if ((current & mask) != node->key) break; /* doesn't match */

		if (node->width == bits) return node->data; /* exact match */

		if (mask & 0x1) {
			/* next "layer" */
			current = input[++pos];
			bits -= RDXBITS;
			node = (current & RDX_BIT(0)) ? node->right : node->left;
		} else {
			node = (current & RDX_BIT(node->width)) ? node->right : node->left;
		}
	}

	return NULL;
}

/* node != NULL */
static void radixtree_foreach(liRadixNode *node, GFunc func, gpointer userdata) {
	if (node->data) {
		func(node->data, userdata);
	}
	if (node->right) radixtree_foreach(node->right, func, userdata);
	if (node->left) radixtree_foreach(node->left, func, userdata);
}

void li_radixtree_foreach(liRadixTree *tree, GFunc func, gpointer userdata) {
	if (tree->zero) radixtree_foreach(tree->zero, func, userdata);
}
