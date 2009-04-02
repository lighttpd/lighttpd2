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

LI_API gboolean mod_cache_disk_etag_init(modules *mods, module *mod);
LI_API gboolean mod_cache_disk_etag_free(modules *mods, module *mod);

struct cache_etag_context;
typedef struct cache_etag_context cache_etag_context;

struct cache_etag_context {
	GString *path;
};

struct cache_etag_file;
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

static gboolean mkdir_for_file(vrequest *vr, char *filename) {
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

static gboolean cache_etag_file_start(vrequest *vr, cache_etag_file *cfile) {
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

static void cache_etag_file_finish(vrequest *vr, cache_etag_file *cfile) {
	close(cfile->fd);
	cfile->fd = -1;
	if (-1 == rename(cfile->tmpfilename->str, cfile->filename->str)) {
		VR_ERROR(vr, "Couldn't move temporary cache file '%s': '%s'", cfile->tmpfilename->str, g_strerror(errno));
		unlink(cfile->tmpfilename->str);
	}
	cache_etag_file_free(cfile);
}

/**********************************************************************************/

static void cache_etag_filter_free(vrequest *vr, filter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	UNUSED(vr);

	cache_etag_file_free(cfile);
}

static handler_t cache_etag_filter_hit(vrequest *vr, filter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	UNUSED(vr);

	if (!cfile) return HANDLER_GO_ON;

	f->in->is_closed = TRUE;

	chunkqueue_append_file_fd(f->out, NULL, 0, cfile->hit_length, cfile->hit_fd);
	cfile->hit_fd = -1;
	cache_etag_file_free(cfile);
	f->param = NULL;

	f->out->is_closed = TRUE;

	return HANDLER_GO_ON;
}

static handler_t cache_etag_filter_miss(vrequest *vr, filter *f) {
	cache_etag_file *cfile = (cache_etag_file*) f->param;
	ssize_t res;
	gchar *buf;
	goffset buflen;
	chunkiter citer = chunkqueue_iter(f->in);
	UNUSED(vr);

	if (0 == f->in->length) return HANDLER_GO_ON;

	if (!cfile) { /* somehow we lost the file */
		chunkqueue_steal_all(f->out, f->in);
		if (f->in->is_closed) f->out->is_closed = TRUE;
		return HANDLER_GO_ON;
	}

	if (HANDLER_GO_ON != chunkiter_read(vr, citer, 0, 64*1024, &buf, &buflen)) {
		VR_ERROR(vr, "%s", "Couldn't read data from chunkqueue");
		cache_etag_file_free(cfile);
		f->param = NULL;
		chunkqueue_steal_all(f->out, f->in);
		if (f->in->is_closed) f->out->is_closed = TRUE;
		return HANDLER_GO_ON;
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
			chunkqueue_steal_all(f->out, f->in);
			if (f->in->is_closed) f->out->is_closed = TRUE;
			return HANDLER_GO_ON;
		}
	} else {
		chunkqueue_steal_len(f->out, f->in, res);
		if (f->in->length == 0 && f->in->is_closed) {
			cache_etag_file_finish(vr, cfile);
			f->param = NULL;
			f->out->is_closed = TRUE;
			return HANDLER_GO_ON;
		}
	}

	return f->in->length ? HANDLER_COMEBACK : HANDLER_GO_ON;
}

static GString* createFileName(vrequest *vr, GString *path, http_header *etagheader) {
	GString *file = g_string_sized_new(255);
	gchar* etag_base64 = g_base64_encode(
		etagheader->data->str + (etagheader->keylen + 2),
		etagheader->data->len - (etagheader->keylen + 2));
	g_string_append_len(file, GSTR_LEN(path));
	g_string_append_len(file, GSTR_LEN(vr->request.uri.path));
	g_string_append_len(file, CONST_STR_LEN("-"));
	g_string_append(file, etag_base64);
	g_free(etag_base64);
	return file;
}

static handler_t cache_etag_cleanup(vrequest *vr, gpointer param, gpointer context) {
	cache_etag_file *cfile = (cache_etag_file*) context;
	UNUSED(vr);
	UNUSED(param);

	cache_etag_file_free(cfile);
	return HANDLER_GO_ON;
}

static handler_t cache_etag_handle(vrequest *vr, gpointer param, gpointer *context) {
	cache_etag_context *ctx = (cache_etag_context*) param;
	cache_etag_file *cfile = (cache_etag_file*) *context;
	GList *etag_entry;
	http_header *etag;
	struct stat st;
	GString *tmp_str = vr->con->wrk->tmp_str;

	if (!cfile) {
		if (vr->request.http_method != HTTP_METHOD_GET) return HANDLER_GO_ON;

		VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

		if (vr->response.http_status != 200) return HANDLER_GO_ON;

		/* Don't cache static files */
		if (vr->out->is_closed && 0 == vr->out->mem_usage) return HANDLER_GO_ON;

		etag_entry = http_header_find_first(vr->response.headers, CONST_STR_LEN("etag"));
		if (!etag_entry) return HANDLER_GO_ON; /* no etag -> no caching */
		if (http_header_find_next(etag_entry, CONST_STR_LEN("etag"))) {
			VR_ERROR(vr, "%s", "duplicate etag header in response, will not cache it");
			return HANDLER_GO_ON;
		}
		etag = (http_header*) etag_entry->data;

		cfile = cache_etag_file_create(createFileName(vr, ctx->path, etag));
		*context = cfile;
	}

	/* TODO use async stat cache*/
	if (0 == stat(cfile->filename->str, &st)) {
		if (!S_ISREG(st.st_mode)) {
			VR_ERROR(vr, "Unexpected file type for cache file '%s' (mode %o)", cfile->filename->str, (unsigned int) st.st_mode);
			return HANDLER_GO_ON; /* no caching */
		}
		if (-1 == (cfile->hit_fd = open(cfile->filename->str, O_RDONLY))) {
			if (EMFILE == errno) {
				server_out_of_fds(vr->con->srv);
			}
			VR_ERROR(vr, "Couldn't open cache file '%s': %s", cfile->filename->str, g_strerror(errno));
			return HANDLER_GO_ON; /* no caching */
		}
#ifdef FD_CLOEXEC
		fcntl(cfile->hit_fd, F_SETFD, FD_CLOEXEC);
#endif
		if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "cache hit for '%s'", vr->request.uri.path->str);
		}
		cfile->hit_length = st.st_size;
		g_string_truncate(tmp_str, 0);
		l_g_string_append_int(tmp_str, st.st_size);
		http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Length"), GSTR_LEN(tmp_str));
		vrequest_add_filter_out(vr, cache_etag_filter_hit, cache_etag_filter_free, cfile);
		*context = NULL;
		return HANDLER_GO_ON;
	}

	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "cache miss for '%s'", vr->request.uri.path->str);
	}

	if (!cache_etag_file_start(vr, cfile)) {
		cache_etag_file_free(cfile);
		return HANDLER_GO_ON; /* no caching */
	}

	vrequest_add_filter_out(vr, cache_etag_filter_miss, cache_etag_filter_free, cfile);
	*context = NULL;

	return HANDLER_GO_ON;
}

static void cache_etag_free(server *srv, gpointer param) {
	cache_etag_context *ctx = (cache_etag_context*) param;
	UNUSED(srv);

	g_string_free(ctx->path, TRUE);
	g_slice_free(cache_etag_context, ctx);
}

static action* cache_etag_create(server *srv, plugin* p, value *val) {
	cache_etag_context *ctx;
	UNUSED(p);

	if (val->type != VALUE_STRING) {
		ERROR(srv, "%s", "cache.disk.etag expects a string as parameter");
		return FALSE;
	}

	ctx = g_slice_new0(cache_etag_context);
	ctx->path = value_extract_ptr(val);

	return action_new_function(cache_etag_handle, cache_etag_cleanup, cache_etag_free, ctx);
}

static const plugin_option options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "cache.disk.etag", cache_etag_create },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

gboolean mod_cache_disk_etag_init(modules *mods, module *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_cache_disk_etag", plugin_init);

	return mod->config != NULL;
}

gboolean mod_cache_disk_etag_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}
