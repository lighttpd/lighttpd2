#include <lighttpd/sys-files.h>

#ifdef _WIN32
DIR *opendir(const char *dn) {
	DIR *d = g_slice_new0(DIR);

	if (INVALID_HANDLE_VALUE == (d->h = FindFirstFile(dn, &(d->finddata)))) {
		return NULL;
	}

	return d;
}

struct dirent *readdir(DIR *d) {
	if (!d->dent.d_name) {
		/* opendir has set a finddata already, push it out */

		d->dent.d_name = d->finddata.cFileName;
		return &(d->dent);
	}
	if (FindNextFile(d->h, &(d->finddata))) {
		d->dent.d_name = d->finddata.cFileName;
		return &(d->dent);
	} else {
		return NULL;
	}
}

void closedir(DIR *d) {
	FindClose(d);

	g_slice_free(DIR, d);
}

GString *pathname_unix2local(GString *fn) {
	size_t i;

	for (i = 0; i < fn->len; i++) {
		if (fn->str[i] == '/') {
			fn->str[i] = '\\';
		}
	}

	return fn;
}

GString *filename_unix2local(GString *fn) {
	size_t i;

	for (i = 0; i < fn->len; i++) {
		if (fn->str[i] == '/') {
			fn->str[i] = '\\';
		}
	}
#if 0
	buffer_prepare_append(fn, 4);
	memmove(fn->ptr + 4, fn->ptr, fn->used);
	memcpy(fn->ptr, "\\\\?\\", 4);
	fn->used += 4;
#endif
	return fn;
}
#endif

