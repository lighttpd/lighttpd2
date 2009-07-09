#ifndef _LIGHTTPD_PLUGIN_H_
#define _LIGHTTPD_PLUGIN_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#define INIT_FUNC(x) \
		LI_EXPORT void * x(server *srv, plugin *)

#define PLUGIN_DATA \
	size_t id; \
	ssize_t option_base_ndx

typedef void     (*liPluginInitCB)          (liServer *srv, liPlugin *p);
typedef void     (*liPluginFreeCB)          (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginParseOptionCB)   (liServer *srv, liPlugin *p, size_t ndx, liValue *val, liOptionValue *oval);
typedef void     (*liPluginFreeOptionCB)    (liServer *srv, liPlugin *p, size_t ndx, liOptionValue oval);
typedef liAction*(*liPluginCreateActionCB)  (liServer *srv, liPlugin *p, liValue *val);
typedef gboolean (*liPluginSetupCB)         (liServer *srv, liPlugin *p, liValue *val);

typedef void     (*liPluginHandleCloseCB)   (liConnection *con, liPlugin *p);
typedef liHandlerResult(*liPluginHandleVRequestCB)(liVRequest *vr, liPlugin *p);
typedef void     (*liPluginHandleVRCloseCB) (liVRequest *vr, liPlugin *p);

struct liPlugin {
	size_t version;
	const gchar *name; /**< name of the plugin */
	guint id;          /**< index in some plugin arrays */

	gpointer data;     /**< private plugin data */

	size_t opt_base_index;

	liPluginFreeCB free;   /**< called before plugin is unloaded */

	liPluginHandleVRequestCB handle_request_body;

	/** called for every plugin after connection got closed (response end, reset by peer, error)
	  * the plugins code must not depend on any order of plugins loaded
	  */
	liPluginHandleCloseCB handle_close;

	/** called for every plugin after vrequest got reset */
	liPluginHandleVRCloseCB handle_vrclose;

	const liPluginOption *options;
	const liPluginAction *actions;
	const liliPluginSetupCB *setups;
};

struct liPluginOption {
	const gchar *name;
	liValueType type;

	gpointer default_value;
	liPluginParseOptionCB li_parse_option;
	liPluginFreeOptionCB free_option;
};

struct liPluginAction {
	const gchar *name;
	liPluginCreateActionCB li_create_action;
};

struct liliPluginSetupCB {
	const gchar *name;
	liPluginSetupCB setup;
};

/* Internal structures */
struct liServerOption {
	liPlugin *p;

	/** the value is freed with li_value_free after the parse call, so you
	  *   probably want to extract the content via li_value_extract*
	  * val is zero to get the global default value if nothing is specified
	  * save result in value
	  *
	  * Default behaviour (NULL) is to extract the inner value from val
	  */
	liPluginParseOptionCB li_parse_option;

	/** the free_option handler has to free all allocated resources;
	  * it may get called with 0 initialized options, so you have to
	  * check the value.
	  */
	liPluginFreeOptionCB free_option;

	/** if li_parse_option is NULL, the default_value is used; it is only used
	  * for the following value types:
	  * - BOOLEAN, NUMBER: casted with GPOINTER_TO_INT, i.e. set it with GINT_TO_POINTER
	  *     the numbers are limited to the 32-bit range according to the glib docs
	  * - STRING: used for g_string_new, i.e. a const char*
	  */
	gpointer default_value;

	size_t index, module_index;
	liValueType type;
};

struct liServerAction {
	liPlugin *p;
	liPluginCreateActionCB li_create_action;
};

struct liServerSetup {
	liPlugin *p;
	liPluginSetupCB setup;
};

/* Needed by modules to register their plugin(s) */
LI_API liPlugin *li_plugin_register(liServer *srv, const gchar *name, liPluginInitCB init);

/* Internal needed functions */
LI_API void li_plugin_free(liServer *srv, liPlugin *p);
LI_API void li_server_plugins_free(liServer *srv);

/** free val after call (val may be modified by parser) */
LI_API gboolean li_parse_option(liServer *srv, const char *name, liValue *val, liOptionSet *mark);
LI_API void li_release_option(liServer *srv, liOptionSet *mark); /**< Does not free the option_set memory */

LI_API void li_plugins_prepare_callbacks(liServer *srv);
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

/* needs vrequest *vr and plugin *p */
#define OPTION(idx) _OPTION(vr, p, idx)
#define _OPTION(vr, p, idx) (vr->options[p->opt_base_index + idx])
#define _OPTION_ABS(vr, idx) (vr->options[idx])

#endif
