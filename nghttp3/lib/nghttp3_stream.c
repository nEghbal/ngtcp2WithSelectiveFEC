/*
 * nghttp3
 *
 * Copyright (c) 2019 nghttp3 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nghttp3_stream.h"

#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "nghttp3_conv.h"
#include "nghttp3_macro.h"
#include "nghttp3_frame.h"
#include "nghttp3_conn.h"
#include "nghttp3_str.h"
#include "nghttp3_http.h"
#include "nghttp3_vec.h"
#include "nghttp3_unreachable.h"
#include <mcheck.h>

/* NGHTTP3_STREAM_MAX_COPY_THRES is the maximum size of buffer which
   makes a copy to outq. */
#define NGHTTP3_STREAM_MAX_COPY_THRES 128

/* NGHTTP3_MIN_RBLEN is the minimum length of nghttp3_ringbuf */
#define NGHTTP3_MIN_RBLEN 4

nghttp3_objalloc_def(stream, nghttp3_stream, oplent);

int nghttp3_stream_new(nghttp3_stream **pstream, int64_t stream_id,
                       const nghttp3_stream_callbacks *callbacks,
                       nghttp3_objalloc *out_chunk_objalloc,
                       nghttp3_objalloc *stream_objalloc,
                       const nghttp3_mem *mem) {
  nghttp3_stream *stream = nghttp3_objalloc_stream_get(stream_objalloc);

  if (stream == NULL) {
    return NGHTTP3_ERR_NOMEM;
  }

  memset(stream, 0, sizeof(*stream));

  stream->out_chunk_objalloc = out_chunk_objalloc;
  stream->stream_objalloc = stream_objalloc;

  nghttp3_tnode_init(&stream->node, stream_id);

  nghttp3_ringbuf_init(&stream->frq, 0, sizeof(nghttp3_frame_entry), mem);
  nghttp3_ringbuf_init(&stream->chunks, 0, sizeof(nghttp3_buf), mem);
  nghttp3_ringbuf_init(&stream->outq, 0, sizeof(nghttp3_typed_buf), mem);
  nghttp3_ringbuf_init(&stream->inq, 0, sizeof(nghttp3_buf), mem);

  nghttp3_qpack_stream_context_init(&stream->qpack_sctx, stream_id, mem);

  stream->qpack_blocked_pe.index = NGHTTP3_PQ_BAD_INDEX;
  stream->mem = mem;
  stream->tx.offset = 0;
  stream->rx.http.status_code = -1;
  stream->rx.http.content_length = -1;
  stream->rx.http.pri.urgency = NGHTTP3_DEFAULT_URGENCY;
  stream->rx.http.fec = -1;
  stream->rx.http.n = -1;
  stream->rx.http.k = -1;
  stream->error_code = NGHTTP3_H3_NO_ERROR;

  if (callbacks) {
    stream->callbacks = *callbacks;
  }

  stream->forward_error_correction.enabled = 0;

  *pstream = stream;

  return 0;
}

static void delete_outq(nghttp3_ringbuf *outq, const nghttp3_mem *mem) {
  nghttp3_typed_buf *tbuf;
  size_t i, len = nghttp3_ringbuf_len(outq);

  for (i = 0; i < len; ++i) {
    tbuf = nghttp3_ringbuf_get(outq, i);
    if (tbuf->type == NGHTTP3_BUF_TYPE_PRIVATE) {
      nghttp3_buf_free(&tbuf->buf, mem);
    }
  }

  nghttp3_ringbuf_free(outq);
}

static void delete_chunks(nghttp3_ringbuf *chunks, const nghttp3_mem *mem) {
  nghttp3_buf *buf;
  size_t i, len = nghttp3_ringbuf_len(chunks);

  for (i = 0; i < len; ++i) {
    buf = nghttp3_ringbuf_get(chunks, i);
    nghttp3_buf_free(buf, mem);
  }

  nghttp3_ringbuf_free(chunks);
}

static void delete_out_chunks(nghttp3_ringbuf *chunks,
                              nghttp3_objalloc *out_chunk_objalloc,
                              const nghttp3_mem *mem) {
  nghttp3_buf *buf;
  size_t i, len = nghttp3_ringbuf_len(chunks);

  for (i = 0; i < len; ++i) {
    buf = nghttp3_ringbuf_get(chunks, i);

    if (nghttp3_buf_cap(buf) == NGHTTP3_STREAM_MIN_CHUNK_SIZE) {
      nghttp3_objalloc_chunk_release(out_chunk_objalloc,
                                     (nghttp3_chunk *)(void *)buf->begin);
      continue;
    }

    nghttp3_buf_free(buf, mem);
  }

  nghttp3_ringbuf_free(chunks);
}

static void delete_frq(nghttp3_ringbuf *frq, const nghttp3_mem *mem) {
  nghttp3_frame_entry *frent;
  size_t i, len = nghttp3_ringbuf_len(frq);

  for (i = 0; i < len; ++i) {
    frent = nghttp3_ringbuf_get(frq, i);
    switch (frent->fr.hd.type) {
    case NGHTTP3_FRAME_HEADERS:
      nghttp3_frame_headers_free(&frent->fr.headers, mem);
      break;
    case NGHTTP3_FRAME_PRIORITY_UPDATE:
      nghttp3_frame_priority_update_free(&frent->fr.priority_update, mem);
      break;
    default:
      break;
    }
  }

  nghttp3_ringbuf_free(frq);
}

void nghttp3_stream_del(nghttp3_stream *stream) {
  if (stream == NULL) {
    return;
  }

  nghttp3_qpack_stream_context_free(&stream->qpack_sctx);
  delete_chunks(&stream->inq, stream->mem);
  delete_outq(&stream->outq, stream->mem);
  delete_out_chunks(&stream->chunks, stream->out_chunk_objalloc, stream->mem);
  delete_frq(&stream->frq, stream->mem);
  nghttp3_tnode_free(&stream->node);

  nghttp3_objalloc_stream_release(stream->stream_objalloc, stream);

  nghttp3_stream_forward_error_correction_release_encoder(stream);
  nghttp3_stream_forward_error_correction_release_decoder(stream);
}

void nghttp3_varint_read_state_reset(nghttp3_varint_read_state *rvint) {
  memset(rvint, 0, sizeof(*rvint));
}

void nghttp3_stream_read_state_reset(nghttp3_stream_read_state *rstate) {
  memset(rstate, 0, sizeof(*rstate));
}

nghttp3_ssize nghttp3_read_varint(nghttp3_varint_read_state *rvint,
                                  const uint8_t *src, size_t srclen, int fin) {
  size_t nread = 0;
  size_t n;
  size_t i;

  assert(srclen > 0);

  if (rvint->left == 0) {
    assert(rvint->acc == 0);

    rvint->left = nghttp3_get_varintlen(src);
    if (rvint->left <= srclen) {
      rvint->acc = nghttp3_get_varint(&nread, src);
      rvint->left = 0;
      return (nghttp3_ssize)nread;
    }

    if (fin) {
      return NGHTTP3_ERR_INVALID_ARGUMENT;
    }

    rvint->acc = nghttp3_get_varint_fb(src);
    nread = 1;
    ++src;
    --srclen;
    --rvint->left;
  }

  n = nghttp3_min(rvint->left, srclen);

  for (i = 0; i < n; ++i) {
    rvint->acc = (rvint->acc << 8) + src[i];
  }

  rvint->left -= n;
  nread += n;

  if (fin && rvint->left) {
    return NGHTTP3_ERR_INVALID_ARGUMENT;
  }

  return (nghttp3_ssize)nread;
}

int nghttp3_stream_frq_add(nghttp3_stream *stream,
                           const nghttp3_frame_entry *frent) {
  nghttp3_ringbuf *frq = &stream->frq;
  nghttp3_frame_entry *dest;
  int rv;

  if (nghttp3_ringbuf_full(frq)) {
    size_t nlen = nghttp3_max(NGHTTP3_MIN_RBLEN, nghttp3_ringbuf_len(frq) * 2);
    rv = nghttp3_ringbuf_reserve(frq, nlen);
    if (rv != 0) {
      return rv;
    }
  }

  dest = nghttp3_ringbuf_push_back(frq);
  *dest = *frent;

  return 0;
}

int nghttp3_stream_fill_outq(nghttp3_stream *stream) {
  nghttp3_ringbuf *frq = &stream->frq;
  nghttp3_frame_entry *frent;
  int data_eof;
  int rv;

  for (; nghttp3_ringbuf_len(frq) &&
         stream->unsent_bytes < NGHTTP3_MIN_UNSENT_BYTES;) {
    frent = nghttp3_ringbuf_get(frq, 0);

    switch (frent->fr.hd.type) {
    case NGHTTP3_FRAME_SETTINGS:
      rv = nghttp3_stream_write_settings(stream, frent);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGHTTP3_FRAME_HEADERS:
      rv = nghttp3_stream_write_headers(stream, frent);
      if (rv != 0) {
        return rv;
      }
      nghttp3_frame_headers_free(&frent->fr.headers, stream->mem);
      break;
    case NGHTTP3_FRAME_DATA:
      rv = nghttp3_stream_write_data(stream, &data_eof, frent);
      if (rv != 0) {
        return rv;
      }
      if (stream->flags & NGHTTP3_STREAM_FLAG_READ_DATA_BLOCKED) {
        return 0;
      }
      if (!data_eof) {
        return 0;
      }
      break;
    case NGHTTP3_FRAME_GOAWAY:
      rv = nghttp3_stream_write_goaway(stream, frent);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGHTTP3_FRAME_PRIORITY_UPDATE:
      rv = nghttp3_stream_write_priority_update(stream, frent);
      if (rv != 0) {
        return rv;
      }
      nghttp3_frame_priority_update_free(&frent->fr.priority_update,
                                         stream->mem);
      break;
    default:
      /* TODO Not implemented */
      break;
    }

    nghttp3_ringbuf_pop_front(frq);
  }

  return 0;
}

static void typed_buf_shared_init(nghttp3_typed_buf *tbuf,
                                  const nghttp3_buf *chunk) {
  nghttp3_typed_buf_init(tbuf, chunk, NGHTTP3_BUF_TYPE_SHARED);
  tbuf->buf.pos = tbuf->buf.last;
}

int nghttp3_stream_write_stream_type(nghttp3_stream *stream) {
  size_t len = nghttp3_put_varintlen((int64_t)stream->type);
  nghttp3_buf *chunk;
  nghttp3_typed_buf tbuf;
  int rv;

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_put_varint(chunk->last, (int64_t)stream->type);
  tbuf.buf.last = chunk->last;

  return nghttp3_stream_outq_add(stream, &tbuf);
}

int nghttp3_stream_write_settings(nghttp3_stream *stream,
                                  nghttp3_frame_entry *frent) {
  size_t len;
  int rv;
  nghttp3_buf *chunk;
  nghttp3_typed_buf tbuf;
  struct {
    nghttp3_frame_settings settings;
    nghttp3_settings_entry iv[15];
  } fr;
  nghttp3_settings_entry *iv;
  nghttp3_settings *local_settings = frent->aux.settings.local_settings;

  fr.settings.hd.type = NGHTTP3_FRAME_SETTINGS;
  fr.settings.niv = 3;
  iv = &fr.settings.iv[0];

  iv[0].id = NGHTTP3_SETTINGS_ID_MAX_FIELD_SECTION_SIZE;
  iv[0].value = local_settings->max_field_section_size;
  iv[1].id = NGHTTP3_SETTINGS_ID_QPACK_MAX_TABLE_CAPACITY;
  iv[1].value = local_settings->qpack_max_dtable_capacity;
  iv[2].id = NGHTTP3_SETTINGS_ID_QPACK_BLOCKED_STREAMS;
  iv[2].value = local_settings->qpack_blocked_streams;

  if (local_settings->h3_datagram) {
    iv[fr.settings.niv].id = NGHTTP3_SETTINGS_ID_H3_DATAGRAM;
    iv[fr.settings.niv].value = 1;

    ++fr.settings.niv;
  }

  if (local_settings->enable_connect_protocol) {
    iv[fr.settings.niv].id = NGHTTP3_SETTINGS_ID_ENABLE_CONNECT_PROTOCOL;
    iv[fr.settings.niv].value = 1;

    ++fr.settings.niv;
  }

  len = nghttp3_frame_write_settings_len(&fr.settings.hd.length, &fr.settings);

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_frame_write_settings(chunk->last, &fr.settings);

  tbuf.buf.last = chunk->last;

  return nghttp3_stream_outq_add(stream, &tbuf);
}

int nghttp3_stream_write_goaway(nghttp3_stream *stream,
                                nghttp3_frame_entry *frent) {
  nghttp3_frame_goaway *fr = &frent->fr.goaway;
  size_t len;
  int rv;
  nghttp3_buf *chunk;
  nghttp3_typed_buf tbuf;

  len = nghttp3_frame_write_goaway_len(&fr->hd.length, fr);

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_frame_write_goaway(chunk->last, fr);

  tbuf.buf.last = chunk->last;

  return nghttp3_stream_outq_add(stream, &tbuf);
}

int nghttp3_stream_write_priority_update(nghttp3_stream *stream,
                                         nghttp3_frame_entry *frent) {
  nghttp3_frame_priority_update *fr = &frent->fr.priority_update;
  size_t len;
  int rv;
  nghttp3_buf *chunk;
  nghttp3_typed_buf tbuf;

  len = nghttp3_frame_write_priority_update_len(&fr->hd.length, fr);

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_frame_write_priority_update(chunk->last, fr);

  tbuf.buf.last = chunk->last;

  return nghttp3_stream_outq_add(stream, &tbuf);
}

int nghttp3_stream_write_headers(nghttp3_stream *stream,
                                 nghttp3_frame_entry *frent) {
  nghttp3_frame_headers *fr = &frent->fr.headers;
  nghttp3_conn *conn = stream->conn;

  assert(conn);

  return nghttp3_stream_write_header_block(
      stream, &conn->qenc, conn->tx.qenc, &conn->tx.qpack.rbuf,
      &conn->tx.qpack.ebuf, NGHTTP3_FRAME_HEADERS, fr->nva, fr->nvlen);
}

int nghttp3_stream_write_header_block(nghttp3_stream *stream,
                                      nghttp3_qpack_encoder *qenc,
                                      nghttp3_stream *qenc_stream,
                                      nghttp3_buf *rbuf, nghttp3_buf *ebuf,
                                      int64_t frame_type, const nghttp3_nv *nva,
                                      size_t nvlen) {
  nghttp3_buf pbuf;
  int rv;
  size_t len;
  nghttp3_buf *chunk;
  nghttp3_typed_buf tbuf;
  nghttp3_frame_hd hd;
  uint8_t raw_pbuf[16];
  size_t pbuflen, rbuflen, ebuflen;

  nghttp3_buf_wrap_init(&pbuf, raw_pbuf, sizeof(raw_pbuf));

  rv = nghttp3_qpack_encoder_encode(qenc, &pbuf, rbuf, ebuf, stream->node.id,
                                    nva, nvlen);
  if (rv != 0) {
    goto fail;
  }

  pbuflen = nghttp3_buf_len(&pbuf);
  rbuflen = nghttp3_buf_len(rbuf);
  ebuflen = nghttp3_buf_len(ebuf);

  hd.type = frame_type;
  hd.length = (int64_t)(pbuflen + rbuflen);

  len = nghttp3_frame_write_hd_len(&hd) + pbuflen;

  if (rbuflen <= NGHTTP3_STREAM_MAX_COPY_THRES) {
    len += rbuflen;
  }

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    goto fail;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_frame_write_hd(chunk->last, &hd);

  chunk->last = nghttp3_cpymem(chunk->last, pbuf.pos, pbuflen);
  nghttp3_buf_init(&pbuf);

  if (rbuflen > NGHTTP3_STREAM_MAX_COPY_THRES) {
    tbuf.buf.last = chunk->last;

    rv = nghttp3_stream_outq_add(stream, &tbuf);
    if (rv != 0) {
      goto fail;
    }

    nghttp3_typed_buf_init(&tbuf, rbuf, NGHTTP3_BUF_TYPE_PRIVATE);
    rv = nghttp3_stream_outq_add(stream, &tbuf);
    if (rv != 0) {
      goto fail;
    }
    nghttp3_buf_init(rbuf);
  } else if (rbuflen) {
    chunk->last = nghttp3_cpymem(chunk->last, rbuf->pos, rbuflen);
    tbuf.buf.last = chunk->last;

    rv = nghttp3_stream_outq_add(stream, &tbuf);
    if (rv != 0) {
      goto fail;
    }
    nghttp3_buf_reset(rbuf);
  }

  if (ebuflen > NGHTTP3_STREAM_MAX_COPY_THRES) {
    assert(qenc_stream);

    nghttp3_typed_buf_init(&tbuf, ebuf, NGHTTP3_BUF_TYPE_PRIVATE);
    rv = nghttp3_stream_outq_add(qenc_stream, &tbuf);
    if (rv != 0) {
      return rv;
    }
    nghttp3_buf_init(ebuf);
  } else if (ebuflen) {
    assert(qenc_stream);

    rv = nghttp3_stream_ensure_chunk(qenc_stream, ebuflen);
    if (rv != 0) {
      goto fail;
    }

    chunk = nghttp3_stream_get_chunk(qenc_stream);
    typed_buf_shared_init(&tbuf, chunk);

    chunk->last = nghttp3_cpymem(chunk->last, ebuf->pos, ebuflen);
    tbuf.buf.last = chunk->last;

    rv = nghttp3_stream_outq_add(qenc_stream, &tbuf);
    if (rv != 0) {
      goto fail;
    }
    nghttp3_buf_reset(ebuf);
  }

  assert(0 == nghttp3_buf_len(&pbuf));
  assert(0 == nghttp3_buf_len(rbuf));
  assert(0 == nghttp3_buf_len(ebuf));

  return 0;

fail:

  return rv;
}

int nghttp3_stream_write_data(nghttp3_stream *stream, int *peof,
                              nghttp3_frame_entry *frent) {
  int rv;
  size_t len;
  nghttp3_typed_buf tbuf;
  nghttp3_buf buf;
  nghttp3_buf *chunk;
  nghttp3_read_data_callback read_data = frent->aux.data.dr.read_data;
  nghttp3_conn *conn = stream->conn;
  int64_t datalen;
  uint32_t flags = 0;
  nghttp3_frame_hd hd;
  nghttp3_vec vec[8];
  nghttp3_vec *v;
  nghttp3_ssize sveccnt;
  size_t i;

  assert(!(stream->flags & NGHTTP3_STREAM_FLAG_READ_DATA_BLOCKED));
  assert(read_data);
  assert(conn);

  *peof = 0;

  sveccnt = read_data(conn, stream->node.id, vec, nghttp3_arraylen(vec), &flags,
                      conn->user_data, stream->user_data);
  if (sveccnt < 0) {
    if (sveccnt == NGHTTP3_ERR_WOULDBLOCK) {
      stream->flags |= NGHTTP3_STREAM_FLAG_READ_DATA_BLOCKED;
      return 0;
    }
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

   /* Add FEC here to add encoding? */
  if (stream->forward_error_correction.enabled) {
    nghttp3_vec v_fec;

    nghttp3_stream_forward_error_correction_encode(stream, vec, sveccnt);
    nghttp3_stream_forward_error_correction_get_encode_symbols(stream, &v_fec);
    printf("After calling get_encode_symbols() \n");

    /*for (uint32_t i = sveccnt; i > 0; i--){
      vec[sveccnt] = vec[sveccnt - 1];
    }

    vec[0] = v_fec;*/
    vec[sveccnt] = v_fec;
    sveccnt++;
  }

  datalen = nghttp3_vec_len_varint(vec, (size_t)sveccnt);
  if (datalen == -1) {
    return NGHTTP3_ERR_STREAM_DATA_OVERFLOW;
  }

  assert(datalen || flags & NGHTTP3_DATA_FLAG_EOF);

  if (flags & NGHTTP3_DATA_FLAG_EOF) {
    *peof = 1;
    if (!(flags & NGHTTP3_DATA_FLAG_NO_END_STREAM)) {
      stream->flags |= NGHTTP3_STREAM_FLAG_WRITE_END_STREAM;
      if (datalen == 0) {
        if (nghttp3_stream_outq_write_done(stream)) {
          /* If this is the last data and its is 0 length, we don't
             need send DATA frame.  We rely on the non-emptiness of
             outq to schedule stream, so add empty tbuf to outq to
             just send fin. */
          nghttp3_buf_init(&buf);
          nghttp3_typed_buf_init(&tbuf, &buf, NGHTTP3_BUF_TYPE_PRIVATE);
          return nghttp3_stream_outq_add(stream, &tbuf);
        }
        return 0;
      }
    }

    if (datalen == 0) {
      /* We are going to send more frames, but no DATA frame this
         time. */
      return 0;
    }
  }

  hd.type = NGHTTP3_FRAME_DATA;
  hd.length = datalen;

  len = nghttp3_frame_write_hd_len(&hd);

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  chunk->last = nghttp3_frame_write_hd(chunk->last, &hd);

  tbuf.buf.last = chunk->last;

  rv = nghttp3_stream_outq_add(stream, &tbuf);
  if (rv != 0) {
    return rv;
  }

  if (datalen) {
    for (i = 0; i < (size_t)sveccnt; ++i) {
      v = &vec[i];
      if (v->len == 0) {
        continue;
      }
      nghttp3_buf_wrap_init(&buf, v->base, v->len);
      buf.last = buf.end;
      nghttp3_typed_buf_init(&tbuf, &buf, NGHTTP3_BUF_TYPE_ALIEN);
      rv = nghttp3_stream_outq_add(stream, &tbuf);
      if (rv != 0) {
        return rv;
      }
    }
  }

  return 0;
}

int nghttp3_stream_write_qpack_decoder_stream(nghttp3_stream *stream) {
  nghttp3_qpack_decoder *qdec;
  nghttp3_buf *chunk;
  int rv;
  nghttp3_typed_buf tbuf;
  size_t len;

  assert(stream->conn);
  assert(stream->conn->tx.qdec == stream);

  qdec = &stream->conn->qdec;

  assert(qdec);

  len = nghttp3_qpack_decoder_get_decoder_streamlen(qdec);
  if (len == 0) {
    return 0;
  }

  rv = nghttp3_stream_ensure_chunk(stream, len);
  if (rv != 0) {
    return rv;
  }

  chunk = nghttp3_stream_get_chunk(stream);
  typed_buf_shared_init(&tbuf, chunk);

  nghttp3_qpack_decoder_write_decoder(qdec, chunk);

  tbuf.buf.last = chunk->last;

  return nghttp3_stream_outq_add(stream, &tbuf);
}

int nghttp3_stream_outq_add(nghttp3_stream *stream,
                            const nghttp3_typed_buf *tbuf) {
  nghttp3_ringbuf *outq = &stream->outq;
  int rv;
  nghttp3_typed_buf *dest;
  size_t len = nghttp3_ringbuf_len(outq);
  size_t buflen = nghttp3_buf_len(&tbuf->buf);

  if (buflen > NGHTTP3_MAX_VARINT - stream->tx.offset) {
    return NGHTTP3_ERR_STREAM_DATA_OVERFLOW;
  }

  stream->tx.offset += buflen;
  stream->unsent_bytes += buflen;

  if (len) {
    dest = nghttp3_ringbuf_get(outq, len - 1);
    if (dest->type == tbuf->type && dest->type == NGHTTP3_BUF_TYPE_SHARED &&
        dest->buf.begin == tbuf->buf.begin && dest->buf.last == tbuf->buf.pos) {
      /* If we have already written last entry, adjust outq_idx and
         offset so that this entry is eligible to send. */
      if (len == stream->outq_idx) {
        --stream->outq_idx;
        stream->outq_offset = nghttp3_buf_len(&dest->buf);
      }

      dest->buf.last = tbuf->buf.last;
      /* TODO Is this required? */
      dest->buf.end = tbuf->buf.end;

      return 0;
    }
  }

  if (nghttp3_ringbuf_full(outq)) {
    size_t nlen = nghttp3_max(NGHTTP3_MIN_RBLEN, len * 2);
    rv = nghttp3_ringbuf_reserve(outq, nlen);
    if (rv != 0) {
      return rv;
    }
  }

  dest = nghttp3_ringbuf_push_back(outq);
  *dest = *tbuf;

  return 0;
}

int nghttp3_stream_ensure_chunk(nghttp3_stream *stream, size_t need) {
  nghttp3_ringbuf *chunks = &stream->chunks;
  nghttp3_buf *chunk;
  size_t len = nghttp3_ringbuf_len(chunks);
  uint8_t *p;
  int rv;
  size_t n = NGHTTP3_STREAM_MIN_CHUNK_SIZE;

  if (len) {
    chunk = nghttp3_ringbuf_get(chunks, len - 1);
    if (nghttp3_buf_left(chunk) >= need) {
      return 0;
    }
  }

  for (; n < need; n *= 2)
    ;

  if (n == NGHTTP3_STREAM_MIN_CHUNK_SIZE) {
    p = (uint8_t *)nghttp3_objalloc_chunk_len_get(stream->out_chunk_objalloc,
                                                  n);
  } else {
    p = nghttp3_mem_malloc(stream->mem, n);
  }
  if (p == NULL) {
    return NGHTTP3_ERR_NOMEM;
  }

  if (nghttp3_ringbuf_full(chunks)) {
    size_t nlen = nghttp3_max(NGHTTP3_MIN_RBLEN, len * 2);
    rv = nghttp3_ringbuf_reserve(chunks, nlen);
    if (rv != 0) {
      return rv;
    }
  }

  chunk = nghttp3_ringbuf_push_back(chunks);
  nghttp3_buf_wrap_init(chunk, p, n);

  return 0;
}

nghttp3_buf *nghttp3_stream_get_chunk(nghttp3_stream *stream) {
  nghttp3_ringbuf *chunks = &stream->chunks;
  size_t len = nghttp3_ringbuf_len(chunks);

  assert(len);

  return nghttp3_ringbuf_get(chunks, len - 1);
}

int nghttp3_stream_is_blocked(nghttp3_stream *stream) {
  return (stream->flags & NGHTTP3_STREAM_FLAG_FC_BLOCKED) ||
         (stream->flags & NGHTTP3_STREAM_FLAG_SHUT_WR) ||
         (stream->flags & NGHTTP3_STREAM_FLAG_READ_DATA_BLOCKED);
}

int nghttp3_stream_require_schedule(nghttp3_stream *stream) {
  return (!nghttp3_stream_outq_write_done(stream) &&
          !(stream->flags & NGHTTP3_STREAM_FLAG_FC_BLOCKED) &&
          !(stream->flags & NGHTTP3_STREAM_FLAG_SHUT_WR)) ||
         (nghttp3_ringbuf_len(&stream->frq) &&
          !(stream->flags & NGHTTP3_STREAM_FLAG_READ_DATA_BLOCKED));
}

size_t nghttp3_stream_writev(nghttp3_stream *stream, int *pfin,
                             nghttp3_vec *vec, size_t veccnt) {
  nghttp3_ringbuf *outq = &stream->outq;
  size_t len = nghttp3_ringbuf_len(outq);
  size_t i = stream->outq_idx;
  uint64_t offset = stream->outq_offset;
  size_t buflen;
  nghttp3_vec *vbegin = vec, *vend = vec + veccnt;
  nghttp3_typed_buf *tbuf;

  assert(veccnt > 0);

  if (i < len) {
    tbuf = nghttp3_ringbuf_get(outq, i);
    buflen = nghttp3_buf_len(&tbuf->buf);

    if (offset < buflen) {
      vec->base = tbuf->buf.pos + offset;
      vec->len = (size_t)(buflen - offset);
      ++vec;
    } else {
      /* This is the only case that satisfies offset >= buflen */
      assert(0 == offset);
      assert(0 == buflen);
    }

    ++i;

    for (; i < len && vec != vend; ++i, ++vec) {
      tbuf = nghttp3_ringbuf_get(outq, i);
      vec->base = tbuf->buf.pos;
      vec->len = nghttp3_buf_len(&tbuf->buf);
    }
  }

  /* TODO Rework this if we have finished implementing HTTP
     messaging */
  *pfin = nghttp3_ringbuf_len(&stream->frq) == 0 && i == len &&
          (stream->flags & NGHTTP3_STREAM_FLAG_WRITE_END_STREAM);

  return (size_t)(vec - vbegin);
}

void nghttp3_stream_add_outq_offset(nghttp3_stream *stream, size_t n) {
  nghttp3_ringbuf *outq = &stream->outq;
  size_t i;
  size_t len = nghttp3_ringbuf_len(outq);
  uint64_t offset = stream->outq_offset + n;
  size_t buflen;
  nghttp3_typed_buf *tbuf;

  for (i = stream->outq_idx; i < len; ++i) {
    tbuf = nghttp3_ringbuf_get(outq, i);
    buflen = nghttp3_buf_len(&tbuf->buf);
    if (offset >= buflen) {
      offset -= buflen;
      continue;
    }

    break;
  }

  assert(i < len || offset == 0);

  stream->unsent_bytes -= n;
  stream->outq_idx = i;
  stream->outq_offset = offset;
}

int nghttp3_stream_outq_write_done(nghttp3_stream *stream) {
  nghttp3_ringbuf *outq = &stream->outq;
  size_t len = nghttp3_ringbuf_len(outq);

  return len == 0 || stream->outq_idx >= len;
}

static void stream_pop_outq_entry(nghttp3_stream *stream,
                                  nghttp3_typed_buf *tbuf) {
  nghttp3_ringbuf *chunks = &stream->chunks;
  nghttp3_buf *chunk;

  switch (tbuf->type) {
  case NGHTTP3_BUF_TYPE_PRIVATE:
    nghttp3_buf_free(&tbuf->buf, stream->mem);
    break;
  case NGHTTP3_BUF_TYPE_ALIEN:
    break;
  case NGHTTP3_BUF_TYPE_SHARED:
    assert(nghttp3_ringbuf_len(chunks));

    chunk = nghttp3_ringbuf_get(chunks, 0);

    assert(chunk->begin == tbuf->buf.begin);
    assert(chunk->end == tbuf->buf.end);

    if (chunk->last == tbuf->buf.last) {
      if (nghttp3_buf_cap(chunk) == NGHTTP3_STREAM_MIN_CHUNK_SIZE) {
        nghttp3_objalloc_chunk_release(stream->out_chunk_objalloc,
                                       (nghttp3_chunk *)(void *)chunk->begin);
      } else {
        nghttp3_buf_free(chunk, stream->mem);
      }
      nghttp3_ringbuf_pop_front(chunks);
    }
    break;
  default:
    nghttp3_unreachable();
  };

  nghttp3_ringbuf_pop_front(&stream->outq);
}

int nghttp3_stream_add_ack_offset(nghttp3_stream *stream, uint64_t n) {
  nghttp3_ringbuf *outq = &stream->outq;
  uint64_t offset = stream->ack_offset + n;
  size_t buflen;
  size_t npopped = 0;
  uint64_t nack;
  nghttp3_typed_buf *tbuf;
  int rv;

  for (; nghttp3_ringbuf_len(outq);) {
    tbuf = nghttp3_ringbuf_get(outq, 0);
    buflen = nghttp3_buf_len(&tbuf->buf);

    if (tbuf->type == NGHTTP3_BUF_TYPE_ALIEN) {
      nack = nghttp3_min(offset, (uint64_t)buflen) - stream->ack_done;
      if (stream->callbacks.acked_data) {
        rv = stream->callbacks.acked_data(stream, stream->node.id, nack,
                                          stream->user_data);
        if (rv != 0) {
          return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
      }
      stream->ack_done += nack;
    }

    if (offset >= buflen) {
      stream_pop_outq_entry(stream, tbuf);

      offset -= buflen;
      ++npopped;
      stream->ack_done = 0;

      if (stream->outq_idx + 1 == npopped) {
        stream->outq_offset = 0;
        break;
      }

      continue;
    }

    break;
  }

  assert(stream->outq_idx + 1 >= npopped);
  if (stream->outq_idx >= npopped) {
    stream->outq_idx -= npopped;
  } else {
    stream->outq_idx = 0;
  }

  stream->ack_offset = offset;

  return 0;
}

int nghttp3_stream_buffer_data(nghttp3_stream *stream, const uint8_t *data,
                               size_t datalen) {
  nghttp3_ringbuf *inq = &stream->inq;
  size_t len = nghttp3_ringbuf_len(inq);
  nghttp3_buf *buf;
  size_t nwrite;
  uint8_t *rawbuf;
  size_t bufleft;
  int rv;

  if (len) {
    buf = nghttp3_ringbuf_get(inq, len - 1);
    bufleft = nghttp3_buf_left(buf);
    nwrite = nghttp3_min(datalen, bufleft);
    buf->last = nghttp3_cpymem(buf->last, data, nwrite);
    data += nwrite;
    datalen -= nwrite;
  }

  for (; datalen;) {
    if (nghttp3_ringbuf_full(inq)) {
      size_t nlen =
          nghttp3_max(NGHTTP3_MIN_RBLEN, nghttp3_ringbuf_len(inq) * 2);
      rv = nghttp3_ringbuf_reserve(inq, nlen);
      if (rv != 0) {
        return rv;
      }
    }

    rawbuf = nghttp3_mem_malloc(stream->mem, 16384);
    if (rawbuf == NULL) {
      return NGHTTP3_ERR_NOMEM;
    }

    buf = nghttp3_ringbuf_push_back(inq);
    nghttp3_buf_wrap_init(buf, rawbuf, 16384);
    bufleft = nghttp3_buf_left(buf);
    nwrite = nghttp3_min(datalen, bufleft);
    buf->last = nghttp3_cpymem(buf->last, data, nwrite);
    data += nwrite;
    datalen -= nwrite;
  }

  return 0;
}

size_t nghttp3_stream_get_buffered_datalen(nghttp3_stream *stream) {
  nghttp3_ringbuf *inq = &stream->inq;
  size_t len = nghttp3_ringbuf_len(inq);
  size_t i, n = 0;
  nghttp3_buf *buf;

  for (i = 0; i < len; ++i) {
    buf = nghttp3_ringbuf_get(inq, i);
    n += nghttp3_buf_len(buf);
  }

  return n;
}

int nghttp3_stream_transit_rx_http_state(nghttp3_stream *stream,
                                         nghttp3_stream_http_event event) {
  int rv;

  switch (stream->rx.hstate) {
  case NGHTTP3_HTTP_STATE_NONE:
    return NGHTTP3_ERR_H3_INTERNAL_ERROR;
  case NGHTTP3_HTTP_STATE_REQ_INITIAL:
    switch (event) {
    case NGHTTP3_HTTP_EVENT_HEADERS_BEGIN:
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_HEADERS_BEGIN;
      return 0;
    default:
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
  case NGHTTP3_HTTP_STATE_REQ_HEADERS_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_HEADERS_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_HEADERS_END;
    return 0;
  case NGHTTP3_HTTP_STATE_REQ_HEADERS_END:
    switch (event) {
    case NGHTTP3_HTTP_EVENT_HEADERS_BEGIN:
      /* TODO Better to check status code */
      if (stream->rx.http.flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) {
        return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_TRAILERS_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_DATA_BEGIN:
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_DATA_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_MSG_END:
      rv = nghttp3_http_on_remote_end_stream(stream);
      if (rv != 0) {
        return rv;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_END;
      return 0;
    default:
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
  case NGHTTP3_HTTP_STATE_REQ_DATA_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_DATA_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_DATA_END;
    return 0;
  case NGHTTP3_HTTP_STATE_REQ_DATA_END:
    switch (event) {
    case NGHTTP3_HTTP_EVENT_DATA_BEGIN:
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_DATA_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_HEADERS_BEGIN:
      /* TODO Better to check status code */
      if (stream->rx.http.flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) {
        return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_TRAILERS_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_MSG_END:
      rv = nghttp3_http_on_remote_end_stream(stream);
      if (rv != 0) {
        return rv;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_END;
      return 0;
    default:
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
  case NGHTTP3_HTTP_STATE_REQ_TRAILERS_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_HEADERS_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_TRAILERS_END;
    return 0;
  case NGHTTP3_HTTP_STATE_REQ_TRAILERS_END:
    if (event != NGHTTP3_HTTP_EVENT_MSG_END) {
      /* TODO Should ignore unexpected frame in this state as per
         spec. */
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
    rv = nghttp3_http_on_remote_end_stream(stream);
    if (rv != 0) {
      return rv;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_REQ_END;
    return 0;
  case NGHTTP3_HTTP_STATE_REQ_END:
    return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
  case NGHTTP3_HTTP_STATE_RESP_INITIAL:
    if (event != NGHTTP3_HTTP_EVENT_HEADERS_BEGIN) {
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_HEADERS_BEGIN;
    return 0;
  case NGHTTP3_HTTP_STATE_RESP_HEADERS_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_HEADERS_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_HEADERS_END;
    return 0;
  case NGHTTP3_HTTP_STATE_RESP_HEADERS_END:
    switch (event) {
    case NGHTTP3_HTTP_EVENT_HEADERS_BEGIN:
      if (stream->rx.http.status_code == -1) {
        stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_HEADERS_BEGIN;
        return 0;
      }
      if ((stream->rx.http.flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) &&
          stream->rx.http.status_code / 100 == 2) {
        return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_TRAILERS_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_DATA_BEGIN:
      if (stream->rx.http.flags & NGHTTP3_HTTP_FLAG_EXPECT_FINAL_RESPONSE) {
        return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_DATA_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_MSG_END:
      rv = nghttp3_http_on_remote_end_stream(stream);
      if (rv != 0) {
        return rv;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_END;
      return 0;
    default:
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
  case NGHTTP3_HTTP_STATE_RESP_DATA_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_DATA_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_DATA_END;
    return 0;
  case NGHTTP3_HTTP_STATE_RESP_DATA_END:
    switch (event) {
    case NGHTTP3_HTTP_EVENT_DATA_BEGIN:
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_DATA_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_HEADERS_BEGIN:
      if ((stream->rx.http.flags & NGHTTP3_HTTP_FLAG_METH_CONNECT) &&
          stream->rx.http.status_code / 100 == 2) {
        return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_TRAILERS_BEGIN;
      return 0;
    case NGHTTP3_HTTP_EVENT_MSG_END:
      rv = nghttp3_http_on_remote_end_stream(stream);
      if (rv != 0) {
        return rv;
      }
      stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_END;
      return 0;
    default:
      return NGHTTP3_ERR_H3_FRAME_UNEXPECTED;
    }
  case NGHTTP3_HTTP_STATE_RESP_TRAILERS_BEGIN:
    if (event != NGHTTP3_HTTP_EVENT_HEADERS_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_TRAILERS_END;
    return 0;
  case NGHTTP3_HTTP_STATE_RESP_TRAILERS_END:
    if (event != NGHTTP3_HTTP_EVENT_MSG_END) {
      return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
    }
    rv = nghttp3_http_on_remote_end_stream(stream);
    if (rv != 0) {
      return rv;
    }
    stream->rx.hstate = NGHTTP3_HTTP_STATE_RESP_END;
    return 0;
  case NGHTTP3_HTTP_STATE_RESP_END:
    return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
  default:
    nghttp3_unreachable();
  }
}

int nghttp3_stream_empty_headers_allowed(nghttp3_stream *stream) {
  switch (stream->rx.hstate) {
  case NGHTTP3_HTTP_STATE_REQ_TRAILERS_BEGIN:
  case NGHTTP3_HTTP_STATE_RESP_TRAILERS_BEGIN:
    return 0;
  default:
    return NGHTTP3_ERR_MALFORMED_HTTP_MESSAGING;
  }
}

int nghttp3_stream_uni(int64_t stream_id) { return (stream_id & 0x2) != 0; }

int nghttp3_client_stream_bidi(int64_t stream_id) {
  return (stream_id & 0x3) == 0;
}

int nghttp3_client_stream_uni(int64_t stream_id) {
  return (stream_id & 0x3) == 0x2;
}

int nghttp3_server_stream_uni(int64_t stream_id) {
  return (stream_id & 0x3) == 0x3;
}

int nghttp3_stream_init_forward_error_correction_params(
  nghttp3_stream *stream, uint32_t n, uint32_t k) {

  //printf("[ciotto][nghttp3_stream_init_forward_error_correction_params]"
    //        " n = %d, k = %d\n", n, k);

  stream->forward_error_correction.enabled = 1;
  stream->forward_error_correction.n = n;
  stream->forward_error_correction.k = k;

  return 0;
}

int nghttp3_stream_init_forward_error_correction_common(
  nghttp3_stream *stream, uint32_t datalen) {

  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;

  of_entity *entity;
  if (stream->forward_error_correction.codec_type == OF_ENCODER) {
    //printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] OF_ENCODER.\n");
    entity = &stream->forward_error_correction.encoder;
  } else if (stream->forward_error_correction.codec_type == OF_DECODER) {
    //printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] OF_DECODER.\n");
    entity = &stream->forward_error_correction.decoder;
  } else {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] "
              "Invalid codec type.\n");
    return -1;
  }

  /* OpenFEC dependent code */
  /* TODO: propose a decoupled solution. Using another callback? */
  if (n <= 255) {
    of_rs_2_m_parameters_t* rs_params;

    entity->codec_id = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;

    if ((rs_params = (of_rs_2_m_parameters_t *) calloc(1, sizeof(of_rs_2_m_parameters_t))) == NULL) {
                             printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] "
                "Not enough memory to allocate of_rs_2_m_parameters_t.\n");
      return -1;
                }

                rs_params->m = 8;
                entity->params = (of_parameters_t *) rs_params;

  } else {
	        printf("We are using LDPC codes!\n");
                of_ldpc_parameters_t* ldpc_params;

                entity->codec_id = OF_CODEC_LDPC_STAIRCASE_STABLE;

    if ((ldpc_params = (of_ldpc_parameters_t *)calloc(1, sizeof(of_ldpc_parameters_t))) == NULL) {
                        printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] "
                "Not enough memory to allocate of_ldpc_parameters_t.\n");
      return -1;
                }

    ldpc_params->prng_seed = 1804289383; //rand();
    printf("ldpc_params->prng_seed = %d \n", ldpc_params->prng_seed);
                   ldpc_params->N1 = 7;
                entity->params = (of_parameters_t *) ldpc_params;
        }

        entity->params->nb_source_symbols = k;
  entity->params->nb_repair_symbols = n - k;
        entity->params->encoding_symbol_length  = datalen / k;

  //printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] "
    //        "entity->params->encoding_symbol_length = %d\n",
    //        entity->params->encoding_symbol_length);

  of_status_t ret;

        if ((ret = of_create_codec_instance(
          &entity->ses,
          entity->codec_id,
          stream->forward_error_correction.codec_type,
          0)) != OF_STATUS_OK) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] "
	                "Failure while creating of_create_codec_instance.\n");
    return -1;
        }

        if (of_set_fec_parameters(
        entity->ses,
        entity->params) != OF_STATUS_OK)        {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_common] Failure while running of_set_fec_parameters.\n");
    return -1;
        }

  return 0;
}

int nghttp3_stream_init_forward_error_correction_decoder(
  nghttp3_stream *stream, uint32_t datalen) {

  //printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] start\n");

  stream->forward_error_correction.codec_type = OF_DECODER;
   of_entity *decoder = &(stream->forward_error_correction.decoder);

  uint32_t esi;
  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;

  decoder->header_offset = 0;
  decoder->received_symb_num = 0;

  if (nghttp3_stream_init_forward_error_correction_common(
          stream,
          datalen)) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
              "Failure to init encoder.\n");
    return -1;
  }

  if ((decoder->src_symbols_tab = (void**) calloc(k, sizeof(void*))) == NULL) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
	                "calloc failed for src_symbols_tab.\n");
    return -1;
  }

  if ((decoder->rec_symbols_tab = (void**) calloc(n, sizeof(void*))) == NULL) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
              "calloc failed for rec_symbols_tab.\n");
    return -1;
  }

  if ((decoder->rec_len_tab = (int*) calloc(n, sizeof(int))) == NULL) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
              "calloc failed for rec_len_tab.\n");
    return -1;
  }

  uint32_t symb_sz = decoder->params->encoding_symbol_length;
    for (uint32_t esi = 0; esi < k; esi++) {
    if ((decoder->src_symbols_tab[esi] = (void*) malloc(symb_sz)) == NULL) {
      printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
              "calloc failed for src_symbols_tab[esi].\n");
      return -1;
    }
    decoder->rec_len_tab[esi] = 0;
  }

  for (uint32_t esi = 0; esi < n; esi++) {
    if ((decoder->rec_symbols_tab[esi] = (void*) malloc(symb_sz)) == NULL) {
      printf("[ciotto][nghttp3_stream_init_forward_error_correction_decoder] "
              "calloc failed for rec_symbols_tab[esi].\n");
      return -1;
    }
  }

  return 0;
}

int nghttp3_stream_init_forward_error_correction_encoder(
  nghttp3_stream *stream, uint32_t datalen) {

  stream->forward_error_correction.codec_type = OF_ENCODER;

  if (nghttp3_stream_init_forward_error_correction_common(
          stream,
          datalen)) {
    printf("[ciotto][nghttp3_stream_init_forward_error_correction_encoder] "
              "Failure to init encoder.\n");
    return -1;
  }

  stream->flags |= NGHTTP3_STREAM_FLAG_SERVER_FORWARD_ERROR_CORRECTION_INIT;
  return 0;
}

int nghttp3_stream_forward_error_correction_try_decode(
  nghttp3_stream *stream, uint64_t offset, uint8_t *data, size_t datalen, uint32_t* is_decoded) {

  of_entity *decoder = &stream->forward_error_correction.decoder;

  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;

  uint32_t symb_sz = decoder->params->encoding_symbol_length;

  uint32_t esi, symb_offset, frame_offset, len_write, ret;
  esi = offset / symb_sz;
  symb_offset = offset % symb_sz;
  frame_offset = 0;

  while (datalen > 0) {
    //printf("[Nooshin] datalen begin of while = %d \n", datalen);

    if (datalen < symb_sz - symb_offset)
             len_write = datalen;
      else
        len_write = symb_sz - symb_offset;

    memcpy(decoder->rec_symbols_tab[esi] + symb_offset, data + frame_offset, len_write);
    decoder->rec_len_tab[esi] += len_write;

    if (decoder->rec_len_tab[esi] == symb_sz) {
      if (of_decode_with_new_symbol(decoder->ses, (char*) decoder->rec_symbols_tab[esi]/*symb*/, esi) == OF_STATUS_ERROR) {
        printf("[ciotto][nghttp3_stream_forward_error_correction_try_decode] "
              "Failure to run of_decode_with_new_symbol for esi %d\n", esi);
        return -1;
      }
      else {
        decoder->received_symb_num += 1;
        //printf("decoder->received_symb_num =%d offset: %d \n", decoder->received_symb_num, offset);
        if ((decoder->received_symb_num >= k) && (of_is_decoding_complete(decoder->ses) == true)) {
            *is_decoded = true;
            //printf("decoder->received_symb_num =%d is_decoded = true \n", decoder->received_symb_num);
            break;
	           }
      }
    }
    esi += 1;
    datalen -= len_write;
    //printf("[Nooshin] datalen = %d \n", datalen);
    symb_offset = 0;
    frame_offset += len_write;
    //printf("[Nooshin][nghttp3_stream_forward_error_correction_try_decode] datalen  =%d \n", datalen);
  }

  if (!*is_decoded && decoder->received_symb_num >= k) {
      ret = of_finish_decoding(decoder->ses);
      if (ret == OF_STATUS_ERROR || ret == OF_STATUS_FATAL_ERROR) {
        printf("of_finish_decoding() failed with error (%d)\n", ret);
      }
      else if (ret == OF_STATUS_OK)
        *is_decoded = true;
  }

   if(*is_decoded) {
    //FILE *fptr = fopen("/users/eghbal/private-ngtcp2-main/examples/server-root/decoder", "w");
    of_get_source_symbols_tab(decoder->ses, decoder->src_symbols_tab);
    /*for (int esi = 0; esi < k; esi ++){
        fwrite((char*) decoder->src_symbols_tab[esi], sizeof(uint8_t), symb_sz, fptr);
        fflush(fptr);
    }*/
    //printf("[Nooshin][nghttp3_stream_forward_error_correction_try_decode] after if(*is_decoded) \n");
  }
}

void nghttp3_stream_forward_error_correction_encode(
  nghttp3_stream *stream, nghttp3_vec* vec, nghttp3_ssize sveccnt) {

  void **enc_symbols_tab;
  uint32_t esi;
  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;
  uint32_t symb_sz =
    stream->forward_error_correction.encoder.params->encoding_symbol_length;
  uint32_t datalen = nghttp3_vec_len_varint(vec, (size_t)sveccnt);

  //printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
  //                  "datalen = %d\n", datalen);
  //printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
  //                  "symb_sz = %d\n", symb_sz);

  uint8_t* data = (uint8_t*) malloc(datalen * sizeof(uint8_t));
  uint32_t offset = 0;
  for (uint32_t sveci = 0; sveci < sveccnt; sveci++) {
	      memcpy(data + offset, vec[sveci].base, vec[sveci].len);
    offset += vec[sveci].len;
  }

  if ((enc_symbols_tab = (void**) calloc(n, sizeof(void*))) == NULL) {
    printf("[ciotto][ngtcp2_strm_forward_error_correction_encode] "
              "calloc failed for enc_symbols_tab.\n");
    free(data);
    return;
  }

  stream->forward_error_correction.encoder.enc_symbols_tab = enc_symbols_tab;

  //FILE* fp1 = fopen("/users/eghbal/ngtcp2/examples/original.txt", "wb");
  //FILE* fp2 = fopen("/users/eghbal/ngtcp2/examples/redundant.txt", "wb");

  for (esi = 0; esi < k; esi++) {
    if ((enc_symbols_tab[esi] = (void*) calloc(1, symb_sz)) == NULL) {
      printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
                "calloc failed for enc_symbols_tab[esi].\n");
      free(data);
      return;
    }
       memcpy(enc_symbols_tab[esi], data + esi * symb_sz, symb_sz);
       //fwrite(enc_symbols_tab[esi], symb_sz, 1, fp1);
       //fflush(fp1);
  }

  //printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
   //         "Building repair symbols...\n");

  for (esi = k; esi < n; esi++) {
    if ((enc_symbols_tab[esi] = (void*) calloc(1, symb_sz)) == NULL) {
      printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
              "calloc failed for enc_symbols_tab[esi].\n");
      free(data);
      return;
    }

    if (of_build_repair_symbol(
          stream->forward_error_correction.encoder.ses,
          enc_symbols_tab,
          esi) != OF_STATUS_OK) {
      printf("[ciotto][nghttp3_stream_forward_error_correction_encode] "
              "of_build_repair_symbol() failed for esi\n");
            free(data);
      return;
    }
    //fwrite(enc_symbols_tab[esi], symb_sz, 1, fp2);
    //fflush(fp2);
  }

  stream->flags |= NGHTTP3_STREAM_FLAG_SERVER_FORWARD_ERROR_CORRECTION_ENCODED;

  free(data);
}

void nghttp3_stream_forward_error_correction_get_encode_symbols(
  nghttp3_stream *stream, nghttp3_vec *vec) {

  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;
  uint32_t symb_sz =
    stream->forward_error_correction.encoder.params->encoding_symbol_length;
  void **enc_symbols_tab =
    stream->forward_error_correction.encoder.enc_symbols_tab;
   vec->base = (uint8_t*) malloc((n - k) * symb_sz);
  vec->len = (n - k) * symb_sz;

  //FILE* fp = fopen("/users/eghbal/ngtcp2/examples/encoder.txt", "wb");

  for (uint32_t i = k; i < n; i++) {
    //fread(vec->base + (i - k) * symb_sz, symb_sz, 1, fp);
    memcpy(vec->base + (i - k) * symb_sz, enc_symbols_tab[i], symb_sz);
    //printf("repair symbol [%i]  n = %d   k = %d symb_sz = %d  \n", i-k, n, k, symb_sz);
    //fwrite(enc_symbols_tab[i], symb_sz, 1, fp);
    //fflush(fp);
  }
}

uint32_t nghttp3_stream_forward_error_correction_get_encode_symbol_sz(nghttp3_stream *stream) {
  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;
  uint32_t symb_sz =
    stream->forward_error_correction.encoder.params->encoding_symbol_length;

  return (n - k) * symb_sz;
}

void nghttp3_stream_forward_error_correction_release_decoder(nghttp3_stream *stream) {
  if (!stream->forward_error_correction.enabled) {
    return;
      }

  if (stream->forward_error_correction.codec_type != OF_DECODER){
    return;
  }

  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;

  if (stream->forward_error_correction.decoder.src_symbols_tab) {
    for (uint32_t esi = 0; esi < k; esi++) {
      if (stream->forward_error_correction.decoder.src_symbols_tab[esi]) {
        free(stream->forward_error_correction.decoder.src_symbols_tab[esi]);
      }
    }
    free(stream->forward_error_correction.decoder.src_symbols_tab);
  }

  if (stream->forward_error_correction.decoder.rec_symbols_tab) {
    for (uint32_t esi = 0; esi < n; esi++) {
	         if (mprobe(stream->forward_error_correction.decoder.rec_symbols_tab[esi]) == MCHECK_OK) {
        free(stream->forward_error_correction.decoder.rec_symbols_tab[esi]);
      }
    }
    free(stream->forward_error_correction.decoder.rec_symbols_tab);
  }

  if (stream->forward_error_correction.decoder.ses) {
    of_release_codec_instance(stream->forward_error_correction.encoder.ses);
  }

  if (stream->forward_error_correction.decoder.params) {
    free(stream->forward_error_correction.decoder.params);
  }
}

void nghttp3_stream_forward_error_correction_release_encoder(nghttp3_stream *stream) {
  if (!stream->forward_error_correction.enabled) {
    return;
  }

    if (stream->forward_error_correction.codec_type != OF_ENCODER){
    return;
  }

  uint32_t k = stream->forward_error_correction.k;
  uint32_t n = stream->forward_error_correction.n;
  void **enc_symbols_tab =
    stream->forward_error_correction.encoder.enc_symbols_tab;

  if (stream->forward_error_correction.encoder.enc_symbols_tab) {
    for (uint32_t esi = 0; esi < n; esi++) {
      if (enc_symbols_tab[esi]) {
        free(enc_symbols_tab[esi]);
      }
    }
    free(stream->forward_error_correction.encoder.enc_symbols_tab);
  }

  if (stream->forward_error_correction.encoder.ses) {
    of_release_codec_instance(stream->forward_error_correction.encoder.ses);
      }

  if (stream->forward_error_correction.encoder.params) {
    free(stream->forward_error_correction.encoder.params);
  }
}
