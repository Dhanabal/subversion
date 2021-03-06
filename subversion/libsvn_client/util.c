/*
 * util.c :  utility functions for the libsvn_client library
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

#include <apr_pools.h>
#include <apr_strings.h>

#include "svn_pools.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_types.h"
#include "svn_opt.h"
#include "svn_props.h"
#include "svn_path.h"
#include "svn_wc.h"
#include "svn_client.h"

#include "private/svn_wc_private.h"

#include "client.h"

#include "svn_private_config.h"

/* Duplicate a HASH containing (char * -> svn_string_t *) key/value
   pairs using POOL. */
static apr_hash_t *
string_hash_dup(apr_hash_t *hash, apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  apr_hash_t *new_hash = apr_hash_make(pool);

  for (hi = apr_hash_first(pool, hash); hi; hi = apr_hash_next(hi))
    {
      const char *key = apr_pstrdup(pool, svn__apr_hash_index_key(hi));
      apr_ssize_t klen = svn__apr_hash_index_klen(hi);
      svn_string_t *val = svn_string_dup(svn__apr_hash_index_val(hi), pool);

      apr_hash_set(new_hash, key, klen, val);
    }
  return new_hash;
}

svn_client_commit_item3_t *
svn_client_commit_item3_create(apr_pool_t *pool)
{
  return apr_pcalloc(pool, sizeof(svn_client_commit_item3_t));
}

svn_error_t *
svn_client_commit_item_create(const svn_client_commit_item3_t **item,
                              apr_pool_t *pool)
{
  *item = svn_client_commit_item3_create(pool);
  return SVN_NO_ERROR;
}

svn_client_commit_item3_t *
svn_client_commit_item3_dup(const svn_client_commit_item3_t *item,
                            apr_pool_t *pool)
{
  svn_client_commit_item3_t *new_item = apr_palloc(pool, sizeof(*new_item));

  *new_item = *item;

  if (new_item->path)
    new_item->path = apr_pstrdup(pool, new_item->path);

  if (new_item->url)
    new_item->url = apr_pstrdup(pool, new_item->url);

  if (new_item->copyfrom_url)
    new_item->copyfrom_url = apr_pstrdup(pool, new_item->copyfrom_url);

  if (new_item->incoming_prop_changes)
    new_item->incoming_prop_changes =
      svn_prop_array_dup(new_item->incoming_prop_changes, pool);

  if (new_item->outgoing_prop_changes)
    new_item->outgoing_prop_changes =
      svn_prop_array_dup(new_item->outgoing_prop_changes, pool);

  return new_item;
}

svn_client_commit_item2_t *
svn_client_commit_item2_dup(const svn_client_commit_item2_t *item,
                            apr_pool_t *pool)
{
  svn_client_commit_item2_t *new_item = apr_palloc(pool, sizeof(*new_item));

  *new_item = *item;

  if (new_item->path)
    new_item->path = apr_pstrdup(pool, new_item->path);

  if (new_item->url)
    new_item->url = apr_pstrdup(pool, new_item->url);

  if (new_item->copyfrom_url)
    new_item->copyfrom_url = apr_pstrdup(pool, new_item->copyfrom_url);

  if (new_item->wcprop_changes)
    new_item->wcprop_changes = svn_prop_array_dup(new_item->wcprop_changes,
                                                  pool);

  return new_item;
}

svn_client_proplist_item_t *
svn_client_proplist_item_dup(const svn_client_proplist_item_t *item,
                             apr_pool_t * pool)
{
  svn_client_proplist_item_t *new_item = apr_pcalloc(pool, sizeof(*new_item));

  if (item->node_name)
    new_item->node_name = svn_stringbuf_dup(item->node_name, pool);

  if (item->prop_hash)
    new_item->prop_hash = string_hash_dup(item->prop_hash, pool);

  return new_item;
}

/* Return LOCAL_ABSPATH's URL and repository root in *URL and REPOS_ROOT,
   respectively.  */
static svn_error_t *
wc_path_to_repos_urls(const char **url,
                      const char **repos_root,
                      svn_wc_context_t *wc_ctx,
                      const char *local_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_client__entry_location(url, NULL, wc_ctx, local_abspath,
                                     svn_opt_revision_unspecified,
                                     result_pool, scratch_pool));

  /* If we weren't provided a REPOS_ROOT, we'll try to read one from
     the entry.  The entry might not hold a URL -- in that case, we'll
     need a fallback plan. */
  if (*repos_root == NULL)
    SVN_ERR(svn_wc__node_get_repos_info(repos_root, NULL, wc_ctx,
                                        local_abspath, TRUE, FALSE,
                                        result_pool, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__path_relative_to_root(const char **rel_path,
                                  svn_wc_context_t *wc_ctx,
                                  const char *abspath_or_url,
                                  const char *repos_root,
                                  svn_boolean_t include_leading_slash,
                                  svn_ra_session_t *ra_session,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  /* ### TODO: Rework this to use svn_ra_get_path_relative_to_root(). */

  SVN_ERR_ASSERT(repos_root != NULL || ra_session != NULL);

  /* If we have a WC path... */
  if (! svn_path_is_url(abspath_or_url))
    {
      /* ...fetch its entry, and attempt to get both its full URL and
         repository root URL.  If we can't get REPOS_ROOT from the WC
         entry, we'll get it from the RA layer.*/
      SVN_ERR(wc_path_to_repos_urls(&abspath_or_url, &repos_root, wc_ctx,
                                    abspath_or_url, scratch_pool,
                                    scratch_pool));
    }

  /* If we weren't provided a REPOS_ROOT, or couldn't find one in the
     WC entry, we'll ask the RA layer.  */
  if (repos_root == NULL)
    SVN_ERR(svn_ra_get_repos_root2(ra_session, &repos_root, scratch_pool));

  /* Check if ABSPATH_OR_URL *is* the repository root URL.  */
  if (strcmp(repos_root, abspath_or_url) == 0)
    {
      *rel_path = include_leading_slash ? "/" : "";
    }
  else
    {
      /* See if PATH_OR_URL is a child of REPOS_ROOT.  If we get NULL
         back from this, the two URLs have no commonality (which
         should only happen if our caller provided us a REPOS_ROOT and
         a PATH_OR_URL of something not in that repository).  */
      const char *rel_url = svn_uri_is_child(repos_root, abspath_or_url,
                                             scratch_pool);
      if (! rel_url)
        {
          return svn_error_createf(SVN_ERR_CLIENT_UNRELATED_RESOURCES, NULL,
                                   _("URL '%s' is not a child of repository "
                                     "root URL '%s'"),
                                   abspath_or_url, repos_root);
        }
      rel_url = svn_path_uri_decode(rel_url, result_pool);
      *rel_path = include_leading_slash
                    ? apr_pstrcat(result_pool, "/", rel_url, NULL) : rel_url;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_repos_root(const char **repos_root,
                           const char *abspath_or_url,
                           const svn_opt_revision_t *peg_revision,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_revnum_t rev;
  const char *target_url;

  /* If PATH_OR_URL is a local path and PEG_REVISION keeps us looking
     locally, we'll first check PATH_OR_URL's entry for a repository
     root URL. */
  if (!svn_path_is_url(abspath_or_url)
      && (peg_revision->kind == svn_opt_revision_working
          || peg_revision->kind == svn_opt_revision_base))
    {
      *repos_root = NULL;
      SVN_ERR(wc_path_to_repos_urls(&abspath_or_url, repos_root,
                                    ctx->wc_ctx, abspath_or_url,
                                    result_pool, scratch_pool));
    }
  else
    {
      *repos_root = NULL;
    }

  /* If PATH_OR_URL was a URL, or PEG_REVISION wasn't a client-side
     revision, or we weren't otherwise able to find the repository
     root URL in PATH_OR_URL's WC entry, we use the RA layer to look
     it up. */
  if (*repos_root == NULL)
    {
      svn_ra_session_t *ra_session;
      SVN_ERR(svn_client__ra_session_from_path(&ra_session,
                                               &rev,
                                               &target_url,
                                               abspath_or_url,
                                               NULL,
                                               peg_revision,
                                               peg_revision,
                                               ctx,
                                               scratch_pool));

      SVN_ERR(svn_ra_get_repos_root2(ra_session, repos_root, result_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__default_walker_error_handler(const char *path,
                                         svn_error_t *err,
                                         void *walk_baton,
                                         apr_pool_t *pool)
{
  return svn_error_return(err);
}


const svn_opt_revision_t *
svn_cl__rev_default_to_head_or_base(const svn_opt_revision_t *revision,
                                    const char *path_or_url)
{
  static svn_opt_revision_t head_rev = { svn_opt_revision_head, { 0 } };
  static svn_opt_revision_t base_rev = { svn_opt_revision_base, { 0 } };

  if (revision->kind == svn_opt_revision_unspecified)
    return svn_path_is_url(path_or_url) ? &head_rev : &base_rev;
  return revision;
}

const svn_opt_revision_t *
svn_cl__rev_default_to_head_or_working(const svn_opt_revision_t *revision,
                                       const char *path_or_url)
{
  static svn_opt_revision_t head_rev = { svn_opt_revision_head, { 0 } };
  static svn_opt_revision_t work_rev = { svn_opt_revision_working, { 0 } };

  if (revision->kind == svn_opt_revision_unspecified)
    return svn_path_is_url(path_or_url) ? &head_rev : &work_rev;
  return revision;
}

const svn_opt_revision_t *
svn_cl__rev_default_to_peg(const svn_opt_revision_t *revision,
                           const svn_opt_revision_t *peg_revision)
{
  if (revision->kind == svn_opt_revision_unspecified)
    return peg_revision;
  return revision;
}
