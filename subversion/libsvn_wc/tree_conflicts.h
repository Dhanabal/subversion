/*
 * tree_conflicts.h: declarations related to tree conflicts
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

#ifndef SVN_LIBSVN_WC_TREE_CONFLICTS_H
#define SVN_LIBSVN_WC_TREE_CONFLICTS_H

#include <apr_pools.h>
#include <apr_tables.h>

#include "svn_string.h"
#include "svn_wc.h"

#include "private/svn_token.h"
#include "private/svn_skel.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*
 * See the notes/tree-conflicts/ directory for more information
 * about tree conflicts in general.
 *
 * A given directory may contain potentially many tree conflicts.
 * Each tree conflict is identified by the path of the file
 * or directory (both a.k.a node) that it affects.
 * We call this file or directory the "victim" of the tree conflict.
 *
 * For example, a file that is deleted by an update but locally
 * modified by the user is a victim of a tree conflict.
 *
 * For now, tree conflict victims are always direct children of the
 * directory in which the tree conflict is recorded.
 * This may change once the way Subversion handles adm areas changes.
 *
 * If a directory has tree conflicts, the "tree-conflict-data" field
 * in the entry for the directory contains one or more tree conflict
 * descriptions stored using the "skel" format.
 */


svn_error_t *
svn_wc__serialize_conflict(svn_skel_t **skel,
                           const svn_wc_conflict_description2_t *conflict,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);


svn_error_t *
svn_wc__deserialize_conflict(const svn_wc_conflict_description2_t **conflict,
                             const svn_skel_t *skel,
                             const char *dir_path,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool);

/*
 * Encode tree conflict descriptions into a single string.
 *
 * Set *CONFLICT_DATA to a string, allocated in POOL, that encodes the tree
 * conflicts in CONFLICTS in a form suitable for storage in a single string
 * field in a WC entry. CONFLICTS is a hash of zero or more pointers to
 * svn_wc_conflict_description2_t objects, index by their basenames. All of the
 * conflict victim paths must be siblings.
 *
 * Do all allocations in POOL.
 *
 * @see svn_wc__read_tree_conflicts()
 */
svn_error_t *
svn_wc__write_tree_conflicts(const char **conflict_data,
                             apr_hash_t *conflicts,
                             apr_pool_t *pool);

/*
 * Read tree conflict descriptions from @a conflict_data.  Set @a *conflicts
 * to a hash of pointers to svn_wc_conflict_description2_t objects indexed by
 * svn_wc_conflict_description2_t.local_abspath, all newly allocated in @a
 * pool.  @a dir_path is the path to the working copy directory whose conflicts
 * are being read.  The conflicts read are the tree conflicts on the immediate
 * child nodes of @a dir_path.  Do all allocations in @a pool.
 */
svn_error_t *
svn_wc__read_tree_conflicts(apr_hash_t **conflicts,
                            const char *conflict_data,
                            const char *dir_path,
                            apr_pool_t *pool);

/* Token mapping tables.  */
extern const svn_token_map_t svn_wc__operation_map[];
extern const svn_token_map_t svn_wc__conflict_action_map[];
extern const svn_token_map_t svn_wc__conflict_reason_map[];


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_TREE_CONFLICTS_H */
