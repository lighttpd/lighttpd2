
#include "base.h"
#include "log.h"
#include "config_parser.h"


int main(int argc, char *argv[]) {
	GError *error;
	GOptionContext *context;
	server *srv;

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

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
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

	log_init(srv);

	/* if no path is specified for the config, read lighttpd.conf from current directory */
	if (config_path == NULL)
		config_path = "lighttpd.conf";

	g_print("config path: %s\n", config_path);

	if (!luaconfig) {
		/* standard config frontend */
		GList *cpd_stack = NULL;
		if (!config_parser_file(srv, &cpd_stack, config_path)) {
			return 1;
		}
	}
	else {
		/* lua config frontend */
	}

	/* if config should only be tested, exit here  */
	if (test_config)
		return 0;

	TRACE(srv, "%s", "Test!");

	log_write_(srv, NULL, LOG_LEVEL_WARNING, "test %s", "foo1");
	log_write_(srv, NULL, LOG_LEVEL_WARNING, "test %s", "foo1");
	log_write_(srv, NULL, LOG_LEVEL_WARNING, "test %s", "foo2");
	log_debug(srv, NULL, "test %s", "message");
	srv->exiting = TRUE;
	sleep(3);
	log_error(srv, NULL, "error %d", 23);
	log_write_(srv, NULL, LOG_LEVEL_WARNING, "test %s", "foo3");

	g_thread_join(srv->log_thread);

	return 0;
}
