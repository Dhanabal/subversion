/*
 * adm_files.h :  handles locations inside the wc adm area
 *                (This should be the only code that actually knows
 *                *where* things are in .svn/.  If you can't get to
 *                something via these interfaces, something's wrong.)
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


#ifndef SVN_LIBSVN_WC_ADM_FILES_H
#define SVN_LIBSVN_WC_ADM_FILES_H

#include <apr_pools.h>
#include "svn_types.h"

#include "props.h"
#include "wc_db.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/* Return a path to CHILD in the administrative area of PATH. If CHILD is
   NULL, then the path to the admin area is returned. The result is
   allocated in RESULT_POOL. */
const char *svn_wc__adm_child(const char *path,
                              const char *child,
                              apr_pool_t *result_pool);

/* Return TRUE if the administrative area exists for this directory. */
svn_boolean_t svn_wc__adm_area_exists(const char *adm_abspath,
                                      apr_pool_t *pool);


#ifndef SVN_EXPERIMENTAL_PRISTINE
/* Atomically rename a temporary text-base file TMP_TEXT_BASE_ABSPATH to its
   canonical location.  LOCAL_ABSPATH in DB is the working file whose
   text-base is to be moved.  The tmp file should be closed already. */
svn_error_t *
svn_wc__sync_text_base(svn_wc__db_t *db,
                       const char *local_abspath,
                       const char *tmp_text_base_path,
                       apr_pool_t *scratch_pool);
#endif


#ifndef SVN_EXPERIMENTAL_PRISTINE
/* Set *RESULT_ABSPATH to the absolute path to where LOCAL_ABSPATH's
   "normal text-base" file is or should be created.  The file does not
   necessarily exist.

   "Normal text-base" means the base of the copied file, if copied or moved,
   else nothing if it's a simple add (even if replacing an existing node),
   else the ultimate base. */
svn_error_t *
svn_wc__text_base_path(const char **result_abspath,
                       svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *pool);
#endif

/* Set *RESULT_ABSPATH to the deterministic absolute path to where
   LOCAL_ABSPATH's temporary text-base file is or should be created. */
svn_error_t *
svn_wc__text_base_deterministic_tmp_path(const char **result_abspath,
                                         svn_wc__db_t *db,
                                         const char *local_abspath,
                                         apr_pool_t *pool);

/* Set *CONTENTS to a readonly stream on the pristine text of the working
 * version of the file LOCAL_ABSPATH in DB.  If the file is locally copied
 * or moved to this path, this means the pristine text of the copy source,
 * even if the file replaces a previously existing base node at this path.
 *
 * Set *CONTENTS to NULL if there is no pristine text because the file is
 * locally added (even if it replaces an existing base node).  Return an
 * error if there is no pristine text for any other reason.
 *
 * For more detail, see the description of svn_wc_get_pristine_contents2().
 */
svn_error_t *
svn_wc__get_pristine_contents(svn_stream_t **contents,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool);


/* Set *CONTENTS to a readonly stream on the pristine text of the base
 * version of LOCAL_ABSPATH in DB.  If LOCAL_ABSPATH is locally replaced,
 * this is distinct from svn_wc__get_pristine_contents(), otherwise it is
 * the same.
 *
 * (In WC-1 terminology, this was known as "the revert base" if the node is
 * replaced by a copy, otherwise simply as "the base".)
 *
 * If the base version of LOCAL_ABSPATH is not present (e.g. because the
 * file is locally added), set *CONTENTS to NULL.
 * The base version of LOCAL_ABSPATH must be a file. */
svn_error_t *
svn_wc__get_ultimate_base_contents(svn_stream_t **contents,
                                   svn_wc__db_t *db,
                                   const char *local_abspath,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool);


#ifndef SVN_EXPERIMENTAL_PRISTINE
/* Set *RESULT_ABSPATH to the absolute path to LOCAL_ABSPATH's revert file. */
svn_error_t *
svn_wc__text_revert_path(const char **result_abspath,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *pool);
#endif

/* Set *PROP_PATH to PATH's PROPS_KIND properties file.
   PATH can be a directory or file, and even have changed w.r.t. the
   working copy's adm knowledge. Valid values for NODE_KIND are svn_node_dir
   and svn_node_file. */
svn_error_t *svn_wc__prop_path(const char **prop_path,
                               const char *path,
                               svn_wc__db_kind_t node_kind,
                               svn_wc__props_kind_t props_kind,
                               apr_pool_t *pool);

/* Set *RESULT_ABSPATH to the absolute path to a readable file containing
   the WC-1 "normal text-base" of LOCAL_ABSPATH in DB.

   "Normal text-base" means the same as in svn_wc__text_base_path().
   ### May want to check the callers' exact requirements and replace this
       definition with something easier to comprehend.

   What the callers want:
     A path to a file that will remain available and unchanged as long as
     the caller wants it - such as for the lifetime of RESULT_POOL.

   What the current implementation provides:
     A path to the file in the pristine store.  This file will be removed or
     replaced the next time this or another Subversion client updates the WC.

   If the node LOCAL_ABSPATH has no such pristine text, return an error of
   type SVN_ERR_WC_PATH_UNEXPECTED_STATUS.

   Allocate *RESULT_PATH in RESULT_POOL.  */
svn_error_t *
svn_wc__text_base_path_to_read(const char **result_abspath,
                               svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool);

/* Set *FINFO to the status of the pristine text of LOCAL_ABSPATH in DB.
   Only the following fields are guaranteed to be set:
     APR_FINFO_TYPE
     APR_FINFO_SIZE
     APR_FINFO_MTIME
 */
svn_error_t *
svn_wc__get_pristine_text_status(apr_finfo_t *finfo,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool);

#ifndef SVN_EXPERIMENTAL_PRISTINE
/* Set *RESULT_ABSPATH to the path of the WC-1 "revert-base" text of the
   versioned file LOCAL_ABSPATH in DB.

   If the node LOCAL_ABSPATH has no such pristine text, return an error of
   type SVN_ERR_WC_PATH_UNEXPECTED_STATUS.  */
svn_error_t *
svn_wc__text_revert_path_to_read(const char **result_abspath,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool);
#endif

/* Set *RESULT_ABSPATH to the path of the ultimate base text of the
   versioned file LOCAL_ABSPATH in DB.  In WC-1 terms this means the
   "normal text-base" or, if the node is replaced by a copy or move, the
   "revert-base".  */
svn_error_t *
svn_wc__ultimate_base_text_path(const char **result_abspath,
                                svn_wc__db_t *db,
                                const char *local_abspath,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool);

/* Set *RESULT_ABSPATH to the path of the ultimate base text of the
   versioned file LOCAL_ABSPATH in DB.  In WC-1 terms this means the
   "normal text-base" or, if the node is replaced by a copy or move, the
   "revert-base".

   If the node LOCAL_ABSPATH has no such pristine text, return an error of
   type SVN_ERR_WC_PATH_UNEXPECTED_STATUS.  */
svn_error_t *
svn_wc__ultimate_base_text_path_to_read(const char **result_abspath,
                                        svn_wc__db_t *db,
                                        const char *local_abspath,
                                        apr_pool_t *result_pool,
                                        apr_pool_t *scratch_pool);

/* Set *MD5_CHECKSUM to the MD-5 checksum of the BASE_NODE pristine text
 * of LOCAL_ABSPATH in DB, or to NULL if it has no BASE_NODE.
 * Allocate *MD5_CHECKSUM in RESULT_POOL. */
svn_error_t *
svn_wc__get_ultimate_base_md5_checksum(const svn_checksum_t **md5_checksum,
                                       svn_wc__db_t *db,
                                       const char *local_abspath,
                                       apr_pool_t *result_pool,
                                       apr_pool_t *scratch_pool);



/*** Opening all kinds of adm files ***/

/* Open `PATH/<adminstrative_subdir>/FNAME'. */
svn_error_t *svn_wc__open_adm_stream(svn_stream_t **stream,
                                     const char *dir_abspath,
                                     const char *fname,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool);


/* Open a writable stream to a temporary (normal or revert) text base,
   associated with the versioned file LOCAL_ABSPATH in DB.  Set *STREAM to
   the opened stream and *TEMP_BASE_ABSPATH to the path to the temporary
   file.  The temporary file will have an arbitrary unique name, in contrast
   to the deterministic name that svn_wc__text_base_deterministic_tmp_path()
   returns.

   Arrange that, on stream closure, *MD5_CHECKSUM and *SHA1_CHECKSUM will be
   set to the MD-5 and SHA-1 checksums respectively of that file.
   MD5_CHECKSUM and/or SHA1_CHECKSUM may be NULL if not wanted.

   Allocate the new stream, path and checksums in RESULT_POOL.
 */
svn_error_t *
svn_wc__open_writable_base(svn_stream_t **stream,
                           const char **temp_base_abspath,
                           svn_checksum_t **md5_checksum,
                           svn_checksum_t **sha1_checksum,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);


/* Blow away the admistrative directory associated with DIR_ABSPATH */
svn_error_t *svn_wc__adm_destroy(svn_wc__db_t *db,
                                 const char *dir_abspath,
                                 apr_pool_t *scratch_pool);


/* Cleanup the temporary storage area of the administrative
   directory (assuming temp and admin areas exist). */
svn_error_t *
svn_wc__adm_cleanup_tmp_area(svn_wc__db_t *db,
                             const char *adm_abspath,
                             apr_pool_t *scratch_pool);


/* Return a path where nothing exists on disk, within the admin directory
   belonging to the versioned directory ADM_ABSPATH in DB. */
const char *
svn_wc__nonexistent_path(svn_wc__db_t *db,
                         const char *adm_abspath,
                         apr_pool_t *scratch_pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_ADM_FILES_H */
