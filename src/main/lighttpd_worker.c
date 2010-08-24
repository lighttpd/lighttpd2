
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <lighttpd/version.h>

#ifdef WITH_PROFILER
# include <lighttpd/profiler.h>
#endif

#ifdef HAVE_LUA_H
# include <lighttpd/config_lua.h>
#endif

#ifndef WITHOUT_CONFIG_PARSER
# include <lighttpd/config_parser.h>
#endif

#ifndef DEFAULT_LIBDIR
# define DEFAULT_LIBDIR "/usr/local/lib/lighttpd2"
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
	gboolean module_resident = FALSE;
	gboolean luaconfig = FALSE;
	gboolean test_config = FALSE;
	gboolean show_version = FALSE;
	gboolean use_angel = FALSE;

	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
		{ "lua", 'l', 0, G_OPTION_ARG_NONE, &luaconfig, "use the lua config frontend", NULL },
		{ "test", 't', 0, G_OPTION_ARG_NONE, &test_config, "test config and exit", NULL },
		{ "module-dir", 'm', 0, G_OPTION_ARG_STRING, &module_dir, "module directory [default: " DEFAULT_LIBDIR "]", "PATH" },
		{ "module-resident", 0, 0, G_OPTION_ARG_NONE, &module_resident, "never unload modules (e.g. for valgrind)", NULL },
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "show version and exit", NULL },
		{ "angel", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &use_angel, "spawned by angel", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

#ifdef WITH_PROFILER
	{
		/* check for environment variable LIGHTY_PROFILE_MEM */
		gchar *profile_mem = getenv("LIGHTY_PROFILE_MEM");
		if (profile_mem) {
			/*g_mem_set_vtable(glib_mem_profiler_table);*/
			li_profiler_enable(profile_mem);
			atexit(li_profiler_finish);
			atexit(li_profiler_dump);
			/*atexit(profiler_dump_table);*/
		}
	}
#endif

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

	srv = li_server_new(module_dir, module_resident);
	li_server_loop_init(srv);

	/* load core plugin */
	srv->core_plugin = li_plugin_register(srv, "core", li_plugin_core_init, NULL);
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
#ifndef WITHOUT_CONFIG_PARSER
		if (!li_config_parse(srv, config_path))
			return 1;
#else
		g_print("standard config frontend not available\n");
		return 1;
#endif
	}
	else {
#ifdef HAVE_LUA_H
		li_config_lua_load(srv->L, srv, srv->main_worker, config_path, &srv->mainaction, TRUE, NULL);
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

	INFO(srv, "%s", "going down");

	li_server_free(srv);

	if (module_dir != def_module_dir)
		g_free((gpointer)module_dir);

	if (free_config_path)
		g_free(config_path);

	mempool_cleanup();

	return 0;
}
