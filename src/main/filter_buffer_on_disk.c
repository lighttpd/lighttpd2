
#include <lighttpd/base.h>
#include <lighttpd/filter_buffer_on_disk.h>

typedef struct bod_state bod_state;

struct bod_state {
	liStream stream;
	liVRequest *vr;

	/* internal state */
	liChunkFile *tempfile;
	goffset flush_pos, write_pos;

	/* config */
	goffset flush_limit;
	gboolean split_on_file_chunks;
};

/* flush current tempfile chunk. ignores out->is_closed. */
static void bod_flush(bod_state *state) {
	liChunkQueue *out = state->stream.out;
	if (NULL != out && NULL != state->tempfile && state->write_pos > state->flush_pos) {
		li_chunkqueue_append_chunkfile(out, state->tempfile, state->flush_pos, state->write_pos - state->flush_pos);
		state->flush_pos = state->write_pos;
		li_stream_notify(&state->stream);
	}
}

/* flush if flush_limit is reached */
static void bod_autoflush(bod_state *state) {
	if (-1 != state->flush_limit && state->write_pos - state->flush_pos > state->flush_limit) {
		bod_flush(state);
	}
}

/* close current file, flush it before if necessary */
static void bod_close(bod_state *state) {
	if (NULL != state->tempfile) {
		bod_flush(state);
		li_chunkfile_release(state->tempfile);
		state->tempfile = NULL;
	}
	state->flush_pos = state->write_pos = 0;
}

/* abort buffering, disconnect streams */
static void bod_error(bod_state *state) {
	bod_close(state);
	li_stream_reset(&state->stream);
	state->vr = NULL;
}

/* stop buffering, forward everyting */
static void bod_stop(bod_state *state) {
	bod_close(state);
	if (NULL != state->stream.source && !state->stream.out->is_closed) {
		li_chunkqueue_steal_all(state->stream.out, state->stream.source->out);
		if (state->stream.source->out->is_closed) {
			state->stream.out->is_closed = TRUE;
			li_stream_disconnect(&state->stream);
		}
		li_stream_notify(&state->stream);
	}
	state->vr = NULL;
}

static gboolean bod_open(bod_state *state) {
	if (NULL == state->tempfile) {
		gint fd;
		GString *tmpfilename;
		const char tmpl[] = "lighttpd-buffer-XXXXXX", basedir[] = "/var/tmp";

		tmpfilename = g_string_sized_new((sizeof(basedir) - 1) + 1 + (sizeof(tmpl) - 1));
		g_string_append_len(tmpfilename, CONST_STR_LEN(basedir)); /* TODO: add config option */
		li_path_append_slash(tmpfilename);
		g_string_append_len(tmpfilename, CONST_STR_LEN(tmpl));

		fd = g_mkstemp(tmpfilename->str);
		if (-1 == fd) {
			VR_ERROR(state->vr, "g_mkstemp failed: %s", g_strerror(errno));
			g_string_free(tmpfilename, TRUE);
			bod_stop(state);
			return FALSE;
		}

		state->tempfile = li_chunkfile_new(tmpfilename, fd, TRUE);
		state->write_pos = 0;
		state->flush_pos = 0;

		g_string_free(tmpfilename, TRUE);
	}

	return TRUE;
}

static void bod_handle_data(bod_state *state) {
	liChunkQueue *out = state->stream.out;
	liChunkQueue *in;

	if (out->is_closed) {
		li_stream_disconnect(&state->stream);
		bod_close(state);
		return;
	}

	in = (state->stream.source != NULL) ? state->stream.source->out : NULL;
	if (NULL == in) goto out;

	if (NULL == state->vr) {
		li_chunkqueue_steal_all(out, in);
		goto out;
	}

	while (in->length > 0) {
		liChunk *c = li_chunkqueue_first_chunk(in);
		liChunkIter ci;
		off_t length, data_len;
		char *data = NULL;
		GError *err;

		assert(UNUSED_CHUNK != c->type);
		switch (c->type) {
		case UNUSED_CHUNK:
			/* shouldn't happen anyway, but stealing it is ok here too */
		case FILE_CHUNK:
			if (state->split_on_file_chunks) {
				bod_close(state);
			} else {
				bod_flush(state);
			}
			li_chunkqueue_steal_chunk(out, in);
			break;
		case STRING_CHUNK:
		case MEM_CHUNK:
		case BUFFER_CHUNK:
			if (!bod_open(state)) return;

			length = li_chunk_length(c);
			ci = li_chunkqueue_iter(in);

			err = NULL;
			if (LI_HANDLER_GO_ON != li_chunkiter_read(ci, 0, length, &data, &data_len, &err)) {
				if (NULL != err) {
					VR_ERROR(state->vr, "%s", err->message);
					g_error_free(err);
				}
				bod_error(state);
				return;
			}

			while ( data_len > 0 ) {
				ssize_t r;

				r = pwrite(state->tempfile->fd, data, data_len, state->write_pos);

				if (r < 0) {
					switch (errno) {
					case EINTR: continue;
					default: break;
					}

					VR_ERROR(state->vr, "pwrite failed: %s", g_strerror(errno));
					bod_stop(state); /* write failures are not critical */
					return;
				}

				data += r;
				data_len -= r;
				state->write_pos += r;
			}

			li_chunkqueue_skip(in, length);

			break;
		}
	}

	bod_autoflush(state);

out:
	if (NULL == in || in->is_closed) {
		out->is_closed = TRUE;
		bod_close(state); /* close/flush ignores out->is_closed */
		li_stream_notify(&state->stream); /* if no flush happened we still notify */
	}
}

static void bod_cb(liStream *stream, liStreamEvent event) {
	bod_state *state = LI_CONTAINER_OF(stream, bod_state, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		bod_handle_data(state);
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST:
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!state->stream.out->is_closed || 0 != state->stream.out->length) {
			li_stream_disconnect(stream);
			bod_close(state);
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!state->stream.out->is_closed) {
			li_stream_disconnect_dest(stream);
			bod_close(state);
		}
		break;
	case LI_STREAM_DESTROY:
		bod_close(state);
		g_slice_free(bod_state, state);
		break;
	}
}

liStream* li_filter_buffer_on_disk(liVRequest *vr, goffset flush_limit, gboolean split_on_file_chunks) {
	bod_state *state = g_slice_new0(bod_state);
	state->vr = vr;
	state->flush_limit = flush_limit;
	state->split_on_file_chunks = split_on_file_chunks;
	li_stream_init(&state->stream, &vr->wrk->loop, bod_cb);
	return &state->stream;
}

void li_filter_buffer_on_disk_stop(liStream *stream) {
	bod_state *state;

	if (NULL == stream) return;
	assert(bod_cb == stream->cb);

	li_stream_acquire(stream);
	state = LI_CONTAINER_OF(stream, bod_state, stream);
	bod_stop(state);
	li_stream_again_later(stream);
	li_stream_release(stream);
}
