
#include <lighttpd/base.h>
#include <lighttpd/config_parser.h>
#include <lighttpd/profiler.h>
#include <lighttpd/plugin_core.h>

#include <lighttpd/version.h>

#ifdef HAVE_LUA_H
# include <lighttpd/config_lua.h>
#endif

#ifndef DEFAULT_LIBDIR
# define DEFAULT_LIBDIR "/usr/local/lib/lighttpd"
#endif

int main(int argc, char *argv[]) {
	GError *error = NULL;
	GOptionContext *context;
	liServer *srv;
	gboolean res;
	gboolean free_config_path = TRUE;

	gchar *config_path = NULL;
	const gchar *def_module_dir = DEFAULT_LIBDIR;
	const gchar *module_dir = def_module_dir;
	gboolean luaconfig = FALSE;
	gboolean test_config = FALSE;
	gboolean show_version = FALSE;
	gboolean use_angel = FALSE;

	GList *ctx_stack = NULL;

	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
		{ "lua", 'l', 0, G_OPTION_ARG_NONE, &luaconfig, "use the lua config frontend", NULL },
		{ "test", 't', 0, G_OPTION_ARG_NONE, &test_config, "test config and exit", NULL },
		{ "module-dir", 'm', 0, G_OPTION_ARG_STRING, &module_dir, "module directory [default: " DEFAULT_LIBDIR "]", "PATH" },
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "show version and exit", NULL },
		{ "angel", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &use_angel, "spawned by angel", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	/* check for environment variable LIGHTY_PROFILE_MEM */
	gchar *profile_mem = getenv("LIGHTY_PROFILE_MEM");
	if (profile_mem != NULL && g_str_equal(profile_mem, "true")) {
		/*g_mem_set_vtable(glib_mem_profiler_table);*/
		li_profiler_enable();
		atexit(li_profiler_finish);
		atexit(li_profiler_dump);
		/*atexit(profiler_dump_table);*/
	}

	/* parse commandline options */
	context = g_option_context_new("- fast and lightweight webserver");
	g_option_context_add_main_entries(context, entries, NULL);

	res = g_option_context_parse(context, &argc, &argv, &error);

	g_option_context_free(context);

	if (!res) {
		g_printerr("failed to parse command line arguments: %s\n", error->message);
		g_error_free(error);
		return 1;
	}

	/* -v, show version and exit */
	if (show_version) {
		g_print(PACKAGE_DESC " - a fast and lightweight webserver\n");
		g_print("Build date: %s\n", PACKAGE_BUILD_DATE);
#ifdef LIGHTTPD_REVISION
		g_print("Revision: %s\n", LIGHTTPD_REVISION);
#endif
		return 0;
	}

	/* initialize threading */
	g_thread_init(NULL);

	srv = li_server_new(module_dir);
	li_server_loop_init(srv);

	/* load core plugin */
	srv->core_plugin = li_plugin_register(srv, "core", li_plugin_core_init);
	if (use_angel) {
		li_angel_setup(srv);
	}

	/* if no path is specified for the config, read lighttpd.conf from current directory */
	if (config_path == NULL) {
		config_path = "lighttpd.conf";
		free_config_path = FALSE;
	}

	DEBUG(srv, "config path: %s", config_path);

	if (!luaconfig) {
		GTimeVal start, end;
		gulong s, millis, micros;
		guint64 d;
		liAction *a;
		liConfigParserContext *ctx;

		g_get_current_time(&start);

		/* standard config frontend */
		ctx_stack = li_config_parser_init(srv);
		ctx = (liConfigParserContext*) ctx_stack->data;
		if (!li_config_parser_file(srv, ctx_stack, config_path)) {
			li_config_parser_finish(srv, ctx_stack, TRUE);
			return 1; /* no cleanup on config error, same as config test */
		}

		/* append fallback "static" action */
		a = li_create_action(srv, "static", NULL);
		g_array_append_val(srv->mainaction->data.list, a);

		g_get_current_time(&end);
		d = end.tv_sec - start.tv_sec;
		d *= 1000000;
		d += end.tv_usec - start.tv_usec;
		s = d / 1000000;
		millis = (d - s) / 1000;
		micros = (d - s - millis) %1000;
		DEBUG(srv, "parsed config file in %lu seconds, %lu milliseconds, %lu microseconds", s, millis, micros);

		if (g_queue_get_length(ctx->option_stack) != 0 || g_queue_get_length(ctx->action_list_stack) != 1)
			DEBUG(srv, "option_stack: %u action_list_stack: %u (should be 0:1)", g_queue_get_length(ctx->option_stack), g_queue_get_length(ctx->action_list_stack));

		li_config_parser_finish(srv, ctx_stack, FALSE);
	}
	else {
#ifdef HAVE_LUA_H
		li_config_lua_load(srv, config_path, &srv->mainaction);
		/* lua config frontend */
#else
		g_print("lua config frontend not available\n");
		return 1;
#endif
	}

	if (!srv->mainaction) {
		ERROR(srv, "%s", "No action handlers defined");
		return 1;
	}

	/* if config should only be tested, exit here  */
	if (test_config)
		return 0;

	/* TRACE(srv, "%s", "Test!"); */

	li_server_reached_state(srv, LI_SERVER_LOADING);
	li_worker_run(srv->main_worker);
	li_server_reached_state(srv, LI_SERVER_DOWN);

	if (!luaconfig)
		li_config_parser_finish(srv, ctx_stack, TRUE);

	INFO(srv, "%s", "going down");

	li_server_free(srv);

	if (module_dir != def_module_dir)
		g_free((gpointer)module_dir);

	if (free_config_path)
		g_free(config_path);

	return 0;
}
