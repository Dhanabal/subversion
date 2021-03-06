/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_eol_private.h
 * @brief Subversion's EOL functions - Internal routines
 */

#ifndef SVN_EOL_PRIVATE_H
#define SVN_EOL_PRIVATE_H

#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_error.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Generic EOL character helper routines */

/* Look for the start of an end-of-line sequence (i.e. CR or LF)
 * in the array pointed to by @a buf , of length @a len.
 * If such a byte is found, return the pointer to it, else return NULL.
 *
 * @since New in 1.7
 */
char *
svn_eol__find_eol_start(char *buf, apr_size_t len);

/* Return the first eol marker found in [@a buf, @a endp) as a
 * NUL-terminated string, or NULL if no eol marker is found.
 *
 * If the last valid character of @a buf is the first byte of a
 * potentially two-byte eol sequence, just return that single-character
 * sequence, that is, assume @a buf represents a CR-only or LF-only file.
 * This is correct for callers that pass an entire file at once, and is
 * no more likely to be incorrect than correct for any caller that
 * doesn't.
 *
 * @since New in 1.7
 */
const char *
svn_eol__detect_eol(char *buf, char *endp);

/* Detect the EOL marker used in @a file and return it in @a *eol.
 * If it cannot be detected, set @a *eol to NULL.
 *
 * The file is searched starting at the current file cursor position.
 * The first EOL marker found will be returnd. So if the file has
 * inconsistent EOL markers, this won't be detected.
 *
 * Upon return, the original file cursor position is always preserved,
 * even if an error is thrown.
 *
 * Do temporary allocations in @a pool.
 *
 * @since New in 1.7 */
svn_error_t *
svn_eol__detect_file_eol(const char **eol, apr_file_t *file, apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_EOL_PRIVATE_H */
