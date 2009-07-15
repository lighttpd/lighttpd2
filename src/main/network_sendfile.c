
#include <lighttpd/base.h>

#ifdef USE_SENDFILE

typedef enum {
	NSR_SUCCESS,
	NSR_WAIT_FOR_EVENT,
	NSR_FALLBACK,
	NSR_CLOSE,
	NSR_FATAL_ERROR
} network_sendfile_result;

static network_sendfile_result lighty_sendfile(liVRequest *vr, int fd, int filefd, goffset offset, ssize_t len, ssize_t *wrote);

#if defined(USE_LINUX_SENDFILE)

static network_sendfile_result lighty_sendfile(liVRequest *vr, int fd, int filefd, goffset offset, ssize_t len, ssize_t *wrote) {
	ssize_t r;
	off_t file_offset = offset;

	while (-1 == (r = sendfile(fd, filefd, &file_offset, len))) {
		switch (errno) {
		case EAGAIN:
#if EWOULDBLOCK != EAGAIN
		case EWOULDBLOCK
#endif
			return NSR_WAIT_FOR_EVENT;
		case ECONNRESET:
		case EPIPE:
			return NSR_CLOSE;
		case EINTR:
			break; /* try again */
		case EINVAL:
		case ENOSYS:
			/* TODO: print a warning? */
			return NSR_FALLBACK;
		default:
			VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
			return NSR_FATAL_ERROR;
		}
	}
	*wrote = r;
	return NSR_SUCCESS;
}

#elif defined(USE_FREEBSD_SENDFILE)

static network_sendfile_result lighty_sendfile(liVRequest *vr, int fd, int filefd, goffset offset, ssize_t len, ssize_t *wrote) {
	off_t r = 0;

	while (-1 == sendfile(filefd, fd, offset, len, NULL, &r, 0)) {
		switch (errno) {
		case EAGAIN:
			if (r) {
				*wrote = r;
				return NSR_SUCCESS;
			}
			return NSR_WAIT_FOR_EVENT;
		case ENOTCONN:
		case EPIPE:
			return NSR_CLOSE;
		case EINTR:
			if (r) {
				*wrote = r;
				return NSR_SUCCESS;
			}
			break; /* try again */
		case EINVAL:
		case EOPNOTSUPP:
		case ENOTSOCK:
			/* TODO: print a warning? */
			return NSR_FALLBACK;
		default:
			VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
			return NSR_FATAL_ERROR;
		}
	}
	*wrote = r;
	return NSR_SUCCESS;
}

#elif defined(USE_SOLARIS_SENDFILEV)

static network_sendfile_result lighty_sendfile(liVRequest *vr, int fd, int filefd, goffset offset, ssize_t len, ssize_t *wrote) {
	sendfilevec_t fvec;

	fvec.sfv_fd = filefd;
	fvec.sfv_flag = 0;
	fvec.sfv_off = offset;
	fvec.sfv_len = len;

	while (-1 == (r = sendfilev(fd, &fvec, 1, (size_t*) wrote))) {
		switch (errno) {
		case EAGAIN:
			return NSR_WAIT_FOR_EVENT;
		case EPIPE:
			return NSR_CLOSE;
		case EINTR:
			break; /* try again */
		case EAFNOSUPPORT:
		case EPROTOTYPE:
			/* TODO: print a warning? */
			return NSR_FALLBACK;
		default:
			VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
			return NSR_FATAL_ERROR;
		}
	}
	return NSR_SUCCESS;
}

#elif defined(USE_OSX_SENDFILE)

static network_sendfile_result lighty_sendfile(liVRequest *vr, int fd, int filefd, goffset offset, ssize_t len, ssize_t *wrote) {
	off_t bytes = len;

	while (-1 == sendfile(filefd, fd, offset, &bytes, NULL, 0)) {
		switch (errno) {
		case EAGAIN:
			if (bytes) {
				*wrote = bytes;
				return NSR_SUCCESS;
			}
			return NSR_WAIT_FOR_EVENT;
		case ENOTCONN:
		case EPIPE:
			return NSR_CLOSE;
		case EINTR:
			if (bytes) {
				*wrote = bytes;
				return NSR_SUCCESS;
			}
			break; /* try again */
		case ENOTSUP:
		case EOPNOTSUPP:
		case ENOTSOCK:
			/* TODO: print a warning? */
			return NSR_FALLBACK;
		default:
			VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
			return NSR_FATAL_ERROR;
		}
	}
	*wrote = bytes;
	return NSR_SUCCESS;
}

#endif



/* first chunk must be a FILE_CHUNK ! */
static liNetworkStatus network_backend_sendfile(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max) {
	off_t file_offset, toSend;
	ssize_t r;
	gboolean did_write_something = FALSE;
	liChunkIter ci;
	liChunk *c;

	if (0 == cq->length) return LI_NETWORK_STATUS_FATAL_ERROR;

	do {
		ci = chunkqueue_iter(cq);

		if (FILE_CHUNK != (c = chunkiter_chunk(ci))->type) {
			return did_write_something ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_FATAL_ERROR;
		}

		switch (li_chunkfile_open(vr, c->file.file)) {
		case LI_HANDLER_GO_ON:
			break;
		default:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}

		file_offset = c->offset + c->file.start;
		toSend = c->file.length - c->offset;
		if (toSend > *write_max) toSend = *write_max;

		r = 0;
		switch (lighty_sendfile(vr, fd, c->file.file->fd, file_offset, toSend, &r)) {
		case NSR_SUCCESS:
			li_chunkqueue_skip(cq, r);
			*write_max -= r;
			break;
		case NSR_WAIT_FOR_EVENT:
			return LI_NETWORK_STATUS_WAIT_FOR_EVENT;
		case NSR_FALLBACK:
			LI_NETWORK_FALLBACK(li_network_backend_write, write_max);
			break;
		case NSR_CLOSE:
			return LI_NETWORK_STATUS_CONNECTION_CLOSE;
		case NSR_FATAL_ERROR:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}
		if (0 == r) {
			/* don't care about cached stat - file is open */
			struct stat st;
			if (-1 == fstat(fd, &st)) {
				VR_ERROR(vr, "Couldn't fstat file: %s", g_strerror(errno));
				return LI_NETWORK_STATUS_FATAL_ERROR;
			}

			if (file_offset > st.st_size) {
				/* file shrinked, close the connection */
				VR_ERROR(vr, "%s", "File shrinked, aborting");
				return LI_NETWORK_STATUS_FATAL_ERROR;
			}
			return LI_NETWORK_STATUS_WAIT_FOR_EVENT;
		}
		did_write_something = TRUE;

		if (0 == cq->length) return LI_NETWORK_STATUS_SUCCESS;
		if (r != toSend) return LI_NETWORK_STATUS_WAIT_FOR_EVENT;
	} while (*write_max > 0);

	return LI_NETWORK_STATUS_SUCCESS;
}

liNetworkStatus li_network_write_sendfile(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max) {
	if (cq->length == 0) return LI_NETWORK_STATUS_FATAL_ERROR;

	do {
		switch (chunkqueue_first_chunk(cq)->type) {
		case MEM_CHUNK:
		case STRING_CHUNK:
			LI_NETWORK_FALLBACK(li_network_backend_writev, write_max);
			break;
		case FILE_CHUNK:
			LI_NETWORK_FALLBACK(network_backend_sendfile, write_max);
			break;
		default:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}

		if (cq->length == 0) return LI_NETWORK_STATUS_SUCCESS;
	} while (*write_max > 0);
	return LI_NETWORK_STATUS_SUCCESS;
}

#endif
