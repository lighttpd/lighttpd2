
#include <lighttpd/base.h>

/* Internal structures */
struct liServerOption {
	liPlugin *p;

	/** the value is freed with li_value_free after the parse call, so you
	  *   probably want to extract pointers via li_value_extract_*
	  * val is zero to get the global default value if nothing is specified
	  * save result in value
	  *
	  * Default behaviour (NULL) is to extract the inner value from val
	  */
	liPluginParseOptionCB parse_option;

	/** if parse_option is NULL, the default_value is used */
	gint64 default_value;

	size_t index, module_index;
	liValueType type;
};

struct liServerOptionPtr {
	liPlugin *p;

	/** the value is freed with li_value_free after the parse call, so you
	  *   probably want to extract pointers via li_value_extract_*
	  * val is zero to get the global default value if nothing is specified
	  * save result in value
	  *
	  * Default behaviour (NULL) is to extract the inner value from val
	  */
	liPluginParseOptionPtrCB parse_option;

	/** the free_option handler has to free all allocated resources;
	  * it may get called with 0 initialized options, so you have to
	  * check the value.
	  */
	liPluginFreeOptionPtrCB free_option;

	/** if parse_option is NULL, the default_value is used; it is only used
	  * for the following value types:
	  * - STRING: used for g_string_new, i.e. a const char*
	  */
	gpointer default_value;

	size_t index, module_index;
	liValueType type;
};

struct liServerAction {
	liPlugin *p;
	liPluginCreateActionCB create_action;
	gpointer userdata;
};

struct liServerSetup {
	liPlugin *p;
	liPluginSetupCB setup;
	gpointer userdata;
};

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt, const char *name);
static gboolean plugin_load_default_optionptr(liServer *srv, liServerOptionPtr *sopt, const char *name);
static void li_plugin_free_default_options(liServer *srv, liPlugin *p);

const liOptionPtrValue li_option_ptr_zero = { 0, { 0 } , 0 };

static liPlugin* plugin_new(const gchar *name) {
	liPlugin *p = g_slice_new0(liPlugin);
	p->name = name;
	return p;
}

static void li_plugin_free_options(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginOption *po;
	liServerOption *so;
	const liPluginOptionPtr *pop;
	liServerOptionPtr *sop;

	if (p->options) for (i = 0; (po = &p->options[i])->name; i++) {
		if (NULL == (so = g_hash_table_lookup(srv->options, po->name))) break;
		if (so->p != p) break;
		g_hash_table_remove(srv->options, po->name);
	}

	if (p->optionptrs) for (i = 0; (pop = &p->optionptrs[i])->name; i++) {
		if (NULL == (sop = g_hash_table_lookup(srv->optionptrs, pop->name))) break;
		if (sop->p != p) break;
		g_hash_table_remove(srv->optionptrs, pop->name);
	}
}

static void li_plugin_free_actions(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginAction *pa;
	liServerAction *sa;

	if (!p->actions) return;
	for (i = 0; (pa = &p->actions[i])->name; i++) {
		if (NULL == (sa = g_hash_table_lookup(srv->actions, pa->name))) break;
		if (sa->p != p) break;
		g_hash_table_remove(srv->actions, pa->name);
	}
}

static void li_plugin_free_setups(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginSetup *ps;
	liServerSetup *ss;

	if (!p->setups) return;
	for (i = 0; (ps = &p->setups[i])->name; i++) {
		if (NULL == (ss = g_hash_table_lookup(srv->setups, ps->name))) break;
		if (ss->p != p) break;
		g_hash_table_remove(srv->setups, ps->name);
	}
}

void li_plugin_free(liServer *srv, liPlugin *p) {
	liServerState s;

	if (!p) return;

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s && LI_SERVER_DOWN != s) {
		ERROR(srv, "Cannot free plugin '%s' while server is running", p->name);
		return;
	}

	g_hash_table_remove(srv->plugins, p->name);
	li_plugin_free_default_options(srv, p);
	li_plugin_free_options(srv, p);
	li_plugin_free_actions(srv, p);
	li_plugin_free_setups(srv, p);
	if (p->free)
		p->free(srv, p);

	g_slice_free(liPlugin, p);
}

static void server_option_free(gpointer _so) {
	g_slice_free(liServerOption, _so);
}

static void server_optionptr_free(gpointer _so) {
	g_slice_free(liServerOptionPtr, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(liServerAction, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(liServerSetup, _ss);
}

void li_server_plugins_init(liServer *srv) {
	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_option_free);
	srv->optionptrs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_optionptr_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->li_plugins_handle_close = g_array_new(FALSE, TRUE, sizeof(liPlugin*));
	srv->li_plugins_handle_vrclose = g_array_new(FALSE, TRUE, sizeof(liPlugin*));
	srv->option_def_values = g_array_new(FALSE, TRUE, sizeof(liOptionValue));
	srv->optionptr_def_values = g_array_new(FALSE, TRUE, sizeof(liOptionPtrValue*));
}

void li_server_plugins_free(liServer *srv) {
	liServerState s;

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s && LI_SERVER_DOWN != s) {
		ERROR(srv, "%s", "Cannot free plugins while server is running");
		return;
	}

	g_array_free(srv->option_def_values, TRUE);
	{
		guint i;
		for (i = 0; i < srv->optionptr_def_values->len; i++) {
			li_release_optionptr(srv, g_array_index(srv->optionptr_def_values, liOptionPtrValue*, i));
		}
	}
	g_array_free(srv->optionptr_def_values, TRUE);

	{
		gpointer key, val;
		GHashTableIter i;
		g_hash_table_iter_init(&i, srv->plugins);
		while (g_hash_table_iter_next(&i, &key, &val)) {
			liPlugin *p = (liPlugin*) val;

			li_plugin_free_options(srv, p);
			li_plugin_free_actions(srv, p);
			li_plugin_free_setups(srv, p);
			if (p->free)
				p->free(srv, p);

			g_slice_free(liPlugin, p);
		}
	}
	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->optionptrs);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);

	g_array_free(srv->li_plugins_handle_close, TRUE);
	g_array_free(srv->li_plugins_handle_vrclose, TRUE);
}

static gboolean check_name_free(liServer *srv, liPlugin *p, const gchar *name, gboolean setup_ns, gboolean action_ns) {
	liServerOption *so;
	liServerOptionPtr *sop;
	liServerAction *sa;
	liServerSetup *ss;

	if (NULL != (so = (liServerOption*)g_hash_table_lookup(srv->options, name))) {
		ERROR(srv, "Name conflict: option '%s' already registered by plugin '%s', unloading '%s'",
			name,
			NULL != so->p ? so->p->name : "<none>",
			p->name);
		return FALSE;
	}
	if (NULL != (sop = (liServerOptionPtr*)g_hash_table_lookup(srv->optionptrs, name))) {
		ERROR(srv, "Name conflict: option '%s' already registered by plugin '%s', unloading '%s'",
			name,
			NULL != sop->p ? sop->p->name : "<none>",
			p->name);
		return FALSE;
	}
	if (action_ns && NULL != (sa = (liServerAction*)g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Name conflict: action '%s' already registered by plugin '%s', unloading '%s'",
			name,
			NULL != sa->p ? sa->p->name : "<none>",
			p->name);
		return FALSE;
	}
	if (setup_ns && NULL != (ss = (liServerSetup*)g_hash_table_lookup(srv->setups, name))) {
		ERROR(srv, "Name conflict: setup '%s' already registered by plugin '%s', unloading '%s'",
			name,
			NULL != ss->p ? ss->p->name : "<none>",
			p->name);
		return FALSE;
	}
	return TRUE;
}

liPlugin *li_plugin_register(liServer *srv, const gchar *name, liPluginInitCB init, gpointer userdata) {
	liPlugin *p;
	liServerState s;

	if (!init) {
		ERROR(srv, "Plugin '%s' needs an init function", name);
		return NULL;
	}

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s) {
		ERROR(srv, "Cannot register plugin '%s' after server was started", name);
		return NULL;
	}

	if (g_hash_table_lookup(srv->plugins, name)) {
		ERROR(srv, "Plugin '%s' already registered", name);
		return NULL;
	}

	p = plugin_new(name);
	p->id = g_hash_table_size(srv->plugins);
	g_hash_table_insert(srv->plugins, (gchar*) p->name, p);

	init(srv, p, userdata);
	p->opt_base_index = g_hash_table_size(srv->options);
	p->optptr_base_index = g_hash_table_size(srv->optionptrs);

	if (p->options) {
		size_t i;
		liServerOption *so;
		const liPluginOption *po;

		for (i = 0; (po = &p->options[i])->name; i++) {
			if (!check_name_free(srv, p, po->name, TRUE, TRUE)) goto fail;
			so = g_slice_new0(liServerOption);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->index = g_hash_table_size(srv->options);
			so->module_index = i;
			so->p = p;
			so->default_value = po->default_value;
			g_hash_table_insert(srv->options, (gchar*) po->name, so);
			plugin_load_default_option(srv, so, po->name);
		}
	}

	if (p->optionptrs) {
		size_t i;
		liServerOptionPtr *so;
		const liPluginOptionPtr *po;

		for (i = 0; (po = &p->optionptrs[i])->name; i++) {
			if (!check_name_free(srv, p, po->name, TRUE, TRUE)) goto fail;
			so = g_slice_new0(liServerOptionPtr);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->free_option = po->free_option;
			so->index = g_hash_table_size(srv->optionptrs);
			so->module_index = i;
			so->p = p;
			so->default_value = po->default_value;
			g_hash_table_insert(srv->optionptrs, (gchar*) po->name, so);
			plugin_load_default_optionptr(srv, so, po->name);
		}
	}

	if (p->actions) {
		size_t i;
		liServerAction *sa;
		const liPluginAction *pa;

		for (i = 0; (pa = &p->actions[i])->name; i++) {
			if (!check_name_free(srv, p, pa->name, FALSE, TRUE)) goto fail;
			sa = g_slice_new0(liServerAction);
			sa->create_action = pa->create_action;
			sa->p = p;
			sa->userdata = pa->userdata;
			g_hash_table_insert(srv->actions, (gchar*) pa->name, sa);
		}
	}

	if (p->setups) {
		size_t i;
		liServerSetup *ss;
		const liPluginSetup *ps;

		for (i = 0; (ps = &p->setups[i])->name; i++) {
			if (!check_name_free(srv, p, ps->name, TRUE, FALSE)) goto fail;
			ss = g_slice_new0(liServerSetup);
			ss->setup = ps->setup;
			ss->p = p;
			ss->userdata = ps->userdata;
			g_hash_table_insert(srv->setups, (gchar*) ps->name, ss);
		}
	}

	return p;

fail:
	li_plugin_free(srv, p);
	return NULL;
}


static liServerOption* find_option(liServer *srv, const char *name) {
	return (liServerOption*) g_hash_table_lookup(srv->options, name);
}

static gboolean li_parse_option(liServer *srv, liWorker *wrk, liServerOption *sopt, const char *name, liValue *val, liOptionSet *mark) {
	assert(NULL != srv && NULL != wrk && NULL != sopt && NULL != name && NULL != mark);

	if (NULL != val && LI_VALUE_LIST == sopt->type && val->type != LI_VALUE_LIST) {
		li_value_wrap_in_list(val);
	}

	if (NULL != val && sopt->type != val->type && sopt->type != LI_VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			li_value_type_string(val), li_valuetype_string(sopt->type), name);
		return FALSE;
	}

	if (NULL == sopt->parse_option) {
		switch (sopt->type) {
		case LI_VALUE_BOOLEAN:
			mark->value.boolean = (NULL == val) ? GPOINTER_TO_INT(sopt->default_value) : val->data.boolean;
			break;
		case LI_VALUE_NUMBER:
			mark->value.number = (NULL == val) ? GPOINTER_TO_INT(sopt->default_value) : val->data.number;
			break;
		default:
			ERROR(srv, "Invalid scalar option type '%s' for option %s",
				li_valuetype_string(sopt->type), name);
			return FALSE;
		}
	} else {
		if (!sopt->parse_option(srv, wrk, sopt->p, sopt->module_index, val, &mark->value)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	mark->ndx = sopt->index;

	return TRUE;
}

static liServerOptionPtr* find_optionptr(liServer *srv, const char *name) {
	return (liServerOptionPtr*) g_hash_table_lookup(srv->optionptrs, name);
}

static gboolean li_parse_optionptr(liServer *srv, liWorker *wrk, liServerOptionPtr *sopt, const char *name, liValue *val, liOptionPtrSet *mark) {
	liOptionPtrValue *oval;
	gpointer ptr = NULL;

	assert(NULL != srv && NULL != wrk && NULL != sopt && NULL != name && NULL != mark);

	if (NULL != val && LI_VALUE_LIST == sopt->type && val->type != LI_VALUE_LIST) {
		li_value_wrap_in_list(val);
	}

	if (NULL != val && sopt->type != val->type && sopt->type != LI_VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			li_value_type_string(val), li_valuetype_string(sopt->type), name);
		return FALSE;
	}

	if (NULL == sopt->parse_option) {
		if (NULL == val) {
			switch (sopt->type) {
			case LI_VALUE_STRING:
				ptr = g_string_new((const char*) sopt->default_value);
				break;
			default:
				ptr = NULL;
			}
		} else {
			ptr = li_value_extract_ptr(val);
		}
	} else {
		if (!sopt->parse_option(srv, wrk, sopt->p, sopt->module_index, val, &ptr)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (NULL != ptr) {
		oval = g_slice_new0(liOptionPtrValue);
		oval->refcount = 1;
		oval->sopt = sopt;
		oval->data.ptr = ptr;
	} else {
		oval = NULL;
	}

	mark->ndx = sopt->index;
	mark->value = oval;

	return TRUE;
}

void li_release_optionptr(liServer *srv, liOptionPtrValue *value) {
	liServerOptionPtr *sopt;
	assert(NULL != srv);

	if (NULL == value) return;

	assert(g_atomic_int_get(&value->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&value->refcount)) return;

	sopt = value->sopt;
	value->sopt = NULL;
	if (!sopt->free_option) {
		switch (sopt->type) {
		case LI_VALUE_NONE:
		case LI_VALUE_BOOLEAN:
		case LI_VALUE_NUMBER:
			/* Nothing to free */
			break;
		case LI_VALUE_STRING:
			if (value->data.string)
				g_string_free(value->data.string, TRUE);
			break;
		case LI_VALUE_LIST:
			if (value->data.list)
				li_value_list_free(value->data.list);
			break;
		case LI_VALUE_ACTION:
			if (value->data.action)
				li_action_release(srv, value->data.action);
			break;
		case LI_VALUE_CONDITION:
			if (value->data.cond)
				li_condition_release(srv, value->data.cond);
			break;
		}
	} else {
		sopt->free_option(srv, sopt->p, sopt->module_index, value->data.ptr);
	}
	g_slice_free(liOptionPtrValue, value);
}

static liValue* option_value(liValue *val) {
	if (li_value_list_has_len(val, 1)) return li_value_list_at(val, 0);
	if (li_value_list_has_len(val, 0)) return NULL;
	return val;
}

liAction *li_plugin_config_action(liServer *srv, liWorker *wrk, const gchar *name, liValue *val) {
	liAction *a = NULL;
	liServerAction *sa;
	liServerOption *sopt;
	liServerOptionPtr *soptptr;

	if (NULL != (sa = (liServerAction*) g_hash_table_lookup(srv->actions, name))) {
		if (NULL == (a = sa->create_action(srv, wrk, sa->p, val, sa->userdata))) {
			ERROR(srv, "Action '%s' creation failed", name);
		}
	} else if (NULL != (sopt = find_option(srv, name))) {
		liOptionSet setting;

		if (!li_parse_option(srv, wrk, sopt, name, option_value(val), &setting)) goto exit;

		a = li_action_new_setting(setting);
	} else if (NULL != (soptptr = find_optionptr(srv, name))) {
		liOptionPtrSet setting;

		if (!li_parse_optionptr(srv, wrk, soptptr, name, option_value(val), &setting)) goto exit;

		a = li_action_new_settingptr(setting);
	} else if (NULL != g_hash_table_lookup(srv->setups, name)) {
		ERROR(srv, "'%s' can only be called in a setup block", name);
	} else {
		ERROR(srv, "unknown action %s", name);
	}

exit:
	li_value_free(val);
	return a;
}

gboolean li_plugin_config_setup(liServer *srv, const char *name, liValue *val) {
	gboolean result = FALSE;
	liServerSetup *ss;
	liServerOption *sopt;
	liServerOptionPtr *soptptr;

	if (NULL != (ss = (liServerSetup*) g_hash_table_lookup(srv->setups, name))) {
		if (!ss->setup(srv, ss->p, val, ss->userdata)) {
			ERROR(srv, "Setup '%s' failed", name);
			goto exit;
		}
		result = TRUE;
	} else if (NULL != (sopt = find_option(srv, name))) {
		liOptionSet setting;

		if (!li_parse_option(srv, srv->main_worker, sopt, name, option_value(val), &setting)) goto exit;

		g_array_index(srv->option_def_values, liOptionValue, sopt->index) = setting.value;
		result = TRUE;
	} else if (NULL != (soptptr = find_optionptr(srv, name))) {
		liOptionPtrSet setting;

		if (!li_parse_optionptr(srv, srv->main_worker, soptptr, name, option_value(val), &setting)) goto exit;

		li_release_optionptr(srv, g_array_index(srv->optionptr_def_values, liOptionPtrValue*, soptptr->index));
		g_array_index(srv->optionptr_def_values, liOptionPtrValue*, soptptr->index) = setting.value;
		result = TRUE;
	} else if (NULL != g_hash_table_lookup(srv->setups, name)) {
		ERROR(srv, "'%s' can only be called in a setup block", name);
	} else {
		ERROR(srv, "unknown setup %s", name);
	}

exit:
	li_value_free(val);
	return result;
}

void li_plugins_prepare_callbacks(liServer *srv) {
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_close)
			g_array_append_val(srv->li_plugins_handle_close, p);
		if (p->handle_vrclose)
			g_array_append_val(srv->li_plugins_handle_vrclose, p);
	}
}

void li_plugins_handle_close(liConnection *con) {
	GArray *a = con->srv->li_plugins_handle_close;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		liPlugin *p = g_array_index(a, liPlugin*, i);
		p->handle_close(con, p);
	}
}

void li_plugins_handle_vrclose(liVRequest *vr) {
	GArray *a = vr->wrk->srv->li_plugins_handle_vrclose;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		liPlugin *p = g_array_index(a, liPlugin*, i);
		p->handle_vrclose(vr, p);
	}
}

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt, const char *name) {
	liOptionSet setting;
	assert(NULL != sopt);

	if (!li_parse_option(srv, srv->main_worker, sopt, name, NULL, &setting)) return FALSE;
	assert(setting.ndx == sopt->index);

	if (srv->option_def_values->len <= sopt->index)
		g_array_set_size(srv->option_def_values, sopt->index + 1);

	g_array_index(srv->option_def_values, liOptionValue, sopt->index) = setting.value;

	return TRUE;
}

static gboolean plugin_load_default_optionptr(liServer *srv, liServerOptionPtr *sopt, const char *name) {
	liOptionPtrSet setting;
	assert(NULL != sopt);

	if (!li_parse_optionptr(srv, srv->main_worker, sopt, name, NULL, &setting)) return FALSE;
	assert(setting.ndx == sopt->index);

	if (srv->optionptr_def_values->len <= sopt->index)
		g_array_set_size(srv->optionptr_def_values, sopt->index + 1);

	li_release_optionptr(srv, g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index));
	g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index) = setting.value;

	return TRUE;
}

static void li_plugin_free_default_options(liServer *srv, liPlugin *p) {
	GHashTableIter iter;
	gpointer k, v;

	g_hash_table_iter_init(&iter, srv->optionptrs);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		liServerOptionPtr *sopt = v;

		if (sopt->p != p)
			continue;

		li_release_optionptr(srv, g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index));
		g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index) = NULL;
	}
}

void li_plugins_prepare_worker(liWorker *wrk) { /* blocking callbacks */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;
	liServer *srv = wrk->srv;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_prepare_worker) {
			p->handle_prepare_worker(srv, p, wrk);
		}
	}
}
void li_plugins_prepare(liServer* srv) { /* "prepare", async */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_prepare) {
			p->handle_prepare(srv, p);
		}
	}
}

void li_plugins_worker_stop(liWorker *wrk) { /* blocking callbacks */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;
	liServer *srv = wrk->srv;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_worker_stop) {
			p->handle_worker_stop(srv, p, wrk);
		}
	}
}

void li_plugins_start_listen(liServer *srv) { /* "warmup" */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_start_listen) {
			p->handle_start_listen(srv, p);
		}
	}
}
void li_plugins_stop_listen(liServer *srv) { /* "prepare suspend", async */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_stop_listen) {
			p->handle_stop_listen(srv, p);
		}
	}
}
void li_plugins_start_log(liServer *srv) { /* "run" */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_start_log) {
			p->handle_start_log(srv, p);
		}
	}
}
void li_plugins_stop_log(liServer *srv) { /* "suspend now" */
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_stop_log) {
			p->handle_stop_log(srv, p);
		}
	}
}

void li_plugins_init_lua(liLuaState *LL, liServer *srv, liWorker *wrk) {
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	if (NULL == srv->plugins) return;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_init_lua) {
			p->handle_init_lua(LL, srv, wrk, p);
		}
	}
}
