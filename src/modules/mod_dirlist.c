/*
 * mod_dirlist - directory listing
 *
 * Description:
 *     mod_dirlist enables you to list files inside a directory.
 *     The output can be customized in various ways from style via css to excluding certain entries.
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     dirlist [options] - show directory listing
 *         options: optional (not required), array, can contain any of the following string => value pairs:
 *             "sort" => criterium       - string, one of "name", "size" or "type"
 *             "css" => url              - string, external css to use for styling, default: use internal css
 *             "hide-dotfiles" => bool   - hide entries beginning with a dot, default: true
 *             "hide-tildefiles" => bool - hide entries ending with a tilde (~), often used for backups, default: true
 *             "include-header" => bool  - include HEADER.txt above the directory listing, default: false
 *             "hide-header" => bool     - hide HEADER.txt from the directory listing, default: false
 *             "include-readme" => bool  - include README.txt below the directory listing, default: false
 *             "hide-header" => bool     - hide README.txt from the directory listing, default: false
 *             "debug" => bool           - outout debug information to log, default: false
 *
 * Example config:
 *     dirlist ("include-header" => true, "hide-header" => true);
 *         - shows a directory listing including the content of HEADER.txt above the list and hiding itself from it
 *
 * Tip:
 *     xyz
 *
 * Todo:
 *     - filters for entries (pattern, regex)
 *     - include-* parameters
 *     - caching
 *     - javascript for sorting
 *     - sort parameter
 *     - parameter to chose if dirs should be seperated from other files (listed first)
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_dirlist_init(modules *mods, module *mod);
LI_API gboolean mod_dirlist_free(modules *mods, module *mod);

/* html snippet constants */
static const gchar html_header[] =
	"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
	"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
	"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
	"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
	"	<head>\n"
	"		<title>Index of %s</title>\n";
	
static const gchar html_table_start[] =
	"	</head>\n"
	"	<body>\n"
	"		<h2 id=\"title\">Index of %s</h2>\n"
	"		<div id=\"dirlist\">\n"
	"			<table summary=\"Directory Listing\" cellpadding=\"0\" cellspacing=\"0\">\n"
	"				<thead><tr><th id=\"name\">Name</th><th id=\"modified\">Last Modified</th><th id=\"size\">Size</th><th id=\"type\">Type</th></tr></thead>\n"
	"				<tbody>\n";

static const gchar html_table_row[] =
	"				<tr><td><a href=\"%s\">%s</a></td>"
	"<td class=\"modified\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
	"<td class=\"size\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
	"<td class=\"type\">%s</td></tr>\n";

static const gchar html_table_end[] =
	"				</tbody>\n"
	"			</table>\n"
	"		</div>\n";

static const gchar html_footer[] =
	"	<div id=\"footer\">%s</div>\n"
	"	</body>\n"
	"</html>";

static const gchar html_css[] =
	"<style type=\"text/css\">\n"
	"	body { background-color: #F5F5F5; }\n"
	"	h2#title { margin-bottom: 12px; }\n"
	"	a, a:active { text-decoration: none; color: blue; }\n"
	"	a:visited { color: #48468F; }\n"
	"	a:hover, a:focus { text-decoration: underline; color: red; }\n"
	"	table { margin-left: 12px; }\n"
	"	th, td { font: 90% monospace; text-align: left; }\n"
	"	th { font-weight: bold; padding-right: 14px; padding-bottom: 3px; }\n"
	"	td { padding-right: 14px; }\n"
	"	td.size, th#size { text-align: right; }\n"
	"	#dirlist { background-color: white; border-top: 1px solid #646464; border-bottom: 1px solid #646464; padding-top: 10px; padding-bottom: 14px; }\n"
	"	div#footer { font: 90% monospace; color: #787878; padding-top: 4px; }\n"
	"</style>\n";

struct dirlist_data {
	plugin *plugin;
	GString *css;
	gboolean hide_dotfiles;
	gboolean hide_tildefiles;
	gboolean debug;
};
typedef struct dirlist_data dirlist_data;

struct dirlist_plugin_data {
	GHashTable *cache;
	GMutex *mutex;
};
typedef struct dirlist_plugin_data dirlist_plugin_data;

static void dirlist_format_size(gchar *buf, goffset size) {
	const gchar unit[] = "BKMGTPE"; /* Kilo, Mega, Tera, Peta, Exa */
	gchar *u = (gchar*)unit;
	guint remaining = 0;

	while (size > 1024) {
		remaining = size & 1023; /* % 1024 */
		size >>= 10; /* /= 1024 */
		u++;
	}

	remaining /= 100;
	if (remaining > 9)
		remaining = 9;
	if (size > 999) {
		size = 0;
		remaining = 9;
		u++;
	}

	if (size > 99) {
		*buf++ = size / 100 + '0';
		size = size % 100;
		*buf++ = size / 10 + '0';
		*buf++ = (size % 10) + '0';
	} else if (size > 9) {
		*buf++ = size / 10 + '0';
		*buf++ = (size % 10) + '0';
	} else {
		*buf++ = size + '0';
	}

	buf[0] = '.';
	buf[1] = remaining + '0';
	buf[2] = *u;
	buf[3] = '\0';
}

static handler_t dirlist(vrequest *vr, gpointer param, gpointer *context) {
	GString *listing;
	stat_cache_entry *sce;
	dirlist_data *dd;
	dirlist_plugin_data *pd;

	UNUSED(context);

	if (!vr->stat_cache_entry) {
		if (vr->physical.path->len == 0) return HANDLER_GO_ON;

		if (!vrequest_handle_direct(vr)) return HANDLER_GO_ON;
	}

	dd = param;
	pd = dd->plugin->data;

	/* redirect to scheme + host + path + / + querystring if directory without trailing slash */
	/* TODO: local addr if HTTP 1.0 without host header, url encoding */
	if (vr->request.uri.path->str[vr->request.uri.path->len-1] != G_DIR_SEPARATOR) {
		GString *host = vr->request.uri.authority->len ? vr->request.uri.authority : vr->con->local_addr_str;
		GString *uri = g_string_sized_new(
			 8 /* https:// */ + host->len +
			vr->request.uri.orig_path->len + 2 /* /? */ + vr->request.uri.query->len
		);

		if (vr->con->is_ssl)
			g_string_append_len(uri, CONST_STR_LEN("https://"));
		else
			g_string_append_len(uri, CONST_STR_LEN("http://"));
		g_string_append_len(uri, GSTR_LEN(host));
		g_string_append_len(uri, GSTR_LEN(vr->request.uri.orig_path));
		g_string_append_c(uri, '/');
		if (vr->request.uri.query->len) {
			g_string_append_c(uri, '?');
			g_string_append_len(uri, GSTR_LEN(vr->request.uri.query));
		}

		vr->response.http_status = 301;
		http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(uri));
		g_string_free(uri, TRUE);
		return HANDLER_GO_ON;
	}

	sce = stat_cache_get_dir(vr, vr->physical.path);
	if (!sce)
		return HANDLER_WAIT_FOR_EVENT;

	if (sce->data.failed) {
		/* stat failed */
		VR_DEBUG(vr, "stat(\"%s\") failed: %s (%d)", sce->data.path->str, g_strerror(sce->data.err), sce->data.err);

		switch (errno) {
		case ENOENT:
			vr->response.http_status = 404; break;
		case EACCES:
		case EFAULT:
			vr->response.http_status = 403; break;
		default:
			vr->response.http_status = 500;
		}
	} else {
		/* everything ok, we have the directory listing */
		guint i;
		stat_cache_entry_data *sced;
		GString *mime_str;
		GArray *directories;
		GArray *files;
		GString *encoded;
		gchar sizebuf[sizeof("999.9K")+1];
		gchar datebuf[sizeof("2005-Jan-01 22:23:24")+1];
		struct tm tm;
		vr->response.http_status = 200;

		if (dd->debug)
			VR_DEBUG(vr, "dirlist for \"%s\", %u entries", sce->data.path->str, sce->dirlist->len);

		/* temporary string for encoded names */
		encoded = g_string_sized_new(64);

		http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));

		/* seperate directories from other files */
		directories = g_array_sized_new(FALSE, FALSE, sizeof(guint), 16);
		files = g_array_sized_new(FALSE, FALSE, sizeof(guint), sce->dirlist->len);
		for (i = 0; i < sce->dirlist->len; i++) {
			sced = &g_array_index(sce->dirlist, stat_cache_entry_data, i);

			/* ingore entries where the stat() failed */
			if (sced->failed)
				continue;

			if (dd->hide_dotfiles && sced->path->str[0] == '.')
				continue;

			if (dd->hide_tildefiles && sced->path->str[sced->path->len-1] == '~')
				continue;

			if (S_ISDIR(sced->st.st_mode))
				g_array_append_val(directories, i);
			else
				g_array_append_val(files, i);
				
		}

		listing = g_string_sized_new(4*1024);
		g_string_append_printf(listing, html_header, vr->request.uri.path->str);

		if (dd->css) {
			/* custom css */
			g_string_append_len(listing, CONST_STR_LEN("		<link rel=\"stylesheet\" type=\"text/css\" href=\""));
			g_string_append_len(listing, GSTR_LEN(dd->css));
			g_string_append_len(listing, CONST_STR_LEN("\" />\n"));
		} else {
			/* default css */
			g_string_append_len(listing, CONST_STR_LEN(html_css));
		}

		g_string_append_printf(listing, html_table_start, vr->request.uri.path->str);

		g_string_append_printf(listing, html_table_row, "../",
			"Parent Directory", (gint64)0, "", (gint64)0, "-", "Directory");

		/* list directories */
		for (i = 0; i < directories->len; i++) {
			sced = &g_array_index(sce->dirlist, stat_cache_entry_data, g_array_index(directories, guint, i));

			localtime_r(&(sced->st.st_mtime), &tm);
			datebuf[strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm)] = '\0';

			g_string_append_len(listing, CONST_STR_LEN("				<tr><td><a href=\""));
			string_encode(sced->path->str, encoded, ENCODING_URI);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_len(listing, CONST_STR_LEN("/\">"));
			string_encode(sced->path->str, encoded, ENCODING_HTML);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_printf(listing,
				"</a></td>"
				"<td class=\"modified\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
				"<td class=\"size\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
				"<td class=\"type\">%s</td></tr>\n",
				(gint64)sced->st.st_mtime, datebuf,
				(gint64)0, "-",
				"Directory"
			);

			/*
			g_string_append_printf(listing, html_table_row,
				vr->request.uri.path->str, sced->path->str, sced->path->str,
				(gint64)sced->st.st_mtime, datebuf,
				(gint64)0, "-",
				"Directory");
			*/
		}

		/*g_string_append_len(listing, CONST_STR_LEN("<tr><td colspan=\"4\">&nbsp;</td></tr>\n"));*/

		/* list files */
		for (i = 0; i < files->len; i++) {
			sced = &g_array_index(sce->dirlist, stat_cache_entry_data, g_array_index(files, guint, i));
			mime_str = mimetype_get(vr, sced->path);

			localtime_r(&(sced->st.st_mtime), &tm);
			datebuf[strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm)] = '\0';

			dirlist_format_size(sizebuf, sced->st.st_size);


			g_string_append_len(listing, CONST_STR_LEN("				<tr><td><a href=\""));
			string_encode(sced->path->str, encoded, ENCODING_URI);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_len(listing, CONST_STR_LEN("\">"));
			string_encode(sced->path->str, encoded, ENCODING_HTML);
			g_string_append_len(listing, GSTR_LEN(encoded));
			g_string_append_printf(listing,
				"</a></td>"
				"<td class=\"modified\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
				"<td class=\"size\" val=\"%"G_GINT64_FORMAT"\">%s</td>"
				"<td class=\"type\">%s</td></tr>\n",
				(gint64)sced->st.st_mtime, datebuf,
				(gint64)0, sizebuf,
				mime_str ? mime_str->str : "application/octet-stream"
			);

			/*
			g_string_append_printf(listing, html_table_row,
				sced->path->str, sced->path->str,
				(gint64)sced->st.st_mtime, datebuf,
				sced->st.st_size, sizebuf,
				mime_str ? mime_str->str : "application/octet-stream");
			*/
		}

		g_string_append_len(listing, CONST_STR_LEN(html_table_end));

		g_string_append_printf(listing, html_footer, CORE_OPTION(CORE_OPTION_SERVER_TAG).string->str);

		chunkqueue_append_string(vr->out, listing);
		g_string_free(encoded, TRUE);
		g_array_free(directories, TRUE);
		g_array_free(files, TRUE);
	}

	stat_cache_entry_release(vr);

	return HANDLER_GO_ON;
}

static void dirlist_free(server *srv, gpointer param) {
	dirlist_data *data = param;

	UNUSED(srv);

	if (data->css)
		g_string_free(data->css, TRUE);

	g_slice_free(dirlist_data, data);
}

static action* dirlist_create(server *srv, plugin* p, value *val) {
	dirlist_data *data;
	guint i;
	value *v, *tmpval;
	GString *k;

	if (val && val->type != VALUE_LIST) {
		ERROR(srv, "%s", "dirlist expects an optional list of string-value pairs");
		return NULL;
	}

	data = g_slice_new0(dirlist_data);
	data->plugin = p;
	data->hide_dotfiles = TRUE;
	data->hide_tildefiles = TRUE;

	if (val) {
		for (i = 0; i < val->data.list->len; i++) {
			tmpval = g_array_index(val->data.list, value*, i);
			if (tmpval->type != VALUE_LIST || tmpval->data.list->len != 2 ||
				g_array_index(tmpval->data.list, value*, 0)->type != VALUE_STRING) {
				ERROR(srv, "%s", "dirlist expects an optional list of string-value pairs");
				dirlist_free(srv, data);
				return NULL;
			}

			k = g_array_index(tmpval->data.list, value*, 0)->data.string;
			v = g_array_index(tmpval->data.list, value*, 1);

			if (g_str_equal(k->str, "css")) {
				if (v->type != VALUE_STRING) {
					ERROR(srv, "%s", "dirlisting: css parameter must be a string");
					dirlist_free(srv, data);
					return NULL;
				}
				data->css = g_string_new_len(GSTR_LEN(v->data.string));
			} else if (g_str_equal(k->str, "hide-dotfiles")) {
				if (v->type != VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlisting: hide-dotfiles parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_dotfiles = v->data.boolean;
			} else if (g_str_equal(k->str, "hide-tildefiles")) {
				if (v->type != VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlisting: hide-tildefiles parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->hide_tildefiles = v->data.boolean;
			} else if (g_str_equal(k->str, "debug")) {
				if (v->type != VALUE_BOOLEAN) {
					ERROR(srv, "%s", "dirlisting: debug parameter must be a boolean (true or false)");
					dirlist_free(srv, data);
					return NULL;
				}
				data->debug = v->data.boolean;
			} else {
				ERROR(srv, "dirlisting: unknown parameter \"%s\"", k->str);
				dirlist_free(srv, data);
				return NULL;
			}
		}
	}

	return action_new_function(dirlist, NULL, dirlist_free, data);
}

static const plugin_option options[] = {
	{ "dirlist.debug", VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "dirlist", dirlist_create },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_dirlist_free(server *srv, plugin *p) {
	dirlist_plugin_data *pd;

	UNUSED(srv);

	pd = p->data;
	g_hash_table_destroy(pd->cache);
	g_mutex_free(pd->mutex);
	g_slice_free(dirlist_plugin_data, pd);
}


static void plugin_dirlist_init(server *srv, plugin *p) {
	dirlist_plugin_data *pd;

	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
	p->free = plugin_dirlist_free;

	pd = g_slice_new0(dirlist_plugin_data);
	pd->cache = g_hash_table_new_full((GHashFunc)g_string_hash, (GEqualFunc)g_string_equal, string_destroy_notify, string_destroy_notify);
	pd->mutex = g_mutex_new();
	p->data = pd;
}


gboolean mod_dirlist_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_dirlist", plugin_dirlist_init);

	return mod->config != NULL;
}

gboolean mod_dirlist_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}