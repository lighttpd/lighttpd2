
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

		/* if there's only one entry and we're not debug logging, don't stat */
		if (i == arr->len - 1 && !CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) break;

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
		if (NULL == pattern) return FALSE;
		g_array_append_val(arr, pattern);
	} else {
		for (i = 0; i < val->data.list->len; i++) {
			v = g_array_index(val->data.list, liValue*, i);

			if (v->type != LI_VALUE_STRING) {
				core_docroot_free(srv, arr);
				return NULL;
			}

			pattern = li_pattern_new(srv, v->data.string->str);
			if (NULL == pattern) {
				core_docroot_free(srv, arr);
				return FALSE;
			}
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
	if (vr->physical.path->str[vr->physical.path->len-1] == '/') return LI_HANDLER_GO_ON;

	if (exclude_arr) {
		guint i;
		GString *tmp_str = vr->wrk->tmp_str;
		gchar *basep = g_path_get_basename(vr->physical.path->str);
		g_string_assign(tmp_str, basep);
		g_free(basep);

		for (i = 0; i < exclude_arr->len; i++) {
			liValue *v = g_array_index(exclude_arr, liValue*, i);
			if (li_string_suffix(tmp_str, GSTR_LEN(v->data.string))) {
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
							li_chunkqueue_append_string(vr->direct_out, subheader);
							li_chunkqueue_append_chunkfile(vr->direct_out, cf, rs.range_start, rs.range_length);
						} else {
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Range"), GSTR_LEN(vr->wrk->tmp_str));
							li_chunkqueue_append_chunkfile(vr->direct_out, cf, rs.range_start, rs.range_length);
						}
						break;
					case LI_PARSE_HTTP_RANGE_DONE:
						ranged_response = TRUE;
						done = TRUE;
						vr->response.http_status = 206;
						if (is_multipart) {
							GString *subheader = g_string_sized_new(1023);
							g_string_append_printf(subheader, "\r\n--%s--\r\n", boundary);
							li_chunkqueue_append_string(vr->direct_out, subheader);

							g_string_printf(vr->wrk->tmp_str, "multipart/byteranges; boundary=%s", boundary);
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(vr->wrk->tmp_str));
						} else {
							li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));
						}
						break;
					case LI_PARSE_HTTP_RANGE_INVALID:
						done = TRUE;
						/* indirect handing: out cq is already "closed" */
						li_chunkqueue_reset(vr->direct_out); vr->direct_out->is_closed = TRUE;
						break;
					case LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE:
						ranged_response = TRUE;
						done = TRUE;
						li_chunkqueue_reset(vr->direct_out);
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
			li_chunkqueue_append_chunkfile(vr->direct_out, cf, 0, st.st_size);
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
		li_chunkqueue_append_mem(vr->direct_out, GSTR_LEN(vr->wrk->tmp_str));
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_respond(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	respond_param *rp;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	rp = g_slice_new(respond_param);

	if (!val) {
		/* respond; */
		rp->status_code = 200;
		rp->pattern = NULL;
	} else if (val->type == LI_VALUE_STRING) {
		/* respond "foo"; */
		rp->status_code = 200;
		rp->pattern = li_pattern_new(srv, val->data.string->str);

		if (!rp->pattern) {
			g_slice_free(respond_param, rp);
			ERROR(srv, "%s", "'respond' action takes an optional string as parameter");
			return NULL;
		}
	} else if (val->type == LI_VALUE_NUMBER) {
		/* respond 404; */
		rp->status_code = val->data.number;
		rp->pattern = NULL;
	} else if (val->type == LI_VALUE_LIST && val->data.list->len == 2 && g_array_index(val->data.list, liValue*, 0)->type == LI_VALUE_NUMBER && g_array_index(val->data.list, liValue*, 1)->type == LI_VALUE_STRING) {
		/* respond 200 => "foo"; */
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

static liLogMap* logmap_from_value(liServer *srv, liValue *val) {
	liLogMap *log_map;
	GHashTableIter iter;
	gpointer k, v;
	int level;
	GString *path;
	GString *level_str;

	if (NULL == val) {
		return li_log_map_new_default();
	}

	if (val->type != LI_VALUE_HASH) return NULL;

	log_map = li_log_map_new();

	g_hash_table_iter_init(&iter, val->data.hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		if (((liValue*)v)->type != LI_VALUE_STRING) {
			ERROR(srv, "log expects a hashtable with string values, %s given", li_value_type_string(((liValue*)v)->type));
			li_log_map_release(log_map);
			return NULL;
		}

		path = ((liValue*)v)->data.string;
		level_str = (GString*)k;

		if (g_str_equal(level_str->str, "*")) {
			for (guint i = 0; i < LI_LOG_LEVEL_COUNT; i++) {
				/* overwrite old path */
				if (NULL != log_map->targets[i]) {
					g_string_free(log_map->targets[i], TRUE);
				}

				log_map->targets[i] = g_string_new_len(GSTR_LEN(path));
			}
		} else {
			level = li_log_level_from_string(level_str);
			if (-1 == level) {
				ERROR(srv, "unknown log level '%s'", level_str->str);
				li_log_map_release(log_map);
				return NULL;
			}
			if (NULL != log_map->targets[level]) {
				g_string_free(log_map->targets[level], TRUE);
			}
			log_map->targets[level] = li_value_extract_string(v);
		}
	}

	return log_map;
}

static void core_log_free(liServer *srv, gpointer param) {
	liLogMap *log_map = param;

	UNUSED(srv);

	li_log_map_release(log_map);
}

static liHandlerResult core_handle_log(liVRequest *vr, gpointer param, gpointer *context) {
	liLogMap *log_map = param;

	UNUSED(context);

	li_log_context_set(&vr->log_context, log_map);

	return LI_HANDLER_GO_ON;
}

static liAction* core_log(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liLogMap *log_map;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val) {
		return li_action_new_function(core_handle_log, NULL, core_log_free, NULL);
	}

	if (val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "log expects a hashtable with string values");
		return NULL;
	}

	log_map = logmap_from_value(srv, val);
	if (NULL == log_map) return NULL;

	return li_action_new_function(core_handle_log, NULL, core_log_free, log_map);
}

static gboolean core_setup_log(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	liLogMap *log_map;
	UNUSED(p); UNUSED(userdata);

	if (!val) {
		log_map = li_log_map_new_default();
		li_log_context_set(&srv->logs.log_context, log_map);
		li_log_map_release(log_map);
		return TRUE;
	}

	if (val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "log expects a hashtable with string values");
		return FALSE;
	}

	log_map = logmap_from_value(srv, val);
	if (NULL == log_map) return FALSE;

	li_log_context_set(&srv->logs.log_context, log_map);
	li_log_map_release(log_map);

	return TRUE;
}

static gboolean core_setup_log_timestamp(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(p);
	UNUSED(userdata);

	if (NULL != srv->logs.timestamp.format) {
		g_string_free(srv->logs.timestamp.format, TRUE);
	}
	srv->logs.timestamp.format = li_value_extract_string(val);
	srv->logs.timestamp.last_ts = 0;

	return TRUE;
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

	li_chunkqueue_use_limit(vr->coninfo->resp->out, limit);

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

	li_chunkqueue_use_limit(vr->coninfo->req->out, limit);

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

typedef struct core_map_data core_map_data;
struct core_map_data {
	liPattern *pattern;
	GHashTable *hash;
	liAction *default_action;
};
static void core_map_free(liServer *srv, gpointer param) {
	core_map_data *md = param;

	UNUSED(srv);

	if (md->default_action)
		li_action_release(srv, md->default_action);

	li_pattern_free(md->pattern);
	g_hash_table_destroy(md->hash);
	g_slice_free(core_map_data, md);
}

static liHandlerResult core_handle_map(liVRequest *vr, gpointer param, gpointer *context) {
	liValue *v;
	core_map_data *md = param;

	UNUSED(context);

	g_string_truncate(vr->wrk->tmp_str, 0);
	li_pattern_eval(vr, vr->wrk->tmp_str, md->pattern, NULL, NULL, NULL, NULL);

	v = g_hash_table_lookup(md->hash, vr->wrk->tmp_str);
	if (v)
		li_action_enter(vr, v->data.val_action.action);
	else if (md->default_action)
		li_action_enter(vr, md->default_action);

	return LI_HANDLER_GO_ON;
}

static liAction* core_map(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	core_map_data *md;
	guint i;
	liValue *list, *l, *r, *v;
	liPattern *pattern;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST || val->data.list->len != 2) {
		ERROR(srv, "%s", "'map' action expects a string => (list of key => action pairs) as parameter");
		return NULL;
	}

	l = g_array_index(val->data.list, liValue*, 0);
	r = g_array_index(val->data.list, liValue*, 1);
	if (l->type != LI_VALUE_STRING || r->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "'map' action expects a string => (list of key => action pairs) as parameter");
		return NULL;
	}

	pattern = li_pattern_new(srv, l->data.string->str);
	if (!pattern) {
		ERROR(srv, "'map' action: failed to compile pattern '%s'", l->data.string->str);
		return NULL;
	}

	md = g_slice_new(core_map_data);
	md->pattern = pattern;
	md->default_action = NULL;
	md->hash = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		(GDestroyNotify) li_string_destroy_notify, (GDestroyNotify) li_value_free);

	list = r;

	for (i = 0; i < list->data.list->len; i++) {
		v = g_array_index(list->data.list, liValue*, i);

		if (v->type != LI_VALUE_LIST || v->data.list->len != 2) {
			ERROR(srv, "%s", "'map' action expects a string => (list of key => action pairs) as parameter");
			core_map_free(srv, md);
			return NULL;
		}

		l = g_array_index(v->data.list, liValue*, 0);
		r = g_array_index(v->data.list, liValue*, 1);

		if (r->type != LI_VALUE_ACTION) {
			ERROR(srv, "%s", "'map' action expects a string => (list of key => action pairs) as parameter");
			core_map_free(srv, md);
			return NULL;
		}

		if (l->type == LI_VALUE_NONE) {
			/* default action */
			md->default_action = li_value_extract_action(r);
		} else if (l->type == LI_VALUE_STRING) {
			/* string => action */
			g_hash_table_insert(md->hash, li_value_extract_string(l), li_value_copy(r));
		} else if (l->type == LI_VALUE_LIST) {
			/* (string, string, ...) => action */
			guint j;
			liValue *v2;

			for (j = 0; j < l->data.list->len; j++) {
				v2 = g_array_index(l->data.list, liValue*, j);

				if (v2->type != LI_VALUE_STRING) {
					ERROR(srv, "%s", "'map' action expects a string => (list of key => action pairs) as parameter");
					core_map_free(srv, md);
					return NULL;
				}

				g_hash_table_insert(md->hash, li_value_extract_string(v2), li_value_copy(r));
			}
		}
	}

	return li_action_new_function(core_handle_map, NULL, core_map_free, md);
}

static void fetch_files_static_lookup(liFetchDatabase* db, gpointer data, liFetchEntry *entry) {
	GHashTable *stringdb = (GHashTable*) data;
	UNUSED(db);
	entry->data = g_hash_table_lookup(stringdb, entry->key);
	li_fetch_entry_ready(entry);
}
static gboolean fetch_files_static_revalidate(liFetchDatabase* db, gpointer data, liFetchEntry *entry) {
	UNUSED(db); UNUSED(data); UNUSED(entry);
	return TRUE;
}
static void fetch_files_static_refresh(liFetchDatabase* db, gpointer data, liFetchEntry *cur_entry, liFetchEntry *new_entry) {
	UNUSED(db); UNUSED(data); UNUSED(cur_entry);
	li_fetch_entry_refresh_skip(new_entry);
}
static void fetch_files_static_free_db(gpointer data) {
	GHashTable *stringdb = (GHashTable*) data;
	g_hash_table_destroy(stringdb);
}

static const liFetchCallbacks fetch_files_static_callbacks = {
	fetch_files_static_lookup,
	fetch_files_static_revalidate,
	fetch_files_static_refresh,
	/* fetch_files_static_free_entry */ NULL,
	fetch_files_static_free_db
};

static gboolean core_register_fetch_files_static(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	const gchar *name;
	gchar *wildcard, *tmp;
	const gchar *_entry;
	GString *pattern;
	GString *basedir = NULL, *subfile = NULL;
	GString *prefix = NULL, *suffix = NULL;
	GString *filename = NULL;
	GDir *dir = NULL;
	GError *err = NULL;
	gboolean result = FALSE;
	GHashTable *stringdb = NULL;
	liFetchDatabase *db = NULL;
	UNUSED(p); UNUSED(userdata);

	if (LI_VALUE_LIST != val->type || 2 != val->data.list->len
		|| LI_VALUE_STRING != g_array_index(val->data.list, liValue*, 0)->type
		|| LI_VALUE_STRING != g_array_index(val->data.list, liValue*, 1)->type) {
		ERROR(srv, "%s", "fetch.files_static expects a two strings as parameter: \"<name>\" => \"/path/abc_*.d/file\"");
		goto out;
	}

	name = g_array_index(val->data.list, liValue*, 0)->data.string->str;
	pattern = g_array_index(val->data.list, liValue*, 1)->data.string;

	wildcard = strchr(pattern->str, '*');
	if (NULL == wildcard || NULL != strchr(wildcard + 1, '*')) {
		ERROR(srv, "%s", "fetch.files_static file pattern doesn't contain exactly one wildcard");
		goto out;
	}

	for (tmp = wildcard; tmp >= pattern->str; --tmp) {
		if ('/' == *tmp) break;
	}
	if (tmp >= pattern->str) { /* found '/' before '*' */
		basedir = g_string_new_len(pattern->str, tmp - pattern->str);
		prefix = g_string_new_len(tmp + 1, wildcard - tmp - 1);
	} else {
		prefix = g_string_new_len(pattern->str, wildcard - pattern->str);
	}
	if (NULL != (tmp = strchr(wildcard, '/'))) {
		subfile = g_string_new(tmp);
		suffix = g_string_new_len(wildcard + 1, tmp - wildcard - 1);
	} else {
		suffix = g_string_new(wildcard + 1);
	}

	filename = g_string_sized_new(127);
	dir = g_dir_open(NULL != basedir ? basedir->str : ".", 0, &err);
	if (NULL == dir) {
		ERROR(srv, "fetch.files_static: couldn't open basedir '%s': %s", NULL != basedir ? basedir->str : ".", err->message);
		goto out;
	}

	stringdb = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, li_g_string_free, li_g_string_free);
	while (NULL != (_entry = g_dir_read_name(dir))) {
		GString entry = li_const_gstring(_entry, strlen(_entry));
		if (entry.len <= prefix->len + suffix->len) continue;
		if (li_string_prefix(&entry, GSTR_LEN(prefix)) && li_string_suffix(&entry, GSTR_LEN(suffix))) {
			GString *key;
			GString *file;
			gchar *file_contents;
			gsize file_len;

			g_string_truncate(filename, 0);
			if (NULL != basedir) {
				g_string_append_len(filename, GSTR_LEN(basedir));
				li_path_append_slash(filename);
			}
			g_string_append_len(filename, GSTR_LEN(&entry));
			if (NULL != subfile) {
				li_path_append_slash(filename);
				g_string_append_len(filename, GSTR_LEN(subfile));
			}

			if (!g_file_test(filename->str, G_FILE_TEST_IS_REGULAR)) continue;

			if (!g_file_get_contents(filename->str, &file_contents, &file_len, &err)) {
				ERROR(srv, "fetch.files_static: couldn't read file '%s': %s", filename->str, err->message);
				goto out;
			}

			key = g_string_new_len(entry.str + prefix->len, entry.len - prefix->len - suffix->len);
			file = g_string_new_len(file_contents, file_len);
			g_free(file_contents);

			g_hash_table_insert(stringdb, key, file);
		}
	}

	db = li_fetch_database_new(&fetch_files_static_callbacks, stringdb, g_hash_table_size(stringdb), 0);
	stringdb = NULL; /* now owned by db */

	if (!li_server_register_fetch_database(srv, name, db)) {
		ERROR(srv, "fetch.files_static: duplicate name: can't register another backend for name '%s'", name);
		goto out;
	}

	result = TRUE;

out:
	if (NULL != basedir) g_string_free(basedir, TRUE);
	if (NULL != subfile) g_string_free(basedir, TRUE);
	if (NULL != prefix) g_string_free(prefix, TRUE);
	if (NULL != suffix) g_string_free(suffix, TRUE);
	if (NULL != filename) g_string_free(filename, TRUE);
	if (NULL != dir) g_dir_close(dir);
	if (NULL != err) g_error_free(err);
	if (NULL != stringdb) g_hash_table_destroy(stringdb);
	if (NULL != db) li_fetch_database_release(db);
	return result;
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

	{ "buffer_request_body", LI_VALUE_BOOLEAN, TRUE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
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

	{ "log", core_log, NULL },
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

	{ "map", core_map, NULL },

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
	{ "log", core_setup_log, NULL },
	{ "log.timestamp", core_setup_log_timestamp, NULL },
	{ "fetch.files_static", core_register_fetch_files_static, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginAngel angelcbs[] = {
	{ "warmup", core_warmup },
	{ "run", core_run },
	{ "suspend", core_suspend },

	{ NULL, NULL }
};
#include <sys/types.h>

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

	p->handle_prepare_worker = plugin_core_prepare_worker;
}
