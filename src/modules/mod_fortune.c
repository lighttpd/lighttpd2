/*
 * mod_fortune - fortune cookies for everyone
 *
 * Description:
 *     mod_fortune loads quotes (aka fortune coookies) from a file and provides actions
 *     to add a random quote as response header (X-fortune) or display it as a page
 *
 * Setups:
 *     fortune.load <filename> - loads cookies from a file, can be called multiple times to load data from multiple files
 * Options:
 *     none
 * Actions:
 *     fortune.header          - adds a random quote as response header X-fortune
 *     fortune.page            - returns a random quote as response content
 *
 * Example config:
 *     setup {
 *         fortune.load "/var/www/fortunes.txt";
 *     }
 *
 *     req.path == "/fortune" {
 *         fortune.page;
 *     } else {
 *         fortune.header;
 *     }
 */

#include <lighttpd/base.h>

/* globals */
struct fortune_data;
typedef struct fortune_data fortune_data;

struct fortune_data {
	GRand *rand;
	GArray *cookies;
};

static GString *fortune_rand(fortune_data *fd) {
	guint r = g_rand_int_range(fd->rand, 0, fd->cookies->len);
	return g_array_index(fd->cookies, GString*, r);
}

static handler_t fortune_header_handle(vrequest *vr, gpointer param) {
	fortune_data *fd = param;

	if (fd->cookies->len) {
		GString *cookie = fortune_rand(fd);
		http_header_insert(vr->response.headers, CONST_STR_LEN("X-fortune"), GSTR_LEN(cookie));
	}
	return HANDLER_GO_ON;
}

static action* fortune_header(server *srv, plugin* p, value *val) {
	UNUSED(srv); UNUSED(val);
	return action_new_function(fortune_header_handle, NULL, p->data);
}

static handler_t fortune_page_handle(vrequest *vr, gpointer param) {
	fortune_data *fd = param;

	if (!vrequest_handle_direct(vr))
		return HANDLER_GO_ON;

	vr->response.http_status = 200;

	if (fd->cookies->len) {
		GString *cookie = fortune_rand(fd);
		chunkqueue_append_mem(vr->out, GSTR_LEN(cookie));
	} else {
		chunkqueue_append_mem(vr->out, CONST_STR_LEN("no cookies in the cookie box"));
	}

	return HANDLER_GO_ON;
}

static action* fortune_page(server *srv, plugin* p, value *val) {
	UNUSED(srv); UNUSED(val);
	return action_new_function(fortune_page_handle, NULL, p->data);
}

static gboolean fortune_load(server *srv, plugin* p, value *val) {
	gchar *file;
	GError *err = NULL;
	gchar *data;
	gsize len;
	guint count = 0;
	fortune_data *fd = p->data;

	if (!val || val->type != VALUE_STRING) {
		ERROR(srv, "fortune.load takes a string as parameter, %s given", val ? value_type_string(val->type) : "none");
		return FALSE;
	}

	file = val->data.string->str;

	if (!g_file_get_contents(file, &data, &len, &err)) {
		ERROR(srv, "could not read fortune file \"%s\". reason: \"%s\" (%d)", file, err->message, err->code);
		g_error_free(err);
		return FALSE;
	}

	/* split lines */
	{
		GString *line = g_string_sized_new(128);
		gchar *cur;
		for (cur = data; *cur; cur++) {
			if (*cur == '\n' && line->len) {
				g_array_append_val(fd->cookies, line);
				line = g_string_sized_new(128);
				count++;
			}

			/* we ignore non-safe chars */
			if (*cur < ' ' || *cur > '~')
				continue;

			g_string_append_c(line, *cur);
		}

		g_string_free(line, TRUE);
	}

	g_free(data);

	TRACE(srv, "loaded %u cookies from file '%s'", count, file);

	return TRUE;
}



static const plugin_option options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "fortune.header", fortune_header },
	{ "fortune.page", fortune_page },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ "fortune.load", fortune_load },
	{ NULL, NULL }
};



static void plugin_fortune_free(server *srv, plugin *p) {
	UNUSED(srv);
	fortune_data *fd = p->data;

	/* free the cookies! */
	for (guint i = 0; i < fd->cookies->len; i++)
		g_string_free(g_array_index(fd->cookies, GString*, i), TRUE);
	g_array_free(fd->cookies, TRUE);

	g_rand_free(fd->rand);

	g_slice_free(fortune_data, fd);
}

static void plugin_fortune_init(server *srv, plugin *p) {
	UNUSED(srv);
	fortune_data *fd;

	p->options = options;
	p->actions = actions;
	p->setups = setups;
	p->free = plugin_fortune_free;

	p->data = fd = g_slice_new(fortune_data);

	fd->rand = g_rand_new();
	fd->cookies = g_array_new(FALSE, TRUE, sizeof(GString*));
}


LI_API gboolean mod_fortune_init(modules *mods, module *mod) {
	server *srv = mods->main;

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(srv, "mod_fortune", plugin_fortune_init);

	if (!mod->config)
		return FALSE;

	return TRUE;
}

LI_API gboolean mod_fortune_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}
