/*
 * sync.h :  The synchronization editor for svnsync.
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

#ifndef SYNC_H
#define SYNC_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


#include "svn_types.h"
#include "svn_delta.h"


/* Normalize the line ending style of the values of properties in REV_PROPS
 * that "need translation" (according to svn_prop_needs_translation(),
 * currently all svn:* props) so that they contain only LF (\n) line endings.
 * The number of properties that needed normalization is returned in
 * *NORMALIZED_COUNT.
 */
svn_error_t *
svnsync_normalize_revprops(apr_hash_t *rev_props,
                           int *normalized_count,
                           apr_pool_t *pool);


/* Set WRAPPED_EDITOR and WRAPPED_EDIT_BATON to an editor/baton pair
 * that wraps our own commit EDITOR/EDIT_BATON.  BASE_REVISION is the
 * revision on which the driver of this returned editor will be basing
 * the commit.  TO_URL is the URL of the root of the repository into
 * which the commit is being made.
 *
 * As the sync editor encounters property values, it might see the need to
 * normalize them (to LF line endings). Each carried out normalization adds 1
 * to the *NORMALIZED_NODE_PROPS_COUNTER (for notification).
 */
svn_error_t *
svnsync_get_sync_editor(const svn_delta_editor_t *wrapped_editor,
                        void *wrapped_edit_baton,
                        svn_revnum_t base_revision,
                        const char *to_url,
                        svn_boolean_t quiet,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        int *normalized_node_props_counter,
                        apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SYNC_H */
