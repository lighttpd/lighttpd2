
#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

LI_API gboolean mod_throttle_init(liModules *mods, liModule *mod);
LI_API gboolean mod_throttle_free(liModules *mods, liModule *mod);

typedef struct {
	int refcount;
	liThrottlePool *pool;
} refcounted_pool_entry;

typedef struct {
	guint refcount;

	GMutex *lock;
	guint plugin_id;

	guint rate, burst;

	guint masklen_ipv4, masklen_ipv6;
	liRadixTree *ipv4_pools; /* <refcounted_pool_entry> */
	liRadixTree *ipv6_pools; /* <refcounted_pool_entry> */
} throttle_ip_pools;

typedef struct {
	throttle_ip_pools *pools;
	liSocketAddress remote_addr_copy;
} vr_ip_pools_entry;

static gboolean sanity_check(liServer *srv, guint64 rate, guint64 burst) {
	if (rate < (8*1024)) {
		ERROR(srv, "throttle: rate %"G_GINT64_FORMAT" is too low (8KiByte/s minimum)", rate);
		return FALSE;
	}

	if (rate > (512*1024*1024)) {
		ERROR(srv, "throttle: rate %"G_GINT64_FORMAT" is too high (512MiByte/s maximum)", rate);
		return FALSE;
	}

	if (burst < (rate * THROTTLE_GRANULARITY) / 1000) {
		ERROR(srv, "%s", "throttle: burst is too small for the specified rate");
		return FALSE;
	}

	if (burst > (512*1024*1024)) {
		ERROR(srv, "throttle: burst %"G_GINT64_FORMAT" is too high (512MiByte maximum)", burst);
		return FALSE;
	}

	return TRUE;
}

static liThrottleState* vr_get_throttle_out_state(liVRequest *vr) {
	return vr->coninfo->callbacks->throttle_out(vr);
}

/*************************************************************/
/* IP pools                                                  */
/*   manage pool per CIDR block                              */
/*************************************************************/

static throttle_ip_pools *ip_pools_new(guint plugin_id, guint rate, guint burst, guint masklen_ipv4, guint masklen_ipv6) {
	throttle_ip_pools *pools = g_slice_new0(throttle_ip_pools);
	pools->lock = g_mutex_new();
	pools->plugin_id = plugin_id;
	pools->rate = rate;
	pools->burst = burst;
	pools->masklen_ipv4 = masklen_ipv4;
	pools->masklen_ipv6 = masklen_ipv6;
	pools->ipv4_pools = li_radixtree_new();
	pools->ipv6_pools = li_radixtree_new();
	return pools;
}

static void ip_pools_free(throttle_ip_pools *pools) {
	assert(g_atomic_int_get(&pools->refcount) > 0);

	if (g_atomic_int_dec_and_test(&pools->refcount)) {
		g_mutex_free(pools->lock);
		pools->lock = NULL;

		/* entries keep references, so radix trees must be empty */
		li_radixtree_free(pools->ipv4_pools, NULL, NULL);
		li_radixtree_free(pools->ipv6_pools, NULL, NULL);
		g_slice_free(throttle_ip_pools, pools);
	}
}

static refcounted_pool_entry* create_ip_pool(liServer *srv, throttle_ip_pools *pools, liSocketAddress *remote_addr) {
	refcounted_pool_entry *result;

	switch (remote_addr->addr->plain.sa_family) {
	case AF_INET:
	case AF_INET6:
		break;
	default:
		return NULL;
	}

	assert(g_atomic_int_get(&pools->refcount) > 0);
	g_atomic_int_inc(&pools->refcount);

	g_mutex_lock(pools->lock);
		if (remote_addr->addr->plain.sa_family == AF_INET) {
			result = li_radixtree_lookup_exact(pools->ipv4_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, pools->masklen_ipv4);
		} else {
			result = li_radixtree_lookup_exact(pools->ipv6_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, pools->masklen_ipv6);
		}
		if (NULL == result) {
			result = g_slice_new0(refcounted_pool_entry);
			result->refcount = 1;
			result->pool = li_throttle_pool_new(srv, pools->rate, pools->burst);

			if (remote_addr->addr->plain.sa_family == AF_INET) {
				li_radixtree_insert(pools->ipv4_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, pools->masklen_ipv4, result);
			} else {
				li_radixtree_insert(pools->ipv6_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, pools->masklen_ipv6, result);
			}
		} else {
			assert(g_atomic_int_get(&result->refcount) > 0);
			g_atomic_int_inc(&result->refcount);
		}
	g_mutex_unlock(pools->lock);

	return result;
}

static void free_ip_pool(liServer *srv, throttle_ip_pools *pools, liSocketAddress *remote_addr) {
	refcounted_pool_entry *entry;

	switch (remote_addr->addr->plain.sa_family) {
	case AF_INET:
	case AF_INET6:
		break;
	default:
		return;
	}

	g_mutex_lock(pools->lock);
		if (remote_addr->addr->plain.sa_family == AF_INET) {
			entry = li_radixtree_lookup_exact(pools->ipv4_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, pools->masklen_ipv4);
		} else {
			entry = li_radixtree_lookup_exact(pools->ipv6_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, pools->masklen_ipv6);
		}
		assert(NULL != entry);
		assert(g_atomic_int_get(&entry->refcount) > 0);
		if (g_atomic_int_dec_and_test(&entry->refcount)) {
			if (remote_addr->addr->plain.sa_family == AF_INET) {
				li_radixtree_remove(pools->ipv4_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, pools->masklen_ipv4);
			} else {
				li_radixtree_remove(pools->ipv6_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, pools->masklen_ipv6);
			}
			li_throttle_pool_release(entry->pool, srv);
			g_slice_free(refcounted_pool_entry, entry);
		}
	g_mutex_unlock(pools->lock);

	ip_pools_free(pools);
}


/*************************************************************/
/* throttle pool                                             */
/*************************************************************/

static void core_throttle_pool_free(liServer *srv, gpointer param) {
	liThrottlePool *pool = param;

	li_throttle_pool_release(pool, srv);
}

static liHandlerResult core_handle_throttle_pool(liVRequest *vr, gpointer param, gpointer *context) {
	liThrottlePool *pool = param;
	liThrottleState *state = vr_get_throttle_out_state(vr);
	UNUSED(context);

	if (NULL != state) {
		li_throttle_add_pool(vr->wrk, state, pool);
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_pool(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liThrottlePool *pool = NULL;
	gint64 rate, burst;

	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "'io.throttle_pool' action expects a number as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	rate = val->data.number;
	burst = rate;
	if (!sanity_check(srv, rate, burst)) return NULL;

	pool = li_throttle_pool_new(srv, rate, burst);

	return li_action_new_function(core_handle_throttle_pool, NULL, core_throttle_pool_free, pool);
}

/*************************************************************/
/* throttle ip pools                                         */
/*************************************************************/

static void core_throttle_ip_free(liServer *srv, gpointer param) {
	throttle_ip_pools *pools = param;
	UNUSED(srv);

	ip_pools_free(pools);
}

static liHandlerResult core_handle_throttle_ip(liVRequest *vr, gpointer param, gpointer *context) {
	throttle_ip_pools *pools = param;
	liThrottleState *state = vr_get_throttle_out_state(vr);
	UNUSED(context);

	if (NULL != state) {
		refcounted_pool_entry *entry = create_ip_pool(vr->wrk->srv, pools, &vr->coninfo->remote_addr);
		if (NULL != entry) {
			if (!li_throttle_add_pool(vr->wrk, state, entry->pool)) {
				/* we already had a reference */
				g_atomic_int_add(&pools->refcount, -1);
				assert(g_atomic_int_get(&pools->refcount) > 0);
				g_atomic_int_add(&entry->refcount, -1);
				assert(g_atomic_int_get(&entry->refcount) > 0);
			} else {
				GArray *vr_ip_pools = (GArray*) g_ptr_array_index(vr->plugin_ctx, pools->plugin_id);
				vr_ip_pools_entry ventry;
				if (NULL == vr_ip_pools) {
					vr_ip_pools = g_array_new(FALSE, TRUE, sizeof(vr_ip_pools_entry));
					g_ptr_array_index(vr->plugin_ctx, pools->plugin_id) = vr_ip_pools;
				}
				ventry.pools = pools;
				ventry.remote_addr_copy = li_sockaddr_dup(vr->coninfo->remote_addr);
				g_array_append_val(vr_ip_pools, ventry);
			}
		}
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_ip(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gint64 rate, burst = 0;
	guint masklen_ipv4 = 32, masklen_ipv6 = 56;
	throttle_ip_pools *pools;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "'io.throttle_ip' action expects a positiv integer as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	rate = val->data.number;
	burst = rate;
	if (!sanity_check(srv, rate, burst)) return NULL;

	pools = ip_pools_new(p->id, rate, burst, masklen_ipv4, masklen_ipv6);

	return li_action_new_function(core_handle_throttle_ip, NULL, core_throttle_ip_free, pools);
}

/*************************************************************/
/* throttle connection                                       */
/*************************************************************/

typedef struct liThrottleParam liThrottleParam;
struct liThrottleParam {
	guint rate, burst;
};

static void core_throttle_connection_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_slice_free(liThrottleParam, param);
}


static liHandlerResult core_handle_throttle_connection(liVRequest *vr, gpointer param, gpointer *context) {
	liThrottleParam *throttle_param = param;
	liThrottleState *state = vr_get_throttle_out_state(vr);
	UNUSED(context);

	if (NULL != state) {
		li_throttle_set(vr->wrk, state, throttle_param->rate, throttle_param->burst);
	}

	return LI_HANDLER_GO_ON;
}

static liAction* core_throttle_connection(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liThrottleParam *param;
	guint64 rate, burst;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val->type == LI_VALUE_LIST && val->data.list->len == 2) {
		liValue *v1 = g_array_index(val->data.list, liValue*, 0);
		liValue *v2 = g_array_index(val->data.list, liValue*, 1);

		if (v1->type != LI_VALUE_NUMBER || v2->type != LI_VALUE_NUMBER) {
			ERROR(srv, "%s", "'io.throttle' action expects a positiv integer or a pair of those as parameter");
			return NULL;
		}

		rate = v2->data.number;
		burst = v1->data.number;
	} else if (val->type == LI_VALUE_NUMBER) {
		rate = val->data.number;
		burst  = 2 * rate;
	} else {
		ERROR(srv, "'io.throttle' action expects a positiv integer or a pair of those as parameter, %s given", li_value_type_string(val->type));
		return NULL;
	}

	if ((rate != 0 || burst != 0) && !sanity_check(srv, rate, burst)) return NULL;

	param = g_slice_new(liThrottleParam);
	param->rate = rate;
	param->burst = burst;

	return li_action_new_function(core_handle_throttle_connection, NULL, core_throttle_connection_free, param);
}

/*************************************************************/
/*************************************************************/

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "io.throttle", core_throttle_connection, NULL },
	{ "io.throttle_pool", core_throttle_pool, NULL },
	{ "io.throttle_ip", core_throttle_ip, NULL },
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

static void throttle_vrclose(liVRequest *vr, liPlugin *p) {
	GArray *vr_ip_pools = (GArray*) g_ptr_array_index(vr->plugin_ctx, p->id);
	guint i, len;
	liServer *srv = vr->wrk->srv;

	if (NULL != vr_ip_pools) {
		g_ptr_array_index(vr->plugin_ctx, p->id) = NULL;
		for (i = 0, len = vr_ip_pools->len; i < len; ++i) {
			vr_ip_pools_entry *ventry = &g_array_index(vr_ip_pools, vr_ip_pools_entry, i);
			free_ip_pool(srv, ventry->pools, &ventry->remote_addr_copy);
			li_sockaddr_clear(&ventry->remote_addr_copy);
			g_slice_free(vr_ip_pools_entry, ventry);
		}
		g_array_free(vr_ip_pools, TRUE);
	}
}

static void plugin_throttle_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;

	p->handle_vrclose = throttle_vrclose;
}


gboolean mod_throttle_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_throttle", plugin_throttle_init, NULL);

	return mod->config != NULL;
}

gboolean mod_throttle_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
