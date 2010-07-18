/*
 * mod_memcached - cache content on memcached servers
 *
 * Description:
 *     cache content on memcached servers
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *       (trailing parameters are optional)
 *     memcached.lookup <options>, <action-hit>, <action-miss>
 *     memcached.store  <options>
 *        options: hash of
 *            - server: socket address as string (default: 127.0.0.1:11211)
 *            - flags: flags for storing (default 0)
 *            - ttl: ttl for storing (default 0 - forever)
 *            - maxsize: maximum size in bytes we want to store
 *            - headers: whether to store/lookup headers too (not supported yet)
 *              if disabled: get mime-type from request.uri.path for lookup
 *            - key: pattern for lookup/store key
 *              default: "%{req.path}"
 *
 * Example config:
 *     memcached.lookup [], ${ header.add "X-Memcached" => "Hit" }, ${ header.add "X-Memcached" => "Miss" };
 *
 *     memcached.lookup ["key": "%{req.scheme}://%{req.host}%{req.path}"];
 *
 * Exports a lua api to per-worker luaStates too.
 *
 * Todo:
 *  - store/lookup headers too
 *
 * Author:
 *     Copyright (c) 2010 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <lighttpd/memcached.h>

#ifdef HAVE_LUA_H
# include <lighttpd/core_lua.h>
# include <lualib.h>
# include <lauxlib.h>
#endif

LI_API gboolean mod_memcached_init(liModules *mods, liModule *mod);
LI_API gboolean mod_memcached_free(liModules *mods, liModule *mod);

typedef struct memcached_ctx memcached_ctx;
struct memcached_ctx {
	int refcount;

	liMemcachedCon **worker_client_ctx;
	liSocketAddress addr;
	liPattern *pattern;
	guint flags;
	ev_tstamp ttl;
	gssize maxsize;
	gboolean headers;

	liAction *act_found, *act_miss;

	liPlugin *p;
	GList mconf_link;
};

typedef struct memcached_config memcached_config;
struct memcached_config {
	GQueue prepare_ctx;
};

typedef struct {
	liMemcachedRequest *req;
	liBuffer *buffer;
	liVRequest *vr;
} memcache_request;

typedef struct {
	memcached_ctx *ctx;
	liBuffer *buf;
} memcache_filter;

/* memcache option names */
static const GString
	mon_server = { CONST_STR_LEN("server"), 0 },
	mon_flags = { CONST_STR_LEN("flags"), 0 },
	mon_ttl = { CONST_STR_LEN("ttl"), 0 },
	mon_maxsize = { CONST_STR_LEN("maxsize"), 0 },
	mon_headers = { CONST_STR_LEN("headers"), 0 },
	mon_key = { CONST_STR_LEN("key"), 0 }
;

static void mc_ctx_acquire(memcached_ctx* ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void mc_ctx_release(liServer *srv, gpointer param) {
	memcached_ctx *ctx = param;
	guint i;

	if (NULL == ctx) return;

	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&ctx->refcount)) return;

	if (ctx->worker_client_ctx) {
		for (i = 0; i < srv->worker_count; i++) {
			li_memcached_con_release(ctx->worker_client_ctx[i]);
		}
		g_slice_free1(sizeof(liMemcachedCon*) * srv->worker_count, ctx->worker_client_ctx);
	}

	li_sockaddr_clear(&ctx->addr);

	li_pattern_free(ctx->pattern);

	li_action_release(srv, ctx->act_found);
	li_action_release(srv, ctx->act_miss);

	if (ctx->mconf_link.data) { /* still in LI_SERVER_INIT */
		memcached_config *mconf = ctx->p->data;
		g_queue_unlink(&mconf->prepare_ctx, &ctx->mconf_link);
		ctx->mconf_link.data = NULL;
	}

	g_slice_free(memcached_ctx, ctx);
}

static memcached_ctx* mc_ctx_parse(liServer *srv, liPlugin *p, liValue *config) {
	memcached_ctx *ctx;
	memcached_config *mconf = p->data;
	GString def_server = li_const_gstring(CONST_STR_LEN("127.0.0.1:11211"));

	if (config && config->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "memcache expects an optional hash of options");
		return NULL;
	}

	ctx = g_slice_new0(memcached_ctx);
	ctx->refcount = 1;
	ctx->p = p;

	ctx->addr = li_sockaddr_from_string(&def_server, 11211);

	ctx->pattern = li_pattern_new("%{req.path}");

	ctx->flags = 0;
	ctx->ttl = 30;
	ctx->maxsize = 64*1024; /* 64 kB */
	ctx->headers = FALSE;

	if (config) {
		GHashTable *ht = config->data.hash;
		GHashTableIter it;
		gpointer pkey, pvalue;

		g_hash_table_iter_init(&it, ht);
		while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
			GString *key = pkey;
			liValue *value = pvalue;

			if (g_string_equal(key, &mon_server)) {
				if (value->type != LI_VALUE_STRING) {
					ERROR(srv, "memcache option '%s' expects string as parameter", mon_server.str);
					goto option_failed;
				}
				li_sockaddr_clear(&ctx->addr);
				ctx->addr = li_sockaddr_from_string(value->data.string, 11211);
				if (NULL == ctx->addr.addr) {
					ERROR(srv, "invalid socket address: '%s'", value->data.string->str);
					goto option_failed;
				}
			} else if (g_string_equal(key, &mon_key)) {
				if (value->type != LI_VALUE_STRING) {
					ERROR(srv, "memcache option '%s' expects string as parameter", mon_key.str);
					goto option_failed;
				}
				li_pattern_free(ctx->pattern);
				ctx->pattern = li_pattern_new(value->data.string->str);
				if (NULL == ctx->pattern) {
					ERROR(srv, "memcache: couldn't parse pattern for key '%s'", value->data.string->str);
					goto option_failed;
				}
			} else if (g_string_equal(key, &mon_flags)) {
				if (value->type != LI_VALUE_NUMBER || value->data.number <= 0) {
					ERROR(srv, "memcache option '%s' expects positive integer as parameter", mon_flags.str);
					goto option_failed;
				}
				ctx->flags = value->data.number;
			} else if (g_string_equal(key, &mon_ttl)) {
				if (value->type != LI_VALUE_NUMBER || value->data.number < 0) {
					ERROR(srv, "memcache option '%s' expects non-negative integer as parameter", mon_ttl.str);
					goto option_failed;
				}
				ctx->ttl = value->data.number;
			} else if (g_string_equal(key, &mon_maxsize)) {
				if (value->type != LI_VALUE_NUMBER || value->data.number <= 0) {
					ERROR(srv, "memcache option '%s' expects positive integer as parameter", mon_maxsize.str);
					goto option_failed;
				}
				ctx->maxsize = value->data.number;
			} else if (g_string_equal(key, &mon_headers)) {
				if (value->type != LI_VALUE_BOOLEAN) {
					ERROR(srv, "memcache option '%s' expects boolean as parameter", mon_headers.str);
					goto option_failed;
				}
				ctx->headers = value->data.boolean;
				if (ctx->headers) {
					ERROR(srv, "%s", "memcache: lookup/storing headers not supported yet");
					goto option_failed;
				}
			} else {
				ERROR(srv, "unknown option for memcache '%s'", key->str);
				goto option_failed;
			}
		}
	}

	if (LI_SERVER_INIT != g_atomic_int_get(&srv->state)) {
		ctx->worker_client_ctx = g_slice_alloc0(sizeof(liMemcachedCon*) * srv->worker_count);
	} else {
		ctx->mconf_link.data = ctx;
		g_queue_push_tail_link(&mconf->prepare_ctx, &ctx->mconf_link);
	}

	return ctx;

option_failed:
	mc_ctx_release(srv, ctx);
	return NULL;
}

static GString* mc_ctx_build_key(memcached_ctx *ctx, liVRequest *vr) {
	GMatchInfo *match_info = NULL;
	GString *key = g_string_sized_new(255);

	g_string_truncate(key, 0);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	li_pattern_eval(vr, key, ctx->pattern, NULL, NULL, li_pattern_regex_cb, match_info);

	li_memcached_mutate_key(key);

	return key;
}

static liMemcachedCon* mc_ctx_prepare(memcached_ctx *ctx, liWorker *wrk) {
	liMemcachedCon *con = ctx->worker_client_ctx[wrk->ndx];

	if (!con) {
		con = li_memcached_con_new(wrk->loop, ctx->addr);
		ctx->worker_client_ctx[wrk->ndx] = con;
	}

	return con;
}

static void memcache_callback(liMemcachedRequest *request, liMemcachedResult result, liMemcachedItem *item, GError **err) {
	memcache_request *req = request->cb_data;
	liVRequest *vr = req->vr;

	/* request done */
	req->req = NULL;

	if (!vr) {
		g_slice_free(memcache_request, req);
		return;
	}

	switch (result) {
	case LI_MEMCACHED_OK: /* STORED, VALUE, DELETED */
		/* steal buffer */
		req->buffer = item->data;
		item->data = NULL;
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "memcached.lookup: key '%s' found, flags = %u", item->key->str, (guint) item->flags);
		}
		break;
	case LI_MEMCACHED_NOT_FOUND:
		/* ok, nothing to do - we just didn't find an entry */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "memcached.lookup: key not found");
		}
		break;
	case LI_MEMCACHED_NOT_STORED:
	case LI_MEMCACHED_EXISTS:
		VR_ERROR(vr, "memcached error: %s", "unexpected result");
		/* TODO (not possible for lookup) */
		break;
	case LI_MEMCACHED_RESULT_ERROR:
		if (err && *err) {
			VR_ERROR(vr, "memcached error: %s", (*err)->message);
		} else {
			VR_ERROR(vr, "memcached error: %s", "Unknown error");
		}
		break;
	}

	li_vrequest_joblist_append(vr);
}

static liHandlerResult mc_handle_lookup(liVRequest *vr, gpointer param, gpointer *context) {
	memcached_ctx *ctx = param;
	memcache_request *req = *context;

	if (req) {
		static const GString default_mime_str = { CONST_STR_LEN("application/octet-stream"), 0 };

		liBuffer *buf = req->buffer;
		const GString *mime_str;

		if (NULL != req->req) return LI_HANDLER_WAIT_FOR_EVENT; /* not done yet */

		g_slice_free(memcache_request, req);
		*context = NULL;

		if (NULL == buf) {
			/* miss */
			if (ctx->act_miss) li_action_enter(vr, ctx->act_miss);
			return LI_HANDLER_GO_ON;
		}

		if (!li_vrequest_handle_direct(vr)) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "memcached.lookup: request already handled");
			}
			li_buffer_release(buf);
			return LI_HANDLER_GO_ON;
		}

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "memcached.lookup: key found, handling request");
		}

		li_chunkqueue_append_buffer(vr->out, buf);

		vr->response.http_status = 200;

		mime_str = li_mimetype_get(vr, vr->request.uri.path);
		if (!mime_str) mime_str = &default_mime_str;
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));

		/* hit */
		if (ctx->act_found) li_action_enter(vr, ctx->act_found);
		return LI_HANDLER_GO_ON;
	} else {
		liMemcachedCon *con;
		GString *key;
		GError *err = NULL;

		if (li_vrequest_is_handled(vr)) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "memcached.lookup: request already handled");
			}
			return LI_HANDLER_GO_ON;
		}

		con = mc_ctx_prepare(ctx, vr->wrk);
		key = mc_ctx_build_key(ctx, vr);

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "memcached.lookup: looking up key '%s'", key->str);
		}

		req = g_slice_new0(memcache_request);
		req->req = li_memcached_get(con, key, memcache_callback, req, &err);
		g_string_free(key, TRUE);

		if (NULL == req->req) {
			if (NULL != err) {
				if (LI_MEMCACHED_DISABLED != err->code) {
					VR_ERROR(vr, "memcached.lookup: get failed: %s", err->message);
				}
				g_clear_error(&err);
			} else {
				VR_ERROR(vr, "memcached.lookup: get failed: %s", "Unkown error");
			}
			g_slice_free(memcache_request, req);

			/* miss */
			if (ctx->act_miss) li_action_enter(vr, ctx->act_miss);

			return LI_HANDLER_GO_ON;
		}
		req->vr = vr;

		*context = req;

		return LI_HANDLER_WAIT_FOR_EVENT;
	}
}

static liHandlerResult mc_lookup_handle_free(liVRequest *vr, gpointer param, gpointer context) {
	memcache_request *req = context;
	UNUSED(vr);
	UNUSED(param);

	if (NULL == req->req) {
		li_buffer_release(req->buffer);
		g_slice_free(memcache_request, req);
	} else {
		req->vr = NULL;
	}

	return LI_HANDLER_GO_ON;
}

static void memcache_store_filter_free(liVRequest *vr, liFilter *f) {
	memcache_filter *mf = (memcache_filter*) f->param;
	UNUSED(vr);

	mc_ctx_release(vr->wrk->srv, mf->ctx);
	li_buffer_release(mf->buf);

	g_slice_free(memcache_filter, mf);
}

static liHandlerResult memcache_store_filter(liVRequest *vr, liFilter *f) {
	memcache_filter *mf = (memcache_filter*) f->param;

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		f->in->is_closed = TRUE;
		return LI_HANDLER_GO_ON;
	}

	/* if already in "forward" mode */
	if (NULL == mf->buf) goto forward;

	/* check if size still fits into buffer */
	if ((gssize) (f->in->length + mf->buf->used) > (gssize) mf->ctx->maxsize) {
		/* response too big, switch to "forward" mode */
		li_buffer_release(mf->buf);
		mf->buf = NULL;
		goto forward;
	}

	while (0 < f->in->length) {
		char *data;
		off_t len;
		liChunkIter ci;
		liHandlerResult res;

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(vr, ci, 0, 16*1024, &data, &len)))
			return res;

		if ((gssize) (len + mf->buf->used) > (gssize) mf->ctx->maxsize) {
			/* response too big, switch to "forward" mode */
			li_buffer_release(mf->buf);
			mf->buf = NULL;
			goto forward;
		}

		memcpy(mf->buf->addr + mf->buf->used, data, len);
		mf->buf->used += len;

		li_chunkqueue_steal_len(f->out, f->in, len);
	}

	if (f->in->is_closed) {
		/* finally: store response in memcached */

		liMemcachedCon *con;
		GString *key;
		GError *err = NULL;
		liMemcachedRequest *req;
		memcached_ctx *ctx = mf->ctx;

		f->out->is_closed = TRUE;

		con = mc_ctx_prepare(ctx, vr->wrk);
		key = mc_ctx_build_key(ctx, vr);

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "memcached.store: storing response for key '%s'", key->str);
		}

		req = li_memcached_set(con, key, ctx->flags, ctx->ttl, mf->buf, NULL, NULL, &err);
		g_string_free(key, TRUE);
		li_buffer_release(mf->buf);
		mf->buf = NULL;

		if (NULL == req) {
			if (NULL != err) {
				if (LI_MEMCACHED_DISABLED != err->code) {
					VR_ERROR(vr, "memcached.store: set failed: %s", err->message);
				}
				g_clear_error(&err);
			} else {
				VR_ERROR(vr, "memcached.store: set failed: %s", "Unkown error");
			}
		}
	}

	return LI_HANDLER_GO_ON;

forward:
	li_chunkqueue_steal_all(f->out, f->in);
	if (f->in->is_closed) f->out->is_closed = f->in->is_closed;
	return LI_HANDLER_GO_ON;
}

static liHandlerResult mc_handle_store(liVRequest *vr, gpointer param, gpointer *context) {
	memcached_ctx *ctx = param;
	memcache_filter *mf;
	UNUSED(context);

	VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

	if (vr->response.http_status != 200) return LI_HANDLER_GO_ON;

	mf = g_slice_new0(memcache_filter);
	mf->ctx = ctx;
	mc_ctx_acquire(ctx);
	mf->buf = li_buffer_new(ctx->maxsize);

	li_vrequest_add_filter_out(vr, memcache_store_filter, memcache_store_filter_free, mf);

	return LI_HANDLER_GO_ON;
}

static liAction* mc_lookup_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	memcached_ctx *ctx;
	liValue *config = val, *act_found = NULL, *act_miss = NULL;
	UNUSED(wrk);
	UNUSED(userdata);

	if (val && LI_VALUE_LIST == val->type) {
		GArray *list = val->data.list;
		config = NULL;

		if (list->len > 3) {
			ERROR(srv, "%s", "memcached.lookup: too many arguments");
			return NULL;
		}

		if (list->len >= 1) config = g_array_index(list, liValue*, 0);
		if (list->len >= 2) act_found = g_array_index(list, liValue*, 1);
		if (list->len >= 3) act_miss = g_array_index(list, liValue*, 2);

		if (config && config->type != LI_VALUE_HASH) {
			ERROR(srv, "%s", "memcached.lookup: expected hash as first argument");
			return NULL;
		}

		if (act_found && act_found->type != LI_VALUE_ACTION) {
			ERROR(srv, "%s", "memcached.lookup: expected action as second argument");
			return NULL;
		}

		if (act_miss && act_miss->type != LI_VALUE_ACTION) {
			ERROR(srv, "%s", "memcached.lookup: expected action as third argument");
			return NULL;
		}
	}

	ctx = mc_ctx_parse(srv, p, config);

	if (!ctx) return NULL;

	if (act_found) ctx->act_found = li_value_extract_action(act_found);
	if (act_miss) ctx->act_miss = li_value_extract_action(act_miss);

	return li_action_new_function(mc_handle_lookup, mc_lookup_handle_free, mc_ctx_release, ctx);
}

static liAction* mc_store_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	memcached_ctx *ctx;
	UNUSED(wrk);
	UNUSED(userdata);

	ctx = mc_ctx_parse(srv, p, val);

	if (!ctx) return NULL;

	return li_action_new_function(mc_handle_store, NULL, mc_ctx_release, ctx);
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "memcached.lookup", mc_lookup_create, NULL },
	{ "memcached.store", mc_store_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

#ifdef HAVE_LUA_H

typedef struct {
	liMemcachedRequest *req;
	int result_ref; /* table if vr_ref == NULL, callback function otherwise */
	liVRequestRef *vr_ref;
	lua_State *L;
} mc_lua_request;

#define LUA_MEMCACHEDCON "liMemcachedCon*"
#define LUA_MEMCACHEDREQUEST "mc_lua_request*"

static liMemcachedCon* li_lua_get_memcached_con(lua_State *L, int ndx);
static int lua_memcached_con_gc(lua_State *L);
static int li_lua_push_memcached_con(lua_State *L, liMemcachedCon *con);
static mc_lua_request* li_lua_get_memcached_req(lua_State *L, int ndx);
static int lua_memcached_req_gc(lua_State *L);
static int li_lua_push_memcached_req(lua_State *L, mc_lua_request *req);

static void lua_memcache_callback(liMemcachedRequest *request, liMemcachedResult result, liMemcachedItem *item, GError **err) {
	mc_lua_request *mreq = request->cb_data;
	lua_State *L = mreq->L;

	if (mreq->req != request) return;

	request->cb_data = NULL;
	request->callback = NULL;
	mreq->req = NULL;

	if (mreq->vr_ref) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, mreq->result_ref); /* get table */
	} else {
		lua_rawgeti(L, LUA_REGISTRYINDEX, mreq->result_ref); /* get function */
		lua_newtable(L);
	}

	lua_pushnumber(L, result);
	lua_setfield(L, -2, "code");

	if (err && *err) {
		lua_pushstring(L, (*err)->message);
		lua_setfield(L, -2, "error");
	} else if (item) {
		if (item->key) {
			lua_pushlstring(L, GSTR_LEN(item->key));
			lua_setfield(L, -2, "key");
		}
		lua_pushnumber(L, item->flags);
		lua_setfield(L, -2, "flags");
		lua_pushnumber(L, item->ttl);
		lua_setfield(L, -2, "ttl");
		{
			GString *cas = g_string_sized_new(31);
			g_string_printf(cas, "%"G_GUINT64_FORMAT, item->cas);
			lua_pushlstring(L, GSTR_LEN(cas));
			lua_setfield(L, -2, "cas");
			g_string_free(cas, TRUE);
		}
		if (item->data) {
			lua_pushlstring(L, item->data->addr, item->data->used);
			lua_setfield(L, -2, "data");
		}
	}

	if (mreq->vr_ref) {
		lua_pop(L, 1);
		li_vrequest_joblist_append_async(mreq->vr_ref);
	} else {
		liServer *srv;
		int errfunc;

		lua_getfield(L, LUA_REGISTRYINDEX, "lighty.srv");
		srv = lua_touserdata(L, -1);
		lua_pop(L, 1);

		errfunc = li_lua_push_traceback(L, 1);
		if (lua_pcall(L, 1, 0, errfunc)) {
			ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		lua_remove(L, errfunc);
		/* function and args were popped */
	}
}

static int lua_mc_get(lua_State *L) {
	liMemcachedCon *con;
	GString key;
	const char *str;
	size_t len;
	GError *err = NULL;
	liVRequest *vr;

	mc_lua_request *mreq;
	liMemcachedRequest *req;

	if (lua_gettop(L) != 3) {
		lua_pushliteral(L, "lua_mc_get(con, key, cb | vr): incorrect number of arguments");
		lua_error(L);
	}

	con = li_lua_get_memcached_con(L, 1);
	vr = li_lua_get_vrequest(L, 3);
	if (NULL == con || !lua_isstring(L, 2) || (NULL == vr && !lua_isfunction(L, 3))) {
		lua_pushliteral(L, "lua_mc_get(con, key, cb | vr): wrong argument types");
		lua_error(L);
	}

	str = lua_tolstring(L, 2, &len);
	key = li_const_gstring(str, len);

	mreq = g_slice_new0(mc_lua_request);

	req = li_memcached_get(con, &key, lua_memcache_callback, mreq, &err);

	if (!req) {
		g_slice_free(mc_lua_request, mreq);

		lua_pushnil(L);
		if (NULL != err) {
			lua_pushstring(L, err->message);
			g_clear_error(&err);
		} else {
			lua_pushliteral(L, "Unknown li_memcached_get error");
		}
		return 2;
	}

	mreq->req = req;
	mreq->L = L;

	if (NULL == vr) {
		/* lua callback function */
		lua_pushvalue(L, 3); /* +1 */
		mreq->result_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
	} else {
		/* push result into table, wake vr if done */
		lua_newtable(L); /* +1 */
		mreq->result_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
		mreq->vr_ref = li_vrequest_get_ref(vr);
	}

	return li_lua_push_memcached_req(L, mreq);
}

static int lua_mc_set(lua_State *L) {
	liMemcachedCon *con;
	GString key, value;
	const char *str;
	size_t len;
	GError *err = NULL;
	liVRequest *vr;
	ev_tstamp ttl;
	liBuffer *valuebuf;

	mc_lua_request *mreq;
	liMemcachedRequest *req;

	if (lua_gettop(L) < 4) {
		lua_pushliteral(L, "lua_mc_set(con, key, value, cb | vr, [ttl]): incorrect number of arguments");
		lua_error(L);
	}

	con = li_lua_get_memcached_con(L, 1);
	vr = li_lua_get_vrequest(L, 4);
	if (NULL == con || !lua_isstring(L, 2) || (NULL == vr && !lua_isfunction(L, 4))) {
		lua_pushliteral(L, "lua_mc_set(con, key, value, cb | vr): wrong argument types");
		lua_error(L);
	}

	str = lua_tolstring(L, 2, &len);
	key = li_const_gstring(str, len);

	str = lua_tolstring(L, 3, &len);
	value = li_const_gstring(str, len);

	if (lua_gettop(L) == 5) {
		ttl = lua_tonumber(L, 5);
	} else {
		ttl = 300;
	}

	mreq = g_slice_new0(mc_lua_request);
	valuebuf = li_buffer_new(value.len);
	valuebuf->used = value.len;
	memcpy(valuebuf->addr, value.str, value.len);

	req = li_memcached_set(con, &key, 0, ttl, valuebuf, lua_memcache_callback, mreq, &err);

	li_buffer_release(valuebuf);

	if (!req) {
		g_slice_free(mc_lua_request, mreq);

		lua_pushnil(L);
		if (NULL != err) {
			lua_pushstring(L, err->message);
			g_clear_error(&err);
		} else {
			lua_pushliteral(L, "Unknown li_memcached_set error");
		}
		return 2;
	}

	mreq->req = req;
	mreq->L = L;

	if (NULL == vr) {
		/* lua callback function */
		lua_pushvalue(L, 3); /* +1 */
		mreq->result_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
	} else {
		/* push result into table, wake vr if done */
		lua_newtable(L); /* +1 */
		mreq->result_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
		mreq->vr_ref = li_vrequest_get_ref(vr);
	}

	return li_lua_push_memcached_req(L, mreq);
}

static int lua_mc_setq(lua_State *L) {
	liMemcachedCon *con;
	GString key, value;
	const char *str;
	size_t len;
	GError *err = NULL;
	ev_tstamp ttl;
	liBuffer *valuebuf;

	liMemcachedRequest *req;

	if (lua_gettop(L) < 3) {
		lua_pushliteral(L, "lua_mc_setq(con, key, value, [ttl]): incorrect number of arguments");
		lua_error(L);
	}

	con = li_lua_get_memcached_con(L, 1);
	if (NULL == con || !lua_isstring(L, 2)) {
		lua_pushliteral(L, "lua_mc_setq(con, key, value): wrong argument types");
		lua_error(L);
	}

	str = lua_tolstring(L, 2, &len);
	key = li_const_gstring(str, len);

	str = lua_tolstring(L, 3, &len);
	value = li_const_gstring(str, len);

	if (lua_gettop(L) == 5) {
		ttl = lua_tonumber(L, 5);
	} else {
		ttl = 300;
	}

	valuebuf = li_buffer_new(value.len);
	valuebuf->used = value.len;
	memcpy(valuebuf->addr, value.str, value.len);

	req = li_memcached_set(con, &key, 0, ttl, valuebuf, NULL, NULL, &err);

	li_buffer_release(valuebuf);

	if (!req) {
		lua_pushnil(L);
		if (NULL != err) {
			lua_pushstring(L, err->message);
			g_clear_error(&err);
		} else {
			lua_pushliteral(L, "Unknown li_memcached_set error");
		}
		return 2;
	}

	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg mc_con_mt[] = {
	{ "__gc", lua_memcached_con_gc },

	{ "get", lua_mc_get },
	{ "set", lua_mc_set },
	{ "setq", lua_mc_setq },

	{ NULL, NULL }
};

typedef int (*lua_mc_req_Attrib)(mc_lua_request *req, lua_State *L);

static int lua_mc_req_attr_read_response(mc_lua_request *req, lua_State *L) {
	if (NULL != req->vr_ref) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, req->result_ref);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

#define AR(m) { #m, lua_mc_req_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_mc_req_attr_write_##m }
#define ARW(m) { #m, lua_mc_req_attr_read_##m, lua_mc_req_attr_write_##m }

static const struct {
	const char* key;
	lua_mc_req_Attrib read_attr, write_attr;
} mc_req_attribs[] = {
	AR(response),

	{ NULL, NULL, NULL }
};

static int lua_mc_req_index(lua_State *L) {
	mc_lua_request *req;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	req = li_lua_get_memcached_req(L, 1);
	if (!req) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; mc_req_attribs[i].key ; i++) {
		if (0 == strcmp(key, mc_req_attribs[i].key)) {
			if (mc_req_attribs[i].read_attr)
				return mc_req_attribs[i].read_attr(req, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in mc_lua_request");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg mc_req_mt[] = {
	{ "__index", lua_mc_req_index },
	{ "__gc", lua_memcached_req_gc },

	{ NULL, NULL }
};

static void init_mc_con_mt(lua_State *L) {
	luaL_register(L, NULL, mc_con_mt);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
}

static void init_mc_req_mt(lua_State *L) {
	luaL_register(L, NULL, mc_req_mt);
}

static liMemcachedCon* li_lua_get_memcached_con(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_MEMCACHEDCON);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liMemcachedCon**) lua_touserdata(L, ndx);
}

static int lua_memcached_con_gc(lua_State *L) {
	liMemcachedCon **pcon = (liMemcachedCon**) luaL_checkudata(L, 1, LUA_MEMCACHEDCON);
	if (!pcon || !*pcon) return 0;

	li_memcached_con_release(*pcon);
	return 0;
}

static int li_lua_push_memcached_con(lua_State *L, liMemcachedCon *con) {
	liMemcachedCon **pcon;

	pcon = (liMemcachedCon**) lua_newuserdata(L, sizeof(liMemcachedCon*));
	*pcon = con;

	if (luaL_newmetatable(L, LUA_MEMCACHEDCON)) {
		init_mc_con_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

static mc_lua_request* li_lua_get_memcached_req(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_MEMCACHEDREQUEST);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(mc_lua_request**) lua_touserdata(L, ndx);
}

static int lua_memcached_req_gc(lua_State *L) {
	mc_lua_request **preq = (mc_lua_request**) luaL_checkudata(L, 1, LUA_MEMCACHEDREQUEST);
	mc_lua_request *req;
	if (!preq || !*preq) return 0;

	req = *preq;
	li_vrequest_ref_release(req->vr_ref);

	if (req->req) {
		req->req->callback = NULL;
		req->req->cb_data = NULL;
	}

	luaL_unref(L, LUA_REGISTRYINDEX, req->result_ref);

	g_slice_free(mc_lua_request, req);

	return 0;
}

static int li_lua_push_memcached_req(lua_State *L, mc_lua_request *req) {
	mc_lua_request **preq;

	preq = (mc_lua_request**) lua_newuserdata(L, sizeof(mc_lua_request*));
	*preq = req;

	if (luaL_newmetatable(L, LUA_MEMCACHEDREQUEST)) {
		init_mc_req_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

static int mc_lua_new(lua_State *L) {
	liWorker *wrk;
	liMemcachedCon *con;
	liSocketAddress addr;
	const char *buf;
	size_t len = 0;
	GString fakestr;

	wrk = (liWorker*) lua_touserdata(L, lua_upvalueindex(1));

	if (lua_type(L, -1) != LUA_TSTRING) {
		/* duplicate */
		lua_pushvalue(L, -1);
	}

	buf = lua_tolstring(L, -1, &len);
	if (!buf) {
		lua_pushliteral(L, "[mod_memcached] mc_lua_new: Couldn't convert parameter to string");
		lua_error(L);
	}
	fakestr = li_const_gstring(buf, len);
	addr = li_sockaddr_from_string(&fakestr, 0);

	if (NULL == addr.addr) {
		lua_pushliteral(L, "[mod_memcached] mc_lua_new: couldn't parse parameter as address: ");
		lua_pushvalue(L, -2);
		lua_concat(L, 2);
		lua_error(L);
	}

	con = li_memcached_con_new(wrk->loop, addr);
	return li_lua_push_memcached_con(L, con);
}

static void mod_memcached_lua_init(lua_State *L, liServer *srv, liWorker *wrk, liPlugin *p) {
	UNUSED(srv);
	UNUSED(p);

	if (wrk) {
		lua_newtable(L);                   /* { } */

		lua_pushlightuserdata(L, wrk);
		lua_pushcclosure(L, mc_lua_new, 1);
		lua_setfield(L, -2, "new");

		lua_setfield(L, LUA_GLOBALSINDEX, "memcached");
	}
}
#endif

static void memcached_prepare(liServer *srv, liPlugin *p) {
	memcached_config *mconf = p->data;
	GList *conf_link;
	memcached_ctx *ctx;

	while (NULL != (conf_link = g_queue_pop_head_link(&mconf->prepare_ctx))) {
		ctx = conf_link->data;
		ctx->worker_client_ctx = g_slice_alloc0(sizeof(liMemcachedCon*) * srv->worker_count);
		conf_link->data = NULL;
	}
}

static void memcached_free(liServer *srv, liPlugin *p) {
	memcached_config *mconf = p->data;
	UNUSED(srv);

	g_slice_free(memcached_config, mconf);
}

static void memcached_init(liServer *srv, liPlugin *p, gpointer userdata) {
	memcached_config *mconf;
	UNUSED(srv); UNUSED(userdata);

	mconf = g_slice_new0(memcached_config);
	p->data = mconf;

	p->options = options;
	p->actions = actions;
	p->setups = setups;

	p->free = memcached_free;

	p->handle_prepare = memcached_prepare;

#ifdef HAVE_LUA_H
	p->handle_init_lua = mod_memcached_lua_init;
#endif
}

gboolean mod_memcached_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_memcached", memcached_init, NULL);

	return mod->config != NULL;
}

gboolean mod_memcached_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
