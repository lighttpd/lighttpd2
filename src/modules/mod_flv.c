/*
 * mod_flv - flash pseudo streaming
 *
 * Description:
 *     mod_flv lets you stream .flv files in a way that flash players can seek into positions in the timeline.
 *
 * Setups:
 *     none
 *
 * Options:
 *     none
 *
 * Actions:
 *     flv;
 *         - enables .flv pseudo streaming
 *
 * Example config:
 *     if phys.path =$ ".flv" {
 *         flv;
 *     }
 *
 * Tip:
 *     Use caching and bandwidth throttling to save traffic.
 *     To prevent the player from buffering at the beginning, use a small burst threshold.
 *
 *     if phys.path =$ ".flv" {
 *         expire "access 1 month";
 *         io.throttle 500kbyte => 150kbyte;
 *         flv;
 *     }
 *
 *     This config will make browsers cache videos for 1 month and limit bandwidth to 150 kilobyte/s after 500 kilobytes.
 *
 * Todo:
 *     - flv audio container support?
 *
 * Author:
 *     Copyright (c) 2010 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

LI_API gboolean mod_flv_init(liModules *mods, liModule *mod);
LI_API gboolean mod_flv_free(liModules *mods, liModule *mod);


static liHandlerResult flv(liVRequest *vr, gpointer param, gpointer *context) {
	gchar *start;
	guint len;
	goffset pos;
	liHandlerResult res;
	gboolean cachable;
	struct stat st;
	int err;
	int fd = -1;

	UNUSED(context);
	UNUSED(param);

	if (li_vrequest_is_handled(vr))
		return LI_HANDLER_GO_ON;

	res = li_stat_cache_get(vr, vr->physical.path, &st, &err, &fd);

	if (res == LI_HANDLER_WAIT_FOR_EVENT)
		return res;

	if (res == LI_HANDLER_ERROR) {
		/* open or fstat failed */

		if (fd != -1)
			close(fd);

		if (!li_vrequest_handle_direct(vr))
			return LI_HANDLER_ERROR;

		switch (err) {
		case ENOENT:
		case ENOTDIR:
			vr->response.http_status = 404;
			return LI_HANDLER_GO_ON;
		case EACCES:
			vr->response.http_status = 403;
			return LI_HANDLER_GO_ON;
		default:
			VR_ERROR(vr, "stat() or open() for '%s' failed: %s", vr->physical.path->str, g_strerror(err));
			return LI_HANDLER_ERROR;
		}
	} else if (S_ISDIR(st.st_mode)) {
		if (fd != -1)
			close(fd);

		return LI_HANDLER_GO_ON;
	} else if (!S_ISREG(st.st_mode)) {
		if (fd != -1)
			close(fd);

		if (!li_vrequest_handle_direct(vr))
			return LI_HANDLER_ERROR;

		vr->response.http_status = 403;
	} else {
		liChunkFile *cf;

#ifdef FD_CLOEXEC
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

		if (!li_vrequest_handle_direct(vr)) {
			close(fd);
			return LI_HANDLER_ERROR;
		}

		if (li_querystring_find(vr->request.uri.query, CONST_STR_LEN("start"), &start, &len)) {
			guint i;
			pos = 0;

			for (i = 0; i < len; i++) {
				if (start[i] >= '0' && start[i] <= '9') {
					pos *= 10;
					pos += start[i] - '0';
				}
			}
		} else {
			pos = 0;
		}

		li_etag_set_header(vr, &st, &cachable);
		if (cachable) {
			vr->response.http_status = 304;
			close(fd);
			return LI_HANDLER_GO_ON;
		}


		vr->response.http_status = 200;
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("video/x-flv"));

		if (pos < 0 || pos > st.st_size)
			pos = 0;

		if (pos != 0)
			li_chunkqueue_append_mem(vr->direct_out, CONST_STR_LEN("FLV\x1\x1\0\0\0\x9\0\0\0\x9"));

		cf = li_chunkfile_new(NULL, fd, FALSE);
		li_chunkqueue_append_chunkfile(vr->direct_out, cf, pos, st.st_size - pos);
		li_chunkfile_release(cf);
	}

	return LI_HANDLER_GO_ON;
}

static liAction* flv_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(userdata);

	if (!li_value_is_nothing(val)) {
		ERROR(srv, "%s", "flv does not take any parameters");
		return NULL;
	}

	return li_action_new_function(flv, NULL, NULL, p);
}

static const liPluginAction actions[] = {
	{ "flv", flv_create, NULL },

	{ NULL, NULL, NULL }
};

static void plugin_flv_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->actions = actions;

}


gboolean mod_flv_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_flv", plugin_flv_init, NULL);

	return mod->config != NULL;
}

gboolean mod_flv_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
