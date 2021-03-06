/*
 * Regression tests for the diff/diff3 library -- parsing unidiffs
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


#include "../svn_test.h"

#include "svn_diff.h"
#include "svn_pools.h"
#include "svn_utf.h"

/* Used to terminate lines in large multi-line string literals. */
#define NL APR_EOL_STR

static const char *unidiff =
  "Index: A/mu (deleted)"                                               NL
  "===================================================================" NL
  "Index: A/C/gamma"                                                    NL
  "===================================================================" NL
  "--- A/C/gamma\t(revision 2)"                                         NL
  "+++ A/C/gamma\t(working copy)"                                       NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'gamma'."                                          NL
  "+some more bytes to 'gamma'"                                         NL
  "Index: A/D/gamma"                                                    NL
  "===================================================================" NL
  "--- A/D/gamma.orig"                                                  NL
  "+++ A/D/gamma"                                                       NL
  "@@ -1,2 +1 @@"                                                       NL
  " This is the file 'gamma'."                                          NL
  "-some less bytes to 'gamma'"                                         NL
  ""                                                                    NL
  "Property changes on: mu-ng"                                          NL
  "___________________________________________________________________" NL
  "Name: newprop"                                                       NL
  "   + newpropval"                                                     NL
  "Name: svn:mergeinfo"                                                 NL
  ""                                                                    NL;

static svn_error_t *
test_parse_unidiff(apr_pool_t *pool)
{
  apr_file_t *patch_file;
  apr_status_t status;
  apr_size_t len;
  const char *fname = "test_parse_unidiff.patch";
  svn_boolean_t reverse;
  svn_boolean_t ignore_whitespace;
  int i;
  apr_pool_t *iterpool;

  /* Create a patch file. */
  status = apr_file_open(&patch_file, fname,
                        (APR_READ | APR_WRITE | APR_CREATE | APR_TRUNCATE |
                         APR_DELONCLOSE), APR_OS_DEFAULT, pool);
  if (status != APR_SUCCESS)
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL, "Cannot open '%s'",
                             fname);
  len = strlen(unidiff);
  status = apr_file_write_full(patch_file, unidiff, len, &len);
  if (status || len != strlen(unidiff))
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                             "Cannot write to '%s'", fname);

  reverse = FALSE;
  ignore_whitespace = FALSE;
  iterpool = svn_pool_create(pool);
  for (i = 0; i < 2; i++)
    {
      svn_patch_t *patch;
      svn_hunk_t *hunk;
      svn_stringbuf_t *buf;
      svn_boolean_t eof;
      apr_off_t pos;
      svn_stream_t *original_text;
      svn_stream_t *modified_text;

      svn_pool_clear(iterpool);

      /* Reset file pointer. */
      pos = 0;
      SVN_ERR(svn_io_file_seek(patch_file, APR_SET, &pos, iterpool));

      /* We have two patches with one hunk each.
       * Parse the first patch. */
      SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, reverse,
                                        ignore_whitespace, iterpool, 
                                        iterpool));
      SVN_ERR_ASSERT(patch);
      SVN_ERR_ASSERT(! strcmp(patch->old_filename, "A/C/gamma"));
      SVN_ERR_ASSERT(! strcmp(patch->new_filename, "A/C/gamma"));
      SVN_ERR_ASSERT(patch->hunks->nelts == 1);

      hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);
      if (reverse)
        {
          /* Hunk texts come out of the parser inverted,
           * so this inverts them a second time. */
          original_text = hunk->modified_text;
          modified_text = hunk->original_text;
        }
      else
        {
          original_text = hunk->original_text;
          modified_text = hunk->modified_text;
        }

      /* Make sure original text was parsed correctly. */
      SVN_ERR(svn_stream_readline(original_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "This is the file 'gamma'."));
      /* Now we should get EOF. */
      SVN_ERR(svn_stream_readline(original_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(eof);
      SVN_ERR_ASSERT(buf->len == 0);

      /* Make sure modified text was parsed correctly. */
      SVN_ERR(svn_stream_readline(modified_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "This is the file 'gamma'."));
      SVN_ERR(svn_stream_readline(modified_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "some more bytes to 'gamma'"));
      /* Now we should get EOF. */
      SVN_ERR(svn_stream_readline(modified_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(eof);
      SVN_ERR_ASSERT(buf->len == 0);

      /* Parse the second patch. */
      SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, reverse, 
                                        ignore_whitespace, pool, pool));
      SVN_ERR_ASSERT(patch);
      if (reverse)
        {
          SVN_ERR_ASSERT(! strcmp(patch->new_filename, "A/D/gamma.orig"));
          SVN_ERR_ASSERT(! strcmp(patch->old_filename, "A/D/gamma"));
        }
      else
        {
          SVN_ERR_ASSERT(! strcmp(patch->old_filename, "A/D/gamma.orig"));
          SVN_ERR_ASSERT(! strcmp(patch->new_filename, "A/D/gamma"));
        }
      SVN_ERR_ASSERT(patch->hunks->nelts == 1);

      hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);
      if (reverse)
        {
          /* Hunk texts come out of the parser inverted,
           * so this inverts them a second time. */
          original_text = hunk->modified_text;
          modified_text = hunk->original_text;
        }
      else
        {
          original_text = hunk->original_text;
          modified_text = hunk->modified_text;
        }

      /* Make sure original text was parsed correctly. */
      SVN_ERR(svn_stream_readline(original_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "This is the file 'gamma'."));
      SVN_ERR(svn_stream_readline(original_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "some less bytes to 'gamma'"));
      /* Now we should get EOF. */
      SVN_ERR(svn_stream_readline(original_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(eof);
      SVN_ERR_ASSERT(buf->len == 0);

      /* Make sure modified text was parsed correctly. */
      SVN_ERR(svn_stream_readline(modified_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(! eof);
      SVN_ERR_ASSERT(! strcmp(buf->data, "This is the file 'gamma'."));
      /* Now we should get EOF. */
      SVN_ERR(svn_stream_readline(modified_text, &buf, NL, &eof, pool));
      SVN_ERR_ASSERT(eof);
      SVN_ERR_ASSERT(buf->len == 0);

      reverse = !reverse;
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* ========================================================================== */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_parse_unidiff,
                   "test unidiff parsing"),
    SVN_TEST_NULL
  };
