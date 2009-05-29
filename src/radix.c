#include <lighttpd/base.h>

RadixTree32 *radixtree32_new(guint32 root_width) {
	guint32 i;
	RadixTree32 *tree = g_slice_new(RadixTree32);

	if (root_width == 0)
		root_width = 1;
	else if (root_width > 8)
		root_width = 8;

	tree->root = g_new0(RadixNode32*, 1 << root_width);
	tree->size = 0;
	tree->root_width = root_width;
	tree->root_mask = 0;

	for (i = 0; i < root_width; i++)
		tree->root_mask = ~(~tree->root_mask >> 1);

	return tree;
}

guint radixtree32_free(RadixTree32 *tree) {
	guint32 i;
	RadixNode32 *node, *parent;
	guint32 n = 0;

	/* walk the tree and free every node */
	for (i = 0; i < ((guint32)1 << tree->root_width); i++) {
		node = tree->root[i];

		while (node) {
			if (node->left)
				node = node->left;
			else if (node->right)
				node = node->right;
			else {
				parent = node->parent;

				if (parent) {
					if (parent->left == node)
						parent->left = NULL;
					else
						parent->right = NULL;
				}

				g_slice_free(RadixNode32, node);
				node = parent;
				n++;
			}
		}
	}

	g_free(tree->root);
	g_slice_free(RadixTree32, tree);

	return n;
}

void radixtree32_insert(RadixTree32 *tree, guint32 key, guint32 mask, gpointer data) {
	RadixNode32 *last_node, *leaf;
	RadixNode32 *node = tree->root[(key & tree->root_mask) >> (32 - tree->root_width)];
	//g_print("root: %p, %x & %x => %x\n", (void*)node, key, tree->root_mask, (key & tree->root_mask) >> (32 - tree->root_width));
	if (!node) {
		/* no root node yet */
		node = g_slice_new(RadixNode32);
		node->key = key & mask;
		node->mask = mask;
		node->data = data;
		node->parent = NULL;
		node->left = NULL;
		node->right = NULL;
		tree->root[(key & tree->root_mask) >> (32 - tree->root_width)] = node;
		tree->size++;

		return;
	}

	do {//g_print("%x & %x => %x != %x\n", key, node->mask, key & node->mask, node->key);
		if ((key & mask & node->mask) != node->key) {guint i;
			/* node key differs, split tree */
			guint32 tmp;
			RadixNode32 *new_node;i=0;

			/* the new internal node */
			new_node = g_slice_new(RadixNode32);
			new_node->data = NULL;
			new_node->parent = node->parent;
			new_node->mask = node->mask;
			new_node->key = node->key;

			node->parent = new_node;

			/* the new leaf */
			leaf = g_slice_new(RadixNode32);
			leaf->key = key & mask;
			leaf->mask = mask;
			leaf->data = data;
			leaf->parent = new_node;
			leaf->left = NULL;
			leaf->right = NULL;

			do {//g_print("xxx #%u %x & %x => %x != %x\n", i++, key, new_node->mask, key&new_node->mask, node->key);
				tmp = new_node->mask;
				new_node->mask <<= 1;
				new_node->key &= new_node->mask;
			} while ((key & mask & new_node->mask) != new_node->key);
			//g_print("xxx %x & %x => %x != %x\n", key, new_node->mask, key&new_node->mask, node->key);

			//if (key & (~ (~ new_node->mask >> 1))) {
			if ((key & new_node->mask) > (key & (~ (~ new_node->mask >> 1)))) {
				new_node->left = node;
				new_node->right = leaf;
			} else {
				new_node->left = leaf;
				new_node->right = node;
			}

			if (new_node->parent) {
				if (new_node->parent->left == node)
					new_node->parent->left = new_node;
				else
					new_node->parent->right = new_node;
			} else {
				tree->root[(key & tree->root_mask) >> (32 - tree->root_width)] = new_node;
			}

			tree->size++;

			return;
		} else if ((key & mask) == node->key) {
			node->data = data;

			return;
		}

		last_node = node;

		/* compare next bit */
		//if (key & (~ (~ node->mask >> 1)))
		if ((key & node->mask) > (key & (~ (~ node->mask >> 1))))
			node = node->right;
		else
			node = node->left;
	} while (node);

	/* new leaf at end of tree */
	leaf = g_slice_new0(RadixNode32);
	leaf->data = data;
	leaf->key = key & mask;
	leaf->mask = mask;
	leaf->parent = last_node;

	//if (key & (~ (~ last_node->key >> 1)))
	if ((key & last_node->mask) > (key & (~ (~ last_node->mask >> 1))))
		last_node->right = leaf;
	else
		last_node->left = leaf;

	tree->size++;
}

gboolean radixtree32_remove(RadixTree32 *tree, guint32 key, guint32 mask) {
	RadixNode32 *node = tree->root[(key & tree->root_mask) >> (32 - tree->root_width)];

	while (node) {
		if (!node->data || (key & mask) != node->key) {
			/* compare next bit */
			//if (key & (~ (~ node->key >> 1)))
			if ((key & node->mask) > (key & (~ (~ node->mask >> 1))))
				node = node->right;
			else
				node = node->left;

			continue;
		}

		if (!node->left && !node->right) {
			/* leaf */
			if (node->parent) {
				if (node->parent->data) {
					/* set current node to parent */
					node->data = node->parent->data;
					node->key = node->parent->key;
					node->mask = node->parent->mask;
					node->parent->data = NULL;

					return TRUE;
				} else {
					/* the parent internal node has no data, we can set our sibling as the new internal node */
					RadixNode32 *sibling = (node->parent->left == node) ? node->parent->right : node->parent->left;
					if (node->parent->parent) {
						if (node->parent->parent->left == node->parent)
							node->parent->parent->left = sibling;
						else
							node->parent->parent->right = sibling;
					} else {
						/* the parent is the tree root, set root to our sibling */
						tree->root[(key & tree->root_mask) >> (32 - tree->root_width)] = sibling;
					}

					/* old internal node not needed anymore */
					tree->size--;
					g_slice_free(RadixNode32, node->parent);
				}
			} else {
				/* tree root */
				tree->root[(key & tree->root_mask) >> (32 - tree->root_width)] = NULL;
			}
		} else {
			/* internal node */
			node->data = NULL;

			return TRUE;
		}

		tree->size--;
		g_slice_free(RadixNode32, node);

		return TRUE;
	}

	return FALSE;
}

RadixNode32 *radixtree32_lookup_node(RadixTree32 *tree, guint32 key) {
	RadixNode32 *node = tree->root[(key & tree->root_mask) >> (32 - tree->root_width)];
	RadixNode32 *result = NULL;

	while (node) {//g_print("%x & %x => %x != %x\n", key, node->mask, key & node->mask, node->key);
		if ((key & node->mask) != node->key)
			return result;

		if (node->data)
			result = node;

		/* compare next bit */
		//if (key & (~ (~ node->key >> 1)))
		if ((key & node->mask) > (key & (~ (~ node->mask >> 1))))
			node = node->right;
		else
			node = node->left;
	}

	return result;
}

gpointer radixtree32_lookup(RadixTree32 *tree, guint32 key) {
	RadixNode32 *node = radixtree32_lookup_node(tree, key);

	return node ? node->data : NULL;
}

gpointer radixtree32_lookup_exact(RadixTree32 *tree, guint32 key) {
	RadixNode32 *node = radixtree32_lookup_node(tree, key);

	if (!node)
		return NULL;

	return (node->key == key) ? node->data : NULL;
}