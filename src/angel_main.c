
#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>

int main(int argc, char *argv[]) {
	GError *error = NULL;
	GOptionContext *context;

	/* options */
	gboolean show_version = FALSE, no_fork = FALSE;
	gchar const *const def_module_dir = "/usr/local/lib"; /* TODO: configure module-dir with make-system */
	gchar const *module_dir = def_module_dir;
	gchar const *config_path = NULL, *pidfile = NULL;

	gboolean res;
	int result = 0;

	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
		{ "module-dir", 'm', 0, G_OPTION_ARG_STRING, &module_dir, "module directory", "PATH" },
		{ "no-daemon", 'n', 0, G_OPTION_ARG_NONE, &no_fork, "Don't fork (for daemontools)", NULL },
		{ "pid-file", 0, 0, G_OPTION_ARG_STRING, &pidfile, "Location of the pid file (only valid in daemon mode)", "PATH" },
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
		g_error_free(error);
		goto cleanup;
	}

	if (show_version) {
		g_print("%s %s - a fast and lightweight webserver\n", PACKAGE_NAME "-angel", PACKAGE_VERSION);
		g_print("Build date: %s\n", PACKAGE_BUILD_DATE);
		goto cleanup;
	}

	if (!angel_config_parse_file(config_path, &error)) {
		g_printerr("failed to parse config file: %s\n", error->message);
		g_error_free(error);
		result = -1;
		goto cleanup;
	}

	g_printerr("Parsed config file\n");

cleanup:
	if (config_path) g_free((gchar*) config_path);
	if (module_dir != def_module_dir) g_free((gchar*) module_dir);

	return 0;
}
