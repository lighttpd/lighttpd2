
#include <lighttpd/filter_buffer_on_disk.h>

typedef liFilterBufferOnDiskState bod_state;

static void bod_close(bod_state *state) {
	if (NULL != state->tempfile) {
		li_chunkfile_release(state->tempfile);
		state->tempfile = NULL;
	}
	state->flush_pos = state->write_pos = 0;
}

static gboolean bod_open(liVRequest *vr, bod_state *state) {
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
			VR_ERROR(vr, "g_mkstemp failed: %s", g_strerror(errno));
			g_string_free(tmpfilename, TRUE);
			return FALSE;
		}

		state->tempfile = li_chunkfile_new(tmpfilename, fd, TRUE);
		state->write_pos = 0;
		state->flush_pos = 0;
	}

	return TRUE;
}

static void bod_flush(liChunkQueue *out, bod_state *state) {
	if (state->tempfile && state->write_pos > state->flush_pos) {
		li_chunkqueue_append_chunkfile(out, state->tempfile, state->flush_pos, state->write_pos - state->flush_pos);
		state->flush_pos = state->write_pos;
	}
}

static void bod_autoflush(liChunkQueue *out, bod_state *state) {
	if (-1 != state->flush_limit && state->tempfile && state->write_pos - state->flush_pos > state->flush_limit) {
		li_chunkqueue_append_chunkfile(out, state->tempfile, state->flush_pos, state->write_pos - state->flush_pos);
		state->flush_pos = state->write_pos;
	}
}

liHandlerResult li_filter_buffer_on_disk(liVRequest *vr, liChunkQueue *out, liChunkQueue *in, bod_state *state) {
	UNUSED(vr);

	if (out->is_closed) {
		in->is_closed = TRUE;
		li_chunkqueue_skip_all(in);
		bod_close(state);
		return LI_HANDLER_GO_ON;
	}

	while (in->length > 0) {
		liChunk *c = li_chunkqueue_first_chunk(in);
		liChunkIter ci;
		off_t length, data_len;
		char *data = NULL;

		switch (c->type) {
		case UNUSED_CHUNK: return LI_HANDLER_ERROR;
		case FILE_CHUNK:
			bod_flush(out, state);
			if (state->split_on_file_chunks) {
				bod_close(state);
			}
			li_chunkqueue_steal_chunk(out, in);
			break;
		case STRING_CHUNK:
		case MEM_CHUNK:
		case BUFFER_CHUNK:
			if (!bod_open(vr, state)) return LI_HANDLER_ERROR;

			length = li_chunk_length(c);
			ci = li_chunkqueue_iter(in);

			if (LI_HANDLER_GO_ON != li_chunkiter_read(vr, ci, 0, length, &data, &data_len)) {
				return LI_HANDLER_ERROR;
			}

			while ( data_len > 0 ) {
				ssize_t r;

				r = pwrite(state->tempfile->fd, data, data_len, state->write_pos);

				if (r < 0) {
					switch (errno) {
					case EINTR: continue;
					default: break;
					}

					VR_ERROR(vr, "pwrite failed: %s", g_strerror(errno));
					return LI_HANDLER_ERROR;
				}

				data += r;
				data_len -= r;
				state->write_pos += r;
			}

			li_chunkqueue_skip(in, length);

			break;
		}
	}

	bod_autoflush(out, state);

	if (in->is_closed) {
		bod_flush(out, state);
		out->is_closed = TRUE;
		bod_close(state);
		return LI_HANDLER_GO_ON;
	}
	return LI_HANDLER_GO_ON;
}

void li_filter_buffer_on_disk_reset(bod_state *state) {
	bod_close(state);
}
