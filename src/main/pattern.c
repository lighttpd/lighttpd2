#include <lighttpd/base.h>

liPattern *li_pattern_new(const gchar* str) {
	GArray *pattern;
	liPatternPart part;
	gchar *c;
	gboolean encoded;

	pattern = g_array_new(FALSE, TRUE, sizeof(liPatternPart));

	for (c = (gchar*)str; *c;) {
		if (*c == '$') {
			/* $n, PATTERN_NTH */
			c++;
			if (*c >= '0' && *c <= '9') {
				part.type = PATTERN_NTH;
				part.data.ndx = *c - '0';
				g_array_append_val(pattern, part);
				c++;
			} else {
				/* parse error */
				li_pattern_free((liPattern*)pattern);
				return NULL;
			}
		} else if (*c == '%') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* %n, PATTERN_NTH_PREV */
				part.type = PATTERN_NTH_PREV;
				part.data.ndx = *c - '0';
				g_array_append_val(pattern, part);
				c++;
			} else if (*c == '{') {
				/* %{var}, PATTERN_VAR */
				guint len;

				c++;
				encoded = FALSE;

				if (g_str_has_prefix(c, "enc:")) {
					/* %{enc:var}, PATTERN_VAR_ENCODED */
					c += sizeof("enc:")-1;
					encoded = TRUE;
				}

				for (len = 0; *c != '\0' && *c != '}'; c++)
					len++;

				if (*c == '\0') {
					/* parse error */
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}

				part.data.var = li_cond_lvalue_from_string(c-len, len);

				if (part.data.var == LI_COMP_UNKNOWN) {
					/* parse error */
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}

				if (len && *c == '}') {
					part.type = encoded ? PATTERN_VAR_ENCODED : PATTERN_VAR;
					g_array_append_val(pattern, part);
					c++;
				} else {
					/* parse error */
					li_pattern_free((liPattern*)pattern);
					return NULL;
				}
			} else {
				/* parse error */
				li_pattern_free((liPattern*)pattern);
				return NULL;
			}
		} else {
			/* string */
			gchar *first = c;
			c++;

			for (;;) {
				if (*c == '\0' || *c == '?' || *c == '$' || *c == '%') {
					break;
				} else if (*c == '\\') {
					c++;
					if (*c == '\\' || *c == '?' || *c == '$' || *c == '%') {
						c++;
					} else {
						/* parse error */
						li_pattern_free((liPattern*)pattern);
						return NULL;
					}
				} else {
					c++;
				}
			}

			part.type = PATTERN_STRING;
			part.data.str= g_string_new_len(first, c - first);
			g_array_append_val(pattern, part);
		}
	}

	return (liPattern*) pattern;
}


void li_pattern_free(liPattern *pattern) {
	guint i;
	GArray *arr = (GArray*) pattern;

	for (i = 0; i < arr->len; i++) {
		if (g_array_index(arr, liPatternPart, i).type == PATTERN_STRING)
			g_string_free(g_array_index(arr, liPatternPart, i).data.str, TRUE);
	}

	g_array_free(arr, TRUE);
}

void li_pattern_eval(liVRequest *vr, GString *dest, liPattern *pattern, liPatternCB nth_callback, gpointer nth_data, liPatternCB nth_prev_callback, gpointer nth_prev_data) {
	guint i;
	gboolean encoded;
	GString *str;
	GString str_stack;
	GArray *arr = (GArray*) pattern;

	for (i = 0; i < arr->len; i++) {
		liPatternPart *part = &g_array_index(arr, liPatternPart, i);
		encoded = FALSE;

		switch (part->type) {
		case PATTERN_STRING:
			g_string_append_len(dest, GSTR_LEN(part->data.str));
			break;
		case PATTERN_NTH:
			if (nth_callback)
				nth_callback(dest, part->data.ndx, nth_data);
			break;
		case PATTERN_NTH_PREV:
			if (nth_prev_callback)
				nth_prev_callback(dest, part->data.ndx, nth_prev_data);
			break;
		case PATTERN_VAR_ENCODED:
			encoded = TRUE;
			/* fall through */
		case PATTERN_VAR:
			switch (part->data.var) {
			case LI_COMP_REQUEST_LOCALIP: str = vr->con->local_addr_str; break;
			case LI_COMP_REQUEST_REMOTEIP: str = vr->con->remote_addr_str; break;
			case LI_COMP_REQUEST_SCHEME:
				if (vr->con->is_ssl)
					str_stack = li_const_gstring(CONST_STR_LEN("https"));
				else
					str_stack = li_const_gstring(CONST_STR_LEN("http"));
				str = &str_stack;
				break;
			case LI_COMP_REQUEST_PATH: str = vr->request.uri.path; break;
			case LI_COMP_REQUEST_HOST: str = vr->request.uri.host; break;
			case LI_COMP_REQUEST_QUERY_STRING: str = vr->request.uri.query; break;
			case LI_COMP_REQUEST_METHOD: str = vr->request.http_method_str; break;
			case LI_COMP_REQUEST_CONTENT_LENGTH:
				g_string_printf(vr->con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, vr->request.content_length);
				str = vr->con->wrk->tmp_str;
				break;
			default: continue;
			}

			if (encoded)
				li_string_encode_append(str->str, dest, LI_ENCODING_URI);
			else
				g_string_append_len(dest, GSTR_LEN(str));

			break;
		}
	}
}

void li_pattern_array_cb(GString *pattern_result, guint8 nth_ndx, gpointer data) {
	GString *str = g_array_index((GArray*)data, GString*, nth_ndx);

	g_string_append_len(pattern_result, GSTR_LEN(str));
}

void li_pattern_regex_cb(GString *pattern_result, guint8 nth_ndx, gpointer data) {
	gint start_pos, end_pos;
	GMatchInfo *match_info = data;

	if (!match_info)
		return;

	if (g_match_info_fetch_pos(match_info, (gint)nth_ndx, &start_pos, &end_pos))
		g_string_append_len(pattern_result, g_match_info_get_string(match_info) + start_pos, end_pos - start_pos);
}
