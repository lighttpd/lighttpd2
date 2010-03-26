
#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>
#include <lighttpd/angel_plugin_core.h>

#include <lighttpd/version.h>

#ifndef DEFAULT_LIBDIR
# define DEFAULT_LIBDIR "/usr/local/lib/lighttpd2"
#endif

# ifndef HAVE_ISSETUGID

static int l_issetugid() {
	return (geteuid() != getuid() || getegid() != getgid());
}

#  define issetugid l_issetugid
# endif

int main(int argc, char *argv[]) {
	GError *error = NULL;
	GOptionContext *context;

	/* options */
	gboolean show_version = FALSE, no_fork = FALSE;
	gchar const *const def_module_dir = DEFAULT_LIBDIR;
	gchar const *module_dir = def_module_dir;
	gchar const *config_path = NULL, *pidfile = NULL;

	gboolean res;
	int result = 0;
	liServer* srv = NULL;

	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
		{ "module-dir", 'm', 0, G_OPTION_ARG_STRING, &module_dir, "module directory [default: " DEFAULT_LIBDIR "]", "PATH" },
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
		g_printerr("lighttpd-angel: failed to parse command line arguments: %s\n", error->message);
		g_error_free(error);
		goto cleanup;
	}

	if (show_version) {
		g_print(PACKAGE_NAME "-angel" "/" PACKAGE_VERSION REPO_VERSION " - a fast and lightweight webserver\n");
		g_print("Build date: %s\n", PACKAGE_BUILD_DATE);
		goto cleanup;
	}

	if (!config_path) {
		g_printerr("lighttpd-angel: missing config filename\n");
		result = -1;
		goto cleanup;
	}

	if (!(getuid() == 0) && issetugid()) {
		g_printerr("Are you nuts ? Don't apply a SUID bit to this binary\n");
		result = -1;
		goto cleanup;
	}

	/* initialize threading */
	g_thread_init(NULL);

	srv = li_server_new(module_dir);

	if (!li_plugins_config_load(srv, config_path)) {
		result = -1;
		goto cleanup;
	}

	INFO(srv, "%s", "parsed config file");

	ev_loop(srv->loop, 0);

	INFO(srv, "%s", "going down");

cleanup:
	if (srv) li_server_free(srv);
	if (config_path) g_free((gchar*) config_path);
	if (module_dir != def_module_dir) g_free((gchar*) module_dir);

	return result;
}
