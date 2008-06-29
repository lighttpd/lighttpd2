
#include "condition.h"
#include "log.h"

static condition* condition_find_cached(server *srv, GString *key);
static void condition_cache_insert(server *srv, GString *key, condition *c);
static condition* condition_new(config_cond_t cond, comp_key_t comp);
static condition* condition_new_with_string(config_cond_t cond, comp_key_t comp, GString *str);
static condition* condition_new_with_int(config_cond_t cond, comp_key_t comp, gint i);
static void condition_free(condition *c);

static condition* condition_find_cached(server *srv, GString *key) {
	UNUSED(srv);
	UNUSED(key);

	return NULL;
}

static void condition_cache_insert(server *srv, GString *key, condition *c) {
	UNUSED(srv);
	UNUSED(c);

	g_string_free(key, TRUE);
}

static condition* condition_new(config_cond_t cond, comp_key_t comp) {
	condition *c = g_slice_new0(condition);
	c->refcount = 1;
	c->cache_index = -1;
	c->cond = cond;
	c->comp = comp;
	return c;
}

static condition* condition_new_with_string(config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c = condition_new(cond, comp);
	switch (c->cond) {
	case CONFIG_COND_EQ:      /** == */
	case CONFIG_COND_NE:      /** != */
		c->value.string = str;
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
#ifdef HAVE_PCRE_H
		/* TODO */
		ERROR("Regular expressions not supported for now in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str);
		condition_free(c);
		return NULL;
		break;
#else
		ERROR("Regular expressions not supported in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str->str);
		condition_free(c);
		return NULL;
#endif
	case CONFIG_COND_GT:      /** > */
	case CONFIG_COND_GE:      /** >= */
	case CONFIG_COND_LT:      /** < */
	case CONFIG_COND_LE:      /** <= */
		ERROR("Cannot compare with strings in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str->str);
		condition_free(c);
		return NULL;
	}
	return c;
}

static condition* condition_new_with_int(config_cond_t cond, comp_key_t comp, gint i) {
	condition *c = condition_new(cond, comp);
	switch (c->cond) {
	case CONFIG_COND_EQ:      /** == */
	case CONFIG_COND_NE:      /** != */
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
		ERROR("Cannot compare with integer in condition: %s %s %i",
			config_cond_to_string(cond), comp_key_to_string(comp),
			i);
		condition_free(c);
		return NULL;
	case CONFIG_COND_GT:      /** > */
	case CONFIG_COND_GE:      /** >= */
	case CONFIG_COND_LT:      /** < */
	case CONFIG_COND_LE:      /** <= */
		c->value.i = i;
		break;
	}
	return c;
}

condition* condition_new_string(server *srv, config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i:%s", (int) cond, (int) comp, str->str);

	if (NULL != (c = condition_find_cached(srv, key))) {
		g_string_free(key, TRUE);
		return c;
	}

	c = condition_new_with_string(cond, comp, str);
	condition_cache_insert(srv, key, c);
	return c;
}

condition* condition_new_int(server *srv, config_cond_t cond, comp_key_t comp, gint i) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i#%i", (int) cond, (int) comp, i);

	if (NULL != (c = condition_find_cached(srv, key))) {
		g_string_free(key, TRUE);
		return c;
	}

	c = condition_new_with_int(cond, comp, i);
	condition_cache_insert(srv, key, c);
	return c;
}

condition* condition_new_string_uncached(server *srv, config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i:%s", (int) cond, (int) comp, str->str);

	c = condition_find_cached(srv, key);
	g_string_free(key, TRUE);
	if (NULL != c) return c;

	return condition_new_with_string(cond, comp, str);
}

condition* condition_new_int_uncached(server *srv, config_cond_t cond, comp_key_t comp, gint i) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i#%i", (int) cond, (int) comp, i);

	c = condition_find_cached(srv, key);
	g_string_free(key, TRUE);
	if (NULL != c) return c;

	return condition_new_with_int(cond, comp, i);
}

static void condition_free(condition *c) {
	switch (c->cond) {
	case CONFIG_COND_EQ:      /** == */
	case CONFIG_COND_NE:      /** != */
		g_string_free(c->value.string, TRUE);
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
#ifdef HAVE_PCRE_H
		if (c->value.regex) pcre_free(c->value.regex);
		if (c->value.regex_study) pcre_free(c->value.regex_study);
#endif
		break;
	case CONFIG_COND_GT:      /** > */
	case CONFIG_COND_GE:      /** >= */
	case CONFIG_COND_LT:      /** < */
	case CONFIG_COND_LE:      /** <= */
		break;
	}
	g_slice_free(condition, c);
}

void condition_release(condition* c) {
	/* assert(c->recount > 0); */
	if (!(--c->refcount)) {
		condition_free(c);
	}
}

const char* config_cond_to_string(config_cond_t cond) {
	UNUSED(cond);

	/* TODO */
	return "";
}

const char* comp_key_to_string(comp_key_t comp) {
	UNUSED(comp);

	/* TODO */
	return "";
}
