/*
 * log.h :  interfaces for running .svn/log files.
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


#ifndef SVN_LIBSVN_WC_LOG_H
#define SVN_LIBSVN_WC_LOG_H

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_wc.h"

#include "wc_db.h"
#include "private/svn_skel.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Each path argument to the svn_wc__loggy_* functions in this section can
   be either absolute or relative to the adm_abspath argument.
*/


/* Insert into DB a work queue instruction to generate a translated
   file from SRC to DST with translation settings from VERSIONED.
   ADM_ABSPATH is the absolute path for the admin directory for PATH.
   DST and SRC and VERSIONED are relative to ADM_ABSPATH.  */
svn_error_t *
svn_wc__loggy_translated_file(svn_skel_t **work_item,
                              svn_wc__db_t *db,
                              const char *adm_abspath,
                              const char *dst_abspath,
                              const char *src_abspath,
                              const char *versioned_abspath,
                              apr_pool_t *result_pool);

/* Insert into DB a work queue instruction to delete the entry
   associated with PATH from the entries file.
   ADM_ABSPATH is the absolute path for the access baton for PATH.

   ### REVISION and KIND

   Use SCRATCH_POOL for temporary allocations.
*/
svn_error_t *
svn_wc__loggy_delete_entry(svn_skel_t **work_item,
                           svn_wc__db_t *db,
                           const char *adm_abspath,
                           const char *local_abspath,
                           svn_revnum_t revision,
                           svn_wc__db_kind_t kind,
                           apr_pool_t *result_pool);


/* Insert into DB a work queue instruction to delete lock related
   fields from the entry belonging to PATH.
   ADM_ABSPATH is the absolute path for the access baton for PATH.

   Use SCRATCH_POOL for temporary allocations.
*/
svn_error_t *
svn_wc__loggy_delete_lock(svn_skel_t **work_item,
                          svn_wc__db_t *db,
                          const char *adm_abspath,
                          const char *local_abspath,
                          apr_pool_t *result_pool);


/* Queue operations to modify the entry associated with PATH
   in ADM_ABSPATH according to the flags specified in MODIFY_FLAGS, based on
   the values supplied in *ENTRY.

   ADM_ABSPATH is the absolute path for the admin directory for PATH.

   The flags in MODIFY_FLAGS are to be taken from the svn_wc__entry_modify()
   parameter by the same name.

   Use SCRATCH_POOL for temporary allocations.
*/
svn_error_t *
svn_wc__loggy_entry_modify(svn_skel_t **work_item,
                           svn_wc__db_t *db,
                           const char *adm_abspath,
                           const char *local_abspath,
                           const svn_wc_entry_t *entry,
                           apr_uint64_t modify_flags,
                           apr_pool_t *result_pool);


/* Queue instructions to move the file SRC_PATH to DST_PATH.

   The test for existence is made now, not at log run time.

   ADM_ABSPATH is the absolute path for the admin directory for PATH.
   SRC_PATH and DST_PATH are relative to ADM_ABSPATH.

   Set *DST_MODIFIED (if DST_MODIFIED isn't NULL) to indicate whether the
   destination path will have been modified after running the log: if either
   the move or the remove will have been carried out.
*/
svn_error_t *
svn_wc__loggy_move(svn_skel_t **work_item,
                   svn_wc__db_t *db,
                   const char *adm_abspath,
                   const char *src_abspath,
                   const char *dst_abspath,
                   apr_pool_t *result_pool);


/* Queue instructions to set the timestamp of PATH to
   the time TIMESTR.

   ADM_ABSPATH is the absolute path for the admin directory for PATH.
*/
svn_error_t *
svn_wc__loggy_set_timestamp(svn_skel_t **work_item,
                            svn_wc__db_t *db,
                            const char *adm_abspath,
                            const char *local_abspath,
                            const char *timestr,
                            apr_pool_t *result_pool);

/* */
svn_error_t *
svn_wc__loggy_add_tree_conflict(svn_skel_t **work_item,
                                svn_wc__db_t *db,
                                const char *adm_abspath,
                                const svn_wc_conflict_description2_t *conflict,
                                apr_pool_t *result_pool);


/* TODO ###

   Use SCRATCH_POOL for temporary allocations.
 */
svn_error_t *
svn_wc__run_xml_log(svn_wc__db_t *db,
                    const char *adm_abspath,
                    const char *log_contents,
                    apr_size_t log_len,
                    apr_pool_t *scratch_pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_LOG_H */
