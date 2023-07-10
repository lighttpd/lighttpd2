/*
 * mod_dirlist - directory listing
 *
 * Todo:
 *     - make output generating "async", give up control every N entries
 *     - filters for entries (pattern, regex)
 *     - include-* parameters
 *     - javascript for sorting
 *     - sort parameter
 *     - parameter to chose if dirs should be separated from other files (listed first)
 *     - etag/last-modified handling when including HEADER.txt/README.txt
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/encoding.h>

#include <sys/stat.h>
#include <fcntl.h>

/* maximum filesize for HEADER.txt and README.txt to have them included */
#define MAX_INCLUDE_FILE_SIZE (64*1024)

LI_API gboolean mod_dirlist_init(liModules *mods, liModule *mod);
LI_API gboolean mod_dirlist_free(liModules *mods, liModule *mod);

/* html snippet constants */
static const gchar html_header_start[] =
	"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\n"
	"         \"http://www.w3.org/TR/html4/loose.dtd\">\n"
	"<html>\n"
	"	<head>\n"
	"		<title>Index of %s</title>\n";
	
static const gchar html_header_end[] =
	"	</head>\n"
	"	<body>\n";

static const gchar html_table_start[] =
	"		<h2 id=\"title\">Index of %s</h2>\n"
	"		<div id=\"dirlist\">\n"
	"			<table summary=\"Directory Listing\" cellpadding=\"0\" cellspacing=\"0\" class=\"sort\">\n"
	"				<thead><tr group=\"-1\"><th id=\"name\">Name:</th><th id=\"modified\" class=\"int\">Last Modified:</th><th id=\"size\" class=\"int\">Size:</th><th id=\"type\">Type:</th></tr></thead>\n"
	"				<tbody>\n";

static const gchar html_table_row[] =
	"				<tr group=\"%c\"><td><a href=\"%s\">%s</a></td>"
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
	"	a.sortheader { color: black; text-decoration: none; display: block; }\n"
	"	span.sortarrow { text-decoration: none; }\n"
	"</style>\n";

static const gchar javascript_sort[] =
	"<script type=\"text/javascript\">\n"
	"// <!--\n"
	"var sort_column;\n"
	"var prev_span = null;\n"
	"var sort_numeric, sort_reverse;\n"

	"function get_inner_text(el) {\n"
	" if(el.getAttribute(\"val\")) return el.getAttribute(\"val\");\n"
	" if((typeof el == 'string')||(typeof el == 'undefined'))\n"
	"  return el;\n"
	" if(el.innerText)\n"
	"  return el.innerText;\n"
	" else {\n"
	"  var str = \"\";\n"
	"  var cs = el.childNodes;\n"
	"  var l = cs.length;\n"
	"  for (var i=0;i<l;i++) {\n"
	"   if (cs[i].nodeType==1) str += get_inner_text(cs[i]);\n"
	"   else if (cs[i].nodeType==3) str += cs[i].nodeValue;\n"
	"  }\n"
	" }\n"
	" return str;\n"
	"}\n"

	"function sortfn(a,b) {\n"
	" var ag = parseInt(a.getAttribute('group'));\n"
	" var bg = parseInt(b.getAttribute('group'));\n"
	" if (ag != bg) return (ag - bg);\n"
	" var at = get_inner_text(a.cells[sort_column]);\n"
	" var bt = get_inner_text(b.cells[sort_column]);\n"
	" if (sort_numeric) {\n"
	"  return sort_reverse*(parseInt(at)-parseInt(bt));\n"
	" } else {\n"
	"  var aa = at.toLowerCase();\n"
	"  var bb = bt.toLowerCase();\n"
	"  if (aa==bb) return 0;\n"
	"  else if (aa<bb) return -1*sort_reverse;\n"
	"  else return sort_reverse;\n"
	" }\n"
	"}\n"

	"function resort(lnk) {\n"
	" var span = lnk.childNodes[1];\n"
	" var table = lnk.parentNode.parentNode.parentNode.parentNode;\n"
	" var rows = new Array();\n"
	" for (var j=1;j<table.rows.length;j++)\n"
	"  rows[j-1] = table.rows[j];\n"
	" sort_column = lnk.parentNode.cellIndex;\n"
	" sort_numeric = (lnk.parentNode.className == 'int');\n"
	" sort_reverse = (span.getAttribute('sortdir')=='down') ? -1 : 1;\n"
	" rows.sort(sortfn);\n"

	" if (prev_span != null) prev_span.innerHTML = ':';\n"
	" if (span.getAttribute('sortdir')=='down') {\n"
	"  span.innerHTML = '&uarr;';\n"
	"  span.setAttribute('sortdir','up');\n"
	" } else {\n"
	"  span.innerHTML = '&darr;';\n"
	"  span.setAttribute('sortdir','down');\n"
	" }\n"
	" for (var i=0;i<rows.length;i++)\n"
	"  table.tBodies[0].appendChild(rows[i]);\n"
	" prev_span = span;\n"
	"}\n"

	"function init_sort() {\n"
	" var tables = document.getElementsByTagName(\"table\");\n"
	" for (var i = 0; i < tables.length; i++) {\n"
	"  var table = tables[i];\n"
	"  var c = table.getAttribute(\"class\")\n"
	"  if (-1 != c.split(\" \").indexOf(\"sort\")) {\n"
	"   var row = table.rows[0].cells;\n"
	"   for (var j = 0; j < row.length; j++) {\n"
	"    var n = row[j];\n"
	"    if (n.childNodes.length == 1 && n.childNodes[0].nodeType == 3) {\n"
	"     var link = document.createElement(\"a\");\n"
	"     var title = n.childNodes[0].nodeValue.replace(/:$/, \"\");\n"
	"     link.appendChild(document.createTextNode(title));\n"
	"     link.setAttribute(\"href\", \"#\");\n"
	"     link.setAttribute(\"class\", \"sortheader\");\n"
    "     link.setAttribute(\"onclick\", \"resort(this);return false;\");\n"
	"     var arrow = document.createElement(\"span\");\n"
	"     arrow.setAttribute(\"class\", \"sortarrow\");\n"
	"     arrow.appendChild(document.createTextNode(\":\"));\n"
	"     link.appendChild(arrow)\n"
	"     n.replaceChild(link, n.firstChild);\n"
	"    }\n"
	"   }\n"
	"   resort(row[0].firstChild);\n"
	"  }\n"
    " }\n"
	"}\n"

	"init_sort();\n"
	"// -->\n"
	"</script>\n";


struct dirlist_data {
	liPlugin *plugin;
	GString *css;
	gboolean hide_dotfiles;
	gboolean hide_tildefiles;
	gboolean include_header, hide_header, encode_header;
	gboolean include_readme, hide_readme, encode_readme;
	gboolean include_sort;
	gboolean hide_directories;
	gboolean debug;
	GPtrArray *exclude_suffix;
	GPtrArray *exclude_prefix;
	GString *content_type;
};
typedef struct dirlist_data dirlist_data;

struct dirlist_plugin_data {
	void *unused;
};
typedef struct dirlist_plugin_data dirlist_plugin_data;

/** uses/modifies wrk->tmp_str */
static void try_append_file(liVRequest *vr, GString **curbuf, const gchar *filename, gboolean encode_html) {
	GString *f = vr->wrk->tmp_str;
	g_string_truncate(f, 0);
	li_g_string_append_len(f, GSTR_LEN(vr->physical.path));
	li_path_append_slash(f);
	g_string_append(f, filename);

	if (!encode_html) {
		int fd;
		struct stat st;

		while (-1 == (fd = open(f->str, O_RDONLY))) {
			if (errno == EINTR)
				continue;
			return; /* failed to open, ignore */
		}
		if (-1 == fstat(fd, &st)) {
			close(fd);
			return; /* failed to open, ignore */
		}
		if (st.st_size > MAX_INCLUDE_FILE_SIZE) {
			close(fd);
			return; /* file too big, ignore */
		}

		/* flush current buffer and append file */
		li_chunkqueue_append_string(vr->direct_out, *curbuf);
		*curbuf = g_string_sized_new(4*1024-1);
		li_chunkqueue_append_file_fd(vr->direct_out, NULL, 0, st.st_size, fd);
	} else {
		GError *error = NULL;
		gchar *contents;
		gsize length;

		if (!g_file_get_contents(f->str, &contents, &length, &error)) {
			g_error_free(error);
			return; /* ignore errors */
		}
		if (length > MAX_INCLUDE_FILE_SIZE) {
			g_free(contents);
			return; /* file too big, ignore */
		}

		li_g_string_append_len(*curbuf, CONST_STR_LEN("<pre>"));
		li_string_encode_append(contents, *curbuf, LI_ENCODING_HTML);
		li_g_string_append_len(*curbuf, CONST_STR_LEN("</pre>"));
		g_free(contents);
	}
}

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

	if (u != unit) {
		*buf++ = '.';
		*buf++ = remaining + '0';
	}
	*buf++ = *u;
	*buf = '\0';
}

static liHandlerResult dirlist(liVRequest *vr, gpointer param, gpointer *context) {
	GString *listing;
	liStatCacheEntry *sce;
	dirlist_data *dd;

	UNUSED(context);

	switch (vr->request.http_method) {
	case LI_HTTP_METHOD_GET:
	case LI_HTTP_METHOD_HEAD:
		break;
	default:
		return LI_HANDLER_GO_ON;
	}

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	if (vr->physical.path->len == 0) return LI_HANDLER_GO_ON;

	switch (li_stat_cache_get_dirlist(vr, vr->physical.path, &sce)) {
	case LI_HANDLER_GO_ON: break;
	case LI_HANDLER_WAIT_FOR_EVENT: return LI_HANDLER_WAIT_FOR_EVENT;
	default: return LI_HANDLER_ERROR;
	}

	dd = param;

	if (sce->data.failed) {
		/* stat failed */
		int e = sce->data.err;

		li_stat_cache_entry_release(vr, sce);

		switch (e) {
		case ENOENT:
		case ENOTDIR:
		case ENAMETOOLONG:
			return LI_HANDLER_GO_ON;
		case EACCES:
			vr->response.http_status = 403;
			return LI_HANDLER_GO_ON;
		default:
			VR_ERROR(vr, "stat('%s') failed: %s", sce->data.path->str, g_strerror(sce->data.err));
			if (!li_vrequest_handle_direct(vr)) return LI_HANDLER_ERROR;
			vr->response.http_status = 500;
			return LI_HANDLER_GO_ON;
		}
	} else if (!S_ISDIR(sce->data.st.st_mode)) {
		li_stat_cache_entry_release(vr, sce);
		return LI_HANDLER_GO_ON;
	} else if (vr->request.uri.path->len == 0 || vr->request.uri.path->str[vr->request.uri.path->len-1] != '/') {
		li_stat_cache_entry_release(vr, sce);
		li_vrequest_redirect_directory(vr);
		return LI_HANDLER_GO_ON;
	} else {
		/* everything ok, we have the directory listing */
		gboolean cachable;
		guint i, j;
		liStatCacheEntryData *sced;
		GString *mime_str, *encoded;
		GArray *directories, *files;
		gchar sizebuf[sizeof("999.9K")+1];
		gchar datebuf[sizeof("2005-Jan-01 22:23:24")+1];
		guint datebuflen;
		struct tm tm;
		gboolean hide;

		if (!li_vrequest_handle_direct(vr)) {
			li_stat_cache_entry_release(vr, sce);
			return LI_HANDLER_ERROR;
		}
		vr->response.http_status = 200;

		if (dd->debug)
			VR_DEBUG(vr, "dirlist for \"%s\", %u entries", sce->data.path->str, sce->dirlist->len);

		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(dd->content_type));
		li_etag_set_header(vr, &sce->data.st, &cachable);
		if (cachable) {
			vr->response.http_status = 304;
			li_stat_cache_entry_release(vr, sce);
			return LI_HANDLER_GO_ON;
		}

		/* temporary string for encoded names */
		encoded = g_string_sized_new(64-1);

		/* separate directories from other files */
		directories = g_array_sized_new(FALSE, FALSE, sizeof(guint), 16);
		files = g_array_sized_new(FALSE, FALSE, sizeof(guint), sce->dirlist->len);
		for (i = 0; i < sce->dirlist->len; i++) {
			sced = &g_array_index(sce->dirlist, liStatCacheEntryData, i);
			hide = FALSE;

			/* ignore entries where the stat() failed */
			if (sced->failed)
				continue;

			if (dd->hide_dotfiles && sced->path->str[0] == '.')
				continue;

			if (dd->hide_tildefiles && sced->path->str[sced->path->len-1] == '~')
				continue;

			for (j = 0; j < dd->exclude_suffix->len; j++) {
				if (li_string_suffix(sced->path, GSTR_LEN((GString*)g_ptr_array_index(dd->exclude_suffix, j)))) {
					hide = TRUE;
					break;
				}
			}

			if (hide)
				continue;

			for (j = 0; j < dd->exclude_prefix->len; j++) {
				if (li_string_prefix(sced->path, GSTR_LEN((GString*)g_ptr_array_index(dd->exclude_prefix, j)))) {
					hide = TRUE;
					break;
				}
			}

			if (hide)
				continue;

			if (S_ISDIR(sced->st.st_mode)) {
				if (dd->hide_directories) continue;
				g_array_append_val(directories, i);
			} else {
				if ((dd->include_header || dd->hide_header) && g_str_equal(sced->path, "HEADER.txt")) {
					if (dd->hide_header) continue;
				} else if ((dd->include_readme || dd->hide_readme) && g_str_equal(sced->path, "README.txt")) {
					if (dd->hide_readme) continue;
				}
				g_array_append_val(files, i);
			}
		}

		listing = g_string_sized_new(4*1024-1);
		g_string_append_printf(listing, html_header_start, vr->request.uri.path->str);

		if (dd->css) {
			/* custom css */
			li_g_string_append_len(listing, CONST_STR_LEN("		<link rel=\"stylesheet\" type=\"text/css\" href=\""));
			li_g_string_append_len(listing, GSTR_LEN(dd->css));
			li_g_string_append_len(listing, CONST_STR_LEN("\" />\n"));
		} else {
			/* default css */
			li_g_string_append_len(listing, CONST_STR_LEN(html_css));
		}
		li_g_string_append_len(listing, CONST_STR_LEN(html_header_end));

		try_append_file(vr, &listing, "HEADER.txt", dd->encode_header);

		g_string_append_printf(listing, html_table_start, vr->request.uri.path->str);

		if (0 != strcmp("/", vr->request.uri.path->str)) {
			g_string_append_printf(listing, html_table_row, '0', "../",
				"Parent Directory", (gint64)0, "", (gint64)0, "-", "Directory");
		}

		/* list directories */
		if (!dd->hide_directories) {
			for (i = 0; i < directories->len; i++) {
				sced = &g_array_index(sce->dirlist, liStatCacheEntryData, g_array_index(directories, guint, i));

				localtime_r(&(sced->st.st_mtime), &tm);
				datebuflen = strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm);
				datebuf[datebuflen] = '\0';

				li_g_string_append_len(listing, CONST_STR_LEN("				<tr group=\"1\"><td><a href=\""));
				li_string_encode(sced->path->str, encoded, LI_ENCODING_URI);
				li_g_string_append_len(listing, GSTR_LEN(encoded));
				li_g_string_append_len(listing, CONST_STR_LEN("/\">"));
				li_string_encode(sced->path->str, encoded, LI_ENCODING_HTML);
				li_g_string_append_len(listing, GSTR_LEN(encoded));
				li_g_string_append_len(listing, CONST_STR_LEN("</a></td><td class=\"modified\" val=\""));
				li_string_append_int(listing, sced->st.st_mtime);
				li_g_string_append_len(listing, CONST_STR_LEN("\">"));
				li_g_string_append_len(listing, datebuf, datebuflen);
				li_g_string_append_len(listing, CONST_STR_LEN("</td>"
					"<td class=\"size\" val=\"0\">-</td>"
					"<td class=\"type\">Directory</td></tr>\n"));
			}
		}

		/*li_g_string_append_len(listing, CONST_STR_LEN("<tr><td colspan=\"4\">&nbsp;</td></tr>\n"));*/

		/* list files */
		for (i = 0; i < files->len; i++) {
			sced = &g_array_index(sce->dirlist, liStatCacheEntryData, g_array_index(files, guint, i));
			mime_str = li_mimetype_get(vr, sced->path);

			localtime_r(&(sced->st.st_mtime), &tm);
			datebuflen = strftime(datebuf, sizeof(datebuf), "%Y-%b-%d %H:%M:%S", &tm);
			datebuf[datebuflen] = '\0';

			dirlist_format_size(sizebuf, sced->st.st_size);

			li_g_string_append_len(listing, CONST_STR_LEN("				<tr group=\"2\"><td><a href=\""));
			li_string_encode(sced->path->str, encoded, LI_ENCODING_URI);
			li_g_string_append_len(listing, GSTR_LEN(encoded));
			li_g_string_append_len(listing, CONST_STR_LEN("\">"));
			li_string_encode(sced->path->str, encoded, LI_ENCODING_HTML);
			li_g_string_append_len(listing, GSTR_LEN(encoded));
			li_g_string_append_len(listing, CONST_STR_LEN(
				"</a></td>"
				"<td class=\"modified\" val=\""));
			li_string_append_int(listing, sced->st.st_mtime);
			li_g_string_append_len(listing, CONST_STR_LEN("\">"));
			li_g_string_append_len(listing, datebuf, datebuflen);
			li_g_string_append_len(listing, CONST_STR_LEN("</td><td class=\"size\" val=\""));
			li_string_append_int(listing, sced->st.st_size);
			li_g_string_append_len(listing, CONST_STR_LEN("\">"));
			g_string_append(listing, sizebuf);
			li_g_string_append_len(listing, CONST_STR_LEN("</td><td class=\"type\">"));
			if (mime_str) {
				li_g_string_append_len(listing, GSTR_LEN(mime_str));
			} else {
				li_g_string_append_len(listing, CONST_STR_LEN("application/octet-stream"));
			}
			li_g_string_append_len(listing, CONST_STR_LEN("</td></tr>\n"));

			/*
			g_string_append_printf(listing, html_table_row,
				sced->path->str, sced->path->str,
				(gint64)sced->st.st_mtime, datebuf,
				sced->st.st_size, sizebuf,
				mime_str ? mime_str->str : "application/octet-stream");
			*/
		}

		li_g_string_append_len(listing, CONST_STR_LEN(html_table_end));

		try_append_file(vr, &listing, "README.txt", dd->encode_readme);

		if (dd->include_sort) {
			li_g_string_append_len(listing, CONST_STR_LEN(javascript_sort));
		}

		g_string_append_printf(listing, html_footer, CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_TAG).string->str);

		li_chunkqueue_append_string(vr->direct_out, listing);
		g_string_free(encoded, TRUE);
		g_array_free(directories, TRUE);
		g_array_free(files, TRUE);
	}

	li_stat_cache_entry_release(vr, sce);

	return LI_HANDLER_GO_ON;
}

static void dirlist_free(liServer *srv, gpointer param) {
	guint i;
	dirlist_data *data = param;

	UNUSED(srv);

	if (NULL != data->css) {
		g_string_free(data->css, TRUE);
	}

	for (i = 0; i < data->exclude_suffix->len; i++) {
		g_string_free(g_ptr_array_index(data->exclude_suffix, i), TRUE);
	}
	g_ptr_array_free(data->exclude_suffix, TRUE);

	for (i = 0; i < data->exclude_prefix->len; i++) {
		g_string_free(g_ptr_array_index(data->exclude_prefix, i), TRUE);
	}
	g_ptr_array_free(data->exclude_prefix, TRUE);

	g_string_free(data->content_type, TRUE);

	g_slice_free(dirlist_data, data);
}

static liAction* dirlist_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	dirlist_data *data;
	UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL != val && NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "dirlist expects an optional list of string-value pairs");
		return NULL;
	}

	data = g_slice_new0(dirlist_data);
	data->plugin = p;
	data->hide_dotfiles = TRUE;
	data->hide_tildefiles = TRUE;
	data->include_readme = TRUE;
	data->encode_header = TRUE;
	data->encode_readme = TRUE;
	data->include_sort = TRUE;
	data->exclude_suffix = g_ptr_array_new();
	data->exclude_prefix = g_ptr_array_new();
	data->content_type = g_string_new("text/html; charset=utf-8");

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) {
			ERROR(srv, "%s", "dirlist doesn't take default keys");
			dirlist_free(srv, data);
			return NULL;
		}
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		/* TODO: check for duplicate keys? */
		if (g_str_equal(entryKeyStr->str, "sort")) { /* "name", "size" or "type" */
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: sort parameter must be a string");
				dirlist_free(srv, data);
				return NULL;
			}
			/* TODO */
			WARNING(srv, "%s", "dirlist: sort parameter not supported yet!");
		} else if (g_str_equal(entryKeyStr->str, "css")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: css parameter must be a string");
				dirlist_free(srv, data);
				return NULL;
			}
			if (NULL != data->css) g_string_free(data->css, TRUE);
			data->css = li_value_extract_string(entryValue);
		} else if (g_str_equal(entryKeyStr->str, "hide-dotfiles")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: hide-dotfiles parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->hide_dotfiles = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "hide-tildefiles")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: hide-tildefiles parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->hide_tildefiles = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "hide-directories")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: hide-directories parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->hide_directories = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "include-header")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: include-header parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->include_header = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "hide-header")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: hide-header parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->hide_header = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "encode-header")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: encode-header parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->encode_header = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "include-readme")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: include-readme parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->include_readme = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "hide-readme")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: hide-readme parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->hide_readme = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "encode-readme")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: encode-readme parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->encode_readme = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "exclude-suffix")) {
			if (LI_VALUE_LIST != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: exclude-suffix parameter must be a list of strings");
				dirlist_free(srv, data);
				return NULL;
			}
			LI_VALUE_FOREACH(suffix, entryValue)
				if (LI_VALUE_STRING != li_value_type(suffix)) {
					ERROR(srv, "%s", "dirlist: exclude-suffix parameter must be a list of strings");
					dirlist_free(srv, data);
					return NULL;
				} else {
					g_ptr_array_add(data->exclude_suffix, li_value_extract_string(suffix));
				}
			LI_VALUE_END_FOREACH()
		} else if (g_str_equal(entryKeyStr->str, "exclude-prefix")) {
			if (LI_VALUE_LIST != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: exclude-prefix parameter must be a list of strings");
				dirlist_free(srv, data);
				return NULL;
			}
			LI_VALUE_FOREACH(prefix, entryValue)
				if (LI_VALUE_STRING != li_value_type(prefix)) {
					ERROR(srv, "%s", "dirlist: exclude-prefix parameter must be a list of strings");
					dirlist_free(srv, data);
					return NULL;
				} else {
					g_ptr_array_add(data->exclude_prefix, li_value_extract_string(prefix));
				}
			LI_VALUE_END_FOREACH()
		} else if (g_str_equal(entryKeyStr->str, "debug")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: debug parameter must be a boolean (true or false)");
				dirlist_free(srv, data);
				return NULL;
			}
			data->debug = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "content-type")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "dirlist: content-type parameter must be a string");
				dirlist_free(srv, data);
				return NULL;
			}
			g_string_assign(data->content_type, entryValue->data.string->str);
		} else {
			ERROR(srv, "dirlist: unknown parameter \"%s\"", entryKeyStr->str);
			dirlist_free(srv, data);
			return NULL;
		}
	LI_VALUE_END_FOREACH()

	return li_action_new_function(dirlist, NULL, dirlist_free, data);
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "dirlist", dirlist_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_dirlist_free(liServer *srv, liPlugin *p) {
	dirlist_plugin_data *pd;

	UNUSED(srv);

	pd = p->data;
	g_slice_free(dirlist_plugin_data, pd);
}


static void plugin_dirlist_init(liServer *srv, liPlugin *p, gpointer userdata) {
	dirlist_plugin_data *pd;

	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
	p->free = plugin_dirlist_free;

	pd = g_slice_new0(dirlist_plugin_data);
	p->data = pd;
}


gboolean mod_dirlist_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_dirlist", plugin_dirlist_init, NULL);

	return mod->config != NULL;
}

gboolean mod_dirlist_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
