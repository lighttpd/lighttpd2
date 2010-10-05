#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

typedef struct {
	enum {
		PATTERN_STRING,     /* literal */
		PATTERN_NTH,        /* $n */
		PATTERN_NTH_PREV,   /* %n */
		PATTERN_VAR,        /* %{req.foo} */
		PATTERN_VAR_ENCODED /* %{enc:req.foo} */
	} type;

	union {
		/* PATTERN_STRING */
		GString *str;
		/* PATTERN_NTH and PATTERN_NTH_PREV */
		struct {
			guint from, to;
		} range;
		/* PATTERN_VAR and PATTERN_VAR_ENCODED */
		liConditionLValue *lvalue;
	} data;
} liPatternPart;

static gboolean parse_range(liServer *srv, liPatternPart *part, const gchar **str, const gchar *origstr) {
	guint64 val;
	gchar *endc = NULL;
	const gchar *c = *str;

	c++; /* skip '[' */
	if (*c == '-') {
		if (c[1] == ']') {
			/* parse error */
			ERROR(srv, "could not parse pattern, empty range %%[-]: \"%s\"", origstr);
			return FALSE;
		}
		part->data.range.from = G_MAXUINT;
	} else if (*c == ']') {
		/* parse error */
		ERROR(srv, "could not parse pattern, empty range %%[]: \"%s\"", origstr);
		return FALSE;
	} else {
		errno = 0;
		val = g_ascii_strtoull(c, &endc, 10);
		if (0 != errno || val > G_MAXUINT) {
			ERROR(srv, "could not parse pattern, range overflow: \"%s\"", origstr);
			return FALSE;
		} else if (endc == c || (*endc != '-' && *endc != ']')) {
			ERROR(srv, "could not parse pattern, invalid range: \"%s\"", origstr);
			return FALSE;
		}
		part->data.range.from = val;
		c = endc;
	}

	part->data.range.to = part->data.range.from;

	if (*c == '-') {
		c++;
		if (*c == ']') {
			part->data.range.to = G_MAXUINT;
		} else {
			errno = 0;
			val = g_ascii_strtoull(c, &endc, 10);
			if (0 != errno || val > G_MAXUINT) {
				ERROR(srv, "could not parse pattern, range overflow: \"%s\"", origstr);
				return FALSE;
			} else if (endc == c || (*endc != ']')) {
				ERROR(srv, "could not parse pattern, invalid range: \"%s\"", origstr);
				return FALSE;
			}
			part->data.range.to = val;
			c = endc;
		}
	}

	c++; /* skip ']' */

	*str = c;
	return TRUE;
}

liPattern *li_pattern_new(liServer *srv, const gchar* str) {
	GArray *pattern;
	liPatternPart part;
	const gchar *c;
	gboolean encoded;

	pattern = g_array_new(FALSE, TRUE, sizeof(liPatternPart));

	for (c = str; *c;) {
		if (*c == '$') {
			/* $n, PATTERN_NTH */
			c++;
			if (*c >= '0' && *c <= '9') {
				part.type = PATTERN_NTH;
				part.data.range.from = part.data.range.to = *c - '0';
				g_array_append_val(pattern, part);
				c++;
			} else if ('[' == *c) {
				part.type = PATTERN_NTH;
				if (!parse_range(srv, &part, &c, str)) {
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}
				g_array_append_val(pattern, part);
			} else {
				/* parse error */
				ERROR(srv, "could not parse pattern: \"%s\"", str);
				li_pattern_free((liPattern*)pattern);
				return NULL;
			}
		} else if (*c == '%') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* %n, PATTERN_NTH_PREV */
				part.type = PATTERN_NTH_PREV;
				part.data.range.from = part.data.range.to = *c - '0';
				g_array_append_val(pattern, part);
				c++;
			} else if ('[' == *c) {
				part.type = PATTERN_NTH_PREV;
				if (!parse_range(srv, &part, &c, str)) {
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}
				g_array_append_val(pattern, part);
			} else if (*c == '{') {
				/* %{var}, PATTERN_VAR */
				const gchar *lval_c, *lval_start;
				guint lval_len = 0;
				GString *key = NULL;

				lval_c = c+1;
				encoded = FALSE;

				if (g_str_has_prefix(lval_c, "enc:")) {
					/* %{enc:var}, PATTERN_VAR_ENCODED */
					lval_c += sizeof("enc:")-1;
					encoded = TRUE;
				}

				/* search for closing '}' */
				for (lval_start = lval_c; *lval_c != '\0' && *lval_c != '}'; lval_c++) {
					/* got a key */
					if (*lval_c == '[') {
						const gchar *key_c, *key_start;
						guint key_len = 0;

						/* search for closing ']' */
						for (key_start = key_c = lval_c+1; *key_c != '\0' && *key_c != ']'; key_c++) {
							key_len++;
						}

						if (key_len == 0 || *key_c != ']' || *(key_c+1) != '}') {
							/* parse error */
							ERROR(srv, "could not parse pattern: \"%s\"", str);
							li_pattern_free((liPattern*)pattern);
							return NULL;
						}

						key = g_string_new_len(key_start, key_len);
						break;
					}

					lval_len++;
				}

				/* adjust c */
				c = lval_start + lval_len + (key ? (key->len+2) : 0);

				if (*c != '}') {
					/* parse error */
					ERROR(srv, "could not parse pattern: \"%s\"", str);
					if (key)
						g_string_free(key, TRUE);
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}

				part.data.lvalue = li_condition_lvalue_new(li_cond_lvalue_from_string(lval_start, lval_len), key);
				part.type = encoded ? PATTERN_VAR_ENCODED : PATTERN_VAR;
				g_array_append_val(pattern, part);
				c++;

				if (part.data.lvalue->type == LI_COMP_UNKNOWN) {
					/* parse error */
					ERROR(srv, "could not parse pattern: \"%s\"", str);
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}
			} else {
				/* parse error */
				ERROR(srv, "could not parse pattern: \"%s\"", str);
				li_pattern_free((liPattern*)pattern);
				return NULL;
			}
		} else {
			/* string */
			const gchar *first;

			part.type = PATTERN_STRING;
			part.data.str= g_string_sized_new(0);

			/* copy every chunk between escapes into dest buffer */
			for (first = c ; *c && '?' != *c && '$' != *c && '%' != *c; c++) {
				if (*c == '\\') {
					if (first != c) g_string_append_len(part.data.str, first, c - first);
					c++;
					first = c;
					if (*c != '\\' && *c != '?' && *c != '$' && *c != '%') {
						/* parse error */
						ERROR(srv, "could not parse pattern: invalid escape in \"%s\"", str);
						g_string_free(part.data.str, TRUE);
						li_pattern_free((liPattern*)pattern);
						return NULL;
					}
				}
			}
			if (first != c) g_string_append_len(part.data.str, first, c - first);

			g_array_append_val(pattern, part);
		}
	}

	return (liPattern*) pattern;
}


void li_pattern_free(liPattern *pattern) {
	guint i;
	GArray *arr;
	liPatternPart *part;

	if (!pattern) return;

	arr = (GArray*) pattern;
	for (i = 0; i < arr->len; i++) {
		part = &g_array_index(arr, liPatternPart, i);
		switch (part->type) {
		case PATTERN_STRING: g_string_free(part->data.str, TRUE); break;
		case PATTERN_VAR_ENCODED: /* fall through */
		case PATTERN_VAR: li_condition_lvalue_release(part->data.lvalue); break;
		default: break;
		}
	}

	g_array_free(arr, TRUE);
}

void li_pattern_eval(liVRequest *vr, GString *dest, liPattern *pattern, liPatternCB nth_callback, gpointer nth_data, liPatternCB nth_prev_callback, gpointer nth_prev_data) {
	guint i;
	gboolean encoded;
	liHandlerResult res;
	liConditionValue cond_val;
	GArray *arr = (GArray*) pattern;

	for (i = 0; i < arr->len; i++) {
		liPatternPart *part = &g_array_index(arr, liPatternPart, i);
		encoded = FALSE;

		switch (part->type) {
		case PATTERN_STRING:
			g_string_append_len(dest, GSTR_LEN(part->data.str));
			break;
		case PATTERN_NTH:
			if (NULL != nth_callback) {
				nth_callback(dest, part->data.range.from, part->data.range.to, nth_data);
			}
			break;
		case PATTERN_NTH_PREV:
			if (NULL != nth_prev_callback) {
				nth_prev_callback(dest, part->data.range.from, part->data.range.to, nth_prev_data);
			}
			break;
		case PATTERN_VAR_ENCODED:
			encoded = TRUE;
			/* fall through */
		case PATTERN_VAR:
			if (vr == NULL) continue;

			res = li_condition_get_value(vr, part->data.lvalue, &cond_val, LI_COND_VALUE_HINT_STRING);
			if (res == LI_HANDLER_GO_ON) {
				if (encoded) {
					li_string_encode_append(li_condition_value_to_string(vr, &cond_val), dest, LI_ENCODING_URI);
				} else {
					g_string_append(dest, li_condition_value_to_string(vr, &cond_val));
				}
			}

			break;
		}
	}
}

void li_pattern_array_cb(GString *pattern_result, guint from, guint to, gpointer data) {
	GArray *a = data;
	guint i;

	if (NULL == a || 0 == a->len) return;

	if (G_LIKELY(from <= to)) {
		to = MIN(to, a->len - 1);
		for (i = from; i <= to; i++) {
			GString *str = g_array_index(a, GString*, i);
			if (NULL != str) {
				g_string_append_len(pattern_result, GSTR_LEN(str));
			}
		}
	} else {
		from = MIN(from, a->len - 1); /* => from+1 is defined */
		for (i = from + 1; i-- >= to; ) {
			GString *str = g_array_index(a, GString*, i);
			if (NULL != str) {
				g_string_append_len(pattern_result, GSTR_LEN(str));
			}
		}
	}
}

void li_pattern_regex_cb(GString *pattern_result, guint from, guint to, gpointer data) {
	GMatchInfo *match_info = data;
	guint i;
	gint start_pos, end_pos;

	if (NULL == match_info) return;

	if (G_LIKELY(from <= to)) {
		to = MIN(to, G_MAXINT);
		for (i = from; i <= to; i++) {
			if (g_match_info_fetch_pos(match_info, (gint) i, &start_pos, &end_pos)) {
				g_string_append_len(pattern_result, g_match_info_get_string(match_info) + start_pos, end_pos - start_pos);
			}
		}
	} else {
		from = MIN(from, G_MAXINT); /* => from+1 is defined */
		for (i = from + 1; --i >= to; ) {
			if (g_match_info_fetch_pos(match_info, (gint) i, &start_pos, &end_pos)) {
				g_string_append_len(pattern_result, g_match_info_get_string(match_info) + start_pos, end_pos - start_pos);
			}
		}
	}
}
