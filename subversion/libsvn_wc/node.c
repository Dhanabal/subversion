/*
 * node.c:  routines for getting information about nodes in the working copy.
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

/* A note about these functions:

   We aren't really sure yet which bits of data libsvn_client needs about
   nodes.  In wc-1, we just grab the entry, and then use whatever we want
   from it.  Such a pattern is Bad.

   This file is intended to hold functions which retrieve specific bits of
   information about a node, and will hopefully give us a better idea about
   what data libsvn_client needs, and how to best provide that data in 1.7
   final.  As such, these functions should only be called from outside
   libsvn_wc; any internal callers are encouraged to use the appropriate
   information fetching function, such as svn_wc__db_read_info().
*/

#include <apr_pools.h>
#include <apr_time.h>

#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_types.h"

#include "wc.h"
#include "lock.h"
#include "props.h"
#include "log.h"
#include "entries.h"
#include "wc_db.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"


svn_error_t *
svn_wc__node_get_children(const apr_array_header_t **children,
                          svn_wc_context_t *wc_ctx,
                          const char *dir_abspath,
                          svn_boolean_t show_hidden,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  const apr_array_header_t *rel_children;
  apr_array_header_t *childs;
  int i;

  SVN_ERR(svn_wc__db_read_children(&rel_children, wc_ctx->db, dir_abspath,
                                   scratch_pool, scratch_pool));

  childs = apr_array_make(result_pool, rel_children->nelts,
                             sizeof(const char *));
  for (i = 0; i < rel_children->nelts; i++)
    {
      const char *child_abspath = svn_dirent_join(dir_abspath,
                                                  APR_ARRAY_IDX(rel_children,
                                                                i,
                                                                const char *),
                                                  result_pool);

      /* Don't add hidden nodes to *CHILDREN if we don't want them. */
      if (!show_hidden)
        {
          svn_boolean_t child_is_hidden;

          SVN_ERR(svn_wc__db_node_hidden(&child_is_hidden, wc_ctx->db,
                                         child_abspath, scratch_pool));
          if (child_is_hidden)
            continue;
        }

      APR_ARRAY_PUSH(childs, const char *) = child_abspath;
    }

  *children = childs;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_get_repos_info(const char **repos_root_url,
                            const char **repos_uuid,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            svn_boolean_t scan_added,
                            svn_boolean_t scan_deleted,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_wc__db_status_t status;

  err = svn_wc__db_read_info(&status, NULL, NULL, NULL,
                             repos_root_url, repos_uuid,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             wc_ctx->db, local_abspath, result_pool,
                             scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND
          && err->apr_err != SVN_ERR_WC_NOT_WORKING_COPY)
        return svn_error_return(err);

      /* This node is not versioned. Return NULL repos info.  */
      svn_error_clear(err);

      if (repos_root_url)
        *repos_root_url = NULL;
      if (repos_uuid)
        *repos_uuid = NULL;
      return SVN_NO_ERROR;
    }

  if (scan_added
      && (status == svn_wc__db_status_added
          || status == svn_wc__db_status_obstructed_add))
    {
      /* We have an addition. scan_addition() will find the intended
         repository location by scanning up the tree.  */
      return svn_error_return(svn_wc__db_scan_addition(
                                &status, NULL,
                                NULL, repos_root_url, repos_uuid,
                                NULL, NULL, NULL, NULL,
                                wc_ctx->db, local_abspath,
                                scratch_pool, scratch_pool));
    }

  /* If we didn't get repository information, and the status means we are
     looking at an unchanged BASE node, then scan upwards for repos info.  */
  if (((repos_root_url != NULL && *repos_root_url == NULL)
       || (repos_uuid != NULL && *repos_uuid == NULL))
      && (status == svn_wc__db_status_normal
          || status == svn_wc__db_status_obstructed
          || status == svn_wc__db_status_absent
          || status == svn_wc__db_status_excluded
          || status == svn_wc__db_status_not_present
          || (scan_deleted && (status == svn_wc__db_status_deleted))
          || (scan_deleted && (status == svn_wc__db_status_obstructed_delete))))
    {
      SVN_ERR(svn_wc__db_scan_base_repos(NULL, repos_root_url, repos_uuid,
                                         wc_ctx->db, local_abspath,
                                         result_pool, scratch_pool));
    }
  /* else maybe a deletion, or an addition w/ SCAN_ADDED==FALSE.  */

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_read_kind(svn_node_kind_t *kind,
                 svn_wc_context_t *wc_ctx,
                 const char *abspath,
                 svn_boolean_t show_hidden,
                 apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t db_kind;
  svn_error_t *err;

  err = svn_wc__db_read_kind(&db_kind, wc_ctx->db, abspath, FALSE,
                             scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      *kind = svn_node_none;
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  switch (db_kind)
    {
      case svn_wc__db_kind_file:
        *kind = svn_node_file;
        break;
      case svn_wc__db_kind_dir:
        *kind = svn_node_dir;
        break;
      case svn_wc__db_kind_symlink:
        *kind = svn_node_file;
        break;
      case svn_wc__db_kind_unknown:
        *kind = svn_node_unknown;
        break;
      default:
        SVN_ERR_MALFUNCTION();
    }

  /* Make sure hidden nodes return svn_node_none. */
  if (! show_hidden)
    {
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__db_node_hidden(&hidden, wc_ctx->db, abspath,
                                     scratch_pool));
      if (hidden)
        *kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_get_depth(svn_depth_t *depth,
                       svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL, depth, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL,
                         wc_ctx->db, local_abspath, scratch_pool,
                         scratch_pool));
}

svn_error_t *
svn_wc__node_get_changed_info(svn_revnum_t *changed_rev,
                              apr_time_t *changed_date,
                              const char **changed_author,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, changed_rev,
                         changed_date, changed_author, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL,
                         wc_ctx->db, local_abspath, result_pool,
                         scratch_pool));
}

svn_error_t *
svn_wc__node_get_changelist(const char **changelist,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             changelist,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             wc_ctx->db, local_abspath, result_pool,
                             scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
      *changelist = NULL;
    }

  return svn_error_return(err);
}

svn_error_t *
svn_wc__node_get_base_checksum(const svn_checksum_t **checksum,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__db_read_info(NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, checksum,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               wc_ctx->db, local_abspath,
                                               result_pool, scratch_pool));
}

svn_error_t *
svn_wc__node_get_translated_size(svn_filesize_t *translated_size,
                                 svn_wc_context_t *wc_ctx,
                                 const char *local_abspath,
                                 apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__db_read_info(NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               translated_size,
                                               NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL,
                                               wc_ctx->db, local_abspath,
                                               scratch_pool, scratch_pool));
}

svn_error_t *
svn_wc__internal_node_get_url(const char **url,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  const char *repos_relpath;
  const char *repos_root_url;
  svn_boolean_t base_shadowed;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, NULL, &repos_relpath,
                               &repos_root_url,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               &base_shadowed, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));
  if (repos_relpath == NULL)
    {
      if (status == svn_wc__db_status_normal
          || status == svn_wc__db_status_incomplete
          || (base_shadowed
              && (status == svn_wc__db_status_deleted
                  || status == svn_wc__db_status_obstructed_delete)))
        {
          SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                             NULL,
                                             db, local_abspath,
                                             scratch_pool, scratch_pool));
        }
      else if (status == svn_wc__db_status_added)
        {
          SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, &repos_relpath,
                                           &repos_root_url, NULL, NULL, NULL,
                                           NULL, NULL,
                                           db, local_abspath,
                                           scratch_pool, scratch_pool));
        }
      else if (status == svn_wc__db_status_absent
               || status == svn_wc__db_status_excluded
               || status == svn_wc__db_status_not_present
               || (!base_shadowed
                   && (status == svn_wc__db_status_deleted
                       || status == svn_wc__db_status_obstructed_delete)))
        {
          const char *parent_abspath;

          svn_dirent_split(local_abspath, &parent_abspath, &repos_relpath,
                           scratch_pool);
          SVN_ERR(svn_wc__internal_node_get_url(&repos_root_url, db,
                                                parent_abspath,
                                                scratch_pool, scratch_pool));
        }
      else
        {
          /* Status: obstructed, obstructed_add */
          *url = NULL;
          return SVN_NO_ERROR;
        }
    }

  SVN_ERR_ASSERT(repos_root_url != NULL && repos_relpath != NULL);
  *url = svn_path_url_add_component2(repos_root_url, repos_relpath,
                                     result_pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_get_url(const char **url,
                     svn_wc_context_t *wc_ctx,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__internal_node_get_url(
                            url, wc_ctx->db, local_abspath,
                            result_pool, scratch_pool));
}

svn_error_t *
svn_wc__node_get_copyfrom_info(const char **copyfrom_url,
                               svn_revnum_t *copyfrom_rev,
                               svn_boolean_t *is_copy_target,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  const char *original_root_url;
  const char *original_repos_relpath;
  svn_revnum_t original_revision;
  svn_wc__db_status_t status;

  if (copyfrom_url)
    *copyfrom_url = NULL;
  if (copyfrom_rev)
    *copyfrom_rev = SVN_INVALID_REVNUM;
  if (is_copy_target)
    *is_copy_target = FALSE;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, &original_repos_relpath,
                               &original_root_url, NULL, &original_revision,
                               NULL, NULL, NULL, NULL, NULL, db,
                               local_abspath, scratch_pool, scratch_pool));
  if (original_root_url && original_repos_relpath)
    {
      /* If this was the root of the copy then the URL is immediately
         available... */
      const char *my_copyfrom_url;

      if (copyfrom_url || is_copy_target)
        my_copyfrom_url = svn_path_url_add_component2(original_root_url,
                                                      original_repos_relpath,
                                                      result_pool);

      if (copyfrom_url)
        *copyfrom_url = my_copyfrom_url;

      if (copyfrom_rev)
        *copyfrom_rev = original_revision;

      if (is_copy_target)
        {
          /* ### At this point we'd just set is_copy_target to TRUE, *but* we
           * currently want to model wc-1 behaviour.  Particularly, this
           * affects mixed-revision copies (e.g. wc-wc copy):
           * - Wc-1 saw only the root of a mixed-revision copy as the copy's
           *   root.
           * - Wc-ng returns an explicit original_root_url,
           *   original_repos_relpath pair for each subtree with mismatching
           *   revision.
           * We need to compensate for that: Find out if the parent of
           * this node is also copied and has a matching copy_from URL. If so,
           * nevermind the revision, just like wc-1 did, and say this was not
           * a separate copy target. */
          const char *parent_abspath;
          const char *base_name;
          const char *parent_copyfrom_url;

          svn_dirent_split(local_abspath, &parent_abspath, &base_name,
                           scratch_pool);

          /* This is a copied node, so we should never fall off the top of a
           * working copy here. */
          SVN_ERR(svn_wc__node_get_copyfrom_info(&parent_copyfrom_url,
                                                 NULL, NULL,
                                                 wc_ctx, parent_abspath,
                                                 scratch_pool, scratch_pool));

          /* So, count this as a separate copy target only if the URLs
           * don't match up, or if the parent isn't copied at all. */
          if (parent_copyfrom_url == NULL
              || strcmp(my_copyfrom_url,
                        svn_path_url_add_component2(parent_copyfrom_url,
                                                    base_name,
                                                    scratch_pool)) != 0)
            *is_copy_target = TRUE;
        }
    }
  else if ((status == svn_wc__db_status_added
            || status == svn_wc__db_status_obstructed_add)
           && (copyfrom_rev || copyfrom_url))
    {
      /* ...But if this is merely the descendant of an explicitly
         copied/moved directory, we need to do a bit more work to
         determine copyfrom_url and copyfrom_rev. */
      const char *op_root_abspath;

      SVN_ERR(svn_wc__db_scan_addition(&status, &op_root_abspath, NULL, NULL,
                                       NULL, &original_repos_relpath,
                                       &original_root_url, NULL,
                                       &original_revision, db, local_abspath,
                                       scratch_pool, scratch_pool));
      if (status == svn_wc__db_status_copied ||
          status == svn_wc__db_status_moved_here)
        {
          const char *src_parent_url;
          const char *src_relpath;

          src_parent_url = svn_path_url_add_component2(original_root_url,
                                                       original_repos_relpath,
                                                       scratch_pool);
          src_relpath = svn_dirent_is_child(op_root_abspath, local_abspath,
                                            scratch_pool);
          if (src_relpath)
            {
              if (copyfrom_url)
                *copyfrom_url = svn_path_url_add_component2(src_parent_url,
                                                            src_relpath,
                                                            result_pool);
              if (copyfrom_rev)
                *copyfrom_rev = original_revision;
            }
        }
    }

  return SVN_NO_ERROR;
}


/* A recursive node-walker, helper for svn_wc__node_walk_children(). */
static svn_error_t *
walker_helper(svn_wc__db_t *db,
              const char *dir_abspath,
              svn_boolean_t show_hidden,
              svn_wc__node_found_func_t walk_callback,
              void *walk_baton,
              svn_depth_t depth,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *scratch_pool)
{
  const apr_array_header_t *rel_children;
  apr_pool_t *iterpool;
  int i;

  if (depth == svn_depth_empty)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_read_children(&rel_children, db, dir_abspath,
                                   scratch_pool, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);
  for (i = 0; i < rel_children->nelts; i++)
    {
      const char *child_abspath;
      svn_wc__db_kind_t child_kind;

      svn_pool_clear(iterpool);

      /* See if someone wants to cancel this operation. */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      child_abspath = svn_dirent_join(dir_abspath,
                                      APR_ARRAY_IDX(rel_children, i,
                                                    const char *),
                                      iterpool);

      if (!show_hidden)
        {
          svn_boolean_t hidden;

          SVN_ERR(svn_wc__db_node_hidden(&hidden, db, child_abspath, iterpool));
          if (hidden)
            continue;
        }

      SVN_ERR(svn_wc__db_read_info(NULL, &child_kind, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   NULL,
                                   db, child_abspath, iterpool, iterpool));

      /* Return the child, if appropriate.  (For a directory,
       * this is the first visit: as a child.) */
      if (child_kind == svn_wc__db_kind_file
            || depth >= svn_depth_immediates)
        {
          SVN_ERR(walk_callback(child_abspath, walk_baton, iterpool));
        }

      /* Recurse into this directory, if appropriate. */
      if (child_kind == svn_wc__db_kind_dir
            && depth >= svn_depth_immediates)
        {
          svn_depth_t depth_below_here = depth;

          if (depth == svn_depth_immediates)
            depth_below_here = svn_depth_empty;

          SVN_ERR(walker_helper(db, child_abspath, show_hidden,
                                walk_callback, walk_baton,
                                depth_below_here, cancel_func, cancel_baton,
                                iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__internal_walk_children(svn_wc__db_t *db,
                               const char *local_abspath,
                               svn_boolean_t show_hidden,
                               svn_wc__node_found_func_t walk_callback,
                               void *walk_baton,
                               svn_depth_t walk_depth,
                               svn_cancel_func_t cancel_func,
                               void *cancel_baton,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  svn_depth_t depth;

  SVN_ERR(svn_wc__db_read_info(NULL, &kind, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, &depth, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  if (kind == svn_wc__db_kind_file || depth == svn_depth_exclude)
    {
      return svn_error_return(
        walk_callback(local_abspath, walk_baton, scratch_pool));
    }

  if (kind == svn_wc__db_kind_dir)
    {
      /* Return the directory first, before starting recursion, since it
         won't get returned as part of the recursion. */
      SVN_ERR(walk_callback(local_abspath, walk_baton, scratch_pool));

      return svn_error_return(
        walker_helper(db, local_abspath, show_hidden, walk_callback, walk_baton,
                      walk_depth, cancel_func, cancel_baton, scratch_pool));
    }

  return svn_error_createf(SVN_ERR_NODE_UNKNOWN_KIND, NULL,
                           _("'%s' has an unrecognized node kind"),
                           svn_dirent_local_style(local_abspath,
                                                  scratch_pool));
}

svn_error_t *
svn_wc__node_walk_children(svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           svn_boolean_t show_hidden,
                           svn_wc__node_found_func_t walk_callback,
                           void *walk_baton,
                           svn_depth_t walk_depth,
                           svn_cancel_func_t cancel_func,
                           void *cancel_baton,
                           apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__internal_walk_children(wc_ctx->db, local_abspath, show_hidden,
                                   walk_callback, walk_baton, walk_depth,
                                   cancel_func, cancel_baton, scratch_pool));
}

svn_error_t *
svn_wc__node_is_status_deleted(svn_boolean_t *is_deleted,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));

  *is_deleted = (status == svn_wc__db_status_deleted) ||
                (status == svn_wc__db_status_obstructed_delete);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_obstructed(svn_boolean_t *is_obstructed,
                                  svn_wc_context_t *wc_ctx,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));

  *is_obstructed = (status == svn_wc__db_status_obstructed) ||
                   (status == svn_wc__db_status_obstructed_add) ||
                   (status == svn_wc__db_status_obstructed_delete);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_absent(svn_boolean_t *is_absent,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_absent = (status == svn_wc__db_status_absent);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_present(svn_boolean_t *is_present,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_present = (status != svn_wc__db_status_not_present);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_added(svn_boolean_t *is_added,
                      svn_wc_context_t *wc_ctx,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_added = (status == svn_wc__db_status_added
               || status == svn_wc__db_status_obstructed_add);

  return SVN_NO_ERROR;
}


/* Equivalent to the old notion of "entry->schedule == schedule_replace"  */
svn_error_t *
svn_wc__internal_is_replaced(svn_boolean_t *replaced,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_boolean_t base_shadowed;
  svn_wc__db_status_t base_status;

  SVN_ERR(svn_wc__db_read_info(
            &status, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, &base_shadowed,
            NULL, NULL,
            db, local_abspath,
            scratch_pool, scratch_pool));
  if (base_shadowed)
    SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     db, local_abspath,
                                     scratch_pool, scratch_pool));

  *replaced = ((status == svn_wc__db_status_added
                || status == svn_wc__db_status_obstructed_add)
               && base_shadowed
               && base_status != svn_wc__db_status_not_present);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_is_replaced(svn_boolean_t *replaced,
                         svn_wc_context_t *wc_ctx,
                         const char *local_abspath,
                         apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__internal_is_replaced(replaced, wc_ctx->db,
                                                       local_abspath,
                                                       scratch_pool));
}


svn_error_t *
svn_wc__node_get_base_rev(svn_revnum_t *base_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_boolean_t base_shadowed;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, base_revision,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, &base_shadowed,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));

  if (SVN_IS_VALID_REVNUM(*base_revision))
    return SVN_NO_ERROR;

  if (base_shadowed)
    {
      /* The node was replaced with something else. Look at the base.  */
      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, base_revision,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       wc_ctx->db, local_abspath,
                                       scratch_pool, scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_get_working_rev_info(svn_revnum_t *revision,
                                  svn_revnum_t *changed_rev, 
                                  apr_time_t *changed_date, 
                                  const char **changed_author,
                                  svn_wc_context_t *wc_ctx, 
                                  const char *local_abspath, 
                                  apr_pool_t *scratch_pool,
                                  apr_pool_t *result_pool)
{
  svn_wc__db_status_t status;
  svn_boolean_t base_shadowed;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, revision, NULL, NULL, NULL,
                               changed_rev, changed_date, changed_author,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, &base_shadowed, NULL,
                               NULL, wc_ctx->db, local_abspath, result_pool,
                               scratch_pool));

  if (status == svn_wc__db_status_deleted)
    {
      const char *work_del_abspath = NULL;
      const char *base_del_abspath = NULL;

      SVN_ERR(svn_wc__db_scan_deletion(&base_del_abspath, NULL,
                                       NULL, &work_del_abspath, wc_ctx->db,
                                       local_abspath, scratch_pool,
                                       result_pool));
      if (work_del_abspath)
        {
          SVN_ERR(svn_wc__db_read_info(&status, NULL, revision, NULL, NULL,
                                       NULL, changed_rev, changed_date,
                                       changed_author, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, &base_shadowed,
                                       NULL, NULL, wc_ctx->db, work_del_abspath,
                                       result_pool, scratch_pool));
        }
      else
        {
          SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, revision, NULL,
                                           NULL, NULL, changed_rev,
                                           changed_date, changed_author,
                                           NULL, NULL, NULL, NULL, NULL,
                                           NULL, wc_ctx->db,
                                           base_del_abspath, result_pool,
                                           scratch_pool));
        }
    }
  else if (base_shadowed)
    {
      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, revision, NULL, NULL,
                                       NULL, changed_rev, changed_date,
                                       changed_author, NULL, NULL, NULL,
                                       NULL, NULL, NULL, wc_ctx->db, local_abspath,
                                       result_pool, scratch_pool));
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_get_commit_base_rev(svn_revnum_t *commit_base_revision,
                                 svn_wc_context_t *wc_ctx,
                                 const char *local_abspath,
                                 apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_boolean_t base_shadowed;

  SVN_ERR(svn_wc__db_read_info(&status, NULL,
                               commit_base_revision,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, &base_shadowed, NULL, NULL,
                               wc_ctx->db, local_abspath, scratch_pool,
                               scratch_pool));

  /* If this returned a valid revnum, there is no WORKING node. The node is
     cleanly checked out, no modifications, copies or replaces. */
  if (SVN_IS_VALID_REVNUM(*commit_base_revision))
    return SVN_NO_ERROR;

  if (status == svn_wc__db_status_added)
    {
      /* If the node was copied/moved-here, return the copy/move source
         revision (not this node's base revision). If it's just added,
         return SVN_INVALID_REVNUM. */
      SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, commit_base_revision,
                                       wc_ctx->db, local_abspath,
                                       scratch_pool, scratch_pool));

      if (! SVN_IS_VALID_REVNUM(*commit_base_revision) && base_shadowed)
        /* It is a replace that does not feature a copy/move-here.
           Return the revert-base revision. */
        return svn_error_return(
          svn_wc__node_get_base_rev(commit_base_revision, wc_ctx,
                                    local_abspath, scratch_pool));
    }
  else if (status == svn_wc__db_status_deleted)
    {
      const char *work_del_abspath;
      const char *parent_abspath;
      svn_wc__db_status_t parent_status;

      SVN_ERR(svn_wc__db_scan_deletion(NULL, NULL, NULL,
                                       &work_del_abspath,
                                       wc_ctx->db, local_abspath,
                                       scratch_pool, scratch_pool));
      if (work_del_abspath != NULL)
        {
          /* This is a deletion within a copied subtree. Get the copied-from
           * revision. */
          parent_abspath = svn_dirent_dirname(work_del_abspath, scratch_pool);

          SVN_ERR(svn_wc__db_read_info(&parent_status,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL,
                                       wc_ctx->db, parent_abspath,
                                       scratch_pool, scratch_pool));

          SVN_ERR_ASSERT(parent_status == svn_wc__db_status_added
                         || parent_status == svn_wc__db_status_obstructed_add);

          SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL, NULL, NULL, NULL,
                                           NULL, NULL,
                                           commit_base_revision,
                                           wc_ctx->db, parent_abspath,
                                           scratch_pool, scratch_pool));
        }
      else
        /* This is a normal delete. Get the base revision. */
        return svn_error_return(
          svn_wc__node_get_base_rev(commit_base_revision, wc_ctx,
                                    local_abspath, scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_get_lock_info(const char **lock_token,
                           const char **lock_owner,
                           const char **lock_comment,
                           apr_time_t *lock_date,
                           svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_lock_t *lock;

  SVN_ERR(svn_wc__db_read_info(NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, &lock,
                               wc_ctx->db, local_abspath,
                               result_pool, scratch_pool));
  if (lock_token)
    *lock_token = lock ? lock->token : NULL;
  if (lock_owner)
    *lock_owner = lock ? lock->owner : NULL;
  if (lock_comment)
    *lock_comment = lock ? lock->comment : NULL;
  if (lock_date)
    *lock_date = lock ? lock->date : 0;
      
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__internal_is_file_external(svn_boolean_t *file_external,
                                  svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool)
{
  const char *serialized;

  SVN_ERR(svn_wc__db_temp_get_file_external(&serialized,
                                            db, local_abspath,
                                            scratch_pool, scratch_pool));
  *file_external = (serialized != NULL);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_is_file_external(svn_boolean_t *file_external,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__internal_is_file_external(file_external,
                                                            wc_ctx->db,
                                                            local_abspath,
                                                            scratch_pool));
}

svn_error_t *
svn_wc__node_check_conflicts(svn_boolean_t *prop_conflicted,
                             svn_boolean_t *text_conflicted,
                             svn_boolean_t *tree_conflicted,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  const apr_array_header_t *conflicts;
  int i;

  if (prop_conflicted)
    *prop_conflicted = FALSE;
  if (text_conflicted)
    *text_conflicted = FALSE;
  if (tree_conflicted)
    *tree_conflicted = FALSE;

  SVN_ERR(svn_wc__db_read_conflicts(&conflicts, wc_ctx->db, local_abspath,
                                    result_pool, scratch_pool));

  for (i = 0; i < conflicts->nelts; i++)
    {
      const svn_wc_conflict_description2_t *cd;
      cd = APR_ARRAY_IDX(conflicts, i, svn_wc_conflict_description2_t *);
      if (prop_conflicted && cd->kind == svn_wc_conflict_kind_property)
        *prop_conflicted = TRUE;
      else if (text_conflicted && cd->kind == svn_wc_conflict_kind_text)
        *text_conflicted = TRUE;
      else if (tree_conflicted && cd->kind == svn_wc_conflict_kind_tree)
        *tree_conflicted = TRUE;
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__temp_get_keep_local(svn_boolean_t *keep_local,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool)
{
  svn_boolean_t is_deleted;

  SVN_ERR(svn_wc__node_is_status_deleted(&is_deleted, wc_ctx, local_abspath,
                                         scratch_pool));
  if (is_deleted)
    SVN_ERR(svn_wc__db_temp_determine_keep_local(keep_local, wc_ctx->db,
                                                 local_abspath, scratch_pool));
  else
    *keep_local = FALSE;

  return SVN_NO_ERROR;
}
