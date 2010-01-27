#ifndef _LIGHTTPD_PLUGIN_H_
#define _LIGHTTPD_PLUGIN_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#define INIT_FUNC(x) \
		LI_EXPORT void * x(server *srv, plugin *)

typedef void     (*liPluginInitCB)          (liServer *srv, liPlugin *p, gpointer userdata);
typedef void     (*liPluginFreeCB)          (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginParseOptionCB)   (liServer *srv, liPlugin *p, size_t ndx, liValue *val, liOptionValue *oval);
typedef gboolean (*liPluginParseOptionPtrCB)(liServer *srv, liPlugin *p, size_t ndx, liValue *val, gpointer *oval);
typedef void     (*liPluginFreeOptionPtrCB) (liServer *srv, liPlugin *p, size_t ndx, gpointer oval);
typedef liAction*(*liPluginCreateActionCB)  (liServer *srv, liPlugin *p, liValue *val, gpointer userdata);
typedef gboolean (*liPluginSetupCB)         (liServer *srv, liPlugin *p, liValue *val, gpointer userdata);
typedef void     (*liPluginAngelCB)         (liServer *srv, liPlugin *p, gint32 id, GString *data);

typedef void     (*liPluginHandleCloseCB)   (liConnection *con, liPlugin *p);
typedef liHandlerResult(*liPluginHandleVRequestCB)(liVRequest *vr, liPlugin *p);
typedef void     (*liPluginHandleVRCloseCB) (liVRequest *vr, liPlugin *p);

struct liPlugin {
	size_t version;
	const gchar *name; /**< name of the plugin */
	guint id;          /**< index in some plugin arrays */

	gpointer data;     /**< private plugin data */

	size_t opt_base_index, optptr_base_index;

	gboolean ready_for_next_state; /**< don't modify this; use li_plugin_ready_for_state() instead */

	liPluginFreeCB free;   /**< called before plugin is unloaded */

	liPluginHandleVRequestCB handle_request_body;

	/** called for every plugin after connection got closed (response end, reset by peer, error)
	  * the plugins code must not depend on any order of plugins loaded
	  */
	liPluginHandleCloseCB handle_close;

	/** called for every plugin after vrequest got reset */
	liPluginHandleVRCloseCB handle_vrclose;

	const liPluginOption *options;
	const liPluginOptionPtr *optionptrs;
	const liPluginAction *actions;
	const liPluginSetup *setups;
	const liPluginAngel *angelcbs;
};

struct liPluginOption {
	const gchar *name;
	liValueType type;

	gint64 default_value;
	liPluginParseOptionCB parse_option;
};

struct liPluginOptionPtr {
	const gchar *name;
	liValueType type;

	gpointer default_value;
	liPluginParseOptionPtrCB parse_option;
	liPluginFreeOptionPtrCB free_option;
};

struct liPluginAction {
	const gchar *name;
	liPluginCreateActionCB create_action;
	gpointer userdata;
};

struct liPluginSetup {
	const gchar *name;
	liPluginSetupCB setup;
	gpointer userdata;
};

struct liPluginAngel {
	const gchar *name;
	liPluginAngelCB angel_cb;
};

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

/* Needed by modules to register their plugin(s) */
LI_API liPlugin *li_plugin_register(liServer *srv, const gchar *name, liPluginInitCB init, gpointer userdata);

/* Internal needed functions */
LI_API void li_plugin_free(liServer *srv, liPlugin *p);
LI_API void li_server_plugins_free(liServer *srv);

LI_API void li_release_optionptr(liServer *srv, liOptionPtrValue *value);

LI_API void li_plugins_prepare_callbacks(liServer *srv);

/* server state machine callbacks */
LI_API void li_plugins_prepare_worker(liWorker *srv); /* blocking callbacks */
LI_API void li_plugins_prepare(liServer *srv); /* "prepare", async */

LI_API void li_plugins_start_listen(liServer *srv); /* "warmup" */
LI_API void li_plugins_stop_listen(liServer *srv); /* "prepare suspend", async */
LI_API void li_plugins_start_log(liServer *srv); /* "run" */
LI_API void li_plugins_stop_log(liServer *srv); /* "suspend now" */

LI_API void li_plugin_ready_for_state(liServer *srv, liPlugin *p, liServerState state);

LI_API void li_plugins_handle_close(liConnection *con);
LI_API void li_plugins_handle_vrclose(liVRequest *vr);

/* Needed for config frontends */
/** For parsing 'somemod.option = "somevalue"', free value after call */
LI_API liAction* li_option_action(liServer *srv, const gchar *name, liValue *val);
/** For parsing 'somemod.action value', e.g. 'rewrite "/url" => "/destination"'
  * free value after call
  */
LI_API liAction* li_create_action(liServer *srv, const gchar *name, liValue *val);
/** For setup function, e.g. 'listen "127.0.0.1:8080"'; free value after call */
LI_API gboolean li_call_setup(liServer *srv, const char *name, liValue *val);

/** free val after call */
LI_API gboolean li_plugin_set_default_option(liServer *srv, const gchar* name, liValue *val);

extern liOptionPtrValue li_option_ptr_zero;

/* needs vrequest *vr and plugin *p */
#define OPTION(idx) _OPTION(vr, p, idx)
#define _OPTION(vr, p, idx) (vr->options[p->opt_base_index + idx])
#define _OPTION_ABS(vr, idx) (vr->options[idx])
#define OPTIONPTR(idx) _OPTIONPTR(vr, p, idx)
#define _OPTIONPTR(vr, p, idx) (vr->optionptrs[p->optptr_base_index + idx] ? vr->optionptrs[p->optptr_base_index + idx]->data : li_option_ptr_zero.data)
#define _OPTIONPTR_ABS(vr, idx) (vr->optionptrs[idx] ? vr->optionptrs[idx]->data : li_option_ptr_zero.data)

#endif
