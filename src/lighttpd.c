
#include "base.h"
#include "config_parser.h"

#ifdef HAVE_LUA_H
#include "config_lua.h"
#endif

void plugin_core_init(server *srv, plugin *p);

int main(int argc, char *argv[]) {
	GError *error;
	GOptionContext *context;
	server *srv;
	gboolean res;

	gchar *config_path = NULL;
	gboolean luaconfig = FALSE;
	gboolean test_config = FALSE;
	gboolean show_version = FALSE;

	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
		{ "lua", 'l', 0, G_OPTION_ARG_NONE, &luaconfig, "use the lua config frontend", NULL },
		{ "test", 't', 0, G_OPTION_ARG_NONE, &test_config, "test config and exit", NULL },
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "show version and exit", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};


	/* parse commandline options */
	context = g_option_context_new("- fast and lightweight webserver");
	g_option_context_add_main_entries(context, entries, NULL);

	res = g_option_context_parse(context, &argc, &argv, &error);

	g_option_context_free(context);

	if (!res) {
		g_printerr("failed to parse command line arguments: %s\n", error->message);
		return 1;
	}

	/* -v, show version and exit */
	if (show_version) {
		g_print("%s-%s - a fast and lightweight webserver\n", PACKAGE_NAME, PACKAGE_VERSION);
		g_print("Build date: %s\n", PACKAGE_BUILD_DATE);
		return 0;
	}

	/* initialize threading */
	g_thread_init(NULL);

	srv = server_new();

	plugin_register(srv, "core", plugin_core_init);

	/* if no path is specified for the config, read lighttpd.conf from current directory */
	if (config_path == NULL)
		config_path = "lighttpd.conf";

	log_debug(srv, NULL, "config path: %s", config_path);

	if (!luaconfig) {
		GTimeVal start, end;
		gulong s, millis, micros;
		g_get_current_time(&start);
		guint64 d;

		/* standard config frontend */
		GList *ctx_stack = config_parser_init(srv);
		config_parser_context_t *ctx = (config_parser_context_t*) ctx_stack->data;
		if (!config_parser_file(srv, ctx_stack, config_path)) {
			config_parser_finish(srv, ctx_stack);
			log_thread_start(srv);
			g_atomic_int_set(&srv->exiting, TRUE);
			log_thread_wakeup(srv);
			server_free(srv);
			return 1;
		}

		g_get_current_time(&end);
		d = end.tv_sec - start.tv_sec;
		d *= 1000000;
		d += end.tv_usec - start.tv_usec;
		s = d / 1000000;
		millis = (d - s) / 1000;
		micros = (d - s - millis) %1000;
		g_print("parsed config file in %lu seconds, %lu milliseconds, %lu microseconds\n", s, millis, micros);
		g_print("option_stack: %u action_list_stack: %u (should be 0:1)\n", g_queue_get_length(ctx->option_stack), g_queue_get_length(ctx->action_list_stack));

		/* TODO config_parser_finish(srv, ctx_stack); */
	}
	else {
#ifdef HAVE_LUA_H
		config_lua_load(srv, config_path);
		/* lua config frontend */
#else
		g_print("lua config frontend not available\n");
		return 1;
#endif
	}

	/* if config should only be tested, exit here  */
	if (test_config)
		return 0;

	TRACE(srv, "%s", "Test!");

	/* srv->log_stderr = log_new(srv, LOG_TYPE_FILE, g_string_new("lightytest.log")); */
	log_write_(srv, NULL, LOG_LEVEL_WARNING, "test %s", "foo1");
	log_warning(srv, NULL, "test %s", "foo1"); /* duplicate won't be logged */
	log_warning(srv, NULL, "test %s", "foo2");
	log_debug(srv, NULL, "test %s", "message");

	server_loop_init(srv);
	server_start(srv);

	server_free(srv);

	return 0;
}
