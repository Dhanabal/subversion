/*
 * stream.c:   svn_stream operations
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_private_config.h"

#include <assert.h>
#include <stdio.h>

#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <apr_errno.h>
#include <apr_md5.h>

#include <zlib.h>

#include "svn_pools.h"
#include "svn_io.h"
#include "svn_error.h"
#include "svn_string.h"
#include "svn_utf.h"
#include "svn_checksum.h"
#include "svn_path.h"
#include "private/svn_eol_private.h"


struct svn_stream_t {
  void *baton;
  svn_read_fn_t read_fn;
  svn_write_fn_t write_fn;
  svn_close_fn_t close_fn;
  svn_io_reset_fn_t reset_fn;
  svn_io_mark_fn_t mark_fn;
  svn_io_seek_fn_t seek_fn;
  svn_io_line_filter_cb_t line_filter_cb;
  svn_io_line_transformer_cb_t line_transformer_cb;
};


/*** Generic streams. ***/

svn_stream_t *
svn_stream_create(void *baton, apr_pool_t *pool)
{
  svn_stream_t *stream;

  stream = apr_palloc(pool, sizeof(*stream));
  stream->baton = baton;
  stream->read_fn = NULL;
  stream->write_fn = NULL;
  stream->close_fn = NULL;
  stream->reset_fn = NULL;
  stream->mark_fn = NULL;
  stream->seek_fn = NULL;
  stream->line_filter_cb = NULL;
  stream->line_transformer_cb = NULL;
  return stream;
}


void
svn_stream_set_baton(svn_stream_t *stream, void *baton)
{
  stream->baton = baton;
}


void
svn_stream_set_read(svn_stream_t *stream, svn_read_fn_t read_fn)
{
  stream->read_fn = read_fn;
}


void
svn_stream_set_write(svn_stream_t *stream, svn_write_fn_t write_fn)
{
  stream->write_fn = write_fn;
}

void
svn_stream_set_close(svn_stream_t *stream, svn_close_fn_t close_fn)
{
  stream->close_fn = close_fn;
}

void
svn_stream_set_reset(svn_stream_t *stream, svn_io_reset_fn_t reset_fn)
{
  stream->reset_fn = reset_fn;
}

void
svn_stream_set_mark(svn_stream_t *stream, svn_io_mark_fn_t mark_fn)
{
  stream->mark_fn = mark_fn;
}

void
svn_stream_set_seek(svn_stream_t *stream, svn_io_seek_fn_t seek_fn)
{
  stream->seek_fn = seek_fn;
}

void
svn_stream_set_line_filter_callback(svn_stream_t *stream,
                                    svn_io_line_filter_cb_t line_filter_cb)
{
  stream->line_filter_cb = line_filter_cb;
}

void
svn_stream_set_line_transformer_callback(
  svn_stream_t *stream,
  svn_io_line_transformer_cb_t line_transformer_cb)
{
  stream->line_transformer_cb = line_transformer_cb;
}

svn_error_t *
svn_stream_read(svn_stream_t *stream, char *buffer, apr_size_t *len)
{
  SVN_ERR_ASSERT(stream->read_fn != NULL);
  return stream->read_fn(stream->baton, buffer, len);
}


svn_error_t *
svn_stream_write(svn_stream_t *stream, const char *data, apr_size_t *len)
{
  SVN_ERR_ASSERT(stream->write_fn != NULL);
  return stream->write_fn(stream->baton, data, len);
}


svn_error_t *
svn_stream_reset(svn_stream_t *stream)
{
  if (stream->reset_fn == NULL)
    return svn_error_create(SVN_ERR_STREAM_RESET_NOT_SUPPORTED, NULL, NULL);

  return stream->reset_fn(stream->baton);
}

svn_error_t *
svn_stream_mark(svn_stream_t *stream, svn_stream_mark_t **mark,
                apr_pool_t *pool)
{
  if (stream->mark_fn == NULL)
    return svn_error_create(SVN_ERR_STREAM_SEEK_NOT_SUPPORTED, NULL, NULL);

  return stream->mark_fn(stream->baton, mark, pool);
}

svn_error_t *
svn_stream_seek(svn_stream_t *stream, svn_stream_mark_t *mark)
{
  if (stream->seek_fn == NULL)
    return svn_error_create(SVN_ERR_STREAM_SEEK_NOT_SUPPORTED, NULL, NULL);

  return stream->seek_fn(stream->baton, mark);
}

svn_error_t *
svn_stream_close(svn_stream_t *stream)
{
  if (stream->close_fn == NULL)
    return SVN_NO_ERROR;
  return stream->close_fn(stream->baton);
}


svn_error_t *
svn_stream_printf(svn_stream_t *stream,
                  apr_pool_t *pool,
                  const char *fmt,
                  ...)
{
  const char *message;
  va_list ap;
  apr_size_t len;

  va_start(ap, fmt);
  message = apr_pvsprintf(pool, fmt, ap);
  va_end(ap);

  len = strlen(message);
  return svn_stream_write(stream, message, &len);
}


svn_error_t *
svn_stream_printf_from_utf8(svn_stream_t *stream,
                            const char *encoding,
                            apr_pool_t *pool,
                            const char *fmt,
                            ...)
{
  const char *message, *translated;
  va_list ap;
  apr_size_t len;

  va_start(ap, fmt);
  message = apr_pvsprintf(pool, fmt, ap);
  va_end(ap);

  SVN_ERR(svn_utf_cstring_from_utf8_ex2(&translated, message, encoding,
                                        pool));

  len = strlen(translated);

  return svn_stream_write(stream, translated, &len);
}

/* If a line filter callback was set on STREAM, invoke it on LINE,
 * and indicate in *FILTERED whether the line should be filtered.
 * If no line filter callback was set on STREAM, just set *FILTERED to FALSE.
 */
static svn_error_t *
line_filter(svn_stream_t *stream, svn_boolean_t *filtered, const char *line,
            apr_pool_t *pool)
{
  apr_pool_t *scratch_pool;

  if (! stream->line_filter_cb)
    {
      *filtered = FALSE;
      return SVN_NO_ERROR;
    }

  scratch_pool = svn_pool_create(pool);
  SVN_ERR(stream->line_filter_cb(filtered, line, stream->baton, scratch_pool));
  svn_pool_destroy(scratch_pool);
  return SVN_NO_ERROR;
}

/* Run the line transformer callback of STREAM with LINE as input,
 * and expect the transformation result to be returned in BUF,
 * allocated in POOL. */
static svn_error_t *
line_transformer(svn_stream_t *stream, svn_stringbuf_t **buf,
                 const char *line, apr_pool_t *pool, apr_pool_t *scratch_pool)
{
  *buf = NULL;  /* so we can assert that the callback has set it non-null */
  SVN_ERR(stream->line_transformer_cb(buf, line, stream->baton,
                                      pool, scratch_pool));

  /* Die if the line transformer didn't provide any output. */
  SVN_ERR_ASSERT(*buf);

  return SVN_NO_ERROR;
}

/* Scan STREAM for an end-of-line indicatior, and return it in *EOL.
 * Set *EOL to NULL if the stream runs out before an end-of-line indicator
 * is found. */
static svn_error_t *
scan_eol(const char **eol, svn_stream_t *stream, apr_pool_t *pool)
{
  const char *eol_str;
  svn_stream_mark_t *mark;

  SVN_ERR(svn_stream_mark(stream, &mark, pool));

  eol_str = NULL;
  while (! eol_str)
    {
      char buf[512];
      apr_size_t len;

      len = sizeof(buf);
      SVN_ERR(svn_stream_read(stream, buf, &len));
      if (len == 0)
          break; /* EOF */
      eol_str = svn_eol__detect_eol(buf, buf + len);
    }

  SVN_ERR(svn_stream_seek(stream, mark));

  *eol = eol_str;

  return SVN_NO_ERROR;
}

/* Guts of svn_stream_readline() and svn_stream_readline_detect_eol().
 * Returns the line read from STREAM in *STRINGBUF, and indicates
 * end-of-file in *EOF.  If DETECT_EOL is TRUE, the end-of-line indicator
 * is detected automatically and returned in *EOL.
 * If DETECT_EOL is FALSE, *EOL must point to the desired end-of-line
 * indicator.  STRINGBUF is allocated in POOL. */
static svn_error_t *
stream_readline(svn_stringbuf_t **stringbuf,
                svn_boolean_t *eof,
                const char **eol,
                svn_stream_t *stream,
                svn_boolean_t detect_eol,
                apr_pool_t *pool)
{
  svn_stringbuf_t *str;
  apr_pool_t *iterpool;
  svn_boolean_t filtered;
  const char *eol_str;

  *eof = FALSE;

  iterpool = svn_pool_create(pool);
  do
    {
      apr_size_t numbytes;
      const char *match;
      char c;

      svn_pool_clear(iterpool);

      /* Since we're reading one character at a time, let's at least
         optimize for the 90% case.  90% of the time, we can avoid the
         stringbuf ever having to realloc() itself if we start it out at
         80 chars.  */
      str = svn_stringbuf_create_ensure(80, iterpool);

      if (detect_eol)
        {
          SVN_ERR(scan_eol(&eol_str, stream, iterpool));
          if (eol)
            *eol = eol_str;
          if (! eol_str)
            {
              /* No newline until EOF, EOL_STR can be anything. */
              eol_str = APR_EOL_STR;
            }
        }
      else
        eol_str = *eol;

      /* Read into STR up to and including the next EOL sequence. */
      match = eol_str;
      numbytes = 1;
      while (*match)
        {
          SVN_ERR(svn_stream_read(stream, &c, &numbytes));
          if (numbytes != 1)
            {
              /* a 'short' read means the stream has run out. */
              *eof = TRUE;
              /* We know we don't have a whole EOL sequence, but ensure we
               * don't chop off any partial EOL sequence that we may have. */
              match = eol_str;
              /* Process this short (or empty) line just like any other
               * except with *EOF set. */
              break;
            }

          if (c == *match)
            match++;
          else
            match = eol_str;

          svn_stringbuf_appendbytes(str, &c, 1);
        }

      svn_stringbuf_chop(str, match - eol_str);

      SVN_ERR(line_filter(stream, &filtered, str->data, iterpool));
    }
  while (filtered && ! *eof);
  /* Not destroying the iterpool just yet since we still need STR
   * which is allocated in it. */

  if (filtered)
    *stringbuf = svn_stringbuf_create_ensure(0, pool);
  else if (stream->line_transformer_cb)
    SVN_ERR(line_transformer(stream, stringbuf, str->data, pool, iterpool));
  else
    *stringbuf = svn_stringbuf_dup(str, pool);

  /* Done. RIP iterpool. */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_stream_readline(svn_stream_t *stream,
                    svn_stringbuf_t **stringbuf,
                    const char *eol,
                    svn_boolean_t *eof,
                    apr_pool_t *pool)
{
  return svn_error_return(stream_readline(stringbuf, eof, &eol, stream,
                                          FALSE, pool));
}

svn_error_t *
svn_stream_readline_detect_eol(svn_stream_t *stream,
                               svn_stringbuf_t **stringbuf,
                               const char **eol,
                               svn_boolean_t *eof,
                               apr_pool_t *pool)
{
  return svn_error_return(stream_readline(stringbuf, eof, eol, stream,
                                          TRUE, pool));
}


svn_error_t *svn_stream_copy3(svn_stream_t *from, svn_stream_t *to,
                              svn_cancel_func_t cancel_func,
                              void *cancel_baton,
                              apr_pool_t *scratch_pool)
{
  char *buf = apr_palloc(scratch_pool, SVN__STREAM_CHUNK_SIZE);
  svn_error_t *err;
  svn_error_t *err2;

  /* Read and write chunks until we get a short read, indicating the
     end of the stream.  (We can't get a short write without an
     associated error.) */
  while (1)
    {
      apr_size_t len = SVN__STREAM_CHUNK_SIZE;

      if (cancel_func)
        {
          err = cancel_func(cancel_baton);
          if (err)
             break;
        }

      err = svn_stream_read(from, buf, &len);
      if (err)
         break;

      if (len > 0)
        err = svn_stream_write(to, buf, &len);

      if (err || (len != SVN__STREAM_CHUNK_SIZE))
          break;
    }

  err2 = svn_error_compose_create(svn_stream_close(from),
                                  svn_stream_close(to));

  return svn_error_compose_create(err, err2);
}

svn_error_t *
svn_stream_contents_same2(svn_boolean_t *same,
                          svn_stream_t *stream1,
                          svn_stream_t *stream2,
                          apr_pool_t *pool)
{
  char *buf1 = apr_palloc(pool, SVN__STREAM_CHUNK_SIZE);
  char *buf2 = apr_palloc(pool, SVN__STREAM_CHUNK_SIZE);
  apr_size_t bytes_read1 = SVN__STREAM_CHUNK_SIZE;
  apr_size_t bytes_read2 = SVN__STREAM_CHUNK_SIZE;
  svn_error_t *err = NULL;

  *same = TRUE;  /* assume TRUE, until disproved below */
  while (bytes_read1 == SVN__STREAM_CHUNK_SIZE
         && bytes_read2 == SVN__STREAM_CHUNK_SIZE)
    {
      err = svn_stream_read(stream1, buf1, &bytes_read1);
      if (err)
        break;
      err = svn_stream_read(stream2, buf2, &bytes_read2);
      if (err)
        break;

      if ((bytes_read1 != bytes_read2)
          || (memcmp(buf1, buf2, bytes_read1)))
        {
          *same = FALSE;
          break;
        }
    }

  return svn_error_compose_create(err,
                                  svn_error_compose_create(
                                    svn_stream_close(stream1),
                                    svn_stream_close(stream2)));
}



/*** Generic readable empty stream ***/

static svn_error_t *
read_handler_empty(void *baton, char *buffer, apr_size_t *len)
{
  *len = 0;
  return SVN_NO_ERROR;
}


static svn_error_t *
write_handler_empty(void *baton, const char *data, apr_size_t *len)
{
  return SVN_NO_ERROR;
}

static svn_error_t *
reset_handler_empty(void *baton)
{
  return SVN_NO_ERROR;
}

static svn_error_t *
mark_handler_empty(void *baton, svn_stream_mark_t **mark, apr_pool_t *pool)
{
  return SVN_NO_ERROR;
}

static svn_error_t *
seek_handler_empty(void *baton, svn_stream_mark_t *mark)
{
  return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_empty(apr_pool_t *pool)
{
  svn_stream_t *stream;

  stream = svn_stream_create(NULL, pool);
  svn_stream_set_read(stream, read_handler_empty);
  svn_stream_set_write(stream, write_handler_empty);
  svn_stream_set_reset(stream, reset_handler_empty);
  svn_stream_set_mark(stream, mark_handler_empty);
  svn_stream_set_seek(stream, seek_handler_empty);
  return stream;
}



/*** Stream duplication support ***/
struct baton_tee {
  svn_stream_t *out1;
  svn_stream_t *out2;
};


static svn_error_t *
write_handler_tee(void *baton, const char *data, apr_size_t *len)
{
  struct baton_tee *bt = baton;

  SVN_ERR(svn_stream_write(bt->out1, data, len));
  SVN_ERR(svn_stream_write(bt->out2, data, len));

  return SVN_NO_ERROR;
}


static svn_error_t *
close_handler_tee(void *baton)
{
  struct baton_tee *bt = baton;

  SVN_ERR(svn_stream_close(bt->out1));
  SVN_ERR(svn_stream_close(bt->out2));

  return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_tee(svn_stream_t *out1,
               svn_stream_t *out2,
               apr_pool_t *pool)
{
  struct baton_tee *baton;
  svn_stream_t *stream;

  if (out1 == NULL)
    return out2;

  if (out2 == NULL)
    return out1;

  baton = apr_palloc(pool, sizeof(*baton));
  baton->out1 = out1;
  baton->out2 = out2;
  stream = svn_stream_create(baton, pool);
  svn_stream_set_write(stream, write_handler_tee);
  svn_stream_set_close(stream, close_handler_tee);

  return stream;
}



/*** Ownership detaching stream ***/

static svn_error_t *
read_handler_disown(void *baton, char *buffer, apr_size_t *len)
{
  return svn_stream_read(baton, buffer, len);
}

static svn_error_t *
write_handler_disown(void *baton, const char *buffer, apr_size_t *len)
{
  return svn_stream_write(baton, buffer, len);
}

static svn_error_t *
reset_handler_disown(void *baton)
{
  return svn_stream_reset(baton);
}

static svn_error_t *
mark_handler_disown(void *baton, svn_stream_mark_t **mark, apr_pool_t *pool)
{
  return svn_stream_mark(baton, mark, pool);
}

static svn_error_t *
seek_handler_disown(void *baton, svn_stream_mark_t *mark)
{
  return svn_stream_seek(baton, mark);
}

svn_stream_t *
svn_stream_disown(svn_stream_t *stream, apr_pool_t *pool)
{
  svn_stream_t *s = svn_stream_create(stream, pool);

  svn_stream_set_read(s, read_handler_disown);
  svn_stream_set_write(s, write_handler_disown);
  svn_stream_set_reset(s, reset_handler_disown);
  svn_stream_set_mark(s, mark_handler_disown);
  svn_stream_set_seek(s, seek_handler_disown);

  return s;
}



/*** Generic stream for APR files ***/
struct baton_apr {
  apr_file_t *file;
  apr_pool_t *pool;

  /* Offsets when reading from a range of the file.
   * When either of these is negative, no range has been specified. */
  apr_off_t start;
  apr_off_t end;
};

/* svn_stream_mark_t for streams backed by APR files. */
struct mark_apr {
  apr_off_t off;
};

static svn_error_t *
read_handler_apr(void *baton, char *buffer, apr_size_t *len)
{
  struct baton_apr *btn = baton;
  svn_error_t *err;

  err = svn_io_file_read_full(btn->file, buffer, *len, len, btn->pool);
  if (err && APR_STATUS_IS_EOF(err->apr_err))
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
    }

  return err;
}

static svn_error_t *
write_handler_apr(void *baton, const char *data, apr_size_t *len)
{
  struct baton_apr *btn = baton;

  return svn_io_file_write_full(btn->file, data, *len, len, btn->pool);
}

static svn_error_t *
close_handler_apr(void *baton)
{
  struct baton_apr *btn = baton;

  return svn_io_file_close(btn->file, btn->pool);
}

static svn_error_t *
reset_handler_apr(void *baton)
{
  apr_off_t offset;
  struct baton_apr *btn = baton;

  /* If we're reading from a range, reset to the start of the range.
   * Otherwise, reset to the start of the file. */
  offset = btn->start >= 0 ? btn->start : 0;

  return svn_io_file_seek(btn->file, APR_SET, &offset, btn->pool);
}

static svn_error_t *
mark_handler_apr(void *baton, svn_stream_mark_t **mark, apr_pool_t *pool)
{
  struct baton_apr *btn = baton;
  struct mark_apr *mark_apr;

  mark_apr = apr_palloc(pool, sizeof(*mark_apr));
  mark_apr->off = 0;
  SVN_ERR(svn_io_file_seek(btn->file, APR_CUR, &mark_apr->off, btn->pool));
  *mark = (svn_stream_mark_t *)mark_apr;
  return SVN_NO_ERROR;
}

static svn_error_t *
seek_handler_apr(void *baton, svn_stream_mark_t *mark)
{
  struct baton_apr *btn = baton;
  struct mark_apr *mark_apr;

  mark_apr = (struct mark_apr *)mark;
  SVN_ERR(svn_io_file_seek(btn->file, APR_SET, &mark_apr->off, btn->pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_stream_open_readonly(svn_stream_t **stream,
                         const char *path,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  apr_file_t *file;

  SVN_ERR(svn_io_file_open(&file, path, APR_READ | APR_BUFFERED | APR_BINARY,
                           APR_OS_DEFAULT, result_pool));
  *stream = svn_stream_from_aprfile2(file, FALSE, result_pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_stream_open_writable(svn_stream_t **stream,
                         const char *path,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  apr_file_t *file;

  SVN_ERR(svn_io_file_open(&file, path,
                           APR_WRITE
                             | APR_BUFFERED
                             | APR_BINARY
                             | APR_CREATE
                             | APR_EXCL,
                           APR_OS_DEFAULT, result_pool));
  *stream = svn_stream_from_aprfile2(file, FALSE, result_pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_stream_open_unique(svn_stream_t **stream,
                       const char **temp_path,
                       const char *dirpath,
                       svn_io_file_del_t delete_when,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  apr_file_t *file;

  SVN_ERR(svn_io_open_unique_file3(&file, temp_path, dirpath,
                                   delete_when, result_pool, scratch_pool));
  *stream = svn_stream_from_aprfile2(file, FALSE, result_pool);

  return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_from_aprfile2(apr_file_t *file,
                         svn_boolean_t disown,
                         apr_pool_t *pool)
{
  struct baton_apr *baton;
  svn_stream_t *stream;

  if (file == NULL)
    return svn_stream_empty(pool);

  baton = apr_palloc(pool, sizeof(*baton));
  baton->file = file;
  baton->pool = pool;
  baton->start = -1;
  baton->end = -1;
  stream = svn_stream_create(baton, pool);
  svn_stream_set_read(stream, read_handler_apr);
  svn_stream_set_write(stream, write_handler_apr);
  svn_stream_set_reset(stream, reset_handler_apr);
  svn_stream_set_mark(stream, mark_handler_apr);
  svn_stream_set_seek(stream, seek_handler_apr);

  if (! disown)
    svn_stream_set_close(stream, close_handler_apr);

  return stream;
}

/* A read handler (#svn_read_fn_t) that forwards to read_handler_apr()
   but only allows reading between BATON->start and BATON->end. */
static svn_error_t *
read_range_handler_apr(void *baton, char *buffer, apr_size_t *len)
{
  struct baton_apr *btn = baton;

  /* ### BH: I think this can be simplified/optimized by just keeping
             track of the current position. */

  /* Check for range restriction. */
  if (btn->start >= 0 && btn->end > 0)
    {
      /* Get the current file position and make sure it is in range. */
      apr_off_t pos;

      pos = 0;
      SVN_ERR(svn_io_file_seek(btn->file, APR_CUR, &pos, btn->pool));
      if (pos < btn->start)
        {
          /* We're before the range, so forward the file cursor to
           * the start of the range. */
          pos = btn->start;
          SVN_ERR(svn_io_file_seek(btn->file, APR_SET, &pos, btn->pool));
        }
      else if (pos >= btn->end)
        {
          /* We're past the range, indicate that no bytes can be read. */
          *len = 0;
          return SVN_NO_ERROR;
        }

      /* We're in range, but don't read over the end of the range. */
      if (pos + *len > btn->end)
        *len = (apr_size_t)(btn->end - pos);
    }

  return read_handler_apr(baton, buffer, len);
}


svn_stream_t *
svn_stream_from_aprfile_range_readonly(apr_file_t *file,
                                       svn_boolean_t disown,
                                       apr_off_t start,
                                       apr_off_t end,
                                       apr_pool_t *pool)
{
  struct baton_apr *baton;
  svn_stream_t *stream;
  apr_off_t pos;

  /* ### HACK: These shortcuts don't handle disown FALSE properly */
  if (file == NULL || start < 0 || end <= 0 || start >= end)
    return svn_stream_empty(pool);

  /* Set the file pointer to the start of the range. */
  pos = start;
  if (apr_file_seek(file, APR_SET, &pos) != APR_SUCCESS)
    return svn_stream_empty(pool);

  baton = apr_palloc(pool, sizeof(*baton));
  baton->file = file;
  baton->pool = pool;
  baton->start = start;
  baton->end = end;
  stream = svn_stream_create(baton, pool);
  svn_stream_set_read(stream, read_range_handler_apr);
  svn_stream_set_reset(stream, reset_handler_apr);
  svn_stream_set_mark(stream, mark_handler_apr);
  svn_stream_set_seek(stream, seek_handler_apr);

  if (! disown)
    svn_stream_set_close(stream, close_handler_apr);

  return stream;
}


/* Compressed stream support */

#define ZBUFFER_SIZE 4096       /* The size of the buffer the
                                   compressed stream uses to read from
                                   the substream. Basically an
                                   arbitrary value, picked to be about
                                   page-sized. */

struct zbaton {
  z_stream *in;                 /* compressed stream for reading */
  z_stream *out;                /* compressed stream for writing */
  svn_read_fn_t read;           /* substream's read function */
  svn_write_fn_t write;         /* substream's write function */
  svn_close_fn_t close;         /* substream's close function */
  void *read_buffer;            /* buffer   used   for  reading   from
                                   substream */
  int read_flush;               /* what flush mode to use while
                                   reading */
  apr_pool_t *pool;             /* The pool this baton is allocated
                                   on */
  void *subbaton;               /* The substream's baton */
};

/* zlib alloc function. opaque is the pool we need. */
static voidpf
zalloc(voidpf opaque, uInt items, uInt size)
{
  apr_pool_t *pool = opaque;

  return apr_palloc(pool, items * size);
}

/* zlib free function */
static void
zfree(voidpf opaque, voidpf address)
{
  /* Empty, since we allocate on the pool */
}

/* Converts a zlib error to an svn_error_t. zerr is the error code,
   function is the function name, and stream is the z_stream we are
   using.  */
static svn_error_t *
zerr_to_svn_error(int zerr, const char *function, z_stream *stream)
{
  apr_status_t status;
  const char *message;

  if (zerr == Z_OK)
    return SVN_NO_ERROR;

  switch (zerr)
    {
    case Z_STREAM_ERROR:
      status = SVN_ERR_STREAM_MALFORMED_DATA;
      message = "stream error";
      break;

    case Z_MEM_ERROR:
      status = APR_ENOMEM;
      message = "out of memory";
      break;

    case Z_BUF_ERROR:
      status = APR_ENOMEM;
      message = "buffer error";
      break;

    case Z_VERSION_ERROR:
      status = SVN_ERR_STREAM_UNRECOGNIZED_DATA;
      message = "version error";
      break;

    case Z_DATA_ERROR:
      status = SVN_ERR_STREAM_MALFORMED_DATA;
      message = "corrupted data";
      break;

    default:
      status = SVN_ERR_STREAM_UNRECOGNIZED_DATA;
      message = "error";
      break;
    }

  if (stream->msg != NULL)
    return svn_error_createf(status, NULL, "zlib (%s): %s: %s", function,
                             message, stream->msg);
  else
    return svn_error_createf(status, NULL, "zlib (%s): %s", function,
                             message);
}

/* Helper function to figure out the sync mode */
static svn_error_t *
read_helper_gz(svn_read_fn_t read_fn,
               void *baton,
               char *buffer,
               uInt *len, int *zflush)
{
  uInt orig_len = *len;

  /* There's no reason this value should grow bigger than the range of
     uInt, but Subversion's API requires apr_size_t. */
  apr_size_t apr_len = (apr_size_t) *len;

  SVN_ERR((*read_fn)(baton, buffer, &apr_len));

  /* Type cast back to uInt type that zlib uses.  On LP64 platforms
     apr_size_t will be bigger than uInt. */
  *len = (uInt) apr_len;

  /* I wanted to use Z_FINISH here, but we need to know our buffer is
     big enough */
  *zflush = (*len) < orig_len ? Z_SYNC_FLUSH : Z_SYNC_FLUSH;

  return SVN_NO_ERROR;
}

/* Handle reading from a compressed stream */
static svn_error_t *
read_handler_gz(void *baton, char *buffer, apr_size_t *len)
{
  struct zbaton *btn = baton;
  int zerr;

  if (btn->in == NULL)
    {
      btn->in = apr_palloc(btn->pool, sizeof(z_stream));
      btn->in->zalloc = zalloc;
      btn->in->zfree = zfree;
      btn->in->opaque = btn->pool;
      btn->read_buffer = apr_palloc(btn->pool, ZBUFFER_SIZE);
      btn->in->next_in = btn->read_buffer;
      btn->in->avail_in = ZBUFFER_SIZE;

      SVN_ERR(read_helper_gz(btn->read, btn->subbaton, btn->read_buffer,
                             &btn->in->avail_in, &btn->read_flush));

      zerr = inflateInit(btn->in);
      SVN_ERR(zerr_to_svn_error(zerr, "inflateInit", btn->in));
    }

  btn->in->next_out = (Bytef *) buffer;
  btn->in->avail_out = *len;

  while (btn->in->avail_out > 0)
    {
      if (btn->in->avail_in <= 0)
        {
          btn->in->avail_in = ZBUFFER_SIZE;
          btn->in->next_in = btn->read_buffer;
          SVN_ERR(read_helper_gz(btn->read, btn->subbaton, btn->read_buffer,
                                 &btn->in->avail_in, &btn->read_flush));
        }

      zerr = inflate(btn->in, btn->read_flush);
      if (zerr == Z_STREAM_END)
        break;
      else if (zerr != Z_OK)
        return zerr_to_svn_error(zerr, "inflate", btn->in);
    }

  *len -= btn->in->avail_out;
  return SVN_NO_ERROR;
}

/* Compress data and write it to the substream */
static svn_error_t *
write_handler_gz(void *baton, const char *buffer, apr_size_t *len)
{
  struct zbaton *btn = baton;
  apr_pool_t *subpool;
  void *write_buf;
  apr_size_t buf_size, write_len;
  int zerr;

  if (btn->out == NULL)
    {
      btn->out = apr_palloc(btn->pool, sizeof(z_stream));
      btn->out->zalloc = zalloc;
      btn->out->zfree = zfree;
      btn->out->opaque =  btn->pool;

      zerr = deflateInit(btn->out, Z_DEFAULT_COMPRESSION);
      SVN_ERR(zerr_to_svn_error(zerr, "deflateInit", btn->out));
    }

  /* The largest buffer we should need is 0.1% larger than the
     compressed data, + 12 bytes. This info comes from zlib.h.  */
  buf_size = *len + (*len / 1000) + 13;
  subpool = svn_pool_create(btn->pool);
  write_buf = apr_palloc(subpool, buf_size);

  btn->out->next_in = (Bytef *) buffer;  /* Casting away const! */
  btn->out->avail_in = *len;

  while (btn->out->avail_in > 0)
    {
      btn->out->next_out = write_buf;
      btn->out->avail_out = buf_size;

      zerr = deflate(btn->out, Z_NO_FLUSH);
      SVN_ERR(zerr_to_svn_error(zerr, "deflate", btn->out));
      write_len = buf_size - btn->out->avail_out;
      if (write_len > 0)
        SVN_ERR(btn->write(btn->subbaton, write_buf, &write_len));
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Handle flushing and closing the stream */
static svn_error_t *
close_handler_gz(void *baton)
{
  struct zbaton *btn = baton;
  int zerr;

  if (btn->in != NULL)
    {
      zerr = inflateEnd(btn->in);
      SVN_ERR(zerr_to_svn_error(zerr, "inflateEnd", btn->in));
    }

  if (btn->out != NULL)
    {
      void *buf;
      apr_size_t write_len;

      buf = apr_palloc(btn->pool, ZBUFFER_SIZE);

      while (TRUE)
        {
          btn->out->next_out = buf;
          btn->out->avail_out = ZBUFFER_SIZE;

          zerr = deflate(btn->out, Z_FINISH);
          if (zerr != Z_STREAM_END && zerr != Z_OK)
            return zerr_to_svn_error(zerr, "deflate", btn->out);
          write_len = ZBUFFER_SIZE - btn->out->avail_out;
          if (write_len > 0)
            SVN_ERR(btn->write(btn->subbaton, buf, &write_len));
          if (zerr == Z_STREAM_END)
            break;
        }

      zerr = deflateEnd(btn->out);
      SVN_ERR(zerr_to_svn_error(zerr, "deflateEnd", btn->out));
    }

  if (btn->close != NULL)
    return btn->close(btn->subbaton);
  else
    return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_compressed(svn_stream_t *stream, apr_pool_t *pool)
{
  struct svn_stream_t *zstream;
  struct zbaton *baton;

  assert(stream != NULL);

  baton = apr_palloc(pool, sizeof(*baton));
  baton->in = baton->out = NULL;
  baton->read = stream->read_fn;
  baton->write = stream->write_fn;
  baton->close = stream->close_fn;
  baton->subbaton = stream->baton;
  baton->pool = pool;
  baton->read_buffer = NULL;
  baton->read_flush = Z_SYNC_FLUSH;

  zstream = svn_stream_create(baton, pool);
  svn_stream_set_read(zstream, read_handler_gz);
  svn_stream_set_write(zstream, write_handler_gz);
  svn_stream_set_close(zstream, close_handler_gz);

  return zstream;
}


/* Checksummed stream support */

struct checksum_stream_baton
{
  svn_checksum_ctx_t *read_ctx, *write_ctx;
  svn_checksum_t **read_checksum;  /* Output value. */
  svn_checksum_t **write_checksum;  /* Output value. */
  svn_stream_t *proxy;

  /* True if more data should be read when closing the stream. */
  svn_boolean_t read_more;

  /* Pool to allocate read buffer and output values from. */
  apr_pool_t *pool;
};

static svn_error_t *
read_handler_checksum(void *baton, char *buffer, apr_size_t *len)
{
  struct checksum_stream_baton *btn = baton;
  apr_size_t saved_len = *len;

  SVN_ERR(svn_stream_read(btn->proxy, buffer, len));

  if (btn->read_checksum)
    SVN_ERR(svn_checksum_update(btn->read_ctx, buffer, *len));

  if (saved_len != *len)
    btn->read_more = FALSE;

  return SVN_NO_ERROR;
}


static svn_error_t *
write_handler_checksum(void *baton, const char *buffer, apr_size_t *len)
{
  struct checksum_stream_baton *btn = baton;

  if (btn->write_checksum && *len > 0)
    SVN_ERR(svn_checksum_update(btn->write_ctx, buffer, *len));

  return svn_stream_write(btn->proxy, buffer, len);
}


static svn_error_t *
close_handler_checksum(void *baton)
{
  struct checksum_stream_baton *btn = baton;

  /* If we're supposed to drain the stream, do so before finalizing the
     checksum. */
  if (btn->read_more)
    {
      char *buf = apr_palloc(btn->pool, SVN__STREAM_CHUNK_SIZE);
      apr_size_t len = SVN__STREAM_CHUNK_SIZE;

      do
        {
          SVN_ERR(read_handler_checksum(baton, buf, &len));
        }
      while (btn->read_more);
    }

  if (btn->read_ctx)
    SVN_ERR(svn_checksum_final(btn->read_checksum, btn->read_ctx, btn->pool));

  if (btn->write_ctx)
    SVN_ERR(svn_checksum_final(btn->write_checksum, btn->write_ctx, btn->pool));

  return svn_stream_close(btn->proxy);
}


svn_stream_t *
svn_stream_checksummed2(svn_stream_t *stream,
                        svn_checksum_t **read_checksum,
                        svn_checksum_t **write_checksum,
                        svn_checksum_kind_t checksum_kind,
                        svn_boolean_t read_all,
                        apr_pool_t *pool)
{
  svn_stream_t *s;
  struct checksum_stream_baton *baton;

  if (read_checksum == NULL && write_checksum == NULL)
    return stream;

  baton = apr_palloc(pool, sizeof(*baton));
  if (read_checksum)
    baton->read_ctx = svn_checksum_ctx_create(checksum_kind, pool);
  else
    baton->read_ctx = NULL;

  if (write_checksum)
    baton->write_ctx = svn_checksum_ctx_create(checksum_kind, pool);
  else
    baton->write_ctx = NULL;

  baton->read_checksum = read_checksum;
  baton->write_checksum = write_checksum;
  baton->proxy = stream;
  baton->read_more = read_all;
  baton->pool = pool;

  s = svn_stream_create(baton, pool);
  svn_stream_set_read(s, read_handler_checksum);
  svn_stream_set_write(s, write_handler_checksum);
  svn_stream_set_close(s, close_handler_checksum);
  return s;
}

struct md5_stream_baton
{
  const unsigned char **read_digest;
  const unsigned char **write_digest;
  svn_checksum_t *read_checksum;
  svn_checksum_t *write_checksum;
  svn_stream_t *proxy;
  apr_pool_t *pool;
};

static svn_error_t *
read_handler_md5(void *baton, char *buffer, apr_size_t *len)
{
  struct md5_stream_baton *btn = baton;
  return svn_stream_read(btn->proxy, buffer, len);
}

static svn_error_t *
write_handler_md5(void *baton, const char *buffer, apr_size_t *len)
{
  struct md5_stream_baton *btn = baton;
  return svn_stream_write(btn->proxy, buffer, len);
}

static svn_error_t *
close_handler_md5(void *baton)
{
  struct md5_stream_baton *btn = baton;

  SVN_ERR(svn_stream_close(btn->proxy));

  if (btn->read_digest)
    *btn->read_digest
      = apr_pmemdup(btn->pool, btn->read_checksum->digest,
                    APR_MD5_DIGESTSIZE);

  if (btn->write_digest)
    *btn->write_digest
      = apr_pmemdup(btn->pool, btn->write_checksum->digest,
                    APR_MD5_DIGESTSIZE);

  return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_checksummed(svn_stream_t *stream,
                       const unsigned char **read_digest,
                       const unsigned char **write_digest,
                       svn_boolean_t read_all,
                       apr_pool_t *pool)
{
  svn_stream_t *s;
  struct md5_stream_baton *baton;

  if (! read_digest && ! write_digest)
    return stream;

  baton = apr_palloc(pool, sizeof(*baton));
  baton->read_digest = read_digest;
  baton->write_digest = write_digest;
  baton->pool = pool;

  /* Set BATON->proxy to a stream that will fill in BATON->read_checksum
   * and BATON->write_checksum (if we want them) when it is closed. */
  baton->proxy
    = svn_stream_checksummed2(stream,
                              read_digest ? &baton->read_checksum : NULL,
                              write_digest ? &baton->write_checksum : NULL,
                              svn_checksum_md5,
                              read_all, pool);

  /* Create a stream that will forward its read/write/close operations to
   * BATON->proxy and will fill in *READ_DIGEST and *WRITE_DIGEST (if we
   * want them) after it closes BATON->proxy. */
  s = svn_stream_create(baton, pool);
  svn_stream_set_read(s, read_handler_md5);
  svn_stream_set_write(s, write_handler_md5);
  svn_stream_set_close(s, close_handler_md5);
  return s;
}




/* Miscellaneous stream functions. */
struct stringbuf_stream_baton
{
  svn_stringbuf_t *str;
  apr_size_t amt_read;
};

/* svn_stream_mark_t for streams backed by stringbufs. */
struct stringbuf_stream_mark {
    apr_size_t pos; 
};

static svn_error_t *
read_handler_stringbuf(void *baton, char *buffer, apr_size_t *len)
{
  struct stringbuf_stream_baton *btn = baton;
  apr_size_t left_to_read = btn->str->len - btn->amt_read;

  *len = (*len > left_to_read) ? left_to_read : *len;
  memcpy(buffer, btn->str->data + btn->amt_read, *len);
  btn->amt_read += *len;
  return SVN_NO_ERROR;
}

static svn_error_t *
write_handler_stringbuf(void *baton, const char *data, apr_size_t *len)
{
  struct stringbuf_stream_baton *btn = baton;

  svn_stringbuf_appendbytes(btn->str, data, *len);
  return SVN_NO_ERROR;
}

static svn_error_t *
reset_handler_stringbuf(void *baton)
{
  struct stringbuf_stream_baton *btn = baton;
  btn->amt_read = 0;
  return SVN_NO_ERROR;
}

static svn_error_t *
mark_handler_stringbuf(void *baton, svn_stream_mark_t **mark, apr_pool_t *pool)
{
  struct stringbuf_stream_baton *btn;
  struct stringbuf_stream_mark *stringbuf_stream_mark;

  btn = baton;
  stringbuf_stream_mark = apr_palloc(pool, sizeof(*stringbuf_stream_mark));
  stringbuf_stream_mark->pos = btn->amt_read;
  *mark = (svn_stream_mark_t *)stringbuf_stream_mark;
  return SVN_NO_ERROR;
}

static svn_error_t *
seek_handler_stringbuf(void *baton, svn_stream_mark_t *mark)
{
  struct stringbuf_stream_baton *btn;
  struct stringbuf_stream_mark *stringbuf_stream_mark;

  btn = baton;
  stringbuf_stream_mark = (struct stringbuf_stream_mark *)mark;
  btn->amt_read = stringbuf_stream_mark->pos;
  return SVN_NO_ERROR;
}

svn_stream_t *
svn_stream_from_stringbuf(svn_stringbuf_t *str,
                          apr_pool_t *pool)
{
  svn_stream_t *stream;
  struct stringbuf_stream_baton *baton;

  if (! str)
    return svn_stream_empty(pool);

  baton = apr_palloc(pool, sizeof(*baton));
  baton->str = str;
  baton->amt_read = 0;
  stream = svn_stream_create(baton, pool);
  svn_stream_set_read(stream, read_handler_stringbuf);
  svn_stream_set_write(stream, write_handler_stringbuf);
  svn_stream_set_reset(stream, reset_handler_stringbuf);
  svn_stream_set_mark(stream, mark_handler_stringbuf);
  svn_stream_set_seek(stream, seek_handler_stringbuf);
  return stream;
}

struct string_stream_baton
{
  const svn_string_t *str;
  apr_size_t amt_read;
};

static svn_error_t *
read_handler_string(void *baton, char *buffer, apr_size_t *len)
{
  struct string_stream_baton *btn = baton;
  apr_size_t left_to_read = btn->str->len - btn->amt_read;

  *len = (*len > left_to_read) ? left_to_read : *len;
  memcpy(buffer, btn->str->data + btn->amt_read, *len);
  btn->amt_read += *len;
  return SVN_NO_ERROR;
}

svn_stream_t *
svn_stream_from_string(const svn_string_t *str,
                       apr_pool_t *pool)
{
  svn_stream_t *stream;
  struct string_stream_baton *baton;

  if (! str)
    return svn_stream_empty(pool);

  baton = apr_palloc(pool, sizeof(*baton));
  baton->str = str;
  baton->amt_read = 0;
  stream = svn_stream_create(baton, pool);
  svn_stream_set_read(stream, read_handler_string);
  return stream;
}


svn_error_t *
svn_stream_for_stdout(svn_stream_t **out, apr_pool_t *pool)
{
  apr_file_t *stdout_file;
  apr_status_t apr_err;

  apr_err = apr_file_open_stdout(&stdout_file, pool);
  if (apr_err)
    return svn_error_wrap_apr(apr_err, "Can't open stdout");

  *out = svn_stream_from_aprfile2(stdout_file, TRUE, pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_string_from_stream(svn_string_t **result,
                       svn_stream_t *stream,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *work = svn_stringbuf_create_ensure(SVN__STREAM_CHUNK_SIZE,
                                                      result_pool);
  char *buffer = apr_palloc(scratch_pool, SVN__STREAM_CHUNK_SIZE);

  while (1)
    {
      apr_size_t len = SVN__STREAM_CHUNK_SIZE;

      SVN_ERR(svn_stream_read(stream, buffer, &len));
      svn_stringbuf_appendbytes(work, buffer, len);

      if (len < SVN__STREAM_CHUNK_SIZE)
        break;
    }

  SVN_ERR(svn_stream_close(stream));

  *result = apr_palloc(result_pool, sizeof(**result));
  (*result)->data = work->data;
  (*result)->len = work->len;

  return SVN_NO_ERROR;
}
