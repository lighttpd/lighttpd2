/*
 * mod_cache_disk_etag - cache generated content on disk if etag header is set
 *
 * Description:
 *     cache generated content on disk if etag header is set
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     cache.disk.etag <path>  - cache in specified directory
 *         path: string
 *       This blocks action progress until the response headers are
 *       done (i.e. there has to be a content generator before it (like fastcgi/static file)
 *       You could insert it multiple times of course (e.g. before and after deflate).
 *
 * Example config:
 *     cache.disk.etag "/var/lib/lighttpd/cache_etag"
 *
 * Todo:
 *  - use stat cache
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <sys/stat.h>
#include <fcntl.h>

LI_API gboolean mod_cache_disk_etag_init(liModules *mods, liModule *mod);
LI_API gboolean mod_cache_disk_etag_free(liModules *mods, liModule *mod);

typedef struct cache_etag_context cache_etag_context;
struct cache_etag_context {
	GString *path;
};

typedef struct cache_etag_file cache_etag_file;
struct cache_etag_file {
	GString *filename, *tmpfilename;
	int fd;
/* cache hit */
	int hit_fd;
	goffset hit_length;
};

static cache_etag_file* cache_etag_file_create(GString *filename) {
	cache_etag_file *cfile = g_slice_new0(cache_etag_file);
	cfile->filename = filename;
	cfile->fd = -1;
	cfile->hit_fd = -1;
	return cfile;
}

static gboolean mkdir_for_file(liVRequest *vr, char *filename) {
	char *p = filename;

	if (!filename || !filename[0])
		return FALSE;

	while ((p = strchr(p + 1, '/')) != NULL) {
		*p = '\0';
		if ((mkdir(filename, 0700) != 0) && (errno != EEXIST)) {
			VR_ERROR(vr, "creating cache-directory '%s' failed: %s", filename, g_strerror(errno));
			*p = '/';
			return FALSE;
		}

		*p++ = '/';
		if (!*p) {
			VR_ERROR(vr, "unexpected trailing slash for filename '%s'", filename);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean cache_etag_file_start(liVRequest *vr, cache_etag_file *cfile) {
	cfile->tmpfilename = g_string_sized_new(cfile->filename->len + 7);
	g_string_append_len(cfile->tmpfilename, GSTR_LEN(cfile->filename));
	g_string_append_len(cfile->tmpfilename, CONST_STR_LEN("-XXXXXX"));

	if (!mkdir_for_file(vr, cfile->tmpfilename->str)) {
		return FALSE;
	}

	errno = 0; /* posix doesn't define any errors */
	if (-1 == (cfile->fd = mkstemp(cfile->tmpfilename->str))) {
		VR_ERROR(vr, "Couldn't create cache tempfile '%s': %s", cfile->tmpfilename->str, g_strerror(errno));
		return FALSE;
	}
#ifdef FD_CLOEXEC
	fcntl(cfile->fd, F_SETFD, FD_CLOEXEC);
#endif
	return TRUE;
}

static void cache_etag_file_free(cache_etag_file *cfile) {
	if (!cfile) return;
	if (cfile->fd != -1) {
		close(cfile->fd);
		unlink(cfile->tmpfilename->str);
	}
	if (cfile->hit_fd != -1) close(cfile->hit_fd);
	if (cfile->filename) g_string_free(cfile->filename, TRUE);
	if (cfile->tmpfilename) g_string_free(cfile->tmpfilename, TRUE);
	g_slice_free(cache_etag_file, cfile);
}

static void cache_etag_file_finish(liVRequest *vr, cache_etag_file *cfile) {
	close(cfile->fd);
	cfile->fd = -1;
	if (-1 == rename(cfile->tmpfilename->str, cfile->filename->str)) {
		VR_ERROR(vr, "Couldn't move temporary cache file '%s': '%s'", cfile->tmpfilename->str, g_strerror(errno));
		unlink(cfile->tmpfilename->str);
	}
	cache_etag_file_free(cfile);
}

/**********************************************************************************/

static void cache_etag_filter_free(liVRequest *vr, liFilter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	UNUSED(vr);

	cache_etag_file_free(cfile);
}

static liHandlerResult cache_etag_filter_hit(liVRequest *vr, liFilter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	UNUSED(vr);

	if (!cfile) return LI_HANDLER_GO_ON;

	if (!f->out->is_closed) li_chunkqueue_append_file_fd(f->out, NULL, 0, cfile->hit_length, cfile->hit_fd);
	cfile->hit_fd = -1;
	cache_etag_file_free(cfile);
	f->param = NULL;

	f->out->is_closed = TRUE;

	return LI_HANDLER_GO_ON;
}

static liHandlerResult cache_etag_filter_miss(liVRequest *vr, liFilter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	ssize_t res;
	gchar *buf;
	off_t buflen;
	liChunkIter citer = li_chunkqueue_iter(f->in);
	GError *err = NULL;
	UNUSED(vr);

	if (0 == f->in->length) return LI_HANDLER_GO_ON;

	if (!cfile) { /* somehow we lost the file */
		li_chunkqueue_steal_all(f->out, f->in);
		if (f->in->is_closed) f->out->is_closed = TRUE;
		return LI_HANDLER_GO_ON;
	}

	if (LI_HANDLER_GO_ON != li_chunkiter_read(citer, 0, 64*1024, &buf, &buflen, &err)) {
		if (NULL != err) {
			VR_ERROR(vr, "Couldn't read data from chunkqueue: %s", err->message);
			g_error_free(err);
		} else {
			VR_ERROR(vr, "%s", "Couldn't read data from chunkqueue");
		}
		cache_etag_file_free(cfile);
		f->param = NULL;
		li_chunkqueue_steal_all(f->out, f->in);
		if (f->in->is_closed) f->out->is_closed = TRUE;
		return LI_HANDLER_GO_ON;
	}

	res = write(cfile->fd, buf, buflen);
	if (res < 0) {
		switch (errno) {
		case EINTR:
		case EAGAIN:
			break; /* come back later */
		default:
			VR_ERROR(vr, "Couldn't write to temporary cache file '%s': %s",
				cfile->tmpfilename->str, g_strerror(errno));
			cache_etag_file_free(cfile);
			f->param = NULL;
			li_chunkqueue_steal_all(f->out, f->in);
			if (f->in->is_closed) f->out->is_closed = TRUE;
			return LI_HANDLER_GO_ON;
		}
	} else {
		li_chunkqueue_steal_len(f->out, f->in, res);
		if (f->in->length == 0 && f->in->is_closed) {
			cache_etag_file_finish(vr, cfile);
			f->param = NULL;
			f->out->is_closed = TRUE;
			return LI_HANDLER_GO_ON;
		}
	}

	return f->in->length ? LI_HANDLER_COMEBACK : LI_HANDLER_GO_ON;
}

static GString* createFileName(liVRequest *vr, GString *path, liHttpHeader *etagheader) {
	GString *file = g_string_sized_new(255);
	gchar* etag_base64 = g_base64_encode((guchar*) LI_HEADER_VALUE_LEN(etagheader));
	g_string_append_len(file, GSTR_LEN(path));
	g_string_append_len(file, GSTR_LEN(vr->request.uri.path));
	g_string_append_len(file, CONST_STR_LEN("-"));
	g_string_append(file, etag_base64);
	g_free(etag_base64);
	return file;
}

static liHandlerResult cache_etag_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	cache_etag_file *cfile = (cache_etag_file*) context;
	UNUSED(vr);
	UNUSED(param);

	cache_etag_file_free(cfile);
	return LI_HANDLER_GO_ON;
}

static liHandlerResult cache_etag_handle(liVRequest *vr, gpointer param, gpointer *context) {
	cache_etag_context *ctx = (cache_etag_context*) param;
	cache_etag_file *cfile = (cache_etag_file*) *context;
	GList *etag_entry;
	liHttpHeader *etag;
	struct stat st;
	GString *tmp_str = vr->wrk->tmp_str;
	liFilter *f;
	liHandlerResult res;
	int err, fd;

	if (!cfile) {
		if (vr->request.http_method != LI_HTTP_METHOD_GET) return LI_HANDLER_GO_ON;

		LI_VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

		if (vr->response.http_status != 200) return LI_HANDLER_GO_ON;

		/* Don't cache static files if filter list is empty */
		if (NULL == vr->filters_out_first && vr->backend_source->out->is_closed && 0 == vr->backend_source->out->mem_usage)
			return LI_HANDLER_GO_ON;

		etag_entry = li_http_header_find_first(vr->response.headers, CONST_STR_LEN("etag"));
		if (!etag_entry) return LI_HANDLER_GO_ON; /* no etag -> no caching */
		if (li_http_header_find_next(etag_entry, CONST_STR_LEN("etag"))) {
			VR_ERROR(vr, "%s", "duplicate etag header in response, will not cache it");
			return LI_HANDLER_GO_ON;
		}
		etag = (liHttpHeader*) etag_entry->data;

		cfile = cache_etag_file_create(createFileName(vr, ctx->path, etag));
		*context = cfile;
	}

	res = li_stat_cache_get(vr, cfile->filename, &st, &err, &fd);
	if (res == LI_HANDLER_WAIT_FOR_EVENT)
		return res;

	if (res == LI_HANDLER_GO_ON) {
		if (!S_ISREG(st.st_mode)) {
			VR_ERROR(vr, "Unexpected file type for cache file '%s' (mode %o)", cfile->filename->str, (unsigned int) st.st_mode);
			close(fd);
			return LI_HANDLER_GO_ON; /* no caching */
		}
		cfile->hit_fd = fd;
#ifdef FD_CLOEXEC
		fcntl(cfile->hit_fd, F_SETFD, FD_CLOEXEC);
#endif
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "cache hit for '%s'", vr->request.uri.path->str);
		}
		cfile->hit_length = st.st_size;
		g_string_truncate(tmp_str, 0);
		li_string_append_int(tmp_str, st.st_size);
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Length"), GSTR_LEN(tmp_str));
		f = li_vrequest_add_filter_out(vr, cache_etag_filter_hit, cache_etag_filter_free, NULL, cfile);
		f->in->is_closed = TRUE;
		*context = NULL;
		return LI_HANDLER_GO_ON;
	}

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "cache miss for '%s'", vr->request.uri.path->str);
	}

	if (!cache_etag_file_start(vr, cfile)) {
		cache_etag_file_free(cfile);
		return LI_HANDLER_GO_ON; /* no caching */
	}

	li_vrequest_add_filter_out(vr, cache_etag_filter_miss, cache_etag_filter_free, NULL, cfile);
	*context = NULL;

	return LI_HANDLER_GO_ON;
}

static void cache_etag_free(liServer *srv, gpointer param) {
	cache_etag_context *ctx = (cache_etag_context*) param;
	UNUSED(srv);

	g_string_free(ctx->path, TRUE);
	g_slice_free(cache_etag_context, ctx);
}

static liAction* cache_etag_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	cache_etag_context *ctx;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "cache.disk.etag expects a string as parameter");
		return FALSE;
	}

	ctx = g_slice_new0(cache_etag_context);
	ctx->path = li_value_extract_string(val);

	return li_action_new_function(cache_etag_handle, cache_etag_cleanup, cache_etag_free, ctx);
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "cache.disk.etag", cache_etag_create, NULL },
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

gboolean mod_cache_disk_etag_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_cache_disk_etag", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_cache_disk_etag_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
