
#include "base.h"
#include "log.h"
#include "config_parser.h"

static gchar *config_path = NULL;
static gboolean luaconfig = FALSE;
static gboolean test_config = FALSE;
static gboolean show_version = FALSE;

static GOptionEntry entries[] = {
	{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "filename/path of the config", "PATH" },
	{ "lua", 'l', 0, G_OPTION_ARG_NONE, &luaconfig, "use the lua config frontend", NULL },
	{ "test", 't', 0, G_OPTION_ARG_NONE, &test_config, "test config and exit", NULL },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "show version and exit", NULL },
	{ NULL, 0, 0, 0, NULL, NULL, NULL }
};


int main(int argc, char *argv[]) {
	GError *error;
	GOptionContext *context;
	server *srv;



	/* parse commandline options */
	context = g_option_context_new("- fast and lightweight webserver");
	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("failed to parse command line arguments: %s\n", error->message);
		return 1;
	}

	if (show_version)
	{
		g_print("%s-%s - a fast and lightweight webserver\n", PACKAGE_NAME, PACKAGE_VERSION);
		g_print("Build date: %s\n", PACKAGE_BUILD_DATE);
		return 0;
	}

	srv = server_new();

	/* if no path is specified for the config, read lighttpd.conf from current directory */
	if (config_path == NULL)
		config_path = "lighttpd.conf";

	g_print("config path: %s\n", config_path);

	if (!luaconfig) {
		/* standard config frontend */
		config_parser_init();
		if (!config_parser_file(srv, config_path))
		{
			return 1;
		}
	}
	else {
		/* lua config frontend */
	}

	/* if config should only be tested, don't go over  */
	if (test_config)
		return 0;

	TRACE("%s", "Test!");

	return 0;
}
