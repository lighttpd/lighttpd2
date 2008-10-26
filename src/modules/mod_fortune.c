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

#include "base.h"

/* globals */
static plugin *fortune_plugin = NULL;
static GRand *grand;
static GArray *cookies;

static GString *fortune_rand() {
	guint r = g_rand_int_range(grand, 0, cookies->len);
	return g_array_index(cookies, GString*, r);
}

static handler_t fortune_header_handle(vrequest *vr, gpointer param) {
	UNUSED(param);
	if (cookies->len) {
		GString *cookie = fortune_rand();
		http_header_insert(vr->response.headers, CONST_STR_LEN("X-fortune"), GSTR_LEN(cookie));
	}
	return HANDLER_GO_ON;
}

static action* fortune_header(server *srv, plugin* p, value *val) {
	UNUSED(srv); UNUSED(p); UNUSED(val);
	return action_new_function(fortune_header_handle, NULL, NULL);
}

static handler_t fortune_page_handle(vrequest *vr, gpointer param) {
	UNUSED(param);

	if (!vrequest_handle_direct(vr))
		return HANDLER_GO_ON;

	vr->response.http_status = 200;

	if (cookies->len) {
		GString *cookie = fortune_rand();
		chunkqueue_append_mem(vr->out, GSTR_LEN(cookie));
	} else {
		chunkqueue_append_mem(vr->out, CONST_STR_LEN("no cookies in the cookie box"));
	}

	return HANDLER_GO_ON;
}

static action* fortune_page(server *srv, plugin* p, value *val) {
	UNUSED(srv); UNUSED(p); UNUSED(val);
	return action_new_function(fortune_page_handle, NULL, NULL);
}

static gboolean fortune_load(server *srv, plugin* p, value *val) {
	gchar *file;
	GError *err = NULL;
	gchar *data;
	gsize len;
	guint count = 0;
	UNUSED(p);

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
				g_array_append_val(cookies, line);
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


static void plugin_fortune_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


LI_API gboolean mod_fortune_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	server *srv = mods->main;

	grand = g_rand_new();
	cookies = g_array_new(FALSE, TRUE, sizeof(GString*));

	fortune_plugin = plugin_register(srv, "mod_fortune", plugin_fortune_init);

	return fortune_plugin != NULL;
}

LI_API gboolean mod_fortune_free(modules *mods, module *mod) {
	UNUSED(mods); UNUSED(mod);

	/* free the cookies! */
	for (guint i = 0; i < cookies->len; i++)
		g_string_free(g_array_index(cookies, GString*, i), TRUE);
	g_array_free(cookies, TRUE);

	g_rand_free(grand);

	if (fortune_plugin)
		plugin_free(mods->main, fortune_plugin);

	return TRUE;
}
