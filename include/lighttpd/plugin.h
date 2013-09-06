#ifndef _LIGHTTPD_PLUGIN_H_
#define _LIGHTTPD_PLUGIN_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef void     (*liPluginInitCB)          (liServer *srv, liPlugin *p, gpointer userdata);
typedef void     (*liPluginFreeCB)          (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginParseOptionCB)   (liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, liOptionValue *oval);
typedef gboolean (*liPluginParseOptionPtrCB)(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, gpointer *oval);
typedef void     (*liPluginFreeOptionPtrCB) (liServer *srv, liPlugin *p, size_t ndx, gpointer oval);
typedef liAction*(*liPluginCreateActionCB)  (liServer *srv, liWorker *wrk, liPlugin *p, liValue *val, gpointer userdata);
typedef gboolean (*liPluginSetupCB)         (liServer *srv, liPlugin *p, liValue *val, gpointer userdata);
typedef void     (*liPluginAngelCB)         (liServer *srv, liPlugin *p, gint32 id, GString *data);

typedef void     (*liPluginServerStateWorker)(liServer *srv, liPlugin *p, liWorker *wrk);
typedef void     (*liPluginServerState)(liServer *srv, liPlugin *p);

typedef void     (*liPluginHandleCloseCB)   (liConnection *con, liPlugin *p);
typedef liHandlerResult(*liPluginHandleVRequestCB)(liVRequest *vr, liPlugin *p);
typedef void     (*liPluginHandleVRCloseCB) (liVRequest *vr, liPlugin *p);

typedef void     (*liPluginInitLua)(liLuaState *LL, liServer *srv, liWorker *wrk, liPlugin *p);


struct liPlugin {
	size_t version;
	const gchar *name; /**< name of the plugin */
	guint id;          /**< index in some plugin arrays */

	gpointer data;     /**< private plugin data */

	size_t opt_base_index, optptr_base_index;

	liPluginFreeCB free;   /**< called before plugin is unloaded */

	liPluginHandleVRequestCB handle_request_body;

	/** called for every plugin after connection got closed (response end, reset by peer, error)
	  * the plugins code must not depend on any order of plugins loaded
	  */
	liPluginHandleCloseCB handle_close;

	/** called for every plugin after vrequest got reset */
	liPluginHandleVRCloseCB handle_vrclose;

	liPluginServerStateWorker handle_prepare_worker; /**< called in the worker thread context once before running the workers */
	liPluginServerStateWorker handle_worker_stop;
	/* server state machine hooks */
	liPluginServerState handle_prepare, handle_start_listen, handle_stop_listen, handle_start_log, handle_stop_log;

	liPluginInitLua handle_init_lua;

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

/* Needed by modules to register their plugin(s) */
LI_API liPlugin *li_plugin_register(liServer *srv, const gchar *name, liPluginInitCB init, gpointer userdata);

/* Internal needed functions */
LI_API void li_plugin_free(liServer *srv, liPlugin *p);
LI_API void li_server_plugins_init(liServer *srv);
LI_API void li_server_plugins_free(liServer *srv);

LI_API void li_release_optionptr(liServer *srv, liOptionPtrValue *value);

LI_API void li_plugins_prepare_callbacks(liServer *srv);

/* server state machine callbacks */
LI_API void li_plugins_prepare_worker(liWorker *wrk); /* blocking callbacks */
LI_API void li_plugins_prepare(liServer *srv); /* "prepare", async */

LI_API void li_plugins_worker_stop(liWorker *wrk); /* blocking callbacks */

LI_API void li_plugins_start_listen(liServer *srv); /* "warmup" */
LI_API void li_plugins_stop_listen(liServer *srv); /* "prepare suspend", async */
LI_API void li_plugins_start_log(liServer *srv); /* "run" */
LI_API void li_plugins_stop_log(liServer *srv); /* "suspend now" */

LI_API void li_plugins_handle_close(liConnection *con);
LI_API void li_plugins_handle_vrclose(liVRequest *vr);

/* Needed for config frontends */
/* "val" gets freed in any case */
LI_API liAction *li_plugin_config_action(liServer *srv, liWorker *wrk, const gchar *name, liValue *val);
LI_API gboolean li_plugin_config_setup(liServer *srv, const char *name, liValue *val);

LI_API void li_plugins_init_lua(liLuaState *LL, liServer *srv, liWorker *wrk);

extern const liOptionPtrValue li_option_ptr_zero;

/* needs vrequest *vr and plugin *p */
#define OPTION(idx) _OPTION(vr, p, idx)
#define _OPTION(vr, p, idx) (vr->options[p->opt_base_index + idx])
#define _OPTION_ABS(vr, idx) (vr->options[idx])
#define OPTIONPTR(idx) _OPTIONPTR(vr, p, idx)
#define _OPTIONPTR(vr, p, idx) (vr->optionptrs[p->optptr_base_index + idx] ? vr->optionptrs[p->optptr_base_index + idx]->data : li_option_ptr_zero.data)
#define _OPTIONPTR_ABS(vr, idx) (vr->optionptrs[idx] ? vr->optionptrs[idx]->data : li_option_ptr_zero.data)

#endif
