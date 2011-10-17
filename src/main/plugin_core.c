
#include <lighttpd/base.h>
#include <lighttpd/pattern.h>

#include <lighttpd/plugin_core.h>

#include <lighttpd/version.h>

#include <lighttpd/http_range_parser.h>

#include <sys/stat.h>
#include <fcntl.h>

static liAction* core_list(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liAction *a;
	guint i;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

	if (val->type == LI_VALUE_ACTION) {
		a = val->data.val_action.action;
		li_action_acquire(a);
		return a;
	}

	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "expected list, got %s", li_value_type_string(val->type));
		return NULL;
	}

	a = li_action_new_list();
	for (i = 0; i < val->data.list->len; i++) {
		liValue *oa = g_array_index(val->data.list, liValue*, i);
		if (oa->type != LI_VALUE_ACTION) {
			ERROR(srv, "expected action at entry %u of list, got %s", i, li_value_type_string(oa->type));
			li_action_release(srv, a);
			return NULL;
		}
		assert(srv == oa->data.val_action.srv);
		li_action_acquire(oa->data.val_action.action);
		g_array_append_val(a->data.list, oa->data.val_action.action);
	}
	return a;
}

static liAction* core_when(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liValue *val_cond, *val_act, *val_act_else;
	liAction *a, *act = NULL, *act_else = NULL;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "expected list, got %s", li_value_type_string(val->type));
		return NULL;
	}
	if (val->data.list->len == 2) {
		val_act_else = NULL;
		act_else = NULL;
	} else if (val->data.list->len == 3) {
		val_act_else = g_array_index(val->data.list, liValue*, 2);
	} else {
		ERROR(srv, "expected list with length 2 or 3, has length %u", val->data.list->len);
		return NULL;
	}
	val_cond = g_array_index(val->data.list, liValue*, 0);
	val_act = g_array_index(val->data.list, liValue*, 1);

	if (NULL == val_cond || val_cond->type != LI_VALUE_CONDITION) {
		ERROR(srv, "expected condition as first parameter, got %s", NULL == val_cond ? "NULL" : li_value_type_string(val_cond->type));
		return NULL;
	}
	if (NULL == val_act || val_act->type == LI_VALUE_NONE) {
		act = NULL;
	} else if (val_act->type == LI_VALUE_ACTION) {
		act = val_act->data.val_action.action;
	} else {
		ERROR(srv, "expected action as second parameter, got %s", li_value_type_string(val_act->type));
		return NULL;
	}
	if (NULL != val_act_else) {
		if (val_act_else->type == LI_VALUE_NONE) {
			act_else = NULL;
		} else if (val_act_else->type == LI_VALUE_ACTION) {
			act_else = val_act_else->data.val_action.action;
		} else {
			ERROR(srv, "expected action as third parameter, got %s", li_value_type_string(val_act_else->type));
			return NULL;
		}
	}
	li_condition_acquire(val_cond->data.val_cond.cond);
	if (NULL != act) li_action_acquire(act);
	if (NULL != act_else) li_action_acquire(act_else);
	a = li_action_new_condition(val_cond->data.val_cond.cond, act, act_else);
	return a;
}

static liAction* core_set(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liValue *val_val, *val_name;
	liAction *a;
	UNUSED(p); UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "expected list, got %s", li_value_type_string(val->type));
		return NULL;
	}
	if (val->data.list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", val->data.list->len);
		return NULL;
	}
	val_name = g_array_index(val->data.list, liValue*, 0);
	val_val = g_array_index(val->data.list, liValue*, 1);
	if (val_name->type != LI_VALUE_STRING) {
		ERROR(srv, "expected string as first parameter, got %s", li_value_type_string(val_name->type));
		return NULL;
	}
	a = li_option_action(srv, wrk, val_name->data.string->str, val_val);
	return a;
}

static gboolean core_setup_set(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	liValue *val_val, *val_name;
	UNUSED(p); UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return FALSE;
	}
	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "expected list, got %s", li_value_type_string(val->type));
		return FALSE;
	}
	if (val->data.list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", val->data.list->len);
		return FALSE;
	}
	val_name = g_array_index(val->data.list, liValue*, 0);
	val_val = g_array_index(val->data.list, liValue*, 1);
	if (val_name->type != LI_VALUE_STRING) {
		ERROR(srv, "expected string as first parameter, got %s", li_value_type_string(val_name->type));
		return FALSE;
	}
	return li_plugin_set_default_option(srv, val_name->data.string->str, val_val);
}

typedef struct docroot_split docroot_split;
struct docroot_split {
	GString *hostname;
	gchar** splits;
	guint split_len;
};

static void core_docroot_nth_cb(GString *pattern_result, guint to, guint from, gpointer data) {
	/* $n means n-th part of hostname from end divided by dots */
	/* range is interpreted reversed !!! */
	gboolean first = TRUE;
	guint i;
	docroot_split *ctx = data;

	if (0 == ctx->hostname->len) return;

	/* ranges including 0 will only get the complete hostname */
	if (0 == from || 0 == to) {
		g_string_append_len(pattern_result, GSTR_LEN(ctx->hostname));
		return;
	}

	if (NULL == ctx->splits) {
		ctx->splits = g_strsplit_set(ctx->hostname->str, ".", 31);
		ctx->split_len = g_strv_length(ctx->splits);
	}

	if (0 == ctx->split_len) return;

	from = MIN(from, ctx->split_len);
	to = MIN(to, ctx->split_len);

	if (from <= to) {
		for (i = from; i <= to; i++) {
			if (first) {
				first = FALSE;
			} else {
				g_string_append_len(pattern_result, CONST_STR_LEN("."));
			}
			g_string_append(pattern_result, ctx->splits[ctx->split_len - i]);
		}
	} else {
		for (i = from; i >= to; i--) { /* to > 0, so no underflow in i possible */
			if (first) {
				first = FALSE;
			} else {
				g_string_append_len(pattern_result, CONST_STR_LEN("."));
			}
			g_string_append(pattern_result, ctx->splits[ctx->split_len - i]);
		}
	}
}

static liHandlerResult core_handle_docroot(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	GMatchInfo *match_info = NULL;
	GArray *arr = param;
	docroot_split dsplit = { vr->request.uri.host, NULL, 0 };

	g_string_truncate(vr->physical.doc_root, 0);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	/* resume from last stat check */
	if (*context) {
		i = GPOINTER_TO_INT(*context);
	} else {
		i = 0;
	}
	*context = NULL;

	/* loop over all the patterns until we find an existing path */
	for (; i < arr->len; i++) {
		struct stat st;
		gint err;


		g_string_truncate(vr->physical.doc_root, 0);
		li_pattern_eval(vr, vr->physical.doc_root, g_array_index(arr, liPattern*, i), core_docroot_nth_cb, &dsplit, li_pattern_regex_cb, match_info);

		if (i == arr->len - 1) break; /* don't stat, we'll use the last entry anyway */

		/* check if path exists */
		switch (li_stat_cache_get(vr, vr->physical.doc_root, &st, &err, NULL)) {
		case LI_HANDLER_GO_ON: break;
		case LI_HANDLER_WAIT_FOR_EVENT:
			*context = GINT_TO_POINTER(i);
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "docroot: waiting for async: \"%s\"", vr->physical.doc_root->str);
			}
			g_strfreev(dsplit.splits);
			return LI_HANDLER_WAIT_FOR_EVENT;
		default:
			/* not found, try next pattern */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "docroot: not found: \"%s\", trying next", vr->physical.doc_root->str);
			}
			continue;
		}

		break;
	}

	g_strfreev(dsplit.splits);

	/* build physical path: docroot + uri.path */
	g_string_truncate(vr->physical.path, 0);
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->physical.doc_root));
	if (vr->request.uri.path->len == 0 || vr->request.uri.path->str[0] != '/')
		li_path_append_slash(vr->physical.path);
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->request.uri.path));

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "docroot: \"%s\"", vr->physical.doc_root->str);
		VR_DEBUG(vr, "physical path: \"%s\"", vr->physical.path->str);
	}

	return LI_HANDLER_GO_ON;
}

static void core_docroot_free(liServer *srv, gpointer param) {
	guint i;
	GArray *arr = param;

	UNUSED(srv);

	for (i = 0; i < arr->len; i++) {
		li_pattern_free(g_array_index(arr, liPattern*, i));
	}

	g_array_free(arr, TRUE);
}

static liAction* core_docroot(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *arr;
	guint i;
	liValue *v;
	liPattern *pattern;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || (val->type != LI_VALUE_STRING && val->type != LI_VALUE_LIST)) {
		ERROR(srv, "%s", "docroot action expects a string or list of strings as parameter");
		return NULL;
	}

	arr = g_array_new(FALSE, TRUE, sizeof(liPattern*));

	if (val->type == LI_VALUE_STRING) {
		pattern = li_pattern_new(srv, val->data.string->str);
		g_array_append_val(arr, pattern);
	} else {
		for (i = 0; i < val->data.list->len; i++) {
			v = g_array_index(val->data.list, liValue*, i);

			if (v->type != LI_VALUE_STRING) {
				core_docroot_free(srv, arr);
				return NULL;
			}

			pattern = li_pattern_new(srv, v->data.string->str);
			g_array_append_val(arr, pattern);
		}
	}

	return li_action_new_function(core_handle_docroot, NULL, core_docroot_free, arr);
}

typedef struct {
	GString *prefix, *path;
} core_alias_config;

static liHandlerResult core_handle_alias(liVRequest *vr, gpointer _param, gpointer *context) {
	GArray *param = _param;
	guint i;
	UNUSED(context);

	for (i = 0; i < param->len; i++) {
		core_alias_config ac = g_array_index(param, core_alias_config, i);
		gsize preflen = ac.prefix->len;
		gboolean isdir = FALSE;

		if (preflen > 0 && ac.prefix->str[preflen-1] == '/') {
			preflen--;
			isdir = TRUE;
		}

		if (li_string_prefix(vr->request.uri.path, ac.prefix->str, preflen)) {
			/* check if url has the form "prefix" or "prefix/.*" */
			if (isdir && vr->request.uri.path->str[preflen] != '\0' && vr->request.uri.path->str[preflen] != '/') continue;

			/* prefix matched */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "alias path: %s", ac.path->str);
			}

			g_string_truncate(vr->physical.doc_root, 0);
			g_string_append_len(vr->physical.doc_root, GSTR_LEN(ac.path));

			/* build physical path: docroot + uri.path */
			g_string_truncate(vr->physical.path, 0);
			g_string_append_len(vr->physical.path, GSTR_LEN(ac.path));
			g_string_append_len(vr->physical.path, vr->request.uri.path->str + preflen, vr->request.uri.path->len - preflen);

			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "alias physical path: %s", vr->physical.path->str);
			}

			return LI_HANDLER_GO_ON;
		}
	}

	return LI_HANDLER_GO_ON;
}

static void core_alias_free(liServer *srv, gpointer _param) {
	GArray *param = _param;
	guint i;
	UNUSED(srv);

	for (i = 0; i < param->len; i++) {
		core_alias_config ac = g_array_index(param, core_alias_config, i);
		g_string_free(ac.prefix, TRUE);
		g_string_free(ac.path, TRUE);
	}
	g_array_free(param, TRUE);
}

static liAction* core_alias(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *a = NULL;
	GArray *vl, *vl1;
	core_alias_config ac;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "unexpected or no parameter for alias action");
		return NULL;
	}
	vl = val->data.list;
	if (vl->len == 2 && g_array_index(vl, liValue*, 0)->type == LI_VALUE_STRING && g_array_index(vl, liValue*, 1)->type == LI_VALUE_STRING) {
		a = g_array_sized_new(FALSE, TRUE, sizeof(core_alias_config), 1);
		ac.prefix = li_value_extract_string(g_array_index(vl, liValue*, 0));
		ac.path = li_value_extract_string(g_array_index(vl, liValue*, 1));
		g_array_append_val(a, ac);
	} else {
		guint i;
		a = g_array_sized_new(FALSE, TRUE, sizeof(core_alias_config), vl->len);
		for (i = 0; i < vl->len; i++) {
			if (g_array_index(vl, liValue*, i)->type != LI_VALUE_LIST) {
				ERROR(srv, "%s", "unexpected entry in parameter for alias action");
				goto error_free;
			}
			vl1 = g_array_index(vl, liValue*, i)->data.list;
			if (g_array_index(vl1, liValue*, 0)->type == LI_VALUE_STRING && g_array_index(vl1, liValue*, 1)->type == LI_VALUE_STRING) {
				ac.prefix = li_value_extract_string(g_array_index(vl1, liValue*, 0));
				ac.path = li_value_extract_string(g_array_index(vl1, liValue*, 1));
				g_array_append_val(a, ac);
			} else {
				ERROR(srv, "%s", "unexpected entry in parameter for alias action");
				goto error_free;
			}
		}
	}

	return li_action_new_function(core_handle_alias, NULL, core_alias_free, a);

error_free:
	core_alias_free(srv, a);
	return NULL;
}


static liHandlerResult core_handle_index(liVRequest *vr, gpointer param, gpointer *context) {
	liHandlerResult res;
	guint i;
	struct stat st;
	gint err;
	GString *file, *tmp_docroot, *tmp_path;
	GArray *files = param;

	UNUSED(context);

	if (!vr->physical.doc_root->len) {
		VR_ERROR(vr, "%s", "no docroot specified yet but index action called");
		return LI_HANDLER_ERROR;
	}

	res = li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL);
	if (res == LI_HANDLER_WAIT_FOR_EVENT)
		return LI_HANDLER_WAIT_FOR_EVENT;

	if (res == LI_HANDLER_ERROR) {
		/* we ignore errors here so a later action can deal with it (e.g. 'static') */
		return LI_HANDLER_GO_ON;
	}

	if (!S_ISDIR(st.st_mode))
		return LI_HANDLER_GO_ON;

	/* need trailing slash */
	if (vr->request.uri.path->len == 0 || vr->request.uri.path->str[vr->request.uri.path->len-1] != '/') {
		li_vrequest_redirect_directory(vr);
		return LI_HANDLER_GO_ON;
	}

	/* we use two temporary strings here, one to append to docroot and one to append to physical path */
	tmp_docroot = vr->wrk->tmp_str;
	g_string_truncate(tmp_docroot, 0);
	g_string_append_len(vr->wrk->tmp_str, GSTR_LEN(vr->physical.doc_root));
	tmp_path = g_string_new_len(GSTR_LEN(vr->physical.path));

	/* loop through the list to find a possible index file */
	for (i = 0; i < files->len; i++) {
		file = g_array_index(files, liValue*, i)->data.string;

		if (file->str[0] == '/') {
			/* entries beginning with a slash shall be looked up directly at the docroot */
			g_string_truncate(tmp_docroot, vr->physical.doc_root->len);
			g_string_append_len(tmp_docroot, GSTR_LEN(file));
			res = li_stat_cache_get(vr, tmp_docroot, &st, &err, NULL);
		} else {
			g_string_truncate(tmp_path, vr->physical.path->len);
			g_string_append_len(tmp_path, GSTR_LEN(file));
			res = li_stat_cache_get(vr, tmp_path, &st, &err, NULL);
		}

		if (res == LI_HANDLER_WAIT_FOR_EVENT) {
			g_string_free(tmp_path, TRUE);
			return LI_HANDLER_WAIT_FOR_EVENT;
		}

		if (res == LI_HANDLER_GO_ON) {
			/* file exists. change physical path */
			if (file->str[0] == '/') {
				g_string_truncate(vr->physical.path, vr->physical.doc_root->len);
				g_string_truncate(vr->request.uri.path, 0);
			}

			g_string_append_len(vr->physical.path, GSTR_LEN(file));
			g_string_append_len(vr->request.uri.path, GSTR_LEN(file));

			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "using index file: '%s'", file->str);
			}

			g_string_free(tmp_path, TRUE);

			return LI_HANDLER_GO_ON;
		}
	}

	g_string_free(tmp_path, TRUE);

	return LI_HANDLER_GO_ON;
}

static void core_index_free(liServer *srv, gpointer param) {
	guint i;
	GArray *files = param;

	UNUSED(srv);

	for (i = 0; i < files->len; i++)
		li_value_free(g_array_index(files, liValue*, i));

	g_array_free(files, TRUE);
}

static liAction* core_index(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *files;
	guint i;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "index action expects a list of strings as parameter");
		return NULL;
	}

	files = val->data.list;

	for (i = 0; i < files->len; i++) {
		if (g_array_index(files, liValue*, i)->type != LI_VALUE_STRING) {
			ERROR(srv, "%s", "index action expects a list of strings as parameter");
			return NULL;
		}
	}

	return li_action_new_function(core_handle_index, NULL, core_index_free, li_value_extract_list(val));
}


static liHandlerResult core_handle_static(liVRequest *vr, gpointer param, gpointer *context) {
	int fd = -1;
	struct stat st;
	int err;
	liHandlerResult res;
	GArray *exclude_arr = CORE_OPTIONPTR(LI_CORE_OPTION_STATIC_FILE_EXCLUDE_EXTENSIONS).list;
	static const gchar boundary[] = "fkj49sn38dcn3";
	gboolean no_fail = GPOINTER_TO_INT(param);

	UNUSED(param);
	UNUSED(context);

	if (li_vrequest_is_handled(vr))
		return LI_HANDLER_GO_ON;

	switch (vr->request.http_method) {
	case LI_HTTP_METHOD_GET:
	case LI_HTTP_METHOD_HEAD:
		break;
	default:
		if (no_fail) return LI_HANDLER_GO_ON;

		if (!li_vrequest_handle_direct(vr)) {
			return LI_HANDLER_ERROR;
		}

		vr->response.http_status = 405;
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Allow"), CONST_STR_LEN("GET, HEAD"));
		return LI_HANDLER_GO_ON;
	}

	if (vr->physical.path->len == 0) return LI_HANDLER_GO_ON;

	if (exclude_arr) {
		const gchar *basep = g_basename(vr->physical.path->str);
		const GString base = li_const_gstring((gchar*) basep, vr->physical.path->len - (basep - vr->physical.path->str));
		guint i;

		for (i = 0; i < exclude_arr->len; i++) {
			liValue *v = g_array_index(exclude_arr, liValue*, i);
			if (li_string_suffix(&base, GSTR_LEN(v->data.string))) {
				if (no_fail) return LI_HANDLER_GO_ON;

				if (!li_vrequest_handle_direct(vr)) {
					return LI_HANDLER_ERROR;
				}

				vr->response.http_status = 403;
				return LI_HANDLER_GO_ON;
			}
		}
	}

	res = li_stat_cache_get(vr, vr->physical.path, &st, &err, &fd);
	if (res == LI_HANDLER_WAIT_FOR_EVENT)
		return res;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "try serving static file: '%s'", vr->physical.path->str);
	}

	if (res == LI_HANDLER_ERROR) {
		/* open or fstat failed */

		if (fd != -1)
			close(fd);

		if (no_fail) return LI_HANDLER_GO_ON;

		if (!li_vrequest_handle_direct(vr)) {
			return LI_HANDLER_ERROR;
		}

		switch (err) {
		case ENOENT:
		case ENOTDIR:
			vr->response.http_status = 404;
			return LI_HANDLER_GO_ON;
		case EACCES:
			vr->response.http_status = 403;
			return LI_HANDLER_GO_ON;
		default:
			VR_ERROR(vr, "stat() or open() for '%s' failed: %s", vr->physical.path->str, g_strerror(err));
			return LI_HANDLER_ERROR;
		}
	} else if (S_ISDIR(st.st_mode)) {
		if (fd != -1)
			close(fd);
		return LI_HANDLER_GO_ON;
	} else if (!S_ISREG(st.st_mode)) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "not a regular file: '%s'", vr->physical.path->str);
		}

		if (fd != -1)
			close(fd);

		if (no_fail) return LI_HANDLER_GO_ON;

		if (!li_vrequest_handle_direct(vr)) {
			return LI_HANDLER_ERROR;
		}
		vr->response.http_status = 403;
		return LI_HANDLER_GO_ON;
	} else {
		const GString *mime_str;
		gboolean cachable;
		gboolean ranged_response = FALSE;
		liHttpHeader *hh_range;
		liChunkFile *cf;
		static const GString default_mime_str = { CONST_STR_LEN("application/octet-stream"), 0 };

#ifdef FD_CLOEXEC
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

		if (!li_vrequest_handle_direct(vr)) {
			close(fd);
			return LI_HANDLER_ERROR;
		}

		li_etag_set_header(vr, &st, &cachable);
		if (cachable) {
			vr->response.http_status = 304;
			close(fd);
			return LI_HANDLER_GO_ON;
		}

		cf = li_chunkfile_new(NULL, fd, FALSE);

		mime_str = li_mimetype_get(vr, vr->physical.path);
		if (!mime_str) mime_str = &default_mime_str;

		if (CORE_OPTION(LI_CORE_OPTION_STATIC_RANGE_REQUESTS).boolean) {
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Accept-Ranges"), CONST_STR_LEN("bytes"));

			hh_range = li_http_header_lookup(vr->request.headers, CONST_STR_LEN("range"));
			if (hh_range) {
				/* TODO: Check If-Range: header */
				const GString range_str = li_const_gstring(LI_HEADER_VALUE_LEN(hh_range));
				liParseHttpRangeState rs;
				gboolean is_multipart = FALSE, done = FALSE;

				li_parse_http_range_init(&rs, &range_str, st.st_size);
				do {
					switch (li_parse_http_range_next(&rs)) {
					case LI_PARSE_HTTP_RANGE_OK:
						if (!is_multipart && !rs.last_range) {
							is_multipart = TRUE;
						}
						g_string_printf(vr->wrk->tmp_str, "bytes %"G_GINT64_FORMAT"-%"G_GINT64_FORMAT"/%"G_GINT64_FORMAT, rs.range_start, rs.range_end, (goffset) st.st_size);
						if (is_multipart) {
							GString *subheader = g_string_sized_new(1023);
							g_string_append_printf(subheader, "\r\n--%s\r\nContent-Type: %s\r\nContent-Range: %s\r\n\r\n", boundary, mime_str->str, vr->wrk->tmp_str->str);
							li_chunkqueue_append_string(vr->out, subheader);
							li_chunkqueue_append_chunkfile(vr->out, cf, rs.range_start, rs.range_length);
						} else {
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Range"), GSTR_LEN(vr->wrk->tmp_str));
							li_chunkqueue_append_chunkfile(vr->out, cf, rs.range_start, rs.range_length);
						}
						break;
					case LI_PARSE_HTTP_RANGE_DONE:
						ranged_response = TRUE;
						done = TRUE;
						vr->response.http_status = 206;
						if (is_multipart) {
							GString *subheader = g_string_sized_new(1023);
							g_string_append_printf(subheader, "\r\n--%s--\r\n", boundary);
							li_chunkqueue_append_string(vr->out, subheader);

							g_string_printf(vr->wrk->tmp_str, "multipart/byteranges; boundary=%s", boundary);
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(vr->wrk->tmp_str));
						} else {
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));
						}
						break;
					case LI_PARSE_HTTP_RANGE_INVALID:
						done = TRUE;
						li_chunkqueue_reset(vr->out);
						break;
					case LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE:
						ranged_response = TRUE;
						done = TRUE;
						li_chunkqueue_reset(vr->out); vr->out->is_closed = TRUE;
						g_string_printf(vr->wrk->tmp_str, "bytes */%"G_GINT64_FORMAT, (goffset) st.st_size);
						li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Range"), GSTR_LEN(vr->wrk->tmp_str));
						vr->response.http_status = 416;
						break;
					}
				} while (!done);
				li_parse_http_range_clear(&rs);
			}
		}

		if (!ranged_response) {
			vr->response.http_status = 200;
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));
			li_chunkqueue_append_chunkfile(vr->out, cf, 0, st.st_size);
		}

		li_chunkfile_release(cf);
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_static(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);
	if (val) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(core_handle_static, NULL, NULL, GINT_TO_POINTER(0));
}

static liAction* core_static_no_fail(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);
	if (val) {
		ERROR(srv, "%s", "static_no_fail action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(core_handle_static, NULL, NULL, GINT_TO_POINTER(1));
}


static liHandlerResult core_handle_pathinfo(liVRequest *vr, gpointer param, gpointer *context) {
	struct stat st;
	int err;
	liHandlerResult res;
	gchar *slash;
	UNUSED(param);
	UNUSED(context);

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

next_round:
	if (vr->physical.path->len <= vr->physical.doc_root->len) return LI_HANDLER_GO_ON;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "stat: physical path: %s", vr->physical.path->str);
	}
	res = li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL);
	if (res == LI_HANDLER_GO_ON) {
		if (vr->physical.pathinfo->len > 0) {
			/* remove PATHINFO from url too ("SCRIPT_NAME") */
			if (li_string_suffix(vr->request.uri.path, GSTR_LEN(vr->physical.pathinfo))) {
				g_string_set_size(vr->request.uri.path, vr->request.uri.path->len - vr->physical.pathinfo->len);
			}
		}
	}
	if (res == LI_HANDLER_WAIT_FOR_EVENT || res == LI_HANDLER_GO_ON)
		return res;

	/* stat failed. why? */
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "stat failed %i: physical path: %s", err, vr->physical.path->str);
	}
	switch (err) {
	case ENOTDIR:
		slash = strrchr(vr->physical.path->str, '/');
		if (!slash) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "no slash: %s", vr->physical.path->str);
			}
			return LI_HANDLER_GO_ON;
		}
		g_string_prepend(vr->physical.pathinfo, slash);
		g_string_set_size(vr->physical.path, slash - vr->physical.path->str);
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "physical path: %s", vr->physical.path->str);
		}
		goto next_round;
	case ENOENT:
		return LI_HANDLER_GO_ON;
	case EACCES:
		return LI_HANDLER_GO_ON;
	default:
		VR_ERROR(vr, "stat() or open() for '%s' failed: %s", vr->physical.path->str, g_strerror(err));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_pathinfo(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);
	if (val) {
		ERROR(srv, "%s", "pathinfo action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(core_handle_pathinfo, NULL, NULL, NULL);
}

static liHandlerResult core_handle_status(liVRequest *vr, gpointer param, gpointer *context) {
	UNUSED(param);
	UNUSED(context);

	vr->response.http_status = GPOINTER_TO_INT(param);

	return LI_HANDLER_GO_ON;
}

static liAction* core_status(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gpointer ptr;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "%s", "set_status action expects a number as parameter");
		return NULL;
	}

	ptr = GINT_TO_POINTER((gint) val->data.number);

	return li_action_new_function(core_handle_status, NULL, NULL, ptr);
}


static void core_log_write_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	li_pattern_free(param);
}

static liHandlerResult core_handle_log_write(liVRequest *vr, gpointer param, gpointer *context) {
	liPattern *pattern = param;
	GMatchInfo *match_info = NULL;

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	UNUSED(context);

	/* eval pattern, ignore $n */
	g_string_truncate(vr->wrk->tmp_str, 0);
	li_pattern_eval(vr, vr->wrk->tmp_str, pattern, NULL, NULL, li_pattern_regex_cb, match_info);

	VR_INFO(vr, "%s", vr->wrk->tmp_str->str);

	return LI_HANDLER_GO_ON;
}

static liAction* core_log_write(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liPattern *pattern;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);
	if (!val || val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "log.write expects a string parameter");
		return NULL;
	}

	pattern = li_pattern_new(srv, val->data.string->str);
	if (!pattern) {
		ERROR(srv, "%s", "log.write failed to parse pattern");
		return NULL;
	}

	return li_action_new_function(core_handle_log_write, NULL, core_log_write_free, pattern);
}


typedef struct respond_param respond_param;
struct respond_param {
	guint status_code;
	liPattern *pattern;
};

static void core_respond_free(liServer *srv, gpointer param) {
	respond_param *rp = param;

	UNUSED(srv);

	if (rp->pattern)
		li_pattern_free(rp->pattern);

	g_slice_free(respond_param, rp);
}

static liHandlerResult core_handle_respond(liVRequest *vr, gpointer param, gpointer *context) {
	respond_param *rp = param;

	UNUSED(context);

	if (!li_vrequest_handle_direct(vr))
		return LI_HANDLER_GO_ON;

	vr->response.http_status = rp->status_code;

	if (!li_http_header_lookup(vr->response.headers, CONST_STR_LEN("content-type")))
		li_http_header_insert(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/plain"));

	if (rp->pattern) {
		g_string_truncate(vr->wrk->tmp_str, 0);
		li_pattern_eval(vr, vr->wrk->tmp_str, rp->pattern, NULL, NULL, NULL, NULL);
		li_chunkqueue_append_mem(vr->out, GSTR_LEN(vr->wrk->tmp_str));
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_respond(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	respond_param *rp;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	rp = g_slice_new(respond_param);

	if (!val) {
		// respond;
		rp->status_code = 200;
		rp->pattern = NULL;
	} else if (val->type == LI_VALUE_STRING) {
		// respond "foo";
		rp->status_code = 200;
		rp->pattern = li_pattern_new(srv, val->data.string->str);

		if (!rp->pattern) {
			g_slice_free(respond_param, rp);
			ERROR(srv, "%s", "'respond' action takes an optional string as parameter");
			return NULL;
		}
	} else if (val->type == LI_VALUE_NUMBER) {
		// respond 404;
		rp->status_code = val->data.number;
		rp->pattern = NULL;
	} else if (val->type == LI_VALUE_LIST && val->data.list->len == 2 && g_array_index(val->data.list, liValue*, 0)->type == LI_VALUE_NUMBER && g_array_index(val->data.list, liValue*, 1)->type == LI_VALUE_STRING) {
		// respond 200 => "foo";
		rp->status_code = g_array_index(val->data.list, liValue*, 0)->data.number;
		rp->pattern = li_pattern_new(srv, g_array_index(val->data.list, liValue*, 1)->data.string->str);

		if (!rp->pattern) {
			g_slice_free(respond_param, rp);
			ERROR(srv, "%s", "'respond' action takes an optional string as parameter");
			return NULL;
		}
	} else {
		g_slice_free(respond_param, rp);
		ERROR(srv, "%s", "'respond' action takes an optional string as parameter");
		return NULL;
	}

	return li_action_new_function(core_handle_respond, NULL, core_respond_free, rp);
}


static void core_env_set_free(liServer *srv, gpointer param) {
	GArray *arr = param;

	UNUSED(srv);

	li_value_free(g_array_index(arr, liValue*, 0));
	li_pattern_free(g_array_index(arr, liPattern*, 1));
	g_array_free(arr, TRUE);
}

static liHandlerResult core_handle_env_set(liVRequest *vr, gpointer param, gpointer *context) {
	GArray *arr = param;
	GMatchInfo *match_info = NULL;

	UNUSED(context);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	g_string_truncate(vr->wrk->tmp_str, 0);
	li_pattern_eval(vr, vr->wrk->tmp_str, g_array_index(arr, liPattern*, 1), NULL, NULL, li_pattern_regex_cb, match_info);
	li_environment_set(&vr->env, GSTR_LEN(g_array_index(arr, liValue*, 0)->data.string), GSTR_LEN(vr->wrk->tmp_str));

	return LI_HANDLER_GO_ON;
}

static liAction* core_env_set(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liPattern *pattern;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST || val->data.list->len != 2
		|| g_array_index(val->data.list, liValue*, 0)->type != LI_VALUE_STRING
		|| g_array_index(val->data.list, liValue*, 1)->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "'env.set' action requires a pair of strings as parameter");
		return NULL;
	}

	if (NULL == (pattern = li_pattern_new(srv, g_array_index(val->data.list, liValue*, 1)->data.string->str)))
		return NULL;

	/* exchange second parameter (string) with the new pattern */
	li_value_free(g_array_index(val->data.list, liValue*, 1));
	g_array_index(val->data.list, liPattern*, 1) = pattern;

	return li_action_new_function(core_handle_env_set, NULL, core_env_set_free, li_value_extract_list(val));
}


static liHandlerResult core_handle_env_add(liVRequest *vr, gpointer param, gpointer *context) {
	GArray *arr = param;
	GMatchInfo *match_info = NULL;

	UNUSED(context);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	g_string_truncate(vr->wrk->tmp_str, 0);
	li_pattern_eval(vr, vr->wrk->tmp_str, g_array_index(arr, liPattern*, 1), NULL, NULL, li_pattern_regex_cb, match_info);
	li_environment_insert(&vr->env, GSTR_LEN(g_array_index(arr, liValue*, 0)->data.string), GSTR_LEN(vr->wrk->tmp_str));

	return LI_HANDLER_GO_ON;
}

static liAction* core_env_add(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liPattern *pattern;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST || val->data.list->len != 2
		|| g_array_index(val->data.list, liValue*, 0)->type != LI_VALUE_STRING
		|| g_array_index(val->data.list, liValue*, 1)->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "'env.add' action requires a pair of strings as parameter");
		return NULL;
	}

	if (NULL == (pattern = li_pattern_new(srv, g_array_index(val->data.list, liValue*, 1)->data.string->str)))
		return NULL;

	/* exchange second parameter (string) with the new pattern */
	li_value_free(g_array_index(val->data.list, liValue*, 1));
	g_array_index(val->data.list, liPattern*, 1) = pattern;

	return li_action_new_function(core_handle_env_add, NULL, core_env_set_free, li_value_extract_list(val));
}


static void core_env_remove_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_string_free(param, TRUE);
}

static liHandlerResult core_handle_env_remove(liVRequest *vr, gpointer param, gpointer *context) {
	GString *key = param;

	UNUSED(context);

	li_environment_remove(&vr->env, GSTR_LEN(key));

	return LI_HANDLER_GO_ON;
}

static liAction* core_env_remove(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "'env.remove' action requires a string as parameter");
		return NULL;
	}

	return li_action_new_function(core_handle_env_remove, NULL, core_env_remove_free, li_value_extract_string(val));
}


static liHandlerResult core_handle_env_clear(liVRequest *vr, gpointer param, gpointer *context) {
	UNUSED(param); UNUSED(context);

	li_environment_reset(&vr->env);

	return LI_HANDLER_GO_ON;
}

static liAction* core_env_clear(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val) {
		ERROR(srv, "%s", "'env.clear' action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(core_handle_env_clear, NULL, NULL, NULL);
}


static gboolean core_listen(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	GString *ipstr;
	UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "listen expects a string as parameter");
		return FALSE;
	}

	ipstr = val->data.string;
	li_angel_listen(srv, ipstr, NULL, NULL);

	return TRUE;
}


static gboolean core_workers(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	gint workers;
	UNUSED(p); UNUSED(userdata);

	workers = val->data.number;
	if (val->type != LI_VALUE_NUMBER || workers < 1) {
		ERROR(srv, "%s", "workers expects a positive integer as parameter");
		return FALSE;
	}

	if (srv->worker_count != 0) {
		ERROR(srv, "workers already called with '%i', overwriting", srv->worker_count);
	}
	srv->worker_count = workers;
	return TRUE;
}

static gboolean core_workers_cpu_affinity(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
#if defined(LIGHTY_OS_LINUX)
	GArray *arr1, *arr2;
	guint i, j;
	liValue *v;

	UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "workers.cpu_affinity expects a list of integers or list of list of integers");
		return FALSE;
	}

	arr1 = val->data.list;

	for (i = 0; i < arr1->len; i++) {
		v = g_array_index(arr1, liValue*, i);
		if (v->type == LI_VALUE_NUMBER)
			continue;
		if (v->type == LI_VALUE_LIST) {
			arr2 = v->data.list;
			for (j = 0; j < arr2->len; j++) {
				if (g_array_index(arr2, liValue*, j)->type != LI_VALUE_NUMBER) {
					ERROR(srv, "%s", "workers.cpu_affinity expects a list of integers or list of list of integers");
					return FALSE;
				}
			}
		} else {
			ERROR(srv, "%s", "workers.cpu_affinity expects a list of integers or list of list of integers");
			return FALSE;
		}
	}

	srv->workers_cpu_affinity = li_value_copy(val);

	return TRUE;
#else
	UNUSED(p); UNUSED(val); UNUSED(userdata);
	ERROR(srv, "%s", "workers.cpu_affinity is only available on Linux systems");
	return FALSE;
#endif
}

static gboolean core_module_load(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	liValue *mods = li_value_new_list();

	UNUSED(p); UNUSED(userdata);

	if (!g_module_supported()) {
		ERROR(srv, "%s", "module loading not supported on this platform");
		li_value_free(mods);
		return FALSE;
	}

	if (val->type == LI_VALUE_STRING) {
		/* load only one module */
		liValue *name = li_value_new_string(li_value_extract_string(val));
		g_array_append_val(mods->data.list, name);
	} else if (val->type == LI_VALUE_LIST) {
		/* load a list of modules */
		for (guint i = 0; i < val->data.list->len; i++) {
			liValue *v = g_array_index(val->data.list, liValue*, i);
			if (v->type != LI_VALUE_STRING) {
				ERROR(srv, "module_load takes either a string or a list of strings as parameter, list with %s entry given", li_value_type_string(v->type));
				li_value_free(mods);
				return FALSE;
			}
		}
		g_array_free(mods->data.list, TRUE);
		mods->data.list = li_value_extract_list(val);
	} else {
		ERROR(srv, "module_load takes either a string or a list of strings as parameter, %s given", li_value_type_string(val->type));
		return FALSE;
	}

	/* parameter types ok, load modules */
	for (guint i = 0; i < mods->data.list->len; i++) {
		GString *name = g_array_index(mods->data.list, liValue*, i)->data.string;
		if (li_module_lookup(srv->modules, name->str)) {
			DEBUG(srv, "module_load: module '%s' already loaded", name->str);
			continue;
		}

		if (!li_module_load(srv->modules, name->str)) {
			ERROR(srv, "could not load module '%s': %s", name->str, g_module_error());
			li_value_free(mods);
			return FALSE;
		}

		DEBUG(srv, "loaded module '%s'", name->str);
	}

	li_value_free(mods);

	return TRUE;
}

static gboolean core_io_timeout(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_NUMBER || val->data.number < 1) {
		ERROR(srv, "%s", "io.timeout expects a positive number as parameter");
		return FALSE;
	}

	srv->io_timeout = val->data.number;

	return TRUE;
}

static gboolean core_stat_cache_ttl(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_NUMBER || val->data.number < 0) {
		ERROR(srv, "%s", "stat_cache.ttl expects a positive number as parameter");
		return FALSE;
	}

	srv->stat_cache_ttl = (gdouble)val->data.number;

	return TRUE;
}

static gboolean core_tasklet_pool_threads(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "%s", "tasklet_pool.threads expects a number as parameter");
		return FALSE;
	}

	srv->tasklet_pool_threads = val->data.number;
	li_tasklet_pool_set_threads(srv->main_worker->tasklets, srv->tasklet_pool_threads);

	return TRUE;
}

/*
 * OPTIONS
 */

static gboolean core_option_log_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	GHashTableIter iter;
	gpointer k, v;
	liLogLevel level;
	GString *path;
	GString *level_str;
	GArray *arr = g_array_sized_new(FALSE, TRUE, sizeof(GString*), 6);
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(ndx);

	*oval = arr;
	g_array_set_size(arr, 6);

	/* default value */
	if (!val) {
		/* default: log LI_LOG_LEVEL_WARNING, LI_LOG_LEVEL_ERROR and LI_LOG_LEVEL_BACKEND to stderr */
		g_array_index(arr, GString*, LI_LOG_LEVEL_WARNING) = g_string_new_len(CONST_STR_LEN("stderr"));
		g_array_index(arr, GString*, LI_LOG_LEVEL_ERROR) = g_string_new_len(CONST_STR_LEN("stderr"));
		g_array_index(arr, GString*, LI_LOG_LEVEL_BACKEND) = g_string_new_len(CONST_STR_LEN("stderr"));
		return TRUE;
	}

	g_hash_table_iter_init(&iter, val->data.hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		if (((liValue*)v)->type != LI_VALUE_STRING) {
			ERROR(srv, "log expects a hashtable with string values, %s given", li_value_type_string(((liValue*)v)->type));
			g_array_free(arr, TRUE);
			return FALSE;
		}

		path = ((liValue*)v)->data.string;
		level_str = (GString*)k;

		if (g_str_equal(level_str->str, "*")) {
			for (guint i = 0; i < arr->len; i++) {
				/* overwrite old path */
				if (NULL != g_array_index(arr, GString*, i))
					g_string_free(g_array_index(arr, GString*, i), TRUE);

				g_array_index(arr, GString*, i) = g_string_new_len(GSTR_LEN(path));
			}
		}
		else {
			level = li_log_level_from_string(level_str);
			g_array_index(arr, GString*, level) = g_string_new_len(GSTR_LEN(path));;
		}
	}

	return TRUE;
}

static void core_option_log_free(liServer *srv, liPlugin *p, size_t ndx, gpointer oval) {
	GArray *arr = oval;

	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	if (!arr) return;

	for (guint i = 0; i < arr->len; i++) {
		if (NULL != g_array_index(arr, GString*, i))
			g_string_free(g_array_index(arr, GString*, i), TRUE);
	}
	g_array_free(arr, TRUE);
}

static gboolean core_option_log_timestamp_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(ndx);

	if (!val) return TRUE;
	*oval = li_log_timestamp_new(srv, li_value_extract_string(val));

	return TRUE;
}

static void core_option_log_timestamp_free(liServer *srv, liPlugin *p, size_t ndx, gpointer oval) {
	UNUSED(p);
	UNUSED(ndx);

	if (!oval) return;
	li_log_timestamp_free(srv, oval);
}

static gboolean core_option_static_exclude_exts_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	GArray *arr;
	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(ndx);

	if (!val) return TRUE;

	arr = val->data.list;
	for (guint i = 0; i < arr->len; i++) {
		liValue *v = g_array_index(arr, liValue*, i);
		if (v->type != LI_VALUE_STRING) {
			ERROR(srv, "static.exclude_extensions option expects a list of strings, entry #%u is of type %s", i, li_value_type_string(v->type));
			return FALSE;
		}
	}

	/* everything ok */
	*oval = li_value_extract_list(val);

	return TRUE;
}


static gboolean core_option_mime_types_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	GArray *arr;
	liMimetypeNode *node;

	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(ndx);

	*oval = node = li_mimetype_node_new();
	node->mimetype = g_string_new_len(CONST_STR_LEN("application/octet-stream"));

	/* default value */
	if (!val) {
		return TRUE;
	}

	/* check if the passed val is of type (("a", "b"), ("x", y")) */
	arr = val->data.list;
	for (guint i = 0; i < arr->len; i++) {
		liValue *v = g_array_index(arr, liValue*, i);
		liValue *v1, *v2;
		if (v->type != LI_VALUE_LIST) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is of type %s", i, li_value_type_string(v->type));
			li_mimetype_node_free(node);
			return FALSE;
		}

		if (v->data.list->len != 2) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is not a tuple", i);
			li_mimetype_node_free(node);
			return FALSE;
		}

		v1 = g_array_index(v->data.list, liValue*, 0);
		v2 = g_array_index(v->data.list, liValue*, 1);
		if (v1->type != LI_VALUE_STRING || v2->type != LI_VALUE_STRING) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is a (%s,%s) tuple", i, li_value_type_string(v1->type), li_value_type_string(v2->type));
			li_mimetype_node_free(node);
			return FALSE;
		}

		li_mimetype_insert(node, v1->data.string, li_value_extract_string(v2));
	}

	return TRUE;
}

static void core_option_mime_types_free(liServer *srv, liPlugin *p, size_t ndx, gpointer oval) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	li_mimetype_node_free(oval);
}

static gboolean core_option_etag_use_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, liOptionValue *oval) {
	GArray *arr;
	guint flags = 0;
	UNUSED(p);
	UNUSED(ndx);
	UNUSED(wrk);

	/* default value */
	if (!val) {
		oval->number = LI_ETAG_USE_INODE | LI_ETAG_USE_MTIME | LI_ETAG_USE_SIZE;
		return TRUE;
	}

	/* Need manual type check, as resulting option type is number */
	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "etag.use option expects a list of strings, parameter is of type %s", li_value_type_string(val->type));
		return FALSE;
	}
	arr = val->data.list;
	for (guint i = 0; i < arr->len; i++) {
		liValue *v = g_array_index(arr, liValue*, i);
		if (v->type != LI_VALUE_STRING) {
			ERROR(srv, "etag.use option expects a list of strings, entry #%u is of type %s", i, li_value_type_string(v->type));
			return FALSE;
		}

		if (0 == strcmp(v->data.string->str, "inode")) {
			flags |= LI_ETAG_USE_INODE;
		} else if (0 == strcmp(v->data.string->str, "mtime")) {
			flags |= LI_ETAG_USE_MTIME;
		} else if (0 == strcmp(v->data.string->str, "size")) {
			flags |= LI_ETAG_USE_SIZE;
		} else {
			ERROR(srv, "unknown etag.use flag: %s", v->data.string->str);
			return FALSE;
		}
	}

	oval->number = (guint64) flags;
	return TRUE;
}

typedef void (*header_cb)(liHttpHeaders *headers, const gchar *key, size_t keylen, const gchar *val, size_t valuelen);

typedef struct header_ctx header_ctx;
struct header_ctx {
	GString *key;
	liPattern *value;
	header_cb cb;
};

static void core_header_free(liServer *srv, gpointer param) {
	header_ctx *ctx = param;

	UNUSED(srv);

	g_string_free(ctx->key, TRUE);
	li_pattern_free(ctx->value);
	g_slice_free(header_ctx, ctx);
}

static liHandlerResult core_handle_header(liVRequest *vr, gpointer param, gpointer *context) {
	header_ctx *ctx = param;
	GMatchInfo *match_info = NULL;

	UNUSED(context);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	g_string_truncate(vr->wrk->tmp_str, 0);
	li_pattern_eval(vr, vr->wrk->tmp_str, ctx->value, NULL, NULL, li_pattern_regex_cb, match_info);

	ctx->cb(vr->response.headers, GSTR_LEN(ctx->key), GSTR_LEN(vr->wrk->tmp_str));

	return LI_HANDLER_GO_ON;
}

static liAction* core_header_add(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *l;
	liPattern *pat;
	header_ctx *ctx;
	UNUSED(wrk); UNUSED(p);

	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "'header.add/append/overwrite' action expects a string tuple as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	l = val->data.list;

	if (l->len != 2) {
		ERROR(srv, "'header.add/append/overwrite' action expects a string tuple as parameter, list has %u entries", l->len);
		return NULL;
	}

	if (g_array_index(l, liValue*, 0)->type != LI_VALUE_STRING || g_array_index(l, liValue*, 0)->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "'header.add/append/overwrite' action expects a string tuple as parameter");
		return NULL;
	}

	if (NULL == (pat = li_pattern_new(srv, g_array_index(l, liValue*, 1)->data.string->str))) {
		ERROR(srv, "%s", "'header.add/append/overwrite': parsing value pattern failed");
		return NULL;
	}

	ctx = g_slice_new(header_ctx);
	ctx->key = li_value_extract_string(g_array_index(l, liValue*, 0));
	ctx->value = pat;
	ctx->cb = (header_cb)(intptr_t)userdata;

	return li_action_new_function(core_handle_header, NULL, core_header_free, ctx);
}

static void core_header_remove_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_string_free(param, TRUE);
}

static liHandlerResult core_handle_header_remove(liVRequest *vr, gpointer param, gpointer *context) {
	GString *str = param;
	UNUSED(context);

	li_http_header_remove(vr->response.headers, GSTR_LEN(str));

	return LI_HANDLER_GO_ON;
}

static liAction* core_header_remove(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "'header.remove' action expects a string as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	return li_action_new_function(core_handle_header_remove, NULL, core_header_remove_free, li_value_extract_string(val));
}

/* chunkqueue memory limits */
static liHandlerResult core_handle_buffer_out(liVRequest *vr, gpointer param, gpointer *context) {
	gint limit = GPOINTER_TO_INT(param);
	UNUSED(context);

	li_cqlimit_set_limit(vr->out->limit, limit);

	return LI_HANDLER_GO_ON;
}

static liAction* core_buffer_out(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gint64 limit;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "'io.buffer_out' action expects an integer as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	limit = val->data.number;

	if (limit < 0) {
		limit = 0; /* no limit */
	} else if (limit < (16*1024)) {
		ERROR(srv, "buffer %"G_GINT64_FORMAT" is too low (need at least 16 kb)", limit);
		return NULL;
	} else if (limit > (1 << 30)) {
		ERROR(srv, "buffer %"G_GINT64_FORMAT" is too high (1GB is the maximum)", limit);
		return NULL;
	}

	return li_action_new_function(core_handle_buffer_out, NULL, NULL, GINT_TO_POINTER((gint) limit));
}

static liHandlerResult core_handle_buffer_in(liVRequest *vr, gpointer param, gpointer *context) {
	gint limit = GPOINTER_TO_INT(param);
	UNUSED(context);

	li_cqlimit_set_limit(vr->in->limit, limit);

	return LI_HANDLER_GO_ON;
}

static liAction* core_buffer_in(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gint64 limit;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "'io.buffer_in' action expects an integer as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	limit = val->data.number;

	if (limit < 0) {
		limit = 0; /* no limit */
	}
	if (limit > (1 << 30)) {
		ERROR(srv, "buffer %"G_GINT64_FORMAT" is too high (1GB is the maximum)", limit);
		return NULL;
	}

	return li_action_new_function(core_handle_buffer_in, NULL, NULL, GINT_TO_POINTER((gint) limit));
}


static void core_throttle_pool_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	li_throttle_pool_free(srv, param);
}

static liHandlerResult core_handle_throttle_pool(liVRequest *vr, gpointer param, gpointer *context) {
	liThrottlePool *pool = param;

	UNUSED(context);

	li_throttle_pool_acquire(vr, pool);

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_pool(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GString *name;
	liThrottlePool *pool = NULL;
	gint64 rate;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING && val->type != LI_VALUE_LIST) {
		ERROR(srv, "'io.throttle_pool' action expects a string or a string-number tuple as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	if (val->type == LI_VALUE_LIST) {
		if (val->data.list->len != 2
			|| g_array_index(val->data.list, liValue*, 0)->type != LI_VALUE_STRING
			|| g_array_index(val->data.list, liValue*, 1)->type != LI_VALUE_NUMBER) {

			ERROR(srv, "%s", "'io.throttle_pool' action expects a string or a string-number tuple as parameter");
			return NULL;
		}

		rate = g_array_index(val->data.list, liValue*, 1)->data.number;

		if (rate && rate < (32*1024)) {
			ERROR(srv, "io.throttle_pool: rate %"G_GINT64_FORMAT" is too low (32kbyte/s minimum)", rate);
			return NULL;
		}

		if (rate > (0xFFFFFFFF)) {
			ERROR(srv, "io.throttle_pool: rate %"G_GINT64_FORMAT" is too high (4gbyte/s maximum)", rate);
			return NULL;
		}

		name = li_value_extract_string(g_array_index(val->data.list, liValue*, 0));
	} else {
		name = li_value_extract_string(val);
		rate = 0;
	}

	pool = li_throttle_pool_new(srv, LI_THROTTLE_POOL_NAME, name, rate);

	if (!pool) {
		ERROR(srv, "io.throttle_pool: rate for pool '%s' hasn't been defined", name->str);
		return NULL;
	}

	if (rate != pool->rate && rate != 0) {
		ERROR(srv, "io.throttle_pool: pool '%s' already defined but with different rate (%ukbyte/s)", pool->data.name->str, pool->rate);
		return NULL;
	}

	return li_action_new_function(core_handle_throttle_pool, NULL, core_throttle_pool_free, pool);
}

static liHandlerResult core_handle_throttle_ip(liVRequest *vr, gpointer param, gpointer *context) {
	liThrottlePool *pool;
	gint rate = GPOINTER_TO_INT(param);

	UNUSED(context);

	pool = li_throttle_pool_new(vr->wrk->srv, LI_THROTTLE_POOL_IP, &vr->coninfo->remote_addr, rate);
	li_throttle_pool_acquire(vr, pool);

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_ip(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gint64 rate;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "'io.throttle_ip' action expects a positiv integer as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}


	rate = val->data.number;

	if (rate < 32*1024) {
		ERROR(srv, "io.throttle_pool: rate %"G_GINT64_FORMAT" is too low (32kbyte/s minimum)", rate);
		return NULL;
	}

	if (rate > 0xFFFFFFFF) {
		ERROR(srv, "io.throttle_pool: rate %"G_GINT64_FORMAT" is too high (4gbyte/s maximum)", rate);
		return NULL;
	}

	return li_action_new_function(core_handle_throttle_ip, NULL, NULL, GINT_TO_POINTER(rate));
}

static void core_throttle_connection_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_slice_free(liThrottleParam, param);
}


static liHandlerResult core_handle_throttle_connection(liVRequest *vr, gpointer param, gpointer *context) {
	liThrottleParam *throttle_param = param;

	UNUSED(context);

	vr->throttle.con.rate = throttle_param->rate;
	vr->throttled = TRUE;

	if (vr->throttle.pool.magazine) {
		gint supply = MAX(vr->throttle.pool.magazine, throttle_param->rate / 1000 * THROTTLE_GRANULARITY);
		vr->throttle.magazine += supply;
		vr->throttle.pool.magazine -= supply;
	} else {
		vr->throttle.magazine += throttle_param->burst;
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_connection(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liThrottleParam *param;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type == LI_VALUE_LIST && val->data.list->len == 2) {
		liValue *v1 = g_array_index(val->data.list, liValue*, 0);
		liValue *v2 = g_array_index(val->data.list, liValue*, 1);

		if (v1->type != LI_VALUE_NUMBER || v2->type != LI_VALUE_NUMBER) {
			ERROR(srv, "%s", "'io.throttle' action expects a positiv integer or a pair of those as parameter");
			return NULL;
		}

		if (v1->data.number > (0xFFFFFFFF) || v2->data.number > (0xFFFFFFFF)) {
			ERROR(srv, "%s", "io.throttle: rate or burst limit is too high (4gbyte/s maximum)");
			return NULL;
		}

		param = g_slice_new(liThrottleParam);
		param->rate = (guint)v2->data.number;
		param->burst = (guint)v1->data.number;
	} else if (val->type == LI_VALUE_NUMBER) {
		if (val->data.number > (0xFFFFFFFF)) {
			ERROR(srv, "io.throttle: rate %"G_GINT64_FORMAT" is too high (4gbyte/s maximum)", val->data.number);
			return NULL;
		}

		param = g_slice_new(liThrottleParam);
		param->rate = (guint)val->data.number;
		param->burst = 0;
	} else {
		ERROR(srv, "'io.throttle' action expects a positiv integer or a pair of those as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	if (param->rate && param->rate < (32*1024)) {
		g_slice_free(liThrottleParam, param);
		ERROR(srv, "io.throttle: rate %u is too low (32kbyte/s minimum or 0 for unlimited)", param->rate);
		return NULL;
	}

	return li_action_new_function(core_handle_throttle_connection, NULL, core_throttle_connection_free, param);
}

static void core_warmup(liServer *srv, liPlugin *p, gint32 id, GString *data) {
	UNUSED(p);
	UNUSED(id);
	UNUSED(data);

	li_server_goto_state(srv, LI_SERVER_WARMUP);
}

static void core_run(liServer *srv, liPlugin *p, gint32 id, GString *data) {
	UNUSED(p);
	UNUSED(id);
	UNUSED(data);

	li_server_goto_state(srv, LI_SERVER_RUNNING);
}

static void core_suspend(liServer *srv, liPlugin *p, gint32 id, GString *data) {
	UNUSED(p);
	UNUSED(id);
	UNUSED(data);

	li_server_goto_state(srv, LI_SERVER_SUSPENDED);
}

static const liPluginOption options[] = {
	{ "debug.log_request_handling", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ "static.range_requests", LI_VALUE_BOOLEAN, TRUE, NULL },

	{ "keepalive.timeout", LI_VALUE_NUMBER, 5, NULL },
	{ "keepalive.requests", LI_VALUE_NUMBER, 0, NULL },

	{ "etag.use", LI_VALUE_NONE, 0, core_option_etag_use_parse }, /* type in config is list, internal type is number for flags */

	{ "stat.async", LI_VALUE_BOOLEAN, TRUE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
	{ "log.timestamp", LI_VALUE_STRING, NULL, core_option_log_timestamp_parse, core_option_log_timestamp_free },
	{ "log", LI_VALUE_HASH, NULL, core_option_log_parse, core_option_log_free },

	{ "static.exclude_extensions", LI_VALUE_LIST, NULL, core_option_static_exclude_exts_parse, NULL },

	{ "server.name", LI_VALUE_STRING, NULL, NULL, NULL },
	{ "server.tag", LI_VALUE_STRING, PACKAGE_DESC, NULL, NULL },

	{ "mime_types", LI_VALUE_LIST, NULL, core_option_mime_types_parse, core_option_mime_types_free },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "list", core_list, NULL },
	{ "when", core_when, NULL },
	{ "set", core_set, NULL },

	{ "docroot", core_docroot, NULL },
	{ "alias", core_alias, NULL },
	{ "index", core_index, NULL },
	{ "static", core_static, NULL },
	{ "static_no_fail", core_static_no_fail, NULL },
	{ "pathinfo", core_pathinfo, NULL },

	{ "set_status", core_status, NULL },

	{ "log.write", core_log_write, NULL },

	{ "respond", core_respond, NULL },

	{ "env.set", core_env_set, NULL },
	{ "env.add", core_env_add, NULL },
	{ "env.remove", core_env_remove, NULL },
	{ "env.clear", core_env_clear, NULL },

	{ "header.add", core_header_add, (void*)(intptr_t)li_http_header_insert },
	{ "header.append", core_header_add, (void*)(intptr_t)li_http_header_append },
	{ "header.overwrite", core_header_add, (void*)(intptr_t)li_http_header_overwrite },
	{ "header.remove", core_header_remove, NULL },

	{ "io.buffer_out", core_buffer_out, NULL },
	{ "io.buffer_in", core_buffer_in, NULL },
	{ "io.throttle", core_throttle_connection, NULL },
	{ "io.throttle_pool", core_throttle_pool, NULL },
	{ "io.throttle_ip", core_throttle_ip, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "set_default", core_setup_set, NULL },
	{ "listen", core_listen, NULL },
	{ "workers", core_workers, NULL },
	{ "workers.cpu_affinity", core_workers_cpu_affinity, NULL },
	{ "module_load", core_module_load, NULL },
	{ "io.timeout", core_io_timeout, NULL },
	{ "stat_cache.ttl", core_stat_cache_ttl, NULL },
	{ "tasklet_pool.threads", core_tasklet_pool_threads, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginAngel angelcbs[] = {
	{ "warmup", core_warmup },
	{ "run", core_run },
	{ "suspend", core_suspend },

	{ NULL, NULL }
};
#include <sys/types.h>

static void plugin_core_prepare(liServer *srv, liPlugin *p) {
	guint i;

	UNUSED(p);

	/* initialize throttle pools that have not been yet */
	g_mutex_lock(srv->action_mutex);
	for (i = 0; i < srv->throttle_pools->len; i++) {
		liThrottlePool *pool = g_array_index(srv->throttle_pools, liThrottlePool*, i);

		if (!pool->worker_queues) {
			guint j;

			pool->worker_magazine = g_new0(gint, srv->worker_count);
			pool->worker_last_rearm = g_new0(gint, srv->worker_count);
			pool->worker_num_cons_queued = g_new0(gint, srv->worker_count);
			pool->worker_queues = g_new0(GQueue*, srv->worker_count);

			for (j = 0; j < srv->worker_count; j++) {
				pool->worker_queues[j] = g_queue_new();
				pool->worker_last_rearm[j] = pool->last_rearm;
			}
		}
	}
	g_mutex_unlock(srv->action_mutex);
}

static void plugin_core_prepare_worker(liServer *srv, liPlugin *p, liWorker *wrk) {
	UNUSED(p);

#if defined(LIGHTY_OS_LINUX)
	/* sched_setaffinity is only available on linux */
	{
		cpu_set_t mask;
		liValue *v = srv->workers_cpu_affinity;
		GArray *arr;

		if (!v)
			return;

		arr = v->data.list;

		if (wrk->ndx >= arr->len) {
			WARNING(srv, "worker #%u has no entry in workers.cpu_affinity", wrk->ndx+1);
			return;
		}

		CPU_ZERO(&mask);

		v = g_array_index(arr, liValue*, wrk->ndx);
		if (v->type == LI_VALUE_NUMBER) {
			CPU_SET(v->data.number, &mask);
			DEBUG(srv, "binding worker #%u to cpu %u", wrk->ndx+1, (guint)v->data.number);
		} else {
			guint i;

			g_string_truncate(wrk->tmp_str, 0);
			arr = v->data.list;

			for (i = 0; i < arr->len; i++) {
				CPU_SET(g_array_index(arr, liValue*, i)->data.number, &mask);
				g_string_append_printf(wrk->tmp_str, i ? ",%u":"%u", (guint)g_array_index(arr, liValue*, i)->data.number);
			}

			DEBUG(srv, "binding worker #%u to cpus %s", wrk->ndx+1, wrk->tmp_str->str);
		}

		if (0 != sched_setaffinity(0, sizeof(mask), &mask)) {
			ERROR(srv, "couldn't set cpu affinity mask for worker #%u: %s", wrk->ndx, g_strerror(errno));
		}
	}
#else
	UNUSED(srv);
	UNUSED(wrk);
#endif
}

void li_plugin_core_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;
	p->angelcbs = angelcbs;

	p->handle_prepare = plugin_core_prepare;
	p->handle_prepare_worker = plugin_core_prepare_worker;
}
