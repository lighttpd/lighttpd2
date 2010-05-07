
#include <lighttpd/base.h>

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt);
static gboolean plugin_load_default_optionptr(liServer *srv, liServerOptionPtr *sopt);
static void li_plugin_free_default_options(liServer *srv, liPlugin *p);

liOptionPtrValue li_option_ptr_zero = { 0, { 0 } , 0 };

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

void li_server_plugins_free(liServer *srv) {
	gpointer key, val;
	GHashTableIter i;
	liServerState s;

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s && LI_SERVER_DOWN != s) {
		ERROR(srv, "%s", "Cannot free plugins while server is running");
		return;
	}

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
	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);
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
		liServerOptionPtr *sop;
		const liPluginOption *po;

		for (i = 0; (po = &p->options[i])->name; i++) {
			if (NULL != (so = (liServerOption*)g_hash_table_lookup(srv->options, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					so->p ? so->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			if (NULL != (sop = (liServerOptionPtr*)g_hash_table_lookup(srv->optionptrs, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					sop->p ? sop->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			so = g_slice_new0(liServerOption);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->index = g_hash_table_size(srv->options);
			so->module_index = i;
			so->p = p;
			so->default_value = po->default_value;
			g_hash_table_insert(srv->options, (gchar*) po->name, so);
			plugin_load_default_option(srv, so);
		}
	}

	if (p->optionptrs) {
		size_t i;
		liServerOption *so_;
		liServerOptionPtr *so;
		const liPluginOptionPtr *po;

		for (i = 0; (po = &p->optionptrs[i])->name; i++) {
			if (NULL != (so_ = (liServerOption*)g_hash_table_lookup(srv->options, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					so_->p ? so_->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			if (NULL != (so = (liServerOptionPtr*)g_hash_table_lookup(srv->optionptrs, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					so->p ? so->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			so = g_slice_new0(liServerOptionPtr);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->free_option = po->free_option;
			so->index = g_hash_table_size(srv->optionptrs);
			so->module_index = i;
			so->p = p;
			so->default_value = po->default_value;
			g_hash_table_insert(srv->optionptrs, (gchar*) po->name, so);
			plugin_load_default_optionptr(srv, so);
		}
	}

	if (p->actions) {
		size_t i;
		liServerAction *sa;
		const liPluginAction *pa;

		for (i = 0; (pa = &p->actions[i])->name; i++) {
			if (NULL != (sa = (liServerAction*)g_hash_table_lookup(srv->actions, pa->name))) {
				ERROR(srv, "Action '%s' already registered by plugin '%s', unloading '%s'",
					pa->name,
					sa->p ? sa->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
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
			if (NULL != (ss = (liServerSetup*)g_hash_table_lookup(srv->setups, ps->name))) {
				ERROR(srv, "Setup '%s' already registered by plugin '%s', unloading '%s'",
					ps->name,
					ss->p ? ss->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			ss = g_slice_new0(liServerSetup);
			ss->setup = ps->setup;
			ss->p = p;
			ss->userdata = ps->userdata;
			g_hash_table_insert(srv->setups, (gchar*) ps->name, ss);
		}
	}

	return p;
}


static liServerOption* find_option(liServer *srv, const char *name) {
	return (liServerOption*) g_hash_table_lookup(srv->options, name);
}

static gboolean li_parse_option(liServer *srv, liWorker *wrk, liServerOption *sopt, const char *name, liValue *val, liOptionSet *mark) {
	if (!srv || !wrk || !name || !mark || !sopt) return FALSE;

	if (sopt->type != val->type && sopt->type != LI_VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			li_value_type_string(val->type), li_value_type_string(sopt->type), name);
		return FALSE;
	}

	if (!sopt->parse_option) {
		switch (sopt->type) {
		case LI_VALUE_BOOLEAN:
			mark->value.boolean = val->data.boolean;
			break;
		case LI_VALUE_NUMBER:
			mark->value.number = val->data.number;
			break;
		default:
			ERROR(srv, "Invalid scalar option type '%s' for option %s",
				li_value_type_string(sopt->type), name);
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

	if (!srv || !wrk || !name || !mark || !sopt) return FALSE;

	if (sopt->type != val->type && sopt->type != LI_VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			li_value_type_string(val->type), li_value_type_string(sopt->type), name);
		return FALSE;
	}

	if (!sopt->parse_option) {
		ptr = li_value_extract_ptr(val);
	} else {
		if (!sopt->parse_option(srv, wrk, sopt->p, sopt->module_index, val, &ptr)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (ptr) {
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

	if (!srv || !value) return;

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
		case LI_VALUE_HASH:
			if (value->data.hash)
				g_hash_table_destroy(value->data.hash);
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

liAction* li_option_action(liServer *srv, liWorker *wrk, const gchar *name, liValue *val) {
	liServerOption *sopt;
	liServerOptionPtr *soptptr;

	if (NULL != (sopt = find_option(srv, name))) {
		liOptionSet setting;

		if (!li_parse_option(srv, wrk, sopt, name, val, &setting)) {
			return NULL;
		}

		return li_action_new_setting(setting);
	} else if (NULL != (soptptr = find_optionptr(srv, name))) {
		liOptionPtrSet setting;

		if (!li_parse_optionptr(srv, wrk, soptptr, name, val, &setting)) {
			return NULL;
		}

		return li_action_new_settingptr(setting);
	} else {
		ERROR(srv, "Unknown option '%s'", name);
		return FALSE;
	}
}

liAction* li_create_action(liServer *srv, liWorker *wrk, const gchar *name, liValue *val) {
	liAction *a;
	liServerAction *sa;

	if (NULL == (sa = (liServerAction*) g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Action '%s' doesn't exist", name);
		return NULL;
	}

	if (NULL == (a = sa->create_action(srv, wrk, sa->p, val, sa->userdata))) {
		ERROR(srv, "Action '%s' creation failed", name);
		return NULL;
	}

	return a;
}

gboolean li_call_setup(liServer *srv, const char *name, liValue *val) {
	liServerSetup *ss;

	if (NULL == (ss = (liServerSetup*) g_hash_table_lookup(srv->setups, name))) {
		ERROR(srv, "Setup function '%s' doesn't exist", name);
		return FALSE;
	}

	if (!ss->setup(srv, ss->p, val, ss->userdata)) {
		ERROR(srv, "Setup '%s' failed", name);
		return FALSE;
	}

	return TRUE;
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

gboolean li_plugin_set_default_option(liServer* srv, const gchar* name, liValue* val) {
	liServerOption *sopt;
	liServerOptionPtr *soptptr;

	if (NULL != (sopt = find_option(srv, name))) {
		liOptionSet setting;

		/* assign new value */
		if (!li_parse_option(srv, srv->main_worker, sopt, name, val, &setting)) {
			return FALSE;
		}

		g_array_index(srv->option_def_values, liOptionValue, sopt->index) = setting.value;
	} else if (NULL != (soptptr = find_optionptr(srv, name))) {
		liOptionPtrSet setting;

		/* assign new value */
		if (!li_parse_optionptr(srv, srv->main_worker, soptptr, name, val, &setting)) {
			return FALSE;
		}

		g_array_index(srv->optionptr_def_values, liOptionPtrValue*, soptptr->index) = setting.value;
	} else {
		ERROR(srv, "unknown option \"%s\"", name);
		return FALSE;
	}

	return TRUE;
}

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt) {
	liOptionValue oval = {0};

	if (!sopt)
		return FALSE;

	if (!sopt->parse_option) {
		switch (sopt->type) {
		case LI_VALUE_BOOLEAN:
			oval.boolean = GPOINTER_TO_INT(sopt->default_value);
			break;
		case LI_VALUE_NUMBER:
			oval.number = GPOINTER_TO_INT(sopt->default_value);
			break;
		default:
			ERROR(srv, "Invalid type '%s' for scalar option",
				li_value_type_string(sopt->type));
			return FALSE;
		}
	} else {
		if (!sopt->parse_option(srv, srv->main_worker, sopt->p, sopt->module_index, NULL, &oval)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (srv->option_def_values->len <= sopt->index)
		g_array_set_size(srv->option_def_values, sopt->index + 1);

	g_array_index(srv->option_def_values, liOptionValue, sopt->index) = oval;

	return TRUE;
}

static gboolean plugin_load_default_optionptr(liServer *srv, liServerOptionPtr *sopt) {
	gpointer ptr = NULL;
	liOptionPtrValue *oval = NULL;

	if (!sopt)
		return FALSE;

	if (!sopt->parse_option) {
		switch (sopt->type) {
		case LI_VALUE_STRING:
			ptr = g_string_new((const char*) sopt->default_value);
			break;
		default:
			ptr = NULL;
		}
	} else {
		if (!sopt->parse_option(srv, srv->main_worker, sopt->p, sopt->module_index, NULL, &ptr)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (ptr) {
		oval = g_slice_new0(liOptionPtrValue);
		oval->refcount = 1;
		oval->data.ptr = ptr;
		oval->sopt = sopt;
	}

	if (srv->optionptr_def_values->len <= sopt->index)
		g_array_set_size(srv->optionptr_def_values, sopt->index + 1);

	li_release_optionptr(srv, g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index));
	g_array_index(srv->optionptr_def_values, liOptionPtrValue*, sopt->index) = oval;

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
