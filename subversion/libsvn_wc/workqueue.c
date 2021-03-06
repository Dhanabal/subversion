/*
 * workqueue.c :  manipulating work queue items
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

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_subst.h"
#include "svn_hash.h"
#include "svn_io.h"

#include "wc.h"
#include "wc_db.h"
#include "workqueue.h"
#include "entries.h"
#include "props.h"
#include "adm_files.h"
#include "translate.h"
#include "log.h"

#include "svn_private_config.h"
#include "private/svn_skel.h"


#define NOT_IMPLEMENTED() \
  return svn_error__malfunction(TRUE, __FILE__, __LINE__, "Not implemented.")


/* Workqueue operation names.  */
#define OP_REVERT "revert"
#define OP_PREPARE_REVERT_FILES "prep-rev-files"
#define OP_KILLME "killme"
#define OP_LOGGY "loggy"
#define OP_DELETION_POSTCOMMIT "deletion-postcommit"
/* Arguments of OP_POSTCOMMIT:
 *   (local_abspath, revnum, date, [author], [checksum],
 *    [dav_cache/wc_props], keep_changelist, [tmp_text_base_abspath]). */
#define OP_POSTCOMMIT "postcommit"
#define OP_INSTALL_PROPERTIES "install-properties-2"
#define OP_DELETE "delete"
#define OP_FILE_INSTALL "file-install"
#define OP_FILE_REMOVE "file-remove"
#define OP_SYNC_FILE_FLAGS "sync-file-flags"
#define OP_PREJ_INSTALL "prej-install"
#define OP_WRITE_OLD_PROPS "write-old-props"
#define OP_RECORD_FILEINFO "record-fileinfo"


/* For work queue debugging. Generates output about its operation.  */
/* #define DEBUG_WORK_QUEUE */


struct work_item_dispatch {
  const char *name;
  svn_error_t *(*func)(svn_wc__db_t *db,
                       const svn_skel_t *work_item,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *scratch_pool);
};


/* ### forward declaration for this. Temporary hack so that a work item
   ### can be constructed within another handler and dispatched
   ### immediately. in most normal cases, appending a work item to the
   ### queue should be fine. but for now... not so much. */
static svn_error_t *
dispatch_work_item(svn_wc__db_t *db,
                   const char *wri_abspath,
                   const svn_skel_t *work_item,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *scratch_pool);


static svn_error_t *
sync_file_flags(svn_wc__db_t *db,
                const char *local_abspath,
                apr_pool_t *scratch_pool)
{
  /* ### right now, the maybe_set_* functions will only positively set those
     ### values. we need to clear them first.  */
  SVN_ERR(svn_io_set_file_read_write(local_abspath, FALSE, scratch_pool));
  SVN_ERR(svn_io_set_file_executable(local_abspath, FALSE, FALSE,
                                     scratch_pool));

  SVN_ERR(svn_wc__maybe_set_read_only(NULL, db, local_abspath, scratch_pool));
  SVN_ERR(svn_wc__maybe_set_executable(NULL, db, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
get_and_record_fileinfo(svn_wc__db_t *db,
                        const char *local_abspath,
                        svn_boolean_t ignore_enoent,
                        apr_pool_t *scratch_pool)
{
  apr_time_t last_mod_time;
  apr_finfo_t finfo;
  svn_error_t *err;

  err = svn_io_file_affected_time(&last_mod_time, local_abspath,
                                  scratch_pool);
  if (err)
    {
      if (!ignore_enoent || !APR_STATUS_IS_ENOENT(err->apr_err))
        return svn_error_return(err);

      /* No biggy. Just skip all this.  */
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_io_stat(&finfo, local_abspath,
                      APR_FINFO_MIN | APR_FINFO_LINK,
                      scratch_pool));

  return svn_error_return(svn_wc__db_global_record_fileinfo(
                            db, local_abspath,
                            finfo.size, last_mod_time,
                            scratch_pool));
}


/* If SOURCE_ABSPATH is present, then move it to DEST_ABSPATH. Ignore any
   ENOENT message for a missing source, which may indicate the move has
   already been performed.  */
static svn_error_t *
move_if_present(const char *source_abspath,
                const char *dest_abspath,
                apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  err = svn_io_file_rename(source_abspath, dest_abspath, scratch_pool);
  if (err)
    {
      if (!APR_STATUS_IS_ENOENT(err->apr_err))
        return svn_error_return(err);

      /* Not there. Maybe the node was moved in a prior run.  */
      svn_error_clear(err);
    }

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_REVERT  */


/* Remove the file at join(PARENT_ABSPATH, BASE_NAME) if it is not the
   working file defined by LOCAL_ABSPATH. If BASE_NAME is NULL, then
   nothing is done. All temp allocations are made within SCRATCH_POOL.  */
static svn_error_t *
maybe_remove_conflict(const char *parent_abspath,
                      const char *base_name,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool)
{
  if (base_name != NULL)
    {
      const char *conflict_abspath = svn_dirent_join(parent_abspath,
                                                     base_name,
                                                     scratch_pool);

      if (strcmp(conflict_abspath, local_abspath) != 0)
        SVN_ERR(svn_io_remove_file2(conflict_abspath, TRUE,
                                    scratch_pool));
    }

  return SVN_NO_ERROR;
}


/* Process the OP_REVERT work item WORK_ITEM.
 * See svn_wc__wq_add_revert() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_revert(svn_wc__db_t *db,
           const svn_skel_t *work_item,
           svn_cancel_func_t cancel_func,
           void *cancel_baton,
           apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;
  svn_boolean_t replaced;
  svn_wc__db_kind_t kind;
  svn_node_kind_t node_kind;
  const char *working_props_path;
  const char *parent_abspath;
  svn_boolean_t conflicted;
  int modify_flags = 0;
  svn_wc_entry_t tmp_entry;

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  replaced = svn_skel__parse_int(arg1->next, scratch_pool) != 0;
  /* magic_changed is extracted further below.  */
  /* use_commit_times is extracted further below.  */

  /* NOTE: we can read KIND here since uncommitted kind changes are not
     (yet) allowed. If we read any conflict files, then we (obviously) have
     not removed them from the metadata (yet).  */
  SVN_ERR(svn_wc__db_read_info(
            NULL, &kind, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            &conflicted, NULL,
            db, local_abspath,
            scratch_pool, scratch_pool));

#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  /* Move the "revert" props over/on the "base" props.  */
  if (replaced)
    {
      const char *revert_props_path;
      const char *base_props_path;

      SVN_ERR(svn_wc__prop_path(&revert_props_path, local_abspath,
                                kind, svn_wc__props_revert, scratch_pool));
      SVN_ERR(svn_wc__prop_path(&base_props_path, local_abspath,
                                kind, svn_wc__props_base, scratch_pool));

      SVN_ERR(move_if_present(revert_props_path, base_props_path,
                              scratch_pool));

      /* ### we should also be setting BASE props. and really... we shouldn't
         ### even bother zero-ing out these props. the WORKING node should
         ### be disappearing after a revert.  */
#if 0
      SVN_ERR(svn_wc__db_temp_working_set_props(db, local_abspath,
                                                apr_hash_make(scratch_pool),
                                                scratch_pool));
#endif
    }
#endif

  /* The "working" props contain changes. Nuke 'em from orbit.  */
#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  SVN_ERR(svn_wc__prop_path(&working_props_path, local_abspath,
                            kind, svn_wc__props_working, scratch_pool));
  SVN_ERR(svn_io_remove_file2(working_props_path, TRUE, scratch_pool));
#endif

  SVN_ERR(svn_wc__db_op_set_props(db, local_abspath, NULL, NULL, NULL,
                                  scratch_pool));

  /* Deal with the working file, as needed.  */
  if (kind == svn_wc__db_kind_file)
    {
      svn_boolean_t magic_changed;
      svn_boolean_t reinstall_working;

      magic_changed = svn_skel__parse_int(arg1->next->next, scratch_pool) != 0;

      /* If there was a magic property change, then we'll reinstall the
         working-file to pick up any/all appropriate changes. If there was
         a replacement, then we definitely want to reinstall the working-file
         using the original base.  */
      reinstall_working = magic_changed || replaced;

      if (replaced)
        {
#ifdef SVN_EXPERIMENTAL_PRISTINE
          /* With the Pristine Store, the checksum of this base stays in the
             BASE_NODE table so we don't need to rename or move anything. */
#else
          /* For WC-1: If there is a "revert base" file (because the file
           * is replaced), then move that revert base over to the normal
           * base and update the normal base checksum accordingly. */
          const char *revert_base_path;
          const char *text_base_path;
          svn_checksum_t *checksum;

          SVN_ERR(svn_wc__text_revert_path(&revert_base_path, db,
                                           local_abspath, scratch_pool));
          SVN_ERR(svn_wc__text_base_path(&text_base_path, db, local_abspath,
                                         scratch_pool));
          SVN_ERR(move_if_present(revert_base_path, text_base_path,
                                  scratch_pool));

          /* At this point, the regular text base has been restored (just
             now, or on a prior run). We need to recompute the checksum
             from that.

             ### in wc-1, this recompute only happened for add-with-history.
             ### need to investigate, but maybe the checksum was not touched
             ### for a simple replacing add? regardless, this recompute is
             ### always okay to do.  */
          SVN_ERR(svn_io_file_checksum2(&checksum, text_base_path,
                                        svn_checksum_md5, scratch_pool));
          tmp_entry.checksum = svn_checksum_to_cstring(checksum, scratch_pool);
          modify_flags |= SVN_WC__ENTRY_MODIFY_CHECKSUM;
#endif
        }
      else if (!reinstall_working)
        {
          svn_node_kind_t check_kind;

          /* If the working file is missing, we need to reinstall it.  */
          SVN_ERR(svn_io_check_path(local_abspath, &check_kind,
                                    scratch_pool));
          reinstall_working = (check_kind == svn_node_none);

          if (!reinstall_working)
            {
              /* ### can we optimize this call? we already fetched some
                 ### info about the node. and *definitely* never want a
                 ### full file-scan.  */

              /* ### for now, just always reinstall. without some extra work,
                 ### we could end up in a situation where the file is copied
                 ### from the base, but then something fails immediately
                 ### after that. on the second time through here, we would
                 ### see the file is "the same" and fail to complete those
                 ### follow-on actions. in some future work, examine the
                 ### points of failure, and possibly precompue the
                 ### "reinstall_working" flag, or maybe do some follow-on
                 ### actions unconditionally.  */
#if 1
              reinstall_working = TRUE;
#endif
#if 0
              /* ### try to avoid altering the timestamp if the intended
                 ### contents are the same as current-contents.  */
              SVN_ERR(svn_wc__text_modified_internal_p(&reinstall_working,
                                                       db, local_abspath,
                                                       FALSE, FALSE,
                                                       scratch_pool));
#endif
            }
        }

      if (reinstall_working)
        {
          svn_boolean_t use_commit_times;
          svn_skel_t *wi_file_install;

          use_commit_times = svn_skel__parse_int(arg1->next->next->next,
                                                 scratch_pool) != 0;

          SVN_ERR(svn_wc__wq_build_file_install(&wi_file_install,
                                                db, local_abspath,
                                                NULL /* source_abspath */,
                                                use_commit_times,
                                                TRUE /* record_fileinfo */,
                                                scratch_pool, scratch_pool));
          SVN_ERR(svn_wc__db_wq_add(db, local_abspath, wi_file_install,
                                    scratch_pool));
        }
    }
  else if (kind == svn_wc__db_kind_symlink)
    {
      NOT_IMPLEMENTED();
    }

  if (kind == svn_wc__db_kind_dir)
    parent_abspath = local_abspath;
  else
    parent_abspath = svn_dirent_dirname(local_abspath, scratch_pool);

  /* ### in wc-ng: the following block clears ACTUAL_NODE.  */
  if (conflicted)
    {
      const apr_array_header_t *conflicts;
      int i;

      SVN_ERR(svn_wc__db_read_conflicts(&conflicts, db, local_abspath,
                                        scratch_pool, scratch_pool));

      for (i = 0; i < conflicts->nelts; i++)
        {
          const svn_wc_conflict_description2_t *cd;

          cd = APR_ARRAY_IDX(conflicts, i,
                             const svn_wc_conflict_description2_t *);

          SVN_ERR(maybe_remove_conflict(parent_abspath, cd->base_file,
                                        local_abspath, scratch_pool));
          SVN_ERR(maybe_remove_conflict(parent_abspath, cd->their_file,
                                        local_abspath, scratch_pool));
          SVN_ERR(maybe_remove_conflict(parent_abspath, cd->my_file,
                                        local_abspath, scratch_pool));
          SVN_ERR(maybe_remove_conflict(parent_abspath, cd->merged_file,
                                        local_abspath, scratch_pool));
        }

      SVN_ERR(svn_wc__db_op_mark_resolved(db, local_abspath,
                                          TRUE, TRUE, FALSE,
                                          scratch_pool));
    }

  /* Clean up the copied state for all replacements.  */
  if (replaced)
    {
      modify_flags |= (SVN_WC__ENTRY_MODIFY_COPIED
                       | SVN_WC__ENTRY_MODIFY_COPYFROM_URL
                       | SVN_WC__ENTRY_MODIFY_COPYFROM_REV);
      tmp_entry.copied = FALSE;
      tmp_entry.copyfrom_url = NULL;
      tmp_entry.copyfrom_rev = SVN_INVALID_REVNUM;
    }

  /* Reset schedule attribute to svn_wc_schedule_normal. It could already be
     "normal", but no biggy if this is a no-op.  */
  modify_flags |= SVN_WC__ENTRY_MODIFY_SCHEDULE;
  tmp_entry.schedule = svn_wc_schedule_normal;

  /* We need the old school KIND...  */
  if (kind == svn_wc__db_kind_dir)
    {
      node_kind = svn_node_dir;
    }
  else
    {
      SVN_ERR_ASSERT(kind == svn_wc__db_kind_file
                     || kind == svn_wc__db_kind_symlink);
      node_kind = svn_node_file;
    }

  SVN_ERR(svn_wc__entry_modify(db, local_abspath, node_kind,
                               &tmp_entry, modify_flags,
                               scratch_pool));

  /* ### need to revert some bits in the parent stub. sigh.  */
  if (kind == svn_wc__db_kind_dir)
    {
      svn_boolean_t is_wc_root, is_switched;

      /* There is no parent stub if we're at the root.  */
      SVN_ERR(svn_wc__check_wc_root(&is_wc_root, NULL, &is_switched,
                                    db, local_abspath, scratch_pool));
      if (!is_wc_root && !is_switched)
        SVN_ERR(svn_wc__db_temp_op_remove_working_stub(db, local_abspath,
                                                       scratch_pool));
    }

  return SVN_NO_ERROR;
}


/* Return an APR_ENOENT error if LOCAL_ABSPATH has no text base.

   For issue #2101, we need to deliver this error. When the wc-ng pristine
   handling comes into play, the issue should be fixed, and this code can
   go away.  */
static svn_error_t *
verify_pristine_present(svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_pool_t *scratch_pool)
{
#ifdef SVN_EXPERIMENTAL_PRISTINE
  const svn_checksum_t *base_checksum;

  SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   &base_checksum, NULL, NULL, NULL,
                                   db, local_abspath,
                                   scratch_pool, scratch_pool));
  if (base_checksum != NULL)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, &base_checksum,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));
  if (base_checksum != NULL)
    return SVN_NO_ERROR;
#else
  const char *base_abspath;
  svn_error_t *err;

  /* Verify that one of the two text bases are present.  */
  err = svn_wc__text_base_path_to_read(&base_abspath, db, local_abspath,
                                       scratch_pool, scratch_pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_UNEXPECTED_STATUS)
    svn_error_clear(err);
  else
    return svn_error_return(err);

  err = svn_wc__text_revert_path_to_read(&base_abspath, db, local_abspath,
                                         scratch_pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_UNEXPECTED_STATUS)
    svn_error_clear(err);
  else
    return svn_error_return(err);
#endif

  /* A real file must have either a regular or a revert text-base.
     If it has neither, we could be looking at the situation described
     in issue #2101, in which case all we can do is deliver the expected
     error.  */
  return svn_error_createf(APR_ENOENT, NULL,
                           _("Error restoring text for '%s'"),
                           svn_dirent_local_style(local_abspath,
                                                  scratch_pool));
}


/* Record a work item to revert LOCAL_ABSPATH.  */
svn_error_t *
svn_wc__wq_add_revert(svn_boolean_t *will_revert,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      svn_boolean_t use_commit_times,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  svn_boolean_t replaced;
  svn_boolean_t magic_changed = FALSE;

  SVN_ERR(svn_wc__db_read_info(
            &status, &kind, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL,
            db, local_abspath,
            scratch_pool, scratch_pool));

  /* Special handling for issue #2101.  */
  if (kind == svn_wc__db_kind_file)
    SVN_ERR(verify_pristine_present(db, local_abspath, scratch_pool));

  /* Gather a few items *before* the revert work-item has a chance to run.
     During its operation, this data could/will change, which means that a
     potential re-run of the work-item may gather incorrect values.  */

  SVN_ERR(svn_wc__internal_is_replaced(&replaced, db, local_abspath,
                                       scratch_pool));

  /* If a replacement has occurred, then a revert definitely happens.  */
  *will_revert = replaced;

  if (!replaced)
    {
      apr_hash_t *base_props;
      apr_hash_t *working_props;
      apr_array_header_t *prop_diffs;

      SVN_ERR(svn_wc__get_pristine_props(&base_props,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));
      SVN_ERR(svn_wc__get_actual_props(&working_props,
                                       db, local_abspath,
                                       scratch_pool, scratch_pool));
      SVN_ERR(svn_prop_diffs(&prop_diffs, working_props, base_props,
                             scratch_pool));
      magic_changed = svn_wc__has_magic_property(prop_diffs);

      if (prop_diffs->nelts > 0)
        {
          /* Property changes cause a revert to occur.  */
          *will_revert = TRUE;
        }
      else
        {
          /* There is nothing to do for NORMAL or ADDED nodes. Typically,
             we won't even be called for added nodes (since a revert
             simply removes it from version control), but it is possible
             that a parent replacement was turned from a replaced copy
             into a normal node, and the (broken) old ENTRY->COPIED logic
             then turns the copied children into typical ADDED nodes.
             Since the recursion has already started, these children are
             visited (unlike most added nodes).  */
          if (status != svn_wc__db_status_normal
              && status != svn_wc__db_status_added)
            {
              *will_revert = TRUE;
            }

          /* We may need to restore a missing working file.  */
          if (! *will_revert)
            {
              svn_node_kind_t on_disk;

              SVN_ERR(svn_io_check_path(local_abspath, &on_disk,
                                        scratch_pool));
              *will_revert = on_disk == svn_node_none;
            }

          if (! *will_revert)
            {
              /* ### there may be ways to simplify this test, rather than
                 ### doing file comparisons and junk... */
              SVN_ERR(svn_wc__internal_text_modified_p(will_revert,
                                                       db, local_abspath,
                                                       FALSE, FALSE,
                                                       scratch_pool));
            }
        }
    }

  /* Don't even bother to queue a work item if there is nothing to do.  */
  if (*will_revert)
    {
      svn_skel_t *work_item;

      work_item = svn_skel__make_empty_list(scratch_pool);

      /* These skel atoms hold references to very transitory state, but
         we only need the work_item to survive for the duration of wq_add.  */
      svn_skel__prepend_int(use_commit_times, work_item, scratch_pool);
      svn_skel__prepend_int(magic_changed, work_item, scratch_pool);
      svn_skel__prepend_int(replaced, work_item, scratch_pool);
      svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
      svn_skel__prepend_str(OP_REVERT, work_item, scratch_pool);

      SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item, scratch_pool));
    }

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_PREPARE_REVERT_FILES  */


/* Process the OP_PREPARE_REVERT_FILES work item WORK_ITEM.
 * See svn_wc__wq_prepare_revert_files() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_prepare_revert_files(svn_wc__db_t *db,
                         const svn_skel_t *work_item,
                         svn_cancel_func_t cancel_func,
                         void *cancel_baton,
                         apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;
  svn_wc__db_kind_t kind;
  const char *revert_prop_abspath;
  const char *base_prop_abspath;
  svn_node_kind_t on_disk;

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  /* Rename the original text base over to the revert text base.  */
  SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, FALSE, scratch_pool));
#ifdef SVN_EXPERIMENTAL_PRISTINE
  /* With the Pristine Store, the checksum of this base stays in the
   * BASE_NODE table so we don't need to rename or move anything. */
#else
  if (kind == svn_wc__db_kind_file)
    {
      const char *text_base;
      const char *text_revert;

      SVN_ERR(svn_wc__text_base_path(&text_base, db, local_abspath,
                                     scratch_pool));
      SVN_ERR(svn_wc__text_revert_path(&text_revert, db, local_abspath,
                                       scratch_pool));

      SVN_ERR(move_if_present(text_base, text_revert, scratch_pool));
    }
#endif

  /* Set up the revert props.  */

#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  SVN_ERR(svn_wc__prop_path(&revert_prop_abspath, local_abspath, kind,
                            svn_wc__props_revert, scratch_pool));
  SVN_ERR(svn_wc__prop_path(&base_prop_abspath, local_abspath, kind,
                            svn_wc__props_base, scratch_pool));

  /* First: try to move any base properties to the revert location.  */
  SVN_ERR(move_if_present(base_prop_abspath, revert_prop_abspath,
                          scratch_pool));

  /* If no props exist at the revert location, then drop a set of empty
     props there. They are expected to be present.  */
  SVN_ERR(svn_io_check_path(revert_prop_abspath, &on_disk, scratch_pool));
  if (on_disk == svn_node_none)
    {
      svn_stream_t *stream;

      /* A set of empty props is just an empty file. */
      SVN_ERR(svn_stream_open_writable(&stream, revert_prop_abspath,
                                       scratch_pool, scratch_pool));
      SVN_ERR(svn_stream_close(stream));
      SVN_ERR(svn_io_set_file_read_only(revert_prop_abspath, FALSE,
                                        scratch_pool));
    }
#endif

  /* Put some blank properties into the WORKING node.  */
  /* ### this seems bogus. something else should come along and put the
     ### correct values in here. we shouldn't put empty values in.  */
#if 0
  SVN_ERR(svn_wc__db_temp_working_set_props(db, local_abspath,
                                            apr_hash_make(scratch_pool),
                                            scratch_pool));
#endif

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_prepare_revert_files(svn_wc__db_t *db,
                                const char *local_abspath,
                                apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);

  /* These skel atoms hold references to very transitory state, but
     we only need the work_item to survive for the duration of wq_add.  */
  svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_PREPARE_REVERT_FILES, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_KILLME  */

/* Process the OP_KILLME work item WORK_ITEM.
 * See svn_wc__wq_add_killme() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_killme(svn_wc__db_t *db,
           const svn_skel_t *work_item,
           svn_cancel_func_t cancel_func,
           void *cancel_baton,
           apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *dir_abspath;
  svn_boolean_t adm_only;
  svn_wc__db_status_t status;
  svn_revnum_t original_revision;
  svn_revnum_t parent_revision;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_error_t *err;

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  dir_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  adm_only = svn_skel__parse_int(arg1->next, scratch_pool) != 0;

  err = svn_wc__db_base_get_info(&status, NULL, &original_revision,
                                 NULL, NULL, NULL,
                                 NULL, NULL, NULL,
                                 NULL, NULL, NULL,
                                 NULL, NULL, NULL,
                                 db, dir_abspath,
                                 scratch_pool, scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      /* The administrative area in the subdir is gone, and the subdir
         is also removed from its parent's record.  */
      svn_error_clear(err);

      /* When we removed the directory, if ADM_ONLY was TRUE, then that
         has definitely been done and there is nothing left to do.

         If ADM_ONLY was FALSE, then the subdir and its contents were
         removed *before* the administrative was removed. Anything that
         may be left are unversioned nodes. We don't want to do anything
         to those, so we're done for this case, too.  */
      return SVN_NO_ERROR;
    }
  if (status == svn_wc__db_status_obstructed)
    {
      /* The subdir's administrative area has already been removed, but
         there was still an entry in the parent. Whatever is in that
         record, it doesn't matter. The subdir has been handled already.  */
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_wc__db_read_info(NULL, NULL, &parent_revision,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL,
                               db, svn_dirent_dirname(dir_abspath,
                                                      scratch_pool),
                               scratch_pool, scratch_pool));

  /* Remember the repository this node is associated with.  */
  SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                     &repos_uuid,
                                     db, dir_abspath,
                                     scratch_pool, scratch_pool));

  /* Blow away the administrative directories, and possibly the working
     copy tree too. */
  err = svn_wc__internal_remove_from_revision_control(
          db, dir_abspath,
          !adm_only /* destroy_wf */, FALSE /* instant_error */,
          cancel_func, cancel_baton,
          scratch_pool);
  if (err && err->apr_err != SVN_ERR_WC_LEFT_LOCAL_MOD)
    return svn_error_return(err);
  svn_error_clear(err);

  /* If revnum of this dir is greater than parent's revnum, then
     recreate 'deleted' entry in parent. */
  if (original_revision > parent_revision)
    {
      SVN_ERR(svn_wc__db_base_add_absent_node(
                db, dir_abspath,
                repos_relpath, repos_root_url, repos_uuid,
                original_revision, svn_wc__db_kind_dir,
                svn_wc__db_status_not_present,
                NULL, NULL,
                scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_add_killme(svn_wc__db_t *db,
                      const char *dir_abspath,
                      svn_boolean_t adm_only,
                      apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);

  /* The skel still points at DIR_ABSPATH, but the skel will be serialized
     just below in the wq_add call.  */
  svn_skel__prepend_int(adm_only, work_item, scratch_pool);
  svn_skel__prepend_str(dir_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_KILLME, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, dir_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_LOGGY  */

/* Process the OP_LOGGY work item WORK_ITEM.
 * See svn_wc__wq_add_loggy() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_loggy(svn_wc__db_t *db,
          const svn_skel_t *work_item,
          svn_cancel_func_t cancel_func,
          void *cancel_baton,
          apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *adm_abspath;

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  adm_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  return svn_error_return(svn_wc__run_xml_log(
                            db, adm_abspath,
                            arg1->next->data, arg1->next->len,
                            scratch_pool));
}


svn_error_t *
svn_wc__wq_build_loggy(svn_skel_t **work_item,
                       svn_wc__db_t *db,
                       const char *adm_abspath,
                       const svn_stringbuf_t *log_content,
                       apr_pool_t *result_pool)
{
  if (log_content == NULL || svn_stringbuf_isempty(log_content))
    {
      *work_item = NULL;
      return SVN_NO_ERROR;
    }

  *work_item = svn_skel__make_empty_list(result_pool);

  /* NOTE: the skel still points at ADM_ABSPATH and LOG_CONTENT, but we
     require these parameters to be allocated in RESULT_POOL.  */
  svn_skel__prepend_str(log_content->data, *work_item, result_pool);
  svn_skel__prepend_str(adm_abspath, *work_item, result_pool);
  svn_skel__prepend_str(OP_LOGGY, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_DELETION_POSTCOMMIT  */

/* Process the OP_DELETION_POSTCOMMIT work item WORK_ITEM.
 * See svn_wc__wq_add_deletion_postcommit() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_deletion_postcommit(svn_wc__db_t *db,
                        const svn_skel_t *work_item,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;
  svn_revnum_t new_revision;
  svn_boolean_t no_unlock;
  svn_wc__db_kind_t kind;

  /* ### warning: this code has not been vetted for running multiple times  */

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  new_revision = (svn_revnum_t)svn_skel__parse_int(arg1->next, scratch_pool);
  no_unlock = svn_skel__parse_int(arg1->next->next, scratch_pool) != 0;

  SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, FALSE, scratch_pool));

  /* ### the section below was ripped out of log.c::log_do_committed().
     ### it needs to be rewritten into wc-ng terms.  */

    {
      const char *repos_relpath;
      const char *repos_root_url;
      const char *repos_uuid;
      svn_revnum_t parent_revision;

      /* If we are suppose to delete "this dir", drop a 'killme' file
         into my own administrative dir as a signal for svn_wc__run_log()
         to blow away the administrative area after it is finished
         processing this logfile.  */
      if (kind == svn_wc__db_kind_dir)
        {
          svn_boolean_t keep_local;
          svn_wc_entry_t tmp_entry;

          /* Bump the revision number of this_dir anyway, so that it
             might be higher than its parent's revnum.  If it's
             higher, then the process that sees KILLME and destroys
             the directory can also place a 'deleted' dir entry in the
             parent. */
          tmp_entry.revision = new_revision;
          SVN_ERR(svn_wc__entry_modify(db, local_abspath,
                                       svn_node_dir,
                                       &tmp_entry,
                                       SVN_WC__ENTRY_MODIFY_REVISION,
                                       scratch_pool));

          SVN_ERR(svn_wc__db_temp_determine_keep_local(&keep_local, db,
                                                       local_abspath,
                                                       scratch_pool));

          /* Ensure the directory is deleted later.  */
          return svn_error_return(svn_wc__wq_add_killme(
                                    db, local_abspath,
                                    keep_local /* adm_only */,
                                    scratch_pool));
        }

      /* Get hold of repository info, if we are going to need it,
         before deleting the file, */
      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, &parent_revision, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL,
                                       db, svn_dirent_dirname(local_abspath,
                                                              scratch_pool),
                                       scratch_pool, scratch_pool));
      if (new_revision > parent_revision)
        SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                           &repos_uuid, db, local_abspath,
                                           scratch_pool, scratch_pool));

      /* We're deleting a file, and we can safely remove files from
         revision control without screwing something else up.  */
      SVN_ERR(svn_wc__internal_remove_from_revision_control(
                db, local_abspath,
                FALSE, FALSE, cancel_func, cancel_baton, scratch_pool));

      /* If the parent entry's working rev 'lags' behind new_rev... */
      if (new_revision > parent_revision)
        {
          /* ...then the parent's revision is now officially a
             lie;  therefore, it must remember the file as being
             'deleted' for a while.  Create a new, uninteresting
             ghost entry:  */
          SVN_ERR(svn_wc__db_base_add_absent_node(
                    db, local_abspath,
                    repos_relpath, repos_root_url, repos_uuid,
                    new_revision, svn_wc__db_kind_file,
                    svn_wc__db_status_not_present,
                    NULL, NULL,
                    scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_add_deletion_postcommit(svn_wc__db_t *db,
                                   const char *local_abspath,
                                   svn_revnum_t new_revision,
                                   svn_boolean_t no_unlock,
                                   apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);

  /* The skel still points at LOCAL_ABSPATH, but the skel will be
     serialized just below in the wq_add call.  */
  svn_skel__prepend_int(no_unlock, work_item, scratch_pool);
  svn_skel__prepend_int(new_revision, work_item, scratch_pool);
  svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_DELETION_POSTCOMMIT, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_POSTCOMMIT  */


/* If TMP_TEXT_BASE_ABSPATH is not NULL, then replace the text base for
 * newly-committed file FILE_ABSPATH with the new post-commit text base,
 * TMP_TEXT_BASE_ABSPATH which is in repository-normal form (aka
 * "detranslated" form).
 *
 * If eol and/or keyword translation would cause the working file to
 * change, then overwrite the working file with a translated copy of
 * the new text base (but only if the translated copy differs from the
 * current working file -- if they are the same, do nothing, to avoid
 * clobbering timestamps unnecessarily).
 *
 * Set the working file's executability according to its svn:executable
 * property, or, if REMOVE_EXECUTABLE is TRUE, set it to not executable.
 *
 * Set the working file's read-only attribute according to its properties
 * and lock status (see svn_wc__maybe_set_read_only()), or, if
 * REMOVE_READ_ONLY is TRUE, set it to writable.
 *
 * If the working file was re-translated or had its executability or
 * read-only state changed,
 * then set OVERWROTE_WORKING to TRUE.  If the working file isn't
 * touched at all, then set to FALSE.
 *
 * Use SCRATCH_POOL for any temporary allocation.
 */
static svn_error_t *
install_committed_file(svn_boolean_t *overwrote_working,
                       svn_wc__db_t *db,
                       const char *file_abspath,
                       const char *tmp_text_base_abspath,
                       svn_boolean_t remove_executable,
                       svn_boolean_t remove_read_only,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *scratch_pool)
{
  svn_boolean_t same, did_set;
  const char *tmp_wfile;
  svn_boolean_t special;

  /* start off assuming that the working file isn't touched. */
  *overwrote_working = FALSE;

  /* In the commit, newlines and keywords may have been
   * canonicalized and/or contracted... Or they may not have
   * been.  It's kind of hard to know.  Here's how we find out:
   *
   *    1. Make a translated tmp copy of the committed text base,
   *       translated according to the versioned file's properties.
   *       Or, if no committed text base exists (the commit must have
   *       been a propchange only), make a translated tmp copy of the
   *       working file.
   *    2. Compare the translated tmpfile to the working file.
   *    3. If different, copy the tmpfile over working file.
   *
   * This means we only rewrite the working file if we absolutely
   * have to, which is good because it avoids changing the file's
   * timestamp unless necessary, so editors aren't tempted to
   * reread the file if they don't really need to.
   */

  /* Copy and translate the new base-to-be file (if found, else the working
   * file) from repository-normal form to working form, writing a new
   * temporary file if any translation was actually done.  Set TMP_WFILE to
   * the translated file's path, which may be the source file's path if no
   * translation was done.  Set SAME to indicate whether the new working
   * text is the same as the old working text (or TRUE if it's a special
   * file). */
  {
    const char *tmp
      = (tmp_text_base_abspath != NULL) ? tmp_text_base_abspath : file_abspath;

    /* Copy and translate, if necessary. The output file will be deleted at
     * scratch_pool cleanup.
     * ### That's not quite safe: we might rename the file and then maybe
     * its path will get re-used for another temp file before pool clean-up.
     * Instead, we should take responsibility for deleting it. */
    SVN_ERR(svn_wc__internal_translated_file(&tmp_wfile, tmp, db,
                                             file_abspath,
                                             SVN_WC_TRANSLATE_FROM_NF,
                                             cancel_func, cancel_baton,
                                             scratch_pool, scratch_pool));

    /* If the translation is a no-op, the text base and the working copy
     * file contain the same content, because we use the same props here
     * as were used to detranslate from working file to text base.
     *
     * In that case: don't replace the working file, but make sure
     * it has the right executable and read_write attributes set.
     */

    SVN_ERR(svn_wc__get_special(&special, db, file_abspath, scratch_pool));
    if (! special && tmp != tmp_wfile)
      SVN_ERR(svn_io_files_contents_same_p(&same, tmp_wfile,
                                           file_abspath, scratch_pool));
    else
      same = TRUE;
  }

  if (! same)
    {
      SVN_ERR(svn_io_file_rename(tmp_wfile, file_abspath, scratch_pool));
      *overwrote_working = TRUE;
    }

  /* ### should be using OP_SYNC_FILE_FLAGS, or an internal version of
     ### that here. do we need to set *OVERWROTE_WORKING?  */

  if (remove_executable)
    {
      /* No need to chmod -x on a new file: new files don't have it. */
      if (same)
        SVN_ERR(svn_io_set_file_executable(file_abspath,
                                           FALSE, /* chmod -x */
                                           FALSE, scratch_pool));
      /* ### We should avoid setting 'overwrote_working' here if we didn't
       * change the executability. */
      *overwrote_working = TRUE; /* entry needs wc-file's timestamp  */
    }
  else
    {
      /* Set the working file's execute bit if props dictate. */
      SVN_ERR(svn_wc__maybe_set_executable(&did_set, db, file_abspath,
                                           scratch_pool));
      if (did_set)
        /* okay, so we didn't -overwrite- the working file, but we changed
           its timestamp, which is the point of returning this flag. :-) */
        *overwrote_working = TRUE;
    }

  if (remove_read_only)
    {
      /* No need to make a new file read_write: new files already are. */
      if (same)
        SVN_ERR(svn_io_set_file_read_write(file_abspath, FALSE,
                                           scratch_pool));
      /* ### We should avoid setting 'overwrote_working' here if we didn't
       * change the read-only-ness. */
      *overwrote_working = TRUE; /* entry needs wc-file's timestamp  */
    }
  else
    {
      SVN_ERR(svn_wc__maybe_set_read_only(&did_set, db, file_abspath,
                                          scratch_pool));
      if (did_set)
        /* okay, so we didn't -overwrite- the working file, but we changed
           its timestamp, which is the point of returning this flag. :-) */
        *overwrote_working = TRUE;
    }

  /* Install the new text base if one is waiting. */
#ifdef SVN_EXPERIMENTAL_PRISTINE
  /* The Pristine Store equivalent is putting the text in the pristine store
     and putting its checksum in the database, both of which happened before
     this function was called. */
#else
  if (tmp_text_base_abspath != NULL)
    SVN_ERR(svn_wc__sync_text_base(db, file_abspath, tmp_text_base_abspath,
                                   scratch_pool));
#endif

  return SVN_NO_ERROR;
}


/* Set the base version of the node LOCAL_ABSPATH to be the same as its
 * working version currently is:
 *
 * - Remove children that are marked deleted (if it's a dir)
 * - Install the new base props
 * - Install the new tree state
 * - Install the new base text (if it's a file) from TMP_TEXT_BASE_ABSPATH
 * - Adjust the parent (if it's a dir)
 * */
static svn_error_t *
log_do_committed(svn_wc__db_t *db,
                 const char *local_abspath,
                 const char *tmp_text_base_abspath,
                 svn_revnum_t new_revision,
                 apr_time_t new_date,
                 const char *new_author,
                 const svn_checksum_t *new_checksum,
                 apr_hash_t *new_dav_cache,
                 svn_boolean_t keep_changelist,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool)
{
  apr_pool_t *pool = scratch_pool;
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
  svn_boolean_t remove_executable = FALSE;
  svn_boolean_t set_read_write = FALSE;
  svn_boolean_t prop_mods;

  /* ### this gets the *intended* kind. for now, this also matches any
     ### potential BASE kind since we cannot change kinds.  */
  SVN_ERR(svn_wc__db_read_info(
            &status, &kind, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL,
            db, local_abspath,
            scratch_pool, scratch_pool));

  /* We should never be running a commit on a not-present node. If we see
     this, then it (probably) means that a prior run has deleted this node,
     and left the not-present behind. There isn't anything more to do.  */
  if (status == svn_wc__db_status_not_present)
    return SVN_NO_ERROR;

  /* We shouldn't be in this function for deleted nodes. They are handled
     by other processes.  */
  SVN_ERR_ASSERT(status != svn_wc__db_status_deleted);

  /*** Mark the committed item committed-to-date ***/

  /* ### this comment is quite old. originally, we looked for
     ### schedule_replace here. that definition is:
     ###
     ### ((status == svn_wc__db_status_added
     ###   || status == svn_wc__db_status_obstructed_add)
     ###  && base_shadowed
     ###  && base_status != svn_wc__db_status_not_present)
     ###
     ### An obstructed add cannot be committed, so we don't have to
     ### worry about that.
     ###
     ### If the BASE node is not-present, then it has no children which
     ### may be marked for deletion, so that won't contribute to this
     ### loop either (ie. we won't accidentally remove something)
     ###
     ### Thus, we're simply looking for status == svn_wc__db_status_added

     If "this dir" has been replaced (delete + add), remove those of
     its children that are marked for deletion.

     All its immmediate children *must* be either scheduled for deletion
     (they were children of "this dir" during the "delete" phase of its
     replacement), added (they are new children of the replaced dir),
     or replaced (they are new children of the replace dir that have
     the same names as children that were present during the "delete"
     phase of the replacement).

     Children which are added or replaced will have been reported as
     individual commit targets, and thus will be re-visited by
     log_do_committed().  Children which were marked for deletion,
     however, need to be outright removed from revision control.  */

  if (status == svn_wc__db_status_added && kind == svn_wc__db_kind_dir)
    {
      /* Loop over all children entries, look for items scheduled for
         deletion. */
      const apr_array_header_t *children;
      int i;
      apr_pool_t *iterpool = svn_pool_create(pool);

      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       pool, pool));

      for (i = 0; i < children->nelts; i++)
        {
          const char *child_name = APR_ARRAY_IDX(children, i, const char*);
          const char *child_abspath;
          svn_wc__db_status_t child_status;

          apr_pool_clear(iterpool);
          child_abspath = svn_dirent_join(local_abspath, child_name, iterpool);

          SVN_ERR(svn_wc__db_read_info(&child_status,
                                       NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       db, child_abspath, iterpool, iterpool));

          /* Committing a deletion should remove the local nodes.  */
          if (child_status == svn_wc__db_status_deleted
              || child_status == svn_wc__db_status_obstructed_delete)
            {
              SVN_ERR(svn_wc__internal_remove_from_revision_control(
                        db, child_abspath,
                        FALSE /* destroy_wf */,
                        FALSE /* instant_error */,
                        cancel_func, cancel_baton,
                        iterpool));
            }
        }
    }

  /* Install the node's current working props as its new base props.
   * Remember some details about the prop changes, for later use. */
  SVN_ERR(svn_wc__props_modified(&prop_mods, db, local_abspath, pool));
  if (prop_mods)
    {
      if (kind == svn_wc__db_kind_file)
        {
          /* Examine propchanges here before installing the new
             propbase.  If the executable prop was -deleted-, remember
             this by setting REMOVE_EXECUTABLE so that we can later
             tell install_committed_file() so.  The same applies to the
             needs-lock property, remembered by setting SET_READ_WRITE. */
          int i;
          apr_array_header_t *propchanges;

          SVN_ERR(svn_wc__internal_propdiff(&propchanges, NULL, db,
                                            local_abspath, pool, pool));
          for (i = 0; i < propchanges->nelts; i++)
            {
              svn_prop_t *propchange
                = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

              if ((! strcmp(propchange->name, SVN_PROP_EXECUTABLE))
                  && (propchange->value == NULL))
                remove_executable = TRUE;
              else if ((! strcmp(propchange->name, SVN_PROP_NEEDS_LOCK))
                       && (propchange->value == NULL))
                set_read_write = TRUE;
            }
        }

      /* Install LOCAL_ABSPATHs working props as base props. */
      SVN_ERR(svn_wc__working_props_committed(db, local_abspath, pool));
    }

  /* If it's a file, install the tree changes and the file's text. */
  if (kind == svn_wc__db_kind_file)
    {
      svn_boolean_t overwrote_working;
      apr_finfo_t finfo;
      svn_filesize_t translated_size;
      apr_time_t last_mod_time;

      SVN_ERR(svn_wc__db_global_commit(db, local_abspath,
                                       new_revision, new_date, new_author,
                                       new_checksum,
                                       NULL /* new_children */,
                                       new_dav_cache,
                                       keep_changelist,
                                       NULL /* work_items */,
                                       pool));

      /* Install the new file, which may involve expanding keywords.
         A copy of this file should have been dropped into our `tmp/text-base'
         directory during the commit process.  Part of this process
         involves setting the textual timestamp for this entry.  We'd like
         to just use the timestamp of the working file, but it is possible
         that at some point during the commit, the real working file might
         have changed again.  If that has happened, we'll use the
         timestamp of the copy of this file in `tmp/text-base' (which
         by then will have moved to `text-base'. */

      SVN_ERR(install_committed_file(&overwrote_working, db,
                                     local_abspath, tmp_text_base_abspath,
                                     remove_executable, set_read_write,
                                     cancel_func, cancel_baton,
                                     pool));

      SVN_ERR(svn_io_stat(&finfo, local_abspath,
                          APR_FINFO_MIN | APR_FINFO_LINK, pool));

      /* We will compute and modify the size and timestamp */

      translated_size = finfo.size;

      if (overwrote_working)
        {
          last_mod_time = finfo.mtime;
        }
      else
        {
          /* The working copy file hasn't been overwritten, meaning
             we need to decide which timestamp to use. */

          apr_finfo_t basef_finfo;
          svn_boolean_t modified;

          /* If the working file was overwritten (due to re-translation)
             or touched (due to +x / -x), then use *that* textual
             timestamp instead. */
          SVN_ERR(svn_wc__get_pristine_text_status(&basef_finfo,
                                                   db, local_abspath,
                                                   pool, pool));

          /* Verify that the working file is the same as the base file
             by comparing file sizes, then timestamps and the contents
             after that. */

          /*###FIXME: if the file needs translation, don't compare
            file-sizes, just compare timestamps and do the rest of the
            hokey pokey. */
          modified = finfo.size != basef_finfo.size;
          if (finfo.mtime != basef_finfo.mtime && ! modified)
            {
              /* Compare the texts.  Don't use
                 svn_wc__internal_text_modified_p's ability to compare
                 against the *recorded* size and time stamp because that's
                 not what we are interested in right here. */
              SVN_ERR(svn_wc__internal_text_modified_p(
                        &modified, db, local_abspath,
                        TRUE /* force_comparison */,
                        FALSE /* compare_textbases */, pool));
            }
          /* If they are the same, use the working file's timestamp,
             else use the base file's timestamp. */
          last_mod_time = modified ? basef_finfo.mtime : finfo.mtime;
        }

      return svn_error_return(svn_wc__db_global_record_fileinfo(
                                db, local_abspath,
                                translated_size, last_mod_time,
                                pool));
    }

  /* It's not a file, so it's a directory. */

  SVN_ERR(svn_wc__db_global_commit(db, local_abspath,
                                   new_revision, new_date, new_author,
                                   NULL /* new_checksum */,
                                   NULL /* new_children */,
                                   new_dav_cache,
                                   keep_changelist,
                                   NULL /* work_items */,
                                   pool));

  /* For directories, we also have to reset the state in the parent's
     entry for this directory, unless the current directory is a `WC
     root' (meaning, our parent directory on disk is not our parent in
     Version Control Land), in which case we're all finished here. */
  {
    svn_boolean_t is_root;
    svn_boolean_t is_switched;

    SVN_ERR(svn_wc__check_wc_root(&is_root, NULL, &is_switched,
                                  db, local_abspath, pool));
    if (is_root || is_switched)
      return SVN_NO_ERROR;
  }

  /* Make sure we have a parent stub in a clean/unmodified state.  */
  {
    svn_wc_entry_t tmp_entry;

    tmp_entry.schedule = svn_wc_schedule_normal;
    tmp_entry.copied = FALSE;
    tmp_entry.deleted = FALSE;
    /* ### We assume we have the right lock to modify the parent record.

           If this fails for you in the transition to one DB phase, please
           run svn cleanup one level higher. */
    SVN_ERR(svn_wc__entry_modify_stub(db, local_abspath,
                                      &tmp_entry,
                                      (SVN_WC__ENTRY_MODIFY_SCHEDULE
                                       | SVN_WC__ENTRY_MODIFY_COPIED
                                       | SVN_WC__ENTRY_MODIFY_DELETED
                                       | SVN_WC__ENTRY_MODIFY_FORCE),
                                      pool));
  }

  return SVN_NO_ERROR;
}


/* Process the OP_POSTCOMMIT work item WORK_ITEM.
 * See svn_wc__wq_add_postcommit() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_postcommit(svn_wc__db_t *db,
               const svn_skel_t *work_item,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const svn_skel_t *arg5 = work_item->children->next->next->next->next->next;
  const char *local_abspath;
  svn_revnum_t new_revision;
  apr_time_t new_date;
  const char *new_author;
  const svn_checksum_t *new_checksum;
  apr_hash_t *new_dav_cache;
  svn_boolean_t keep_changelist;
  const char *tmp_text_base_abspath;
  svn_error_t *err;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  new_revision = (svn_revnum_t)svn_skel__parse_int(arg1->next, scratch_pool);
  new_date = svn_skel__parse_int(arg1->next->next, scratch_pool);
  if (arg1->next->next->next->len == 0)
    new_author = NULL;
  else
    new_author = apr_pstrmemdup(scratch_pool,
                                arg1->next->next->next->data,
                                arg1->next->next->next->len);
  if (arg5->len == 0)
    {
      new_checksum = NULL;
    }
  else
    {
      const char *data = apr_pstrmemdup(scratch_pool, arg5->data, arg5->len);
      SVN_ERR(svn_checksum_deserialize(&new_checksum, data,
                                       scratch_pool, scratch_pool));
    }
  if (arg5->next->is_atom)
    new_dav_cache = NULL;
  else
    SVN_ERR(svn_skel__parse_proplist(&new_dav_cache, arg5->next,
                                     scratch_pool));
  keep_changelist = svn_skel__parse_int(arg5->next->next, scratch_pool) != 0;

  /* Before r927056, this WQ item didn't have this next field.  Catch any
   * attempt to run this code on a WC having a stale WQ item in it. */
  SVN_ERR_ASSERT(arg5->next->next->next != NULL);
  if (arg5->next->next->next->len == 0)
    tmp_text_base_abspath = NULL;
  else
    tmp_text_base_abspath = apr_pstrmemdup(scratch_pool,
                                           arg5->next->next->next->data,
                                           arg5->next->next->next->len);

  err = log_do_committed(db, local_abspath, tmp_text_base_abspath,
                         new_revision, new_date,
                         new_author, new_checksum, new_dav_cache,
                         keep_changelist,
                         cancel_func, cancel_baton,
                         scratch_pool);
  if (err)
    return svn_error_createf(SVN_ERR_WC_BAD_ADM_LOG, err,
                             _("Error processing post-commit work for '%s'"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_add_postcommit(svn_wc__db_t *db,
                          const char *local_abspath,
                          const char *tmp_text_base_abspath,
                          svn_revnum_t new_revision,
                          apr_time_t new_date,
                          const char *new_author,
                          const svn_checksum_t *new_checksum,
                          apr_hash_t *new_dav_cache,
                          svn_boolean_t keep_changelist,
                          apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);

  svn_skel__prepend_str(tmp_text_base_abspath ? tmp_text_base_abspath : "",
                        work_item, scratch_pool);
  svn_skel__prepend_int(keep_changelist, work_item, scratch_pool);
  if (new_dav_cache == NULL || apr_hash_count(new_dav_cache) == 0)
    {
      svn_skel__prepend_str("", work_item, scratch_pool);
    }
  else
    {
      svn_skel_t *props_skel;

      SVN_ERR(svn_skel__unparse_proplist(&props_skel, new_dav_cache,
                                         scratch_pool));
      svn_skel__prepend(props_skel, work_item);
    }
  svn_skel__prepend_str(new_checksum
                          ? svn_checksum_serialize(new_checksum,
                                                   scratch_pool, scratch_pool)
                          : "",
                        work_item, scratch_pool);
  svn_skel__prepend_str(new_author ? new_author : "", work_item, scratch_pool);
  svn_skel__prepend_int(new_date, work_item, scratch_pool);
  svn_skel__prepend_int(new_revision, work_item, scratch_pool);
  svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_POSTCOMMIT, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}

/* ------------------------------------------------------------------------ */

/* OP_INSTALL_PROPERTIES */

/* See props.h  */
#ifdef SVN__SUPPORT_BASE_MERGE

/* Process the OP_INSTALL_PROPERTIES work item WORK_ITEM.
 * See svn_wc__wq_add_install_properties() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_install_properties(svn_wc__db_t *db,
                       const svn_skel_t *work_item,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg = work_item->children->next;
  const char *local_abspath;
  apr_hash_t *base_props;
  apr_hash_t *actual_props;

  /* We need a NUL-terminated path, so copy it out of the skel.  */
  local_abspath = apr_pstrmemdup(scratch_pool, arg->data, arg->len);

  arg = arg->next;
  if (arg->is_atom)
    base_props = NULL;
  else
    SVN_ERR(svn_skel__parse_proplist(&base_props, arg, scratch_pool));

  arg = arg->next;
  if (arg->is_atom)
    actual_props = NULL;
  else
    SVN_ERR(svn_skel__parse_proplist(&actual_props, arg, scratch_pool));

  if (base_props != NULL)
    {
        svn_boolean_t written = FALSE;

          {
            svn_error_t *err;

            /* Try writing to the WORKING tree first.  */
            err = svn_wc__db_temp_working_set_props(db, local_abspath,
                                                    base_props,
                                                    scratch_pool);
            if (err)
              {
                if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
                  return svn_error_return(err);
                svn_error_clear(err);
                /* The WORKING node is not present.  */
              }
            else
              {
                /* The WORKING node is present, and we wrote the props.  */
                written = TRUE;
              }
          }

        if (!written)
          SVN_ERR(svn_wc__db_temp_base_set_props(db, local_abspath,
                                                 base_props, scratch_pool));
    }

  /* Okay. It's time to save the ACTUAL props.  */
  SVN_ERR(svn_wc__db_op_set_props(db, local_abspath, actual_props,
                                  NULL, NULL, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_add_install_properties(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_hash_t *base_props,
                                  apr_hash_t *actual_props,
                                  apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);
  svn_skel_t *props;

  if (actual_props != NULL)
    {
      SVN_ERR(svn_skel__unparse_proplist(&props, actual_props, scratch_pool));
      svn_skel__prepend(props, work_item);
    }
  else
    svn_skel__prepend_str("", work_item, scratch_pool);

  if (base_props != NULL)
    {
      SVN_ERR(svn_skel__unparse_proplist(&props, base_props, scratch_pool));
      svn_skel__prepend(props, work_item);
    }
  else
    svn_skel__prepend_str("", work_item, scratch_pool);

  svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_INSTALL_PROPERTIES, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}

#endif /* SVN__SUPPORT_BASE_MERGE  */


/* ------------------------------------------------------------------------ */

/* OP_DELETE */

/* Process the OP_DELETE work item WORK_ITEM.
 * See svn_wc__wq_add_delete() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_delete(svn_wc__db_t *db,
           const svn_skel_t *work_item,
           svn_cancel_func_t cancel_func,
           void *cancel_baton,
           apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg = work_item->children->next;
  const char *local_abspath;
  svn_wc__db_kind_t kind;
  svn_boolean_t was_added, was_copied, was_replaced;

  local_abspath = apr_pstrmemdup(scratch_pool, arg->data, arg->len);
  arg = arg->next;
  kind = (int) svn_skel__parse_int(arg, scratch_pool);
  arg = arg->next;
  was_added = svn_skel__parse_int(arg, scratch_pool) != 0;
  arg = arg->next;
  was_copied = svn_skel__parse_int(arg, scratch_pool) != 0;
  arg = arg->next;
  was_replaced = svn_skel__parse_int(arg, scratch_pool) != 0;

  if (was_replaced && was_copied)
    {
#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
      const char *props_base, *props_revert;

      SVN_ERR(svn_wc__prop_path(&props_base, local_abspath, kind,
                                svn_wc__props_base, scratch_pool));
      SVN_ERR(svn_wc__prop_path(&props_revert, local_abspath, kind,
                                svn_wc__props_revert, scratch_pool));
      SVN_ERR(move_if_present(props_revert, props_base, scratch_pool));
#endif

#ifndef SVN_EXPERIMENTAL_PRISTINE
      if (kind != svn_wc__db_kind_dir)
        {
          const char *text_base, *text_revert;

          SVN_ERR(svn_wc__text_base_path(&text_base, db, local_abspath,
                                         scratch_pool));
          SVN_ERR(svn_wc__text_revert_path(&text_revert, db,
                                           local_abspath, scratch_pool));
          SVN_ERR(move_if_present(text_revert, text_base, scratch_pool));
        }
#endif
    }
#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  if (was_added)
    {
      const char *props_base, *props_working;

      SVN_ERR(svn_wc__prop_path(&props_base, local_abspath, kind,
                                svn_wc__props_base, scratch_pool));
      SVN_ERR(svn_wc__prop_path(&props_working, local_abspath, kind,
                                svn_wc__props_working, scratch_pool));

      SVN_ERR(svn_io_remove_file2(props_base, TRUE, scratch_pool));
      SVN_ERR(svn_io_remove_file2(props_working, TRUE, scratch_pool));
    }
#endif

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__wq_add_delete(svn_wc__db_t *db,
                      const char *parent_abspath,
                      const char *local_abspath,
                      svn_wc__db_kind_t kind,
                      svn_boolean_t was_added,
                      svn_boolean_t was_copied,
                      svn_boolean_t was_replaced,
                      apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item = svn_skel__make_empty_list(scratch_pool);

  svn_skel__prepend_int(was_replaced, work_item, scratch_pool);
  svn_skel__prepend_int(was_copied, work_item, scratch_pool);
  svn_skel__prepend_int(was_added, work_item, scratch_pool);
  svn_skel__prepend_int(kind, work_item, scratch_pool);
  svn_skel__prepend_str(local_abspath, work_item, scratch_pool);
  svn_skel__prepend_str(OP_DELETE, work_item, scratch_pool);

  SVN_ERR(svn_wc__db_wq_add(db, parent_abspath, work_item, scratch_pool));

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_FILE_INSTALL */

/* Process the OP_FILE_INSTALL work item WORK_ITEM.
 * See svn_wc__wq_build_file_install() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_file_install(svn_wc__db_t *db,
                 const svn_skel_t *work_item,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const svn_skel_t *arg4 = arg1->next->next->next;
  const char *local_abspath;
  svn_boolean_t use_commit_times;
  svn_boolean_t record_fileinfo;
  svn_boolean_t special;
  svn_stream_t *src_stream;
  svn_subst_eol_style_t style;
  const char *eol;
  apr_hash_t *keywords;
  const char *temp_dir_abspath;
  svn_stream_t *dst_stream;
  const char *dst_abspath;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  use_commit_times = svn_skel__parse_int(arg1->next, scratch_pool) != 0;
  record_fileinfo = svn_skel__parse_int(arg1->next->next, scratch_pool) != 0;

  if (arg4 == NULL)
    {
      /* Get the pristine contents (from WORKING or BASE, as appropriate).  */
      SVN_ERR(svn_wc__get_pristine_contents(&src_stream, db, local_abspath,
                                            scratch_pool, scratch_pool));
      SVN_ERR_ASSERT(src_stream != NULL);
    }
  else
    {
      const char *source_abspath;

      /* Use the provided path for the source.  */
      source_abspath = apr_pstrmemdup(scratch_pool, arg4->data, arg4->len);
      SVN_ERR(svn_stream_open_readonly(&src_stream, source_abspath,
                                       scratch_pool, scratch_pool));
    }

  SVN_ERR(svn_wc__get_special(&special, db, local_abspath, scratch_pool));
  if (special)
    {
      /* When this stream is closed, the resulting special file will
         atomically be created/moved into place at LOCAL_ABSPATH.  */
      SVN_ERR(svn_subst_create_specialfile(&dst_stream, local_abspath,
                                           scratch_pool, scratch_pool));

      /* Copy the "repository normal" form of the special file into the
         special stream.  */
      SVN_ERR(svn_stream_copy3(src_stream, dst_stream,
                               cancel_func, cancel_baton,
                               scratch_pool));

      /* No need to set exec or read-only flags on special files.  */
      return SVN_NO_ERROR;
    }

  /* Fetch all the translation bits.  */
  SVN_ERR(svn_wc__get_eol_style(&style, &eol, db, local_abspath,
                                scratch_pool, scratch_pool));
  SVN_ERR(svn_wc__get_keywords(&keywords, db, local_abspath, NULL,
                               scratch_pool, scratch_pool));

  if (svn_subst_translation_required(style, eol, keywords,
                                     FALSE /* special */,
                                     TRUE /* force_eol_check */))
    {
      /* Wrap it in a translating (expanding) stream.  */
      src_stream = svn_subst_stream_translated(src_stream, eol,
                                               TRUE /* repair */,
                                               keywords,
                                               TRUE /* expand */,
                                               scratch_pool);
    }

  /* Where is the Right Place to put a temp file in this working copy?  */
  SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&temp_dir_abspath,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));

  /* Translate to a temporary file. We don't want the user seeing a partial
     file, nor let them muck with it while we translate. We may also need to
     get its TRANSLATED_SIZE before the user can monkey it.  */
  SVN_ERR(svn_stream_open_unique(&dst_stream, &dst_abspath,
                                 temp_dir_abspath,
                                 svn_io_file_del_none,
                                 scratch_pool, scratch_pool));

  /* Copy from the source to the dest, translating as we go. This will also
     close both streams.  */
  SVN_ERR(svn_stream_copy3(src_stream, dst_stream,
                           cancel_func, cancel_baton,
                           scratch_pool));

  /* ### post-commit feature: avoid overwrite if same as working file.  */

  /* All done. Move the file into place.  */
  /* ### fix this. we should delay the rename.  */
  SVN_ERR(svn_io_file_rename(dst_abspath, local_abspath, scratch_pool));

  /* Tweak the on-disk file according to its properties.  */
  SVN_ERR(sync_file_flags(db, local_abspath, scratch_pool));

  if (use_commit_times)
    {
      apr_time_t changed_date;

      SVN_ERR(svn_wc__db_read_info(
                NULL, NULL, NULL, NULL, NULL, NULL,
                NULL, &changed_date, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                NULL, NULL,
                db, local_abspath,
                scratch_pool, scratch_pool));

      if (changed_date)
        SVN_ERR(svn_io_set_file_affected_time(changed_date,
                                              local_abspath,
                                              scratch_pool));
    }

  /* ### this should happen before we rename the file into place.  */
  if (record_fileinfo)
    {
      SVN_ERR(get_and_record_fileinfo(db, local_abspath,
                                      FALSE /* ignore_enoent */,
                                      scratch_pool));

      /* ### there used to be a call to entry_modify() above, to set the
         ### TRANSLATED_SIZE and LAST_MOD_TIME values. that function elided
         ### copyfrom information that snuck into the database. it should
         ### not be there in the first place, but we can manually get rid
         ### of the erroneous, inheritable copyfrom data.  */
      SVN_ERR(svn_wc__db_temp_elide_copyfrom(db, local_abspath, scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_build_file_install(svn_skel_t **work_item,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              const char *source_abspath,
                              svn_boolean_t use_commit_times,
                              svn_boolean_t record_fileinfo,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  *work_item = svn_skel__make_empty_list(result_pool);

  /* If a SOURCE_ABSPATH was provided, then put it into the skel. If this
     value is not provided, then the file's pristine contents will be used.  */
  if (source_abspath != NULL)
    svn_skel__prepend_str(apr_pstrdup(result_pool, source_abspath),
                          *work_item, result_pool);

  svn_skel__prepend_int(record_fileinfo, *work_item, result_pool);
  svn_skel__prepend_int(use_commit_times, *work_item, result_pool);
  svn_skel__prepend_str(apr_pstrdup(result_pool, local_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_FILE_INSTALL, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_FILE_REMOVE  */

/* Process the OP_FILE_REMOVE work item WORK_ITEM.
 * See svn_wc__wq_build_file_remove() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_file_remove(svn_wc__db_t *db,
                 const svn_skel_t *work_item,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  /* Remove the path, no worrying if it isn't there.  */
  return svn_error_return(svn_io_remove_file2(local_abspath, TRUE,
                                              scratch_pool));
}


svn_error_t *
svn_wc__wq_build_file_remove(svn_skel_t **work_item,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  *work_item = svn_skel__make_empty_list(result_pool);

  svn_skel__prepend_str(apr_pstrdup(result_pool, local_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_FILE_REMOVE, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_SYNC_FILE_FLAGS  */

/* Process the OP_SYNC_FILE_FLAGS work item WORK_ITEM.
 * See svn_wc__wq_build_sync_file_flags() which generates this work item.
 * Implements (struct work_item_dispatch).func. */
static svn_error_t *
run_sync_file_flags(svn_wc__db_t *db,
                    const svn_skel_t *work_item,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  return svn_error_return(sync_file_flags(db, local_abspath, scratch_pool));
}


svn_error_t *
svn_wc__wq_build_sync_file_flags(svn_skel_t **work_item,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool)
{
  *work_item = svn_skel__make_empty_list(result_pool);

  svn_skel__prepend_str(apr_pstrdup(result_pool, local_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_SYNC_FILE_FLAGS, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_PREJ_INSTALL  */

static svn_error_t *
run_prej_install(svn_wc__db_t *db,
                 const svn_skel_t *work_item,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;
  const svn_skel_t *conflict_skel;
  const char *tmp_prejfile_abspath;
  const char *prejfile_abspath;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);
  if (arg1->next != NULL)
    conflict_skel = arg1->next;
  else
    SVN_ERR_MALFUNCTION();  /* ### wc_db can't provide it ... yet.  */

  /* Construct a property reject file in the temporary area.  */
  SVN_ERR(svn_wc__create_prejfile(&tmp_prejfile_abspath,
                                  db, local_abspath,
                                  conflict_skel,
                                  scratch_pool, scratch_pool));

  /* Get the (stored) name of where it should go.  */
  SVN_ERR(svn_wc__get_prejfile_abspath(&prejfile_abspath, db, local_abspath,
                                       scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(prejfile_abspath != NULL);

  /* ... and atomically move it into place.  */
  SVN_ERR(svn_io_file_rename(tmp_prejfile_abspath,
                             prejfile_abspath,
                             scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_build_prej_install(svn_skel_t **work_item,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              const svn_skel_t *conflict_skel,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  *work_item = svn_skel__make_empty_list(result_pool);

  /* ### gotta have this, today  */
  SVN_ERR_ASSERT(conflict_skel != NULL);

  if (conflict_skel != NULL)
    /* ### woah! this needs to dup the skel into RESULT_POOL  */
    svn_skel__prepend((svn_skel_t *)conflict_skel, *work_item);
  svn_skel__prepend_str(apr_pstrdup(result_pool, local_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_PREJ_INSTALL, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_WRITE_OLD_PROPS  */


static svn_error_t *
run_write_old_props(svn_wc__db_t *db,
                    const svn_skel_t *work_item,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *props_abspath;
  svn_stream_t *stream;
  apr_hash_t *props;

  props_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  /* Torch whatever may be there.  */
  SVN_ERR(svn_io_remove_file2(props_abspath, TRUE, scratch_pool));

  if (arg1->next == NULL)
    {
      /* PROPS == NULL means the file should be removed. Note that an
         empty set of properties has an entirely different meaning.

         The file has already been removed. Simply exit.  */
      return SVN_NO_ERROR;
    }
  SVN_ERR(svn_skel__parse_proplist(&props, arg1->next, scratch_pool));

  SVN_ERR(svn_stream_open_writable(&stream, props_abspath,
                                   scratch_pool, scratch_pool));

  /* An empty file is shorthand for an empty set of properties.  */
  if (apr_hash_count(props) != 0)
    SVN_ERR(svn_hash_write2(props, stream, SVN_HASH_TERMINATOR, scratch_pool));

  SVN_ERR(svn_stream_close(stream));

  SVN_ERR(svn_io_set_file_read_only(props_abspath,
                                    FALSE /* ignore_enoent */,
                                    scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_build_write_old_props(svn_skel_t **work_item,
                                 const char *props_abspath,
                                 apr_hash_t *props,
                                 apr_pool_t *result_pool)
{
#if (SVN_WC__VERSION >= SVN_WC__PROPS_IN_DB)
  *work_item = NULL;
#else
  *work_item = svn_skel__make_empty_list(result_pool);

  SVN_ERR_ASSERT(svn_dirent_is_absolute(props_abspath));

  if (props != NULL)
    {
      svn_skel_t *props_skel;

      SVN_ERR(svn_skel__unparse_proplist(&props_skel, props, result_pool));
      svn_skel__prepend(props_skel, *work_item);
    }
  svn_skel__prepend_str(apr_pstrdup(result_pool, props_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_WRITE_OLD_PROPS, *work_item, result_pool);
#endif

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

/* OP_RECORD_FILEINFO  */


static svn_error_t *
run_record_fileinfo(svn_wc__db_t *db,
                    const svn_skel_t *work_item,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    apr_pool_t *scratch_pool)
{
  const svn_skel_t *arg1 = work_item->children->next;
  const char *local_abspath;

  local_abspath = apr_pstrmemdup(scratch_pool, arg1->data, arg1->len);

  return svn_error_return(get_and_record_fileinfo(db, local_abspath,
                                                  TRUE /* ignore_enoent */,
                                                  scratch_pool));
}


svn_error_t *
svn_wc__wq_build_record_fileinfo(svn_skel_t **work_item,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool)
{
  *work_item = svn_skel__make_empty_list(result_pool);

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  svn_skel__prepend_str(apr_pstrdup(result_pool, local_abspath),
                        *work_item, result_pool);
  svn_skel__prepend_str(OP_RECORD_FILEINFO, *work_item, result_pool);

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------------ */

static const struct work_item_dispatch dispatch_table[] = {
  { OP_REVERT, run_revert },
  { OP_PREPARE_REVERT_FILES, run_prepare_revert_files },
  { OP_KILLME, run_killme },
  { OP_LOGGY, run_loggy },
  { OP_DELETION_POSTCOMMIT, run_deletion_postcommit },
  { OP_POSTCOMMIT, run_postcommit },
  { OP_DELETE, run_delete },
  { OP_FILE_INSTALL, run_file_install },
  { OP_FILE_REMOVE, run_file_remove },
  { OP_SYNC_FILE_FLAGS, run_sync_file_flags },
  { OP_PREJ_INSTALL, run_prej_install },
  { OP_WRITE_OLD_PROPS, run_write_old_props },
  { OP_RECORD_FILEINFO, run_record_fileinfo },

/* See props.h  */
#ifdef SVN__SUPPORT_BASE_MERGE
  { OP_INSTALL_PROPERTIES, run_install_properties },
#endif

  /* Sentinel.  */
  { NULL }
};


static svn_error_t *
dispatch_work_item(svn_wc__db_t *db,
                   const char *wri_abspath,
                   const svn_skel_t *work_item,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *scratch_pool)
{
  const struct work_item_dispatch *scan;

  /* Scan the dispatch table for a function to handle this work item.  */
  for (scan = &dispatch_table[0]; scan->name != NULL; ++scan)
    {
      if (svn_skel__matches_atom(work_item->children, scan->name))
        {

#ifdef DEBUG_WORK_QUEUE
          SVN_DBG(("dispatch: operation='%s'\n", scan->name));
#endif
          SVN_ERR((*scan->func)(db, work_item,
                                cancel_func, cancel_baton,
                                scratch_pool));
          break;
        }
    }

  if (scan->name == NULL)
    {
      /* We should know about ALL possible work items here. If we do not,
         then something is wrong. Most likely, some kind of format/code
         skew. There is nothing more we can do. Erasing or ignoring this
         work item could leave the WC in an even more broken state.

         Contrary to issue #1581, we cannot simply remove work items and
         continue, so bail out with an error.  */
      return svn_error_createf(SVN_ERR_WC_BAD_ADM_LOG, NULL,
                               _("Unrecognized work item in the queue "
                                 "associated with '%s'"),
                               svn_dirent_local_style(wri_abspath,
                                                      scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wq_run(svn_wc__db_t *db,
               const char *wri_abspath,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

#ifdef DEBUG_WORK_QUEUE
  SVN_DBG(("wq_run: wri='%s'\n", wri_abspath));
#endif

  while (TRUE)
    {
      svn_wc__db_kind_t kind;
      apr_uint64_t id;
      svn_skel_t *work_item;

      /* Stop work queue processing, if requested. A future 'svn cleanup'
         should be able to continue the processing.  */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      svn_pool_clear(iterpool);

      /* ### right now, we expect WRI_ABSPATH to exist. this section should
         ### disappear in single-db. also, note that db_wq_fetch() will
         ### watch out for missing/obstructed subdirs (ie. wq is gone)  */
      SVN_ERR(svn_wc__db_read_kind(&kind, db, wri_abspath, TRUE,
                                   scratch_pool));
      if (kind == svn_wc__db_kind_unknown)
        break;

      SVN_ERR(svn_wc__db_wq_fetch(&id, &work_item, db, wri_abspath,
                                  iterpool, iterpool));
      if (work_item == NULL)
        break;

      SVN_ERR(dispatch_work_item(db, wri_abspath, work_item,
                                 cancel_func, cancel_baton, iterpool));

      /* The work item finished without error. Mark it completed.  */
      SVN_ERR(svn_wc__db_wq_completed(db, wri_abspath, id, iterpool));
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


svn_skel_t *
svn_wc__wq_merge(svn_skel_t *work_item1,
                 svn_skel_t *work_item2,
                 apr_pool_t *result_pool)
{
  /* If either argument is NULL, then just return the other.  */
  if (work_item1 == NULL)
    return work_item2;
  if (work_item2 == NULL)
    return work_item1;

  /* We have two items. Figure out how to join them.  */
  if (SVN_WC__SINGLE_WORK_ITEM(work_item1))
    {
      if (SVN_WC__SINGLE_WORK_ITEM(work_item2))
        {
          /* Both are singular work items. Construct a list, then put
             both work items into it (in the proper order).  */

          svn_skel_t *result = svn_skel__make_empty_list(result_pool);

          svn_skel__prepend(work_item2, result);
          svn_skel__prepend(work_item1, result);
          return result;
        }

      /* WORK_ITEM2 is a list of work items. We can simply shove WORK_ITEM1
         in the front to keep the ordering.  */
      svn_skel__prepend(work_item1, work_item2);
      return work_item2;
    }
  /* WORK_ITEM1 is a list of work items.  */

  if (SVN_WC__SINGLE_WORK_ITEM(work_item2))
    {
      /* Put WORK_ITEM2 onto the end of the WORK_ITEM1 list.  */
      svn_skel__append(work_item1, work_item2);
      return work_item1;
    }

  /* We have two lists of work items. We need to chain all of the work
     items into one big list. We will leave behind the WORK_ITEM2 skel,
     as we only want its children.  */
  svn_skel__append(work_item1, work_item2->children);
  return work_item1;
}
