#ifndef _LIGHTTPD_PATTERN_H_
#define _LIGHTTPD_PATTERN_H_

/* liPattern are a parsed representation of a string that can contain various placeholders like $n, %n, %{var} or {enc:var} */

typedef struct {
	enum {
		PATTERN_STRING,		/* literal */
		PATTERN_NTH,		/* $n */
		PATTERN_NTH_PREV,	/* %n */
		PATTERN_VAR,		/* %{req.foo} */
		PATTERN_VAR_ENCODED	/* %{enc:req.foo} */
	} type;

	union {
		GString *str;		/* PATTERN_STRING */
		guint8 ndx;			/* PATTERN_NTH and PATTERN_NTH_PREV */
		liCondLValue var;	/* PATTERN_VAR and PATTERN_VAR_ENCODED */
	} data;
} liPatternPart;

/* liPattern is a GArray in disguise */
typedef GArray liPattern;

/* a pattern callback receives an integer index and a data pointer (usually an array) and must return a GString* which gets inserted into the pattern result */
typedef void (*liPatternCB) (GString *pattern_result, guint8 nth_ndx, gpointer data);

/* constructs a new liPattern* by parsing the given string, returns NULL on error */
LI_API liPattern *li_pattern_new(const gchar* str);
LI_API void li_pattern_free(liPattern *pattern);

LI_API void li_pattern_eval(liVRequest *vr, GString *dest, liPattern *pattern, liPatternCB nth_callback, gpointer nth_data, liPatternCB nth_prev_callback, gpointer nth_prev_data);

/* default array callback, expects a GArray* containing GString* elements */
LI_API void li_pattern_array_cb(GString *pattern_result, guint8 nth_ndx, gpointer data);
/* default regex callback, expects a GMatchInfo* */
LI_API void li_pattern_regex_cb(GString *pattern_result, guint8 nth_ndx, gpointer data);

#endif
