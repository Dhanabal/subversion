/*
 * entries.c :  manipulating the administrative `entries' file.
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

#include <string.h>
#include <assert.h>

#include <apr_strings.h>

#include "svn_error.h"
#include "svn_types.h"
#include "svn_time.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_ctype.h"
#include "svn_string.h"

#include "wc.h"
#include "adm_files.h"
#include "entries.h"
#include "lock.h"
#include "tree_conflicts.h"
#include "wc_db.h"
#include "wc-queries.h"  /* for STMT_*  */

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_sqlite.h"

#define MAYBE_ALLOC(x,p) ((x) ? (x) : apr_pcalloc((p), sizeof(*(x))))


/* Temporary structures which mirror the tables in wc-metadata.sql.
   For detailed descriptions of each field, see that file. */
typedef struct {
  apr_int64_t wc_id;
  const char *local_relpath;
  apr_int64_t repos_id;
  const char *repos_relpath;
  const char *parent_relpath;
  svn_wc__db_status_t presence;
  svn_revnum_t revision;
  svn_node_kind_t kind;  /* ### should switch to svn_wc__db_kind_t */
  svn_checksum_t *checksum;
  svn_filesize_t translated_size;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  svn_depth_t depth;
  apr_time_t last_mod_time;
  apr_hash_t *properties;
} db_base_node_t;

typedef struct {
  apr_int64_t wc_id;
  const char *local_relpath;
  const char *parent_relpath;
  svn_wc__db_status_t presence;
  svn_node_kind_t kind;  /* ### should switch to svn_wc__db_kind_t */
  apr_int64_t copyfrom_repos_id;
  const char *copyfrom_repos_path;
  svn_revnum_t copyfrom_revnum;
  svn_boolean_t moved_here;
  const char *moved_to;
  svn_checksum_t *checksum;
  svn_filesize_t translated_size;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  svn_depth_t depth;
  apr_time_t last_mod_time;
  apr_hash_t *properties;
  svn_boolean_t keep_local;
} db_working_node_t;

typedef struct {
  apr_int64_t wc_id;
  const char *local_relpath;
  const char *parent_relpath;
  apr_hash_t *properties;
  const char *conflict_old;
  const char *conflict_new;
  const char *conflict_working;
  const char *prop_reject;
  const char *changelist;
  /* ### enum for text_mod */
  const char *tree_conflict_data;
} db_actual_node_t;



/*** reading and writing the entries file ***/


/* */
static svn_wc_entry_t *
alloc_entry(apr_pool_t *pool)
{
  svn_wc_entry_t *entry = apr_pcalloc(pool, sizeof(*entry));
  entry->revision = SVN_INVALID_REVNUM;
  entry->copyfrom_rev = SVN_INVALID_REVNUM;
  entry->cmt_rev = SVN_INVALID_REVNUM;
  entry->kind = svn_node_none;
  entry->working_size = SVN_WC_ENTRY_WORKING_SIZE_UNKNOWN;
  entry->depth = svn_depth_infinity;
  entry->file_external_path = NULL;
  entry->file_external_peg_rev.kind = svn_opt_revision_unspecified;
  entry->file_external_rev.kind = svn_opt_revision_unspecified;
  return entry;
}


/* Is the entry in a 'hidden' state in the sense of the 'show_hidden'
 * switches on svn_wc_entries_read(), svn_wc_walk_entries*(), etc.? */
svn_error_t *
svn_wc__entry_is_hidden(svn_boolean_t *hidden, const svn_wc_entry_t *entry)
{
  /* In English, the condition is: "the entry is not present, and I haven't
     scheduled something over the top of it."  */
  if (entry->deleted
      || entry->absent
      || entry->depth == svn_depth_exclude)
    {
      /* These kinds of nodes cannot be marked for deletion (which also
         means no "replace" either).  */
      SVN_ERR_ASSERT(entry->schedule == svn_wc_schedule_add
                     || entry->schedule == svn_wc_schedule_normal);

      /* Hidden if something hasn't been added over it.

         ### is this even possible with absent or excluded nodes?  */
      *hidden = entry->schedule != svn_wc_schedule_add;
    }
  else
    *hidden = FALSE;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
fetch_wc_id(apr_int64_t *wc_id, svn_sqlite__db_t *sdb)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_WCROOT_NULL));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_create(SVN_ERR_WC_DB_ERROR, NULL, _("No WC table entry"));
  *wc_id = svn_sqlite__column_int(stmt, 0);
  return svn_error_return(svn_sqlite__reset(stmt));
}


/* Hit the database to check the file external information for the given
   entry.  The entry will be modified in place. */
static svn_error_t *
check_file_external(svn_wc_entry_t *entry,
                    svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  const char *serialized;

  SVN_ERR(svn_wc__db_temp_get_file_external(&serialized,
                                            db, local_abspath,
                                            scratch_pool, scratch_pool));
  if (serialized != NULL)
    {
      SVN_ERR(svn_wc__unserialize_file_external(
                    &entry->file_external_path,
                    &entry->file_external_peg_rev,
                    &entry->file_external_rev,
                    serialized,
                    result_pool));
    }

  return SVN_NO_ERROR;
}


/* Fill in the following fields of ENTRY:

     REVISION
     REPOS
     UUID
     CMT_REV
     CMT_DATE
     CMT_AUTHOR
     TEXT_TIME
     DEPTH
     WORKING_SIZE
     COPIED

   Return: KIND, REPOS_RELPATH, CHECKSUM
*/
static svn_error_t *
get_base_info_for_deleted(svn_wc_entry_t *entry,
                          svn_wc__db_kind_t *kind,
                          const char **repos_relpath,
                          const svn_checksum_t **checksum,
                          svn_wc__db_t *db,
                          const char *entry_abspath,
                          const svn_wc_entry_t *parent_entry,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  /* Get the information from the underlying BASE node.  */
  err = svn_wc__db_base_get_info(NULL, kind,
                                 &entry->revision,
                                 NULL, NULL, NULL,
                                 &entry->cmt_rev,
                                 &entry->cmt_date,
                                 &entry->cmt_author,
                                 &entry->text_time,
                                 &entry->depth,
                                 checksum,
                                 &entry->working_size,
                                 NULL,
                                 NULL,
                                 db,
                                 entry_abspath,
                                 result_pool,
                                 scratch_pool);
  /* SVN_EXPERIMENTAL_PRISTINE:
     *checksum is originally MD-5 but will later be SHA-1.  That's OK here -
     we are just returning what is stored. */

  if (err)
    {
      const char *work_del_abspath;
      const char *parent_repos_relpath;
      const char *parent_abspath;
      svn_wc__db_status_t parent_status;

      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      /* No base node? This is a deleted child of a copy/move-here,
         so we need to scan up the WORKING tree to find the root of
         the deletion. Then examine its parent to discover its
         future location in the repository.  */
      svn_error_clear(err);

      SVN_ERR(svn_wc__db_scan_deletion(NULL,
                                       NULL,
                                       NULL,
                                       &work_del_abspath,
                                       db, entry_abspath,
                                       scratch_pool, scratch_pool));

      SVN_ERR_ASSERT(work_del_abspath != NULL);
      parent_abspath = svn_dirent_dirname(work_del_abspath, scratch_pool);

      /* During post-commit the parent that was previously added may
         have been moved from the WORKING tree to the BASE tree.  */
      SVN_ERR(svn_wc__db_read_info(&parent_status, NULL, NULL,
                                   &parent_repos_relpath,
                                   &entry->repos, &entry->uuid,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   db, parent_abspath,
                                   scratch_pool, scratch_pool));
      if (parent_status == svn_wc__db_status_added
          || parent_status == svn_wc__db_status_obstructed_add)
        SVN_ERR(svn_wc__db_scan_addition(NULL, NULL,
                                         &parent_repos_relpath,
                                         &entry->repos,
                                         &entry->uuid,
                                         NULL, NULL, NULL, NULL,
                                         db, parent_abspath,
                                         result_pool, scratch_pool));

      /* Now glue it all together */
      *repos_relpath = svn_relpath_join(
        parent_repos_relpath,
        svn_dirent_is_child(parent_abspath,
                            entry_abspath,
                            NULL),
        result_pool);
    }
  else
    {
      SVN_ERR(svn_wc__db_scan_base_repos(repos_relpath,
                                         &entry->repos,
                                         &entry->uuid,
                                         db,
                                         entry_abspath,
                                         result_pool,
                                         scratch_pool));
    }

  /* Do some extra work for the child nodes.  */
  if (parent_entry != NULL)
    {
      /* For child nodes without a revision, pick up the parent's
         revision.  */
      if (!SVN_IS_VALID_REVNUM(entry->revision))
        entry->revision = parent_entry->revision;
    }

  /* For deleted nodes, our COPIED flag has a rather complex meaning.

     In general, COPIED means "an operation on an ancestor took care
     of me." This typically refers to a copy of an ancestor (and
     this node just came along for the ride). However, in certain
     situations the COPIED flag is set for deleted nodes.

     First off, COPIED will *never* be set for nodes/subtrees that
     are simply deleted. The deleted node/subtree *must* be under
     an ancestor that has been copied. Plain additions do not count;
     only copies (add-with-history).

     The basic algorithm to determine whether we live within a
     copied subtree is as follows:

     1) find the root of the deletion operation that affected us
     (we may be that root, or an ancestor was deleted and took
     us with it)

     2) look at the root's *parent* and determine whether that was
     a copy or a simple add.

     It would appear that we would be done at this point. Once we
     determine that the parent was copied, then we could just set
     the COPIED flag.

     Not so fast. Back to the general concept of "an ancestor
     operation took care of me." Further consider two possibilities:

     1) this node is scheduled for deletion from the copied subtree,
     so at commit time, we copy then delete

     2) this node is scheduled for deletion because a subtree was
     deleted and then a copied subtree was added (causing a
     replacement). at commit time, we delete a subtree, and then
     copy a subtree. we do not need to specifically touch this
     node -- all operations occur on ancestors.

     Given the "ancestor operation" concept, then in case (1) we
     must *clear* the COPIED flag since we'll have more work to do.
     In case (2), we *set* the COPIED flag to indicate that no
     real work is going to happen on this node.

     Great fun. And just maybe the code reading the entries has no
     bugs in interpreting that gobbledygook... but that *is* the
     expectation of the code. Sigh.

     We can get a little bit of shortcut here if THIS_DIR is
     also schduled for deletion.
  */
  if (parent_entry != NULL
      && parent_entry->schedule == svn_wc_schedule_delete)
    {
      /* ### not entirely sure that we can rely on the parent. for
         ### example, what if we are a deletion of a BASE node, but
         ### the parent is a deletion of a copied subtree? sigh.  */

      /* Child nodes simply inherit the parent's COPIED flag.  */
      entry->copied = parent_entry->copied;
    }
  else
    {
      svn_boolean_t base_replaced;
      const char *work_del_abspath;

      /* Find out details of our deletion.  */
      SVN_ERR(svn_wc__db_scan_deletion(NULL,
                                       &base_replaced,
                                       NULL,
                                       &work_del_abspath,
                                       db, entry_abspath,
                                       scratch_pool, scratch_pool));

      /* If there is no deletion in the WORKING tree, then the
         node is a child of a simple explicit deletion of the
         BASE tree. It certainly isn't copied. If we *do* find
         a deletion in the WORKING tree, then we need to discover
         information about the parent.  */
      if (work_del_abspath != NULL)
        {
          const char *parent_abspath;
          svn_wc__db_status_t parent_status;

          /* The parent is in WORKING except during post-commit when
             it may have been moved from the WORKING tree to the BASE
             tree.  */
          parent_abspath = svn_dirent_dirname(work_del_abspath,
                                              scratch_pool);
          SVN_ERR(svn_wc__db_read_info(&parent_status,
                                       NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       db, parent_abspath,
                                       scratch_pool, scratch_pool));
          if (parent_status == svn_wc__db_status_added
              || parent_status == svn_wc__db_status_obstructed_add)
            SVN_ERR(svn_wc__db_scan_addition(&parent_status,
                                             NULL,
                                             NULL, NULL, NULL,
                                             NULL, NULL, NULL, NULL,
                                             db,
                                             parent_abspath,
                                             scratch_pool, scratch_pool));
          if (parent_status == svn_wc__db_status_copied
              || parent_status == svn_wc__db_status_moved_here
              || parent_status == svn_wc__db_status_normal)
            {
              /* The parent is copied/moved here, so WORK_DEL_ABSPATH
                 is the root of a deleted subtree. Our COPIED status
                 is now dependent upon whether the copied root is
                 replacing a BASE tree or not.

                 But: if we are schedule-delete as a result of being
                 a copied DELETED node, then *always* mark COPIED.
                 Normal copies have cmt_* data; copied DELETED nodes
                 are missing this info.

                 Note: MOVED_HERE is a concept foreign to this old
                 interface, but it is best represented as if a copy
                 had occurred, so we'll model it that way to old
                 clients.

                 Note: svn_wc__db_status_normal corresponds to the
                 post-commit parent that was copied or moved in
                 WORKING but has now been converted to BASE.
              */
              if (SVN_IS_VALID_REVNUM(entry->cmt_rev))
                {
                  /* The scan_deletion call will tell us if there
                     was an explicit move-away of an ancestor (which
                     also means a replacement has occurred since
                     there is a WORKING tree that isn't simply
                     BASE deletions). The call will also tell us if
                     there was an implicit deletion caused by a
                     replacement. All stored in BASE_REPLACED.  */
                  entry->copied = base_replaced;
                }
              else
                {
                  entry->copied = TRUE;
                }
            }
          else
            {
              SVN_ERR_ASSERT(parent_status == svn_wc__db_status_added);

              /* Whoops. WORK_DEL_ABSPATH is scheduled for deletion,
                 yet the parent is scheduled for a plain addition.
                 This can occur when a subtree is deleted, and then
                 nodes are added *later*. Since the parent is a simple
                 add, then nothing has been copied. Nothing more to do.

                 Note: if a subtree is added, *then* deletions are
                 made, the nodes should simply be removed from
                 version control.  */
            }
        }
    }

  return SVN_NO_ERROR;
}


/* Read one entry from wc_db. It will be allocated in RESULT_POOL and
   returned in *NEW_ENTRY.

   DIR_ABSPATH is the name of the directory to read this entry from, and
   it will be named NAME (use "" for "this dir").

   DB specifies the wc_db database, and WC_ID specifies which working copy
   this information is being read from.

   If this node is "this dir", then PARENT_ENTRY should be NULL. Otherwise,
   it should refer to the entry for the child's parent directory.

   Temporary allocations are made in SCRATCH_POOL.  */
static svn_error_t *
read_one_entry(const svn_wc_entry_t **new_entry,
               svn_wc__db_t *db,
               apr_uint64_t wc_id,
               const char *dir_abspath,
               const char *name,
               const svn_wc_entry_t *parent_entry,
               apr_pool_t *result_pool,
               apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
  svn_wc__db_lock_t *lock;
  const char *repos_relpath;
  const svn_checksum_t *checksum;
  svn_filesize_t translated_size;
  svn_wc_entry_t *entry = alloc_entry(result_pool);
  const char *entry_abspath;
  const char *original_repos_relpath;
  const char *original_root_url;
  svn_boolean_t conflicted;
  svn_boolean_t base_shadowed;

  entry->name = name;

  entry_abspath = svn_dirent_join(dir_abspath, entry->name, scratch_pool);

  SVN_ERR(svn_wc__db_read_info(
            &status,
            &kind,
            &entry->revision,
            &repos_relpath,
            &entry->repos,
            &entry->uuid,
            &entry->cmt_rev,
            &entry->cmt_date,
            &entry->cmt_author,
            &entry->text_time,
            &entry->depth,
            &checksum,
            &translated_size,
            NULL,
            &entry->changelist,
            &original_repos_relpath,
            &original_root_url,
            NULL,
            &entry->copyfrom_rev,
            NULL,
            NULL,
            &base_shadowed,
            &conflicted,
            &lock,
            db,
            entry_abspath,
            result_pool,
            scratch_pool));

  if (strcmp(entry->name, SVN_WC_ENTRY_THIS_DIR) == 0)
    {
      /* get the tree conflict data. */
      apr_hash_t *tree_conflicts = NULL;
      const apr_array_header_t *conflict_victims;
      int k;

      SVN_ERR(svn_wc__db_read_conflict_victims(&conflict_victims, db,
                                               dir_abspath,
                                               scratch_pool,
                                               scratch_pool));

      for (k = 0; k < conflict_victims->nelts; k++)
        {
          int j;
          const apr_array_header_t *child_conflicts;
          const char *child_name;
          const char *child_abspath;

          child_name = APR_ARRAY_IDX(conflict_victims, k, const char *);
          child_abspath = svn_dirent_join(dir_abspath, child_name,
                                          scratch_pool);

          SVN_ERR(svn_wc__db_read_conflicts(&child_conflicts,
                                            db, child_abspath,
                                            scratch_pool, scratch_pool));

          for (j = 0; j < child_conflicts->nelts; j++)
            {
              const svn_wc_conflict_description2_t *conflict =
                APR_ARRAY_IDX(child_conflicts, j,
                              svn_wc_conflict_description2_t *);

              if (conflict->kind == svn_wc_conflict_kind_tree)
                {
                  if (!tree_conflicts)
                    tree_conflicts = apr_hash_make(scratch_pool);
                  apr_hash_set(tree_conflicts, child_name,
                               APR_HASH_KEY_STRING, conflict);
                }
            }
        }

      if (tree_conflicts)
        {
          SVN_ERR(svn_wc__write_tree_conflicts(&entry->tree_conflict_data,
                                               tree_conflicts,
                                               result_pool));
        }
    }

  if (status == svn_wc__db_status_normal
      || status == svn_wc__db_status_incomplete)
    {
      svn_boolean_t have_row = FALSE;

      /* Ugh. During a checkout, it is possible that we are constructing
         a subdirectory "over" a not-present directory. The read_info()
         will return information out of the wc.db in the subdir. We
         need to detect this situation and create a DELETED entry
         instead.  */
      if (kind == svn_wc__db_kind_dir)
        {
          svn_sqlite__db_t *sdb;
          svn_sqlite__stmt_t *stmt;

          SVN_ERR(svn_wc__db_temp_borrow_sdb(
                    &sdb, db, dir_abspath,
                    svn_wc__db_openmode_readonly,
                    scratch_pool));

          SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                            STMT_SELECT_NOT_PRESENT));
          SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, entry->name));
          SVN_ERR(svn_sqlite__step(&have_row, stmt));
          SVN_ERR(svn_sqlite__reset(stmt));
        }

      if (have_row)
        {
          /* Just like a normal "not-present" node: schedule=normal
             and DELETED.  */
          entry->schedule = svn_wc_schedule_normal;
          entry->deleted = TRUE;
        }
      else
        {
          /* Plain old BASE node.  */
          entry->schedule = svn_wc_schedule_normal;

          /* Grab inherited repository information, if necessary. */
          if (repos_relpath == NULL)
            {
              SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath,
                                                 &entry->repos,
                                                 &entry->uuid,
                                                 db,
                                                 entry_abspath,
                                                 result_pool,
                                                 scratch_pool));
            }

          entry->incomplete = (status == svn_wc__db_status_incomplete);
        }
    }
  else if (status == svn_wc__db_status_deleted
           || status == svn_wc__db_status_obstructed_delete)
    {
      /* ### we don't have to worry about moves, so this is a delete. */
      entry->schedule = svn_wc_schedule_delete;

      /* ### keep_local ... ugh. hacky.  */
      /* We only read keep_local in the directory itself, because we
         can't rely on the actual record being available in the parent
         stub when the directory is recorded as deleted in the directory
         itself. (This last value is the status that brought us in this
         if block).

         This is safe because we will only write this flag in the
         directory itself (see mark_deleted() in adm_ops.c), and also
         because we will never use keep_local in the final version of
         WC-NG. With a central db and central pristine store we can
         remove working copy directories directly. So any left over
         directories after the delete operation are always kept locally.
      */
      if (*entry->name == '\0')
        SVN_ERR(svn_wc__db_temp_determine_keep_local(&entry->keep_local,
                                                     db, entry_abspath,
                                                     scratch_pool));
    }
  else if (status == svn_wc__db_status_added
           || status == svn_wc__db_status_obstructed_add)
    {
      svn_wc__db_status_t work_status;
      const char *op_root_abspath;
      const char *scanned_original_relpath;
      svn_revnum_t original_revision;

      /* For child nodes, pick up the parent's revision.  */
      if (*entry->name != '\0')
        {
          assert(parent_entry != NULL);
          assert(entry->revision == SVN_INVALID_REVNUM);

          entry->revision = parent_entry->revision;
        }

      if (base_shadowed)
        {
          svn_wc__db_status_t base_status;

          /* ENTRY->REVISION is overloaded. When a node is schedule-add
             or -replace, then REVISION refers to the BASE node's revision
             that is being overwritten. We need to fetch it now.  */
          SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL,
                                           &entry->revision,
                                           NULL, NULL, NULL,
                                           NULL, NULL, NULL,
                                           NULL, NULL, NULL,
                                           NULL, NULL, NULL,
                                           db, entry_abspath,
                                           scratch_pool,
                                           scratch_pool));

          if (base_status == svn_wc__db_status_not_present)
            {
              /* The underlying node is DELETED in this revision.  */
              entry->deleted = TRUE;

              /* This is an add since there isn't a node to replace.  */
              entry->schedule = svn_wc_schedule_add;
            }
          else
            entry->schedule = svn_wc_schedule_replace;
        }
      else
        {
          /* If we are reading child directories, then we need to
             correctly populate the DELETED flag. WC_DB normally
             wants to provide all of a directory's metadata from
             its own area. But this information is stored only in
             the parent directory, so we need to call a custom API
             to fetch this value.

             ### we should start generating BASE_NODE rows for THIS_DIR
             ### in the subdir. future step because it is harder.  */
          if (kind == svn_wc__db_kind_dir && *entry->name != '\0')
            {
              SVN_ERR(svn_wc__db_temp_is_dir_deleted(&entry->deleted,
                                                     &entry->revision,
                                                     db, entry_abspath,
                                                     scratch_pool));
            }
          if (entry->deleted)
            {
              /* There was a DELETED marker in the parent, meaning
                 that we truly are shadowing a base node. It isn't
                 called a 'replace' though (the BASE is pretending
                 not to exist).  */
              entry->schedule = svn_wc_schedule_add;
            }
          else
            {
              /* There was NOT a 'not-present' BASE_NODE in the parent
                 directory. And there is no BASE_NODE in this directory.
                 Therefore, we are looking at some kind of add/copy
                 rather than a replace.  */

              /* ### if this looks like a plain old add, then rev=0.  */
              if (!SVN_IS_VALID_REVNUM(entry->copyfrom_rev)
                  && !SVN_IS_VALID_REVNUM(entry->cmt_rev))
                entry->revision = 0;

              if (status == svn_wc__db_status_obstructed_add)
                entry->revision = SVN_INVALID_REVNUM;

              /* ### when we're reading a directory that is not present,
                 ### then it must be "normal" rather than "add".  */
              if (*entry->name == '\0'
                  && status == svn_wc__db_status_obstructed_add)
                entry->schedule = svn_wc_schedule_normal;
              else
                entry->schedule = svn_wc_schedule_add;
            }
        }

      /* If we don't have "real" data from the entry (obstruction),
         then we cannot begin a scan for data. The original node may
         have important data. Set up stuff to kill that idea off,
         and finish up this entry.  */
      if (status == svn_wc__db_status_obstructed_add)
        {
          entry->cmt_rev = SVN_INVALID_REVNUM;
          work_status = svn_wc__db_status_normal;
          scanned_original_relpath = NULL;
        }
      else
        {
          SVN_ERR(svn_wc__db_scan_addition(&work_status,
                                           &op_root_abspath,
                                           &repos_relpath,
                                           &entry->repos,
                                           &entry->uuid,
                                           &scanned_original_relpath,
                                           NULL, NULL, /* original_root|uuid */
                                           &original_revision,
                                           db,
                                           entry_abspath,
                                           result_pool, scratch_pool));
        }

      if (!SVN_IS_VALID_REVNUM(entry->cmt_rev)
          && scanned_original_relpath == NULL)
        {
          /* There is NOT a last-changed revision (last-changed date and
             author may be unknown, but we can always check the rev).
             The absence of a revision implies this node was added WITHOUT
             any history. Avoid the COPIED checks in the else block.  */
          /* ### scan_addition may need to be updated to avoid returning
             ### status_copied in this case.  */
        }
      else if (work_status == svn_wc__db_status_copied)
        {
          entry->copied = TRUE;

          /* If this is a child of a copied subtree, then it should be
             schedule_normal.  */
          if (original_repos_relpath == NULL)
            {
              /* ### what if there is a BASE node under there? */
              entry->schedule = svn_wc_schedule_normal;
            }

          /* Copied nodes need to mirror their copyfrom_rev, if they
             don't have a revision of their own already. */
          if (!SVN_IS_VALID_REVNUM(entry->revision)
              || entry->revision == 0 /* added */)
            entry->revision = original_revision;
        }

      /* Does this node have copyfrom_* information?  */
      if (scanned_original_relpath != NULL)
        {
          svn_boolean_t is_copied_child;
          svn_boolean_t is_mixed_rev = FALSE;

          SVN_ERR_ASSERT(work_status == svn_wc__db_status_copied);

          /* If this node inherits copyfrom information from an
             ancestor node, then it must be a copied child.  */
          is_copied_child = (original_repos_relpath == NULL);

          /* If this node has copyfrom information on it, then it may
             be an actual copy-root, or it could be participating in
             a mixed-revision copied tree. So if we don't already know
             this is a copied child, then we need to look for this
             mixed-revision situation.  */
          if (!is_copied_child)
            {
              const char *parent_abspath;
              svn_error_t *err;
              const char *parent_repos_relpath;
              const char *parent_root_url;

              /* When we insert entries into the database, we will
                 construct additional copyfrom records for mixed-revision
                 copies. The old entries would simply record the different
                 revision in the entry->revision field. That is not
                 available within wc-ng, so additional copies are made
                 (see the logic inside write_entry()). However, when
                 reading these back *out* of the database, the additional
                 copies look like new "Added" nodes rather than a simple
                 mixed-rev working copy.

                 That would be a behavior change if we did not compensate.
                 If there is copyfrom information for this node, then the
                 code below looks at the parent to detect if it *also* has
                 copyfrom information, and if the copyfrom_url would align
                 properly. If it *does*, then we omit storing copyfrom_url
                 and copyfrom_rev (ie. inherit the copyfrom info like a
                 normal child), and update entry->revision with the
                 copyfrom_rev in order to (re)create the mixed-rev copied
                 subtree that was originally presented for storage.  */

              /* Get the copyfrom information from our parent.

                 Note that the parent could be added/copied/moved-here.
                 There is no way for it to be deleted/moved-away and
                 have *this* node appear as copied.  */
              parent_abspath = svn_dirent_dirname(entry_abspath,
                                                  scratch_pool);
              err = svn_wc__db_scan_addition(NULL,
                                             &op_root_abspath,
                                             NULL, NULL, NULL,
                                             &parent_repos_relpath,
                                             &parent_root_url,
                                             NULL, NULL,
                                             db,
                                             parent_abspath,
                                             scratch_pool,
                                             scratch_pool);
              if (err)
                {
                  if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
                    return svn_error_return(err);
                  svn_error_clear(err);
                }
              else if (parent_root_url != NULL
                       && strcmp(original_root_url, parent_root_url) == 0)
                {
                  const char *relpath_to_entry = svn_dirent_is_child(
                    op_root_abspath, entry_abspath, NULL);
                  const char *entry_repos_relpath = svn_relpath_join(
                    parent_repos_relpath, relpath_to_entry, scratch_pool);

                  /* The copyfrom repos roots matched.

                     Now we look to see if the copyfrom path of the parent
                     would align with our own path. If so, then it means
                     this copyfrom was spontaneously created and inserted
                     for mixed-rev purposes and can be eliminated without
                     changing the semantics of a mixed-rev copied subtree.

                     See notes/api-errata/wc003.txt for some additional
                     detail, and potential issues.  */
                  if (strcmp(entry_repos_relpath,
                             original_repos_relpath) == 0)
                    {
                      is_copied_child = TRUE;
                      is_mixed_rev = TRUE;
                    }
                }
            }

          if (is_copied_child)
            {
              /* We won't be settig the  copyfrom_url, yet need to
                 clear out the copyfrom_rev. Thus, this node becomes a
                 child of a copied subtree (rather than its own root).  */
              entry->copyfrom_rev = SVN_INVALID_REVNUM;

              /* Children in a copied subtree are schedule normal
                 since we don't plan to actually *do* anything with
                 them. Their operation is implied by ancestors.  */
              entry->schedule = svn_wc_schedule_normal;

              /* And *finally* we turn this entry into the mixed
                 revision node that it was intended to be. This
                 node's revision is taken from the copyfrom record
                 that we spontaneously constructed.  */
              if (is_mixed_rev)
                entry->revision = original_revision;
            }
          else if (original_repos_relpath != NULL)
            {
              entry->copyfrom_url =
                svn_path_url_add_component2(original_root_url,
                                            original_repos_relpath,
                                            result_pool);
            }
          else
            {
              /* NOTE: if original_repos_relpath == NULL, then the
                 second call to scan_addition() will not have occurred.
                 Thus, this use of OP_ROOT_ABSPATH still contains the
                 original value where we fetched a value for
                 SCANNED_REPOS_RELPATH.  */
              const char *relpath_to_entry = svn_dirent_is_child(
                op_root_abspath, entry_abspath, NULL);
              const char *entry_repos_relpath = svn_relpath_join(
                scanned_original_relpath, relpath_to_entry, scratch_pool);

              entry->copyfrom_url =
                svn_path_url_add_component2(original_root_url,
                                            entry_repos_relpath,
                                            result_pool);
            }
        }
    }
  else if (status == svn_wc__db_status_not_present)
    {
      /* ### buh. 'deleted' nodes are actually supposed to be
         ### schedule "normal" since we aren't going to actually *do*
         ### anything to this node at commit time.  */
      entry->schedule = svn_wc_schedule_normal;
      entry->deleted = TRUE;
    }
  else if (status == svn_wc__db_status_obstructed)
    {
      /* ### set some values that should (hopefully) let this directory
         ### be usable.  */
      entry->revision = SVN_INVALID_REVNUM;
    }
  else if (status == svn_wc__db_status_absent)
    {
      entry->absent = TRUE;
    }
  else if (status == svn_wc__db_status_excluded)
    {
      entry->schedule = svn_wc_schedule_normal;
      entry->depth = svn_depth_exclude;
    }
  else
    {
      /* ### we should have handled all possible status values.  */
      SVN_ERR_MALFUNCTION();
    }

  /* ### higher levels want repos information about deleted nodes, even
     ### tho they are not "part of" a repository any more.  */
  if (entry->schedule == svn_wc_schedule_delete)
    {
      SVN_ERR(get_base_info_for_deleted(entry,
                                        &kind,
                                        &repos_relpath,
                                        &checksum,
                                        db, entry_abspath,
                                        parent_entry,
                                        result_pool, scratch_pool));
    }

  /* ### default to the infinite depth if we don't know it. */
  if (entry->depth == svn_depth_unknown)
    entry->depth = svn_depth_infinity;

  if (kind == svn_wc__db_kind_dir)
    entry->kind = svn_node_dir;
  else if (kind == svn_wc__db_kind_file)
    entry->kind = svn_node_file;
  else if (kind == svn_wc__db_kind_symlink)
    entry->kind = svn_node_file;  /* ### no symlink kind */
  else
    entry->kind = svn_node_unknown;

  /* We should always have a REPOS_RELPATH, except for:
     - deleted nodes
     - certain obstructed nodes
     - not-present nodes
     - absent nodes
     - excluded nodes

     ### the last three should probably have an "implied" REPOS_RELPATH
  */
  SVN_ERR_ASSERT(repos_relpath != NULL
                 || entry->schedule == svn_wc_schedule_delete
                 || status == svn_wc__db_status_obstructed
                 || status == svn_wc__db_status_obstructed_add
                 || status == svn_wc__db_status_obstructed_delete
                 || status == svn_wc__db_status_not_present
                 || status == svn_wc__db_status_absent
                 || status == svn_wc__db_status_excluded
                 );
  if (repos_relpath)
    entry->url = svn_path_url_add_component2(entry->repos,
                                             repos_relpath,
                                             result_pool);

  if (checksum)
    {
      /* SVN_EXPERIMENTAL_PRISTINE:
         If we got a SHA-1, get the corresponding MD-5. */
      if (checksum->kind != svn_checksum_md5)
        SVN_ERR(svn_wc__db_pristine_get_md5(&checksum, db,
                                            entry_abspath, checksum,
                                            scratch_pool, scratch_pool));

      SVN_ERR_ASSERT(checksum->kind == svn_checksum_md5);
      entry->checksum = svn_checksum_to_cstring(checksum, result_pool);
    }

  if (conflicted)
    {
      const apr_array_header_t *conflicts;
      int j;
      SVN_ERR(svn_wc__db_read_conflicts(&conflicts, db, entry_abspath,
                                        scratch_pool, scratch_pool));

      for (j = 0; j < conflicts->nelts; j++)
        {
          const svn_wc_conflict_description2_t *cd;
          cd = APR_ARRAY_IDX(conflicts, j,
                             const svn_wc_conflict_description2_t *);

          switch (cd->kind)
            {
            case svn_wc_conflict_kind_text:
              entry->conflict_old = apr_pstrdup(result_pool,
                                                cd->base_file);
              entry->conflict_new = apr_pstrdup(result_pool,
                                                cd->their_file);
              entry->conflict_wrk = apr_pstrdup(result_pool,
                                                cd->my_file);
              break;
            case svn_wc_conflict_kind_property:
              entry->prejfile = apr_pstrdup(result_pool,
                                            cd->their_file);
              break;
            case svn_wc_conflict_kind_tree:
              break;
            }
        }
    }

  if (lock)
    {
      entry->lock_token = lock->token;
      entry->lock_owner = lock->owner;
      entry->lock_comment = lock->comment;
      entry->lock_creation_date = lock->date;
    }

  /* Let's check for a file external.
     ### right now this is ugly, since we have no good way querying
     ### for a file external OR retrieving properties.  ugh.  */
  if (entry->kind == svn_node_file)
    SVN_ERR(check_file_external(entry, db, entry_abspath, result_pool,
                                scratch_pool));

  entry->working_size = translated_size;

  *new_entry = entry;

  return SVN_NO_ERROR;
}

/* Read entries for PATH/LOCAL_ABSPATH from DB. The entries
   will be allocated in RESULT_POOL, with temporary allocations in
   SCRATCH_POOL. The entries are returned in RESULT_ENTRIES.  */
static svn_error_t *
read_entries_new(apr_hash_t **result_entries,
                 svn_wc__db_t *db,
                 const char *local_abspath,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  apr_hash_t *entries;
  const apr_array_header_t *children;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;
  const svn_wc_entry_t *parent_entry;
  apr_uint64_t wc_id = 1;  /* ### hacky. should remove.  */

  entries = apr_hash_make(result_pool);

  SVN_ERR(read_one_entry(&parent_entry, db, wc_id, local_abspath,
                         "" /* name */,
                         NULL /* parent_entry */,
                         result_pool, iterpool));
  apr_hash_set(entries, "", APR_HASH_KEY_STRING, parent_entry);

  /* Use result_pool so that the child names (used by reference, rather
     than copied) appear in result_pool.  */
  SVN_ERR(svn_wc__db_read_children(&children, db,
                                   local_abspath,
                                   result_pool, iterpool));
  for (i = children->nelts; i--; )
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const svn_wc_entry_t *entry;

      svn_pool_clear(iterpool);

      SVN_ERR(read_one_entry(&entry,
                             db, wc_id, local_abspath, name, parent_entry,
                             result_pool, iterpool));
      apr_hash_set(entries, entry->name, APR_HASH_KEY_STRING, entry);
    }

  svn_pool_destroy(iterpool);

  *result_entries = entries;

  return SVN_NO_ERROR;
}


/* Read a pair of entries from wc_db in the directory DIR_ABSPATH. Return
   the directory's entry in *PARENT_ENTRY and NAME's entry in *ENTRY. The
   two returned pointers will be the same if NAME=="" ("this dir").

   The parent entry must exist.

   The requested entry MAY exist. If it does not, then NULL will be returned.

   The resulting entries are allocated in RESULT_POOL, and all temporary
   allocations are made in SCRATCH_POOL.  */
static svn_error_t *
read_entry_pair(const svn_wc_entry_t **parent_entry,
                const svn_wc_entry_t **entry,
                svn_wc__db_t *db,
                const char *dir_abspath,
                const char *name,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  apr_uint64_t wc_id = 1;  /* ### hacky. should remove.  */

  SVN_ERR(read_one_entry(parent_entry, db, wc_id, dir_abspath,
                         "" /* name */,
                         NULL /* parent_entry */,
                         result_pool, scratch_pool));

  /* If we need the entry for "this dir", then return the parent_entry
     in both outputs. Otherwise, read the child node.  */
  if (*name == '\0')
    {
      /* If the retrieved node is a FILE, then we have a problem. We asked
         for a directory. This implies there is an obstructing, unversioned
         directory where a FILE should be. We navigated from the obstructing
         subdir up to the parent dir, then returned the FILE found there.

         Let's return WC_MISSING cuz the caller thought we had a dir, but
         that (versioned subdir) isn't there.  */
      if ((*parent_entry)->kind == svn_node_file)
        {
          *parent_entry = NULL;
          return svn_error_createf(SVN_ERR_WC_MISSING, NULL,
                                 _("'%s' is not a versioned working copy"),
                                 svn_dirent_local_style(dir_abspath,
                                                        scratch_pool));
        }

      *entry = *parent_entry;
    }
  else
    {
      const apr_array_header_t *children;
      int i;

      /* Default to not finding the child.  */
      *entry = NULL;

      /* Determine whether the parent KNOWS about this child. If it does
         not, then we should not attempt to look for it.

         For example: the parent doesn't "know" about the child, but the
         versioned directory *does* exist on disk. We don't want to look
         into that subdir.  */
      SVN_ERR(svn_wc__db_read_children(&children, db, dir_abspath,
                                       scratch_pool, scratch_pool));
      for (i = children->nelts; i--; )
        {
          const char *child = APR_ARRAY_IDX(children, i, const char *);

          if (strcmp(child, name) == 0)
            {
              svn_error_t *err;

              err = read_one_entry(entry,
                                   db, wc_id, dir_abspath, name, *parent_entry,
                                   result_pool, scratch_pool);
              if (err)
                {
                  if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
                    return svn_error_return(err);

                  /* No problem. Clear the error and leave the default value
                     of "missing".  */
                  svn_error_clear(err);
                }

              /* Found it. No need to keep searching.  */
              break;
            }
        }
      /* if the loop ends without finding a child, then we have the default
         ENTRY value of NULL.  */
    }

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
read_entries(apr_hash_t **entries,
             svn_wc__db_t *db,
             const char *wcroot_abspath,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  int wc_format;

  SVN_ERR(svn_wc__db_temp_get_format(&wc_format, db, wcroot_abspath,
                                     scratch_pool));

  if (wc_format < SVN_WC__WC_NG_VERSION)
    return svn_error_return(svn_wc__read_entries_old(entries,
                                                     wcroot_abspath,
                                                     result_pool,
                                                     scratch_pool));

  return svn_error_return(read_entries_new(entries, db, wcroot_abspath,
                                           result_pool, scratch_pool));
}


/* For a given LOCAL_ABSPATH, using DB, set *ADM_ABSPATH to the directory in
   which the entry information is located, and *ENTRY_NAME to the entry name
   to access that entry.

   KIND and NEED_PARENT_STUB are as in svn_wc__get_entry().

   Return the results in RESULT_POOL and use SCRATCH_POOL for temporary
   allocations. */
static svn_error_t *
get_entry_access_info(const char **adm_abspath,
                      const char **entry_name,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      svn_node_kind_t kind,
                      svn_boolean_t need_parent_stub,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  svn_wc_adm_access_t *adm_access;
  svn_boolean_t read_from_subdir = FALSE;

  /* Can't ask for the parent stub if the node is a file.  */
  SVN_ERR_ASSERT(!need_parent_stub || kind != svn_node_file);

  /* If the caller didn't know the node kind, then stat the path. Maybe
     it is really there, and we can speed up the steps below.  */
  if (kind == svn_node_unknown)
    {
      svn_node_kind_t on_disk;

      /* Do we already have an access baton for LOCAL_ABSPATH?  */
      adm_access = svn_wc__adm_retrieve_internal2(db, local_abspath,
                                                  scratch_pool);
      if (adm_access)
        {
          /* Sweet. The node is a directory.  */
          on_disk = svn_node_dir;
        }
      else
        {
          svn_boolean_t special;

          /* What's on disk?  */
          SVN_ERR(svn_io_check_special_path(local_abspath, &on_disk, &special,
                                            scratch_pool));
        }

      if (on_disk != svn_node_dir)
        {
          /* If this is *anything* besides a directory (FILE, NONE, or
             UNKNOWN), then we cannot treat it as a versioned directory
             containing entries to read. Leave READ_FROM_SUBDIR as FALSE,
             so that the parent will be examined.

             For NONE and UNKNOWN, it may be that metadata exists for the
             node, even though on-disk is unhelpful.

             If NEED_PARENT_STUB is TRUE, and the entry is not a DIRECTORY,
             then we'll error.

             If NEED_PARENT_STUB if FALSE, and we successfully read a stub,
             then this on-disk node is obstructing the read.  */
        }
      else
        {
          /* We found a directory for this UNKNOWN node. Determine whether
             we need to read inside it.  */
          read_from_subdir = !need_parent_stub;
        }
    }
  else if (kind == svn_node_dir && !need_parent_stub)
    {
      read_from_subdir = TRUE;
    }

  if (read_from_subdir)
    {
      /* KIND must be a DIR or UNKNOWN (and we found a subdir). We want
         the "real" data, so treat LOCAL_ABSPATH as a versioned directory.  */
      *adm_abspath = apr_pstrdup(result_pool, local_abspath);
      *entry_name = "";
    }
  else
    {
      /* FILE node needs to read the parent directory. Or a DIR node
         needs to read from the parent to get at the stub entry. Or this
         is an UNKNOWN node, and we need to examine the parent.  */
      svn_dirent_split(local_abspath, adm_abspath, entry_name, result_pool);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__get_entry(const svn_wc_entry_t **entry,
                  svn_wc__db_t *db,
                  const char *local_abspath,
                  svn_boolean_t allow_unversioned,
                  svn_node_kind_t kind,
                  svn_boolean_t need_parent_stub,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  const char *dir_abspath;
  const char *entry_name;

  /* Can't ask for the parent stub if the node is a file.  */
  SVN_ERR_ASSERT(!need_parent_stub || kind != svn_node_file);

  SVN_ERR(get_entry_access_info(&dir_abspath, &entry_name, db, local_abspath,
                                kind, need_parent_stub, scratch_pool,
                                scratch_pool));

    {
      const svn_wc_entry_t *parent_entry;
      svn_error_t *err;

      /* NOTE: if KIND is UNKNOWN and we decided to examine the *parent*
         directory, then it is possible we moved out of the working copy.
         If the on-disk node is a DIR, and we asked for a stub, then we
         obviously can't provide that (parent has no info). If the on-disk
         node is a FILE/NONE/UNKNOWN, then it is obstructing the real
         LOCAL_ABSPATH (or it was never a versioned item). In all these
         cases, the read_entries() will (properly) throw an error.

         NOTE: if KIND is a DIR and we asked for the real data, but it is
         obstructed on-disk by some other node kind (NONE, FILE, UNKNOWN),
         then this will throw an error.  */

      err = read_entry_pair(&parent_entry, entry,
                            db, dir_abspath, entry_name,
                            result_pool, scratch_pool);
      if (err)
        {
          if (err->apr_err != SVN_ERR_WC_MISSING || kind != svn_node_unknown
              || *entry_name != '\0')
            return svn_error_return(err);
          svn_error_clear(err);

          /* The caller didn't know the node type, we saw a directory there,
             we attempted to read IN that directory, and then wc_db reports
             that it is NOT a working copy directory. It is possible that
             one of two things has happened:

             1) a directory is obstructing a file in the parent
             2) the (versioned) directory's contents have been removed

             Let's assume situation (1); if that is true, then we can just
             return the newly-found data.

             If we assumed (2), then a valid result still won't help us
             since the caller asked for the actual contents, not the stub
             (which is why we read *into* the directory). However, if we
             assume (1) and get back a stub, then we have verified a
             missing, versioned directory, and can return an error
             describing that.

             Redo the fetch, but "insist" we are trying to find a file.
             This will read from the parent directory of the "file".  */
          err = svn_wc__get_entry(entry, db, local_abspath, allow_unversioned,
                                  svn_node_file, FALSE,
                                  result_pool, scratch_pool);
          if (err == SVN_NO_ERROR)
            return SVN_NO_ERROR;
          if (err->apr_err != SVN_ERR_NODE_UNEXPECTED_KIND)
            return svn_error_return(err);
          svn_error_clear(err);

          /* We asked for a FILE, but the node found is a DIR. Thus, we
             are looking at a stub. Originally, we tried to read into the
             subdir because NEED_PARENT_STUB is FALSE. The stub we just
             read is not going to work for the caller, so inform them of
             the missing subdirectory.  */
          SVN_ERR_ASSERT(*entry != NULL && (*entry)->kind == svn_node_dir);
          return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                 _("Admin area of '%s' is missing"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));
        }
    }

  if (*entry == NULL)
    {
      if (allow_unversioned)
        return SVN_NO_ERROR;
      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                               _("'%s' is not under version control"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  /* The caller had the wrong information.  */
  if ((kind == svn_node_file && (*entry)->kind != svn_node_file)
      || (kind == svn_node_dir && (*entry)->kind != svn_node_dir))
    return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                             _("'%s' is not of the right kind"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  if (kind == svn_node_unknown)
    {
      /* They wanted a (directory) stub, but this isn't a directory.  */
      if (need_parent_stub && (*entry)->kind != svn_node_dir)
        return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                                 _("'%s' is not of the right kind"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));

      /* The actual (directory) information was wanted, but we got a stub.  */
      if (!need_parent_stub
          && (*entry)->kind == svn_node_dir
          && *(*entry)->name != '\0')
        return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                                 _("'%s' is not of the right kind"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__get_entry_versioned(const svn_wc_entry_t **entry,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            svn_node_kind_t kind,
                            svn_boolean_t show_hidden,
                            svn_boolean_t need_parent_stub,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* We call this with allow_unversioned=TRUE, since the error returned is
     different than our callers currently expect.  We catch the NULL entry
     below and return the correct error. */
  err = svn_wc__get_entry(entry, wc_ctx->db, local_abspath, TRUE, kind,
                          need_parent_stub, result_pool, scratch_pool);
  if (err && (err->apr_err == SVN_ERR_WC_MISSING
                || err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND
                || err->apr_err == SVN_ERR_NODE_UNEXPECTED_KIND))
    {
      svn_error_clear(err);
      *entry = NULL;
    }
  else if (err)
    return svn_error_return(err);


  if (*entry && !show_hidden)
    {
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__entry_is_hidden(&hidden, *entry));
      if (hidden)
        *entry = NULL;
    }

  if (! *entry)
    return svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                             _("'%s' is not under version control"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_is_deleted(svn_boolean_t *deleted,
                        svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_pool_t *scratch_pool)
{
  const svn_wc_entry_t *entry;
  svn_error_t *err;

  /* ### rewrite this in terms of wc_db.  */

  err = svn_wc__get_entry(&entry, db, local_abspath, FALSE,
                          svn_node_unknown, TRUE, scratch_pool, scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_NODE_UNEXPECTED_KIND)
        return svn_error_return(err);

      /* We asked for the parent stub, but got a file. No big deal. We have
         what we wanted for a file.  */
      svn_error_clear(err);
    }

  *deleted = entry->deleted;
  return SVN_NO_ERROR;
}


/* TODO ### Rewrite doc string to mention ENTRIES_ALL; not ADM_ACCESS.

   Prune the deleted entries from the cached entries in ADM_ACCESS, and
   return that collection in *ENTRIES_PRUNED.  SCRATCH_POOL is used for local,
   short term, memory allocation, RESULT_POOL for permanent stuff.  */
static svn_error_t *
prune_deleted(apr_hash_t **entries_pruned,
              apr_hash_t *entries_all,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  if (!entries_all)
    {
      *entries_pruned = NULL;
      return SVN_NO_ERROR;
    }

  /* I think it will be common for there to be no deleted entries, so
     it is worth checking for that case as we can optimise it. */
  for (hi = apr_hash_first(scratch_pool, entries_all);
       hi;
       hi = apr_hash_next(hi))
    {
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__entry_is_hidden(&hidden,
                                      svn__apr_hash_index_val(hi)));
      if (hidden)
        break;
    }

  if (! hi)
    {
      /* There are no deleted entries, so we can use the full hash */
      *entries_pruned = entries_all;
      return SVN_NO_ERROR;
    }

  /* Construct pruned hash without deleted entries */
  *entries_pruned = apr_hash_make(result_pool);
  for (hi = apr_hash_first(scratch_pool, entries_all);
       hi;
       hi = apr_hash_next(hi))
    {
      const void *key = svn__apr_hash_index_key(hi);
      const svn_wc_entry_t *entry = svn__apr_hash_index_val(hi);
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__entry_is_hidden(&hidden, entry));
      if (!hidden)
        apr_hash_set(*entries_pruned, key, APR_HASH_KEY_STRING, entry);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_entries_read(apr_hash_t **entries,
                    svn_wc_adm_access_t *adm_access,
                    svn_boolean_t show_hidden,
                    apr_pool_t *pool)
{
  apr_hash_t *new_entries;

  new_entries = svn_wc__adm_access_entries(adm_access);
  if (! new_entries)
    {
      svn_wc__db_t *db = svn_wc__adm_get_db(adm_access);
      const char *local_abspath = svn_wc__adm_access_abspath(adm_access);
      apr_pool_t *result_pool = svn_wc_adm_access_pool(adm_access);

      SVN_ERR(read_entries(&new_entries, db, local_abspath,
                           result_pool, pool));
      svn_wc__adm_access_set_entries(adm_access, new_entries);
    }

  if (show_hidden)
    *entries = new_entries;
  else
    SVN_ERR(prune_deleted(entries, new_entries,
                          svn_wc_adm_access_pool(adm_access),
                          pool));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
insert_base_node(svn_sqlite__db_t *sdb,
                 const db_base_node_t *base_node,
                 apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_INSERT_BASE_NODE_FOR_ENTRY));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 1, base_node->wc_id));
  SVN_ERR(svn_sqlite__bind_text(stmt, 2, base_node->local_relpath));

  if (base_node->repos_id)
    {
      SVN_ERR(svn_sqlite__bind_int64(stmt, 3, base_node->repos_id));
      SVN_ERR(svn_sqlite__bind_text(stmt, 4, base_node->repos_relpath));
    }

  if (base_node->parent_relpath)
    SVN_ERR(svn_sqlite__bind_text(stmt, 5, base_node->parent_relpath));

  if (base_node->presence == svn_wc__db_status_not_present)
    SVN_ERR(svn_sqlite__bind_text(stmt, 6, "not-present"));
  else if (base_node->presence == svn_wc__db_status_normal)
    SVN_ERR(svn_sqlite__bind_text(stmt, 6, "normal"));
  else if (base_node->presence == svn_wc__db_status_absent)
    SVN_ERR(svn_sqlite__bind_text(stmt, 6, "absent"));
  else if (base_node->presence == svn_wc__db_status_incomplete)
    SVN_ERR(svn_sqlite__bind_text(stmt, 6, "incomplete"));
  else if (base_node->presence == svn_wc__db_status_excluded)
    SVN_ERR(svn_sqlite__bind_text(stmt, 6, "excluded"));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 7, base_node->revision));

  /* ### in per-subdir operation, if we're about to write a directory and
     ### it is *not* "this dir", then we're writing a row in the parent
     ### directory about the child. note that in the kind.  */
  /* ### kind might be "symlink" or "unknown" */
  if (base_node->kind == svn_node_dir && *base_node->local_relpath != '\0')
    SVN_ERR(svn_sqlite__bind_text(stmt, 8, "subdir"));
  else if (base_node->kind == svn_node_none)
    SVN_ERR(svn_sqlite__bind_text(stmt, 5, "unknown"));
  else
    SVN_ERR(svn_sqlite__bind_text(stmt, 8,
                                  svn_node_kind_to_word(base_node->kind)));

  if (base_node->checksum)
    SVN_ERR(svn_sqlite__bind_checksum(stmt, 9, base_node->checksum,
                                      scratch_pool));

  if (base_node->translated_size != SVN_INVALID_FILESIZE)
    SVN_ERR(svn_sqlite__bind_int64(stmt, 10, base_node->translated_size));

  /* ### strictly speaking, changed_rev should be valid for present nodes. */
  if (SVN_IS_VALID_REVNUM(base_node->changed_rev))
    SVN_ERR(svn_sqlite__bind_int64(stmt, 11, base_node->changed_rev));
  if (base_node->changed_date)
    SVN_ERR(svn_sqlite__bind_int64(stmt, 12, base_node->changed_date));
  if (base_node->changed_author)
    SVN_ERR(svn_sqlite__bind_text(stmt, 13, base_node->changed_author));

  SVN_ERR(svn_sqlite__bind_text(stmt, 14, svn_depth_to_word(base_node->depth)));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 15, base_node->last_mod_time));

  if (base_node->properties)
    SVN_ERR(svn_sqlite__bind_properties(stmt, 16, base_node->properties,
                                        scratch_pool));

  /* Execute and reset the insert clause. */
  return svn_error_return(svn_sqlite__insert(NULL, stmt));
}

/* */
static svn_error_t *
insert_working_node(svn_sqlite__db_t *sdb,
                    const db_working_node_t *working_node,
                    apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_WORKING_NODE));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 1, working_node->wc_id));
  SVN_ERR(svn_sqlite__bind_text(stmt, 2, working_node->local_relpath));
  SVN_ERR(svn_sqlite__bind_text(stmt, 3, working_node->parent_relpath));

  /* ### need rest of values */
  if (working_node->presence == svn_wc__db_status_normal)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, "normal"));
  else if (working_node->presence == svn_wc__db_status_not_present)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, "not-present"));
  else if (working_node->presence == svn_wc__db_status_base_deleted)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, "base-deleted"));
  else if (working_node->presence == svn_wc__db_status_incomplete)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, "incomplete"));
  else if (working_node->presence == svn_wc__db_status_excluded)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, "excluded"));

  /* ### in per-subdir operation, if we're about to write a directory and
     ### it is *not* "this dir", then we're writing a row in the parent
     ### directory about the child. note that in the kind.  */
  if (working_node->kind == svn_node_dir
      && *working_node->local_relpath != '\0')
    SVN_ERR(svn_sqlite__bind_text(stmt, 5, "subdir"));
  else if (working_node->kind == svn_node_none)
    SVN_ERR(svn_sqlite__bind_text(stmt, 5, "unknown"));
  else
    SVN_ERR(svn_sqlite__bind_text(stmt, 5,
                                  svn_node_kind_to_word(working_node->kind)));

  if (working_node->copyfrom_repos_path)
    {
      SVN_ERR(svn_sqlite__bind_int64(stmt, 6,
                                     working_node->copyfrom_repos_id));
      SVN_ERR(svn_sqlite__bind_text(stmt, 7,
                                    working_node->copyfrom_repos_path));
      SVN_ERR(svn_sqlite__bind_int64(stmt, 8, working_node->copyfrom_revnum));
    }

  if (working_node->moved_here)
    SVN_ERR(svn_sqlite__bind_int(stmt, 9, working_node->moved_here));

  if (working_node->moved_to)
    SVN_ERR(svn_sqlite__bind_text(stmt, 10, working_node->moved_to));

  if (working_node->checksum)
    SVN_ERR(svn_sqlite__bind_checksum(stmt, 11, working_node->checksum,
                                      scratch_pool));

  if (working_node->translated_size != SVN_INVALID_FILESIZE)
    SVN_ERR(svn_sqlite__bind_int64(stmt, 12, working_node->translated_size));

  if (SVN_IS_VALID_REVNUM(working_node->changed_rev))
    SVN_ERR(svn_sqlite__bind_int64(stmt, 13, working_node->changed_rev));
  if (working_node->changed_date)
    SVN_ERR(svn_sqlite__bind_int64(stmt, 14, working_node->changed_date));
  if (working_node->changed_author)
    SVN_ERR(svn_sqlite__bind_text(stmt, 15, working_node->changed_author));

  SVN_ERR(svn_sqlite__bind_text(stmt, 16,
                                svn_depth_to_word(working_node->depth)));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 17, working_node->last_mod_time));

  if (working_node->properties)
    SVN_ERR(svn_sqlite__bind_properties(stmt, 18, working_node->properties,
                                        scratch_pool));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 19, working_node->keep_local));

  /* ### we should bind 'symlink_target' (20) as appropriate.  */

  /* Execute and reset the insert clause. */
  return svn_error_return(svn_sqlite__insert(NULL, stmt));
}

/* */
static svn_error_t *
insert_actual_node(svn_sqlite__db_t *sdb,
                   const db_actual_node_t *actual_node,
                   apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_ACTUAL_NODE));

  SVN_ERR(svn_sqlite__bind_int64(stmt, 1, actual_node->wc_id));
  SVN_ERR(svn_sqlite__bind_text(stmt, 2, actual_node->local_relpath));
  SVN_ERR(svn_sqlite__bind_text(stmt, 3, actual_node->parent_relpath));

  if (actual_node->properties)
    SVN_ERR(svn_sqlite__bind_properties(stmt, 4, actual_node->properties,
                                        scratch_pool));

  if (actual_node->conflict_old)
    {
      SVN_ERR(svn_sqlite__bind_text(stmt, 5, actual_node->conflict_old));
      SVN_ERR(svn_sqlite__bind_text(stmt, 6, actual_node->conflict_new));
      SVN_ERR(svn_sqlite__bind_text(stmt, 7, actual_node->conflict_working));
    }

  if (actual_node->prop_reject)
    SVN_ERR(svn_sqlite__bind_text(stmt, 8, actual_node->prop_reject));

  if (actual_node->changelist)
    SVN_ERR(svn_sqlite__bind_text(stmt, 9, actual_node->changelist));

  /* ### column 10 is text_mod */

  if (actual_node->tree_conflict_data)
    SVN_ERR(svn_sqlite__bind_text(stmt, 11, actual_node->tree_conflict_data));

  /* Execute and reset the insert clause. */
  return svn_error_return(svn_sqlite__insert(NULL, stmt));
}


/* Write the information for ENTRY to WC_DB.  The WC_ID, REPOS_ID and
   REPOS_ROOT will all be used for writing ENTRY.
   ### transitioning from straight sql to using the wc_db APIs.  For the
   ### time being, we'll need both parameters. */
static svn_error_t *
write_entry(svn_wc__db_t *db,
            svn_sqlite__db_t *sdb,
            apr_int64_t wc_id,
            apr_int64_t repos_id,
            const char *repos_root,
            const svn_wc_entry_t *entry,
            const char *local_relpath,
            const char *entry_abspath,
            const svn_wc_entry_t *this_dir,
            svn_boolean_t always_create_actual,
            svn_boolean_t create_locks,
            apr_pool_t *scratch_pool)
{
  db_base_node_t *base_node = NULL;
  db_working_node_t *working_node = NULL;
  db_actual_node_t *actual_node = NULL;
  const char *parent_relpath;

  if (*local_relpath == '\0')
    parent_relpath = NULL;
  else
    parent_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

  switch (entry->schedule)
    {
      case svn_wc_schedule_normal:
        if (entry->copied)
          working_node = MAYBE_ALLOC(working_node, scratch_pool);
        else
          base_node = MAYBE_ALLOC(base_node, scratch_pool);
        break;

      case svn_wc_schedule_add:
        working_node = MAYBE_ALLOC(working_node, scratch_pool);
        break;

      case svn_wc_schedule_delete:
        working_node = MAYBE_ALLOC(working_node, scratch_pool);
        /* If the entry is part of a REPLACED (not COPIED) subtree,
           then it needs a BASE node. */
       if (! (entry->copied
               || (this_dir->copied
                   && (this_dir->schedule == svn_wc_schedule_add ||
                       this_dir->schedule == svn_wc_schedule_delete ||
                       this_dir->schedule == svn_wc_schedule_replace))))
          base_node = MAYBE_ALLOC(base_node, scratch_pool);
        break;

      case svn_wc_schedule_replace:
        working_node = MAYBE_ALLOC(working_node, scratch_pool);
        base_node = MAYBE_ALLOC(base_node, scratch_pool);
        break;
    }

  /* Something deleted in this revision means there should always be a
     BASE node to indicate the not-present node.  */
  if (entry->deleted)
    {
      base_node = MAYBE_ALLOC(base_node, scratch_pool);
    }

  if (entry->copied)
    {
      /* Make sure we get a WORKING_NODE inserted. The copyfrom information
         will occur here or on a parent, as appropriate.  */
      working_node = MAYBE_ALLOC(working_node, scratch_pool);

      if (entry->copyfrom_url)
        {
          const char *relative_url;

          working_node->copyfrom_repos_id = repos_id;
          relative_url = svn_uri_is_child(repos_root, entry->copyfrom_url,
                                          NULL);
          if (relative_url == NULL)
            working_node->copyfrom_repos_path = "";
          else
            {
              /* copyfrom_repos_path is NOT a URI. decode into repos path.  */
              working_node->copyfrom_repos_path =
                svn_path_uri_decode(relative_url, scratch_pool);
            }
          working_node->copyfrom_revnum = entry->copyfrom_rev;
        }
      else
        {
          const char *parent_abspath = svn_dirent_dirname(entry_abspath,
                                                          scratch_pool);
          const char *op_root_abspath;
          const char *original_repos_relpath;
          svn_revnum_t original_revision;
          svn_error_t *err;

          /* The parent will *always* have info in the WORKING tree, since
             we've been designated as COPIED but do not have our own
             COPYFROM information. Therefore, our parent or a more distant
             ancestor has that information. Grab the data.  */
          err = svn_wc__db_scan_addition(
                    NULL,
                    &op_root_abspath,
                    NULL, NULL, NULL,
                    &original_repos_relpath, NULL, NULL, &original_revision,
                    db,
                    parent_abspath,
                    scratch_pool, scratch_pool);

          /* We could be reading the entries while in a transitional state
             during an add/copy operation. The scan_addition *does* throw
             errors sometimes. So clear anything that may come out of it,
             and perform the copyfrom construction only when it looks like
             we have a good/real set of return values.  */
          svn_error_clear(err);

          /* We may have been copied from a mixed-rev working copy. We need
             to simulate additional copies around revision changes. The old
             code could separately store the revision, but NG needs to create
             copies at each change.  */
          if (err == NULL
              && op_root_abspath != NULL
              && original_repos_relpath != NULL
              && SVN_IS_VALID_REVNUM(original_revision)
              /* above is valid result testing. below is the key test.  */
              && original_revision != entry->revision)
            {
              const char *relpath_to_entry = svn_dirent_is_child(
                op_root_abspath, entry_abspath, NULL);
              const char *new_copyfrom_relpath = svn_relpath_join(
                original_repos_relpath, relpath_to_entry, scratch_pool);

              working_node->copyfrom_repos_id = repos_id;
              working_node->copyfrom_repos_path = new_copyfrom_relpath;
              working_node->copyfrom_revnum = entry->revision;
            }
        }
    }

  if (entry->keep_local)
    {
      SVN_ERR_ASSERT(working_node != NULL);
      SVN_ERR_ASSERT(entry->schedule == svn_wc_schedule_delete);
      working_node->keep_local = TRUE;
    }

  if (entry->absent)
    {
      SVN_ERR_ASSERT(working_node == NULL);
      SVN_ERR_ASSERT(base_node != NULL);
      base_node->presence = svn_wc__db_status_absent;
    }

  if (entry->conflict_old)
    {
      actual_node = MAYBE_ALLOC(actual_node, scratch_pool);
      actual_node->conflict_old = entry->conflict_old;
      actual_node->conflict_new = entry->conflict_new;
      actual_node->conflict_working = entry->conflict_wrk;
    }

  if (entry->prejfile)
    {
      actual_node = MAYBE_ALLOC(actual_node, scratch_pool);
      actual_node->prop_reject = entry->prejfile;
    }

  if (entry->changelist)
    {
      actual_node = MAYBE_ALLOC(actual_node, scratch_pool);
      actual_node->changelist = entry->changelist;
    }

  /* ### set the text_mod value? */

  if (entry->tree_conflict_data)
    {
      actual_node = MAYBE_ALLOC(actual_node, scratch_pool);
      actual_node->tree_conflict_data = entry->tree_conflict_data;
    }

  if (entry->file_external_path != NULL)
    {
      base_node = MAYBE_ALLOC(base_node, scratch_pool);
    }

  /* Insert the base node. */
  if (base_node)
    {
      base_node->wc_id = wc_id;
      base_node->local_relpath = local_relpath;
      base_node->parent_relpath = parent_relpath;
      base_node->revision = entry->revision;
      base_node->last_mod_time = entry->text_time;
      base_node->translated_size = entry->working_size;

      if (entry->depth != svn_depth_exclude)
        base_node->depth = entry->depth;
      else
        {
          base_node->presence = svn_wc__db_status_excluded;
          base_node->depth = svn_depth_infinity;
        }

      if (entry->deleted)
        {
          SVN_ERR_ASSERT(!entry->incomplete);

          base_node->presence = svn_wc__db_status_not_present;
          /* ### should be svn_node_unknown, but let's store what we have. */
          base_node->kind = entry->kind;
        }
      else
        {
          base_node->kind = entry->kind;

          if (entry->incomplete)
            {
              /* ### nobody should have set the presence.  */
              SVN_ERR_ASSERT(base_node->presence == svn_wc__db_status_normal);
              base_node->presence = svn_wc__db_status_incomplete;
            }
        }

      if (entry->kind == svn_node_dir)
        base_node->checksum = NULL;
      else
        SVN_ERR(svn_checksum_parse_hex(&base_node->checksum, svn_checksum_md5,
                                       entry->checksum, scratch_pool));

      if (repos_root)
        {
          base_node->repos_id = repos_id;

          /* repos_relpath is NOT a URI. decode as appropriate.  */
          if (entry->url != NULL)
            {
              const char *relative_url = svn_uri_is_child(repos_root,
                                                          entry->url,
                                                          scratch_pool);

              if (relative_url == NULL)
                base_node->repos_relpath = "";
              else
                base_node->repos_relpath = svn_path_uri_decode(relative_url,
                                                               scratch_pool);
            }
          else
            {
              const char *base_path = svn_uri_is_child(repos_root,
                                                       this_dir->url,
                                                       scratch_pool);
              if (base_path == NULL)
                base_node->repos_relpath = entry->name;
              else
                base_node->repos_relpath =
                  svn_dirent_join(svn_path_uri_decode(base_path, scratch_pool),
                                  entry->name,
                                  scratch_pool);
            }
        }

      /* TODO: These values should always be present, if they are missing
         during an upgrade, set a flag, and then ask the user to talk to the
         server.

         Note: cmt_rev is the distinguishing value. The others may be 0 or
         NULL if the corresponding revprop has been deleted.  */
      base_node->changed_rev = entry->cmt_rev;
      base_node->changed_date = entry->cmt_date;
      base_node->changed_author = entry->cmt_author;

      SVN_ERR(insert_base_node(sdb, base_node, scratch_pool));

      /* We have to insert the lock after the base node, because the node
         must exist to lookup various bits of repos related information for
         the abs path. */
      if (entry->lock_token && create_locks)
        {
          svn_wc__db_lock_t lock;

          lock.token = entry->lock_token;
          lock.owner = entry->lock_owner;
          lock.comment = entry->lock_comment;
          lock.date = entry->lock_creation_date;

          SVN_ERR(svn_wc__db_lock_add(db, entry_abspath, &lock, scratch_pool));
        }

      /* Now, update the file external information.
         ### This is a hack!  */
      if (entry->file_external_path)
        {
          svn_sqlite__stmt_t *stmt;
          const char *str;

          SVN_ERR(svn_wc__serialize_file_external(&str,
                                                  entry->file_external_path,
                                                  &entry->file_external_peg_rev,
                                                  &entry->file_external_rev,
                                                  scratch_pool));

          SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                            STMT_UPDATE_FILE_EXTERNAL));
          SVN_ERR(svn_sqlite__bindf(stmt, "iss",
                                    (apr_uint64_t)1 /* wc_id */,
                                    entry->name,
                                    str));
          SVN_ERR(svn_sqlite__step_done(stmt));
        }
    }

  /* Insert the working node. */
  if (working_node)
    {
      working_node->wc_id = wc_id;
      working_node->local_relpath = local_relpath;
      working_node->parent_relpath = parent_relpath;
      working_node->changed_rev = SVN_INVALID_REVNUM;
      working_node->last_mod_time = entry->text_time;
      working_node->translated_size = entry->working_size;

      if (entry->depth != svn_depth_exclude)
        working_node->depth = entry->depth;
      else
        {
          working_node->presence = svn_wc__db_status_excluded;
          working_node->depth = svn_depth_infinity;
        }

      if (entry->kind == svn_node_dir)
        working_node->checksum = NULL;
      else
        SVN_ERR(svn_checksum_parse_hex(&working_node->checksum,
                                       svn_checksum_md5,
                                       entry->checksum, scratch_pool));

      if (entry->schedule == svn_wc_schedule_delete)
        {
          if (entry->incomplete)
            {
              /* A transition from a schedule-delete state to incomplete
                 is most likely caused by svn_wc_remove_from_revision_control.
                 By setting this node's presence to 'incomplete', we will
                 lose the scheduling information, but this directory is
                 being deleted (by the logs) ... we won't need the state.  */
              working_node->presence = svn_wc__db_status_incomplete;
            }
          else
            {
              /* If the entry is part of a COPIED (not REPLACED) subtree,
                 then the deletion is referring to the WORKING node, not
                 the BASE node. */
              if (entry->copied
                  || (this_dir->copied
                      && this_dir->schedule == svn_wc_schedule_add))
                working_node->presence = svn_wc__db_status_not_present;
              else
                working_node->presence = svn_wc__db_status_base_deleted;
            }

          /* ### should be svn_node_unknown, but let's store what we have. */
          working_node->kind = entry->kind;
        }
      else
        {
          /* presence == normal  */
          working_node->kind = entry->kind;

          if (entry->incomplete)
            {
              /* We shouldn't be overwriting another status.  */
              SVN_ERR_ASSERT(working_node->presence
                             == svn_wc__db_status_normal);
              working_node->presence = svn_wc__db_status_incomplete;
            }
        }

      /* These should generally be unset for added and deleted files,
         and contain whatever information we have for copied files. Let's
         just store whatever we have.

         Note: cmt_rev is the distinguishing value. The others may be 0 or
         NULL if the corresponding revprop has been deleted.  */
      working_node->changed_rev = entry->cmt_rev;
      working_node->changed_date = entry->cmt_date;
      working_node->changed_author = entry->cmt_author;

      SVN_ERR(insert_working_node(sdb, working_node, scratch_pool));
    }

  /* Insert the actual node. */
  if (actual_node || always_create_actual)
    {
      actual_node = MAYBE_ALLOC(actual_node, scratch_pool);

      actual_node->wc_id = wc_id;
      actual_node->local_relpath = local_relpath;
      actual_node->parent_relpath = parent_relpath;

      SVN_ERR(insert_actual_node(sdb, actual_node, scratch_pool));
    }

  return SVN_NO_ERROR;
}

struct entries_write_baton
{
  svn_wc__db_t *db;
  apr_int64_t repos_id;
  apr_int64_t wc_id;
  const char *local_abspath;
  apr_hash_t *entries;
};

/* Writes entries inside a sqlite transaction
   Implements svn_sqlite__transaction_callback_t. */
static svn_error_t *
entries_write_new_cb(void *baton,
                     svn_sqlite__db_t *sdb,
                     apr_pool_t *scratch_pool)
{
  struct entries_write_baton *ewb = baton;
  svn_wc__db_t *db = ewb->db;
  const char *local_abspath = ewb->local_abspath;
  const svn_wc_entry_t *this_dir;
  apr_hash_index_t *hi;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  const char *repos_root;

  /* Get a copy of the "this dir" entry for comparison purposes. */
  this_dir = apr_hash_get(ewb->entries, SVN_WC_ENTRY_THIS_DIR,
                          APR_HASH_KEY_STRING);

  /* If there is no "this dir" entry, something is wrong. */
  if (! this_dir)
    return svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                             _("No default entry in directory '%s'"),
                             svn_dirent_local_style(local_abspath,
                                                    iterpool));
  repos_root = this_dir->repos;

  /* Write out "this dir" */
  SVN_ERR(write_entry(db, sdb, ewb->wc_id, ewb->repos_id, repos_root,
                      this_dir, SVN_WC_ENTRY_THIS_DIR, local_abspath,
                      this_dir, FALSE, FALSE, iterpool));

  for (hi = apr_hash_first(scratch_pool, ewb->entries); hi;
       hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);
      const svn_wc_entry_t *this_entry = svn__apr_hash_index_val(hi);
      const char *child_abspath;

      svn_pool_clear(iterpool);

      /* Don't rewrite the "this dir" entry! */
      if (strcmp(name, SVN_WC_ENTRY_THIS_DIR) == 0)
        continue;

      /* Write the entry. Pass TRUE for create locks, because we still
         use this function for upgrading old working copies. */
      child_abspath = svn_dirent_join(local_abspath, name, iterpool);
      SVN_ERR(write_entry(db, sdb, ewb->wc_id, ewb->repos_id, repos_root,
                          this_entry, name, child_abspath, this_dir,
                          FALSE, TRUE,
                          iterpool));
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__write_upgraded_entries(svn_wc__db_t *db,
                               svn_sqlite__db_t *sdb,
                               apr_int64_t repos_id,
                               apr_int64_t wc_id,
                               const char *local_abspath,
                               apr_hash_t *entries,
                               apr_pool_t *scratch_pool)
{
  struct entries_write_baton ewb;

  ewb.db = db;
  ewb.repos_id = repos_id;
  ewb.wc_id = wc_id;
  ewb.local_abspath = local_abspath;
  ewb.entries = entries;

  /* Run this operation in a transaction to speed up SQLite.
     See http://www.sqlite.org/faq.html#q19 for more details */
  return svn_error_return(
      svn_sqlite__with_transaction(sdb, entries_write_new_cb, &ewb,
                                   scratch_pool));
}

struct write_one_entry_baton
{
  svn_wc__db_t *db;
  const char *local_abspath;
  const svn_wc_entry_t *this_dir;
  const svn_wc_entry_t *this_entry;
};

/* Rewrites a single entry inside a sqlite transaction
   Implements svn_sqlite__transaction_callback_t. */
static svn_error_t *
write_one_entry_cb(void *baton,
                   svn_sqlite__db_t *sdb,
                   apr_pool_t *scratch_pool)
{
  struct write_one_entry_baton *woeb = baton;
  svn_wc__db_t *db = woeb->db;
  const char *local_abspath = woeb->local_abspath;
  const svn_wc_entry_t *this_dir = woeb->this_dir;
  const svn_wc_entry_t *this_entry = woeb->this_entry;
  const char *this_abspath = svn_dirent_join(local_abspath, this_entry->name,
                                             scratch_pool);
  const void *base_props = NULL;
  const void *working_props = NULL;
  const void *actual_props = NULL;
  apr_size_t base_prop_len;
  apr_size_t working_prop_len;
  apr_size_t actual_prop_len;
  apr_hash_t *dav_cache;
  const svn_checksum_t *base_checksum;
  svn_sqlite__stmt_t *stmt;
  const char *repos_root;
  apr_int64_t repos_id;
  apr_int64_t wc_id;
  svn_error_t *err;
  svn_boolean_t got_row;

  SVN_ERR_ASSERT(this_dir && this_entry);

  /* Get the repos ID. */
  if (this_dir->uuid != NULL)
    {
      /* ### does this need to be done on a per-entry basis instead of
         ### the per-directory way we do it now?  me thinks yes...
         ###
         ### when do we harvest repository entries which no longer have
         ### any members?  */
      SVN_ERR(svn_wc__db_repos_ensure(&repos_id, db, local_abspath,
                                      this_dir->repos, this_dir->uuid,
                                      scratch_pool));
      repos_root = this_dir->repos;
    }
  else
    {
      repos_id = 0;
      repos_root = NULL;
    }

  SVN_ERR(fetch_wc_id(&wc_id, sdb));

  /* Before we nuke all the nodes, we need to get a few values */

  /* The dav cache is not in STMT_SELECT_BASE_NODE */
  err = svn_wc__db_base_get_dav_cache(&dav_cache, db, this_abspath,
                                      scratch_pool, scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);
      svn_error_clear(err); /* No BASE record */
      dav_cache = NULL;
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  if (got_row)
    {
      base_props = svn_sqlite__column_blob(stmt, 13, &base_prop_len,
                                           scratch_pool);

      err = svn_sqlite__column_checksum(&base_checksum, stmt, 5, scratch_pool);
      /* ### SVN_EXPERIMENTAL_PRISTINE:
         base_checksum is originally MD-5 but will later be SHA-1.  The
         base_checksum is not yet handled by this function. */

      SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
    }
  else
    SVN_ERR(svn_sqlite__reset(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  if (got_row)
    {
      /* No need to store the working checksum, that is stored in the entry */
      working_props = svn_sqlite__column_blob(stmt, 15, &working_prop_len,
                                              scratch_pool);
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  if (got_row)
    {
      actual_props = svn_sqlite__column_blob(stmt, 6, &actual_prop_len,
                                             scratch_pool);
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  /* Remove the WORKING, BASE and ACTUAL nodes for this entry */
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_DELETE_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step_done(stmt));
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_DELETE_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step_done(stmt));
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_DELETE_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, this_entry->name));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(write_entry(db, sdb, wc_id, repos_id, repos_root, this_entry,
                      this_entry->name, this_abspath, this_dir,
                      actual_props != NULL, FALSE, scratch_pool));

  if (dav_cache)
    SVN_ERR(svn_wc__db_base_set_dav_cache(db, this_abspath, dav_cache,
                                          scratch_pool));

  if (base_props)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_UPDATE_BASE_PROPS));
      SVN_ERR(svn_sqlite__bindf(stmt, "isb", wc_id, this_entry->name,
                                base_props, base_prop_len));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  if (working_props)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                        STMT_UPDATE_WORKING_PROPS));
      SVN_ERR(svn_sqlite__bindf(stmt, "isb", wc_id, this_entry->name,
                                working_props, working_prop_len));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  if (actual_props)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                        STMT_UPDATE_ACTUAL_PROPS));
      SVN_ERR(svn_sqlite__bindf(stmt, "isb", wc_id, this_entry->name,
                                actual_props, actual_prop_len));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  /* TODO: Update base checksum if needed */
  return SVN_NO_ERROR;
}

/* */
static svn_error_t *
write_one_entry(svn_wc__db_t *db,
                const char *local_abspath,
                const svn_wc_entry_t *this_dir,
                const svn_wc_entry_t *this_entry,
                apr_pool_t *scratch_pool)
{
  struct write_one_entry_baton woeb;
  svn_sqlite__db_t *sdb;

  /* ### need the SDB so we can jam rows directly into it.  */
  SVN_ERR(svn_wc__db_temp_borrow_sdb(&sdb, db, local_abspath,
                                     svn_wc__db_openmode_readwrite,
                                     scratch_pool));
  woeb.db = db;
  woeb.local_abspath = local_abspath;
  woeb.this_dir = this_dir;
  woeb.this_entry = this_entry;

  /* Run this operation in a transaction to speed up SQLite.
     See http://www.sqlite.org/faq.html#q19 for more details */
  return svn_error_return(
      svn_sqlite__with_transaction(sdb, write_one_entry_cb, &woeb,
                                   scratch_pool));
}



/* Update the entry CUR_ENTRY, according to the combination of
   entry data found in ENTRY and masked by MODIFY_FLAGS.
   The requested changes will be folded (merged) into
   the entry's existing state.
   Also cleanups meaningless fields combinations.

   PARENT_ENTRY must be passed, in order to grab certain "default" values.

   POOL will be used to allocate memory referenced by ENTRIES.
 */
static svn_error_t *
fold_entry(svn_wc_entry_t *cur_entry,
           const char *name,
           int modify_flags,
           const svn_wc_entry_t *entry,
           const svn_wc_entry_t *parent_entry,
           apr_pool_t *pool)
{
  SVN_ERR_ASSERT(cur_entry != NULL);
  SVN_ERR_ASSERT(name != NULL);
  SVN_ERR_ASSERT(entry != NULL);

  /* Name (just a safeguard here, really) */
  if (! cur_entry->name)
    cur_entry->name = apr_pstrdup(pool, name);

  /* Revision */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_REVISION)
    cur_entry->revision = entry->revision;

  /* Ancestral URL in repository */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_URL)
    cur_entry->url = entry->url ? apr_pstrdup(pool, entry->url) : NULL;

  /* Kind */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_KIND)
    cur_entry->kind = entry->kind;

  /* Schedule: handled by caller.  */

  /* Checksum */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_CHECKSUM)
    cur_entry->checksum = entry->checksum
      ? apr_pstrdup(pool, entry->checksum)
                          : NULL;

  /* Copy-related stuff */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_COPIED)
    cur_entry->copied = entry->copied;

  if (modify_flags & SVN_WC__ENTRY_MODIFY_COPYFROM_URL)
    cur_entry->copyfrom_url = entry->copyfrom_url
      ? apr_pstrdup(pool, entry->copyfrom_url)
                              : NULL;

  if (modify_flags & SVN_WC__ENTRY_MODIFY_COPYFROM_REV)
    cur_entry->copyfrom_rev = entry->copyfrom_rev;

  /* Deleted state */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_DELETED)
    cur_entry->deleted = entry->deleted;

  /* Absent state */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_ABSENT)
    cur_entry->absent = entry->absent;

  /* text_time, prop_time no longer passed to entry_modify()  */

  /* Conflict stuff */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_CONFLICT_OLD)
    cur_entry->conflict_old = entry->conflict_old
      ? apr_pstrdup(pool, entry->conflict_old)
                              : NULL;

  if (modify_flags & SVN_WC__ENTRY_MODIFY_CONFLICT_NEW)
    cur_entry->conflict_new = entry->conflict_new
      ? apr_pstrdup(pool, entry->conflict_new)
                              : NULL;

  if (modify_flags & SVN_WC__ENTRY_MODIFY_CONFLICT_WRK)
    cur_entry->conflict_wrk = entry->conflict_wrk
      ? apr_pstrdup(pool, entry->conflict_wrk)
                              : NULL;

  if (modify_flags & SVN_WC__ENTRY_MODIFY_PREJFILE)
    cur_entry->prejfile = entry->prejfile
      ? apr_pstrdup(pool, entry->prejfile)
                          : NULL;

  /* Last-commit flags are no longer passed to entry_modify() */

  /* LOCK flags are no longer passed to entry_modify().  */

  /* changelist is no longer modified with this function.  */

  /* has-props, prop-mods, cachable-props, and present-props are deprecated,
     so we do not copy them. */

  /* keep_local is no longer modified with this function */

  /* Note that we don't bother to fold entry->depth, because it is
     only meaningful on the this-dir entry anyway. */

  /* tree_conflict_data is never modified via entry_t.  */

  /* Absorb defaults from the parent dir, if any, unless this is a
     subdir entry. */
  if (cur_entry->kind != svn_node_dir && parent_entry != NULL)
    {
      if ((cur_entry->revision == SVN_INVALID_REVNUM) 
          && (cur_entry->kind != svn_node_dir))
        cur_entry->revision = parent_entry->revision;
    }

  /* Cleanup meaningless fields */

  /* ### svn_wc_schedule_delete is the minimal value. We need it because it's
     impossible to NULLify copyfrom_url with log-instructions.

     Note that I tried to find the smallest collection not to clear these
     fields for, but this condition still fails the test suite:

     !(entry->schedule == svn_wc_schedule_add
       || entry->schedule == svn_wc_schedule_replace
       || (entry->schedule == svn_wc_schedule_normal && entry->copied)))

  */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_SCHEDULE
      && cur_entry->schedule == svn_wc_schedule_delete)
    {
      cur_entry->copied = FALSE;
      cur_entry->copyfrom_rev = SVN_INVALID_REVNUM;
      cur_entry->copyfrom_url = NULL;
    }

  /* working_size is no longer passed to entry_modify()  */

  /* keep_local makes sense only when we are going to delete directory. */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_SCHEDULE
      && cur_entry->schedule != svn_wc_schedule_delete)
    {
      cur_entry->keep_local = FALSE;
    }

  /* File externals are no longer passed to entry_modify(). */

  return SVN_NO_ERROR;
}


/* Our general purpose intelligence module for handling a scheduling change
   to a single entry.

   Given an ENTRY with name NAME, examine the caller's requested scheduling
    change and the current state of the entry and its directory entry
   THIS_DIR_ENTRY, which can be equal to ENTRY.

   Determine the final schedule for the entry based on NEW_SCHEDULE and the
   entries.

   The output can be:
    * *SKIP_SCHEDULE_CHANGE set to true, when no schedule change is necessary.
    * Or a schedule change.

   In all these cases *RESULT_SCHEDULE contains the new schedule value.
 */
static svn_error_t *
fold_scheduling(svn_boolean_t *skip_schedule_change,
                svn_wc_schedule_t *result_schedule,
                const svn_wc_entry_t *this_dir_entry,
                const svn_wc_entry_t *entry,
                svn_wc_schedule_t new_schedule,
                const char *name)
{
  SVN_ERR_ASSERT(this_dir_entry);
  SVN_ERR_ASSERT(new_schedule != svn_wc_schedule_replace);

  *skip_schedule_change = FALSE;
  *result_schedule = new_schedule;

  /* The only operation valid on an item not already in revision
     control is addition. */
  if (entry == NULL)
    {
      if (new_schedule == svn_wc_schedule_add)
        return SVN_NO_ERROR;

      return svn_error_createf(SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
                               _("'%s' is not under version control"),
                               name);
    }

  /* At this point, we know the following things:

     1. There is already an entry for this item in the entries file
        whose existence is either _normal or _added (or about to
        become such), which for our purposes mean the same thing.

     2. We have been asked to merge in a state change, not to
        explicitly set the state.  */

  /* Here are some cases that are parent-directory sensitive.
     Basically, we make sure that we are not allowing versioned
     resources to just sorta dangle below directories marked for
     deletion. */
  if ((entry != this_dir_entry)
      && (this_dir_entry->schedule == svn_wc_schedule_delete))
    {
      if (new_schedule == svn_wc_schedule_add)
        return
          svn_error_createf(SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
                            _("Can't add '%s' to deleted directory; "
                              "try undeleting its parent directory first"),
                            name);
    }

  if (entry->absent && (new_schedule == svn_wc_schedule_add))
    {
      return svn_error_createf(SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
                               _("'%s' is marked as absent, so it cannot "
                                 "be scheduled for addition"),
                               name);
    }

  if (entry->schedule == svn_wc_schedule_normal
      && new_schedule == svn_wc_schedule_add
      && !entry->deleted)
    {
      /* You can't add something that's already been added to
         revision control... unless it's got a 'deleted' state */
      return svn_error_createf(SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
                               _("Entry '%s' is already under version "
                                 "control"),
                               name);
    }

  if (entry->schedule == svn_wc_schedule_normal)
    {
      if (new_schedule == svn_wc_schedule_normal)
        {
          /* No-op case.  */
          *skip_schedule_change = TRUE;
        }
    }
  else if (entry->schedule == svn_wc_schedule_add)
    {
      if (new_schedule == svn_wc_schedule_normal
          || new_schedule == svn_wc_schedule_add)
        {
          /* These are both no-op cases.  Normal is obvious, as is add.

             ### Neither case is obvious: above, we throw an error if
             ### already versioned, so why not here too?
          */
          *skip_schedule_change = TRUE;
        }
      else if (new_schedule == svn_wc_schedule_delete)
        {
          /* This is deleting a node added over the top of a not-present
             (DELETED=true) node. Return it to the not-present state.  */
          /* ### not trying to delete the directory, and this is a
             ### not-present node. (otherwise, caller handles this case)  */
          SVN_ERR_ASSERT(entry != this_dir_entry);
          SVN_ERR_ASSERT(entry->deleted);

          *result_schedule = svn_wc_schedule_normal;
        }
    }
  else if (entry->schedule == svn_wc_schedule_delete)
    {
      if (new_schedule == svn_wc_schedule_normal)
        {
          /* Reverting a delete results in normal */
        }
      else if (new_schedule == svn_wc_schedule_delete)
        {
          /* This is a no-op case  */
          *skip_schedule_change = TRUE;
        }
      else if (new_schedule == svn_wc_schedule_add)
        {
          /* Re-adding an entry marked for deletion?  This is really a
             replace operation. */
          *result_schedule = svn_wc_schedule_replace;
        }
    }
  else
    {
      /* Only possible state left.  */
      SVN_ERR_ASSERT(entry->schedule == svn_wc_schedule_replace);

      if (new_schedule == svn_wc_schedule_normal)
        {
          /* Reverting replacements results in normal  */
        }
      else if (new_schedule == svn_wc_schedule_add)
        {
          /* Adding a to-be-replaced entry breaks down to ((delete +
             add) + add) which might deserve a warning, but we'll just
             no-op it. */
          *skip_schedule_change = TRUE;
        }
      else if (new_schedule == svn_wc_schedule_delete)
        {
          /* Deleting a to-be-replaced entry breaks down to ((delete +
             add) + delete) which resolves to a flat deletion. */
          *result_schedule = svn_wc_schedule_delete;
        }
    }

  return SVN_NO_ERROR;
}



static svn_error_t *
entry_modify(svn_wc__db_t *db,
             const char *local_abspath,
             svn_node_kind_t kind,
             svn_boolean_t parent_stub,
             const svn_wc_entry_t *entry_mods,
             int modify_flags,
             apr_pool_t *scratch_pool)
{
  apr_pool_t *subpool = svn_pool_create(scratch_pool);
  svn_error_t *err;
  svn_wc_adm_access_t *adm_access;
  const char *adm_abspath;
  const char *name;
  const svn_wc_entry_t *parent_entry;
  svn_wc_entry_t *cur_entry;
  svn_wc_schedule_t new_schedule;

  SVN_ERR_ASSERT(entry_mods);

  SVN_ERR(get_entry_access_info(&adm_abspath, &name, db, local_abspath,
                                kind, parent_stub, subpool, subpool));

  /* Load ADM_ABSPATH's whole entries file:
     Is there an existing access baton for this path?  */
  adm_access = svn_wc__adm_retrieve_internal2(db, adm_abspath, subpool);
  if (adm_access != NULL)
    {
      /* Are we allowed to write to this admin area?  */
      SVN_ERR(svn_wc__write_check(db, svn_wc__adm_access_abspath(adm_access),
                                  subpool));

      /* Zap any cached entries. We're about to change them.  */
      svn_wc__adm_access_set_entries(adm_access, NULL);
    }
  /* ### else: should we have some kind of write check here?  */

  /* Cast our non-const CUR_ENTRY appropriately. It will be allocated for
     us in SUB_POOL, so we actually know it is modifiable.  */
  SVN_ERR(read_entry_pair(&parent_entry, (const svn_wc_entry_t **)&cur_entry,
                          db, adm_abspath, name, subpool, subpool));

  if (modify_flags & SVN_WC__ENTRY_MODIFY_SCHEDULE)
    {
      new_schedule = entry_mods->schedule;

      /* We may just want to force the scheduling change in. Otherwise,
         call our special function to fold the change in.  */
      if (!(modify_flags & SVN_WC__ENTRY_MODIFY_FORCE))
        {
          svn_boolean_t skip_schedule_change;

          /* ### adm_ops.c is the only code that attempts to transition to
             ### schedule_replace, but it uses FORCE.  */
          SVN_ERR_ASSERT(entry_mods->schedule != svn_wc_schedule_replace);

          /* If we are deleting a node that has been added, then simply
             remove the entry. Do NOT do this for an add over a not-present
             BASE node (the DELETED flag).  */
          if (entry_mods->schedule == svn_wc_schedule_delete
              && cur_entry != NULL
              && cur_entry->schedule == svn_wc_schedule_add
              && !cur_entry->deleted)
            {
              SVN_ERR(svn_wc__db_temp_op_remove_entry(db, local_abspath,
                                                      subpool));
              svn_pool_destroy(subpool);
              return SVN_NO_ERROR;
            }

          /* If scheduling changes were made, we have a special routine to
             manage those modifications. */
          SVN_ERR(fold_scheduling(&skip_schedule_change,
                                  &new_schedule,
                                  parent_entry,
                                  cur_entry,
                                  entry_mods->schedule,
                                  name));

          if (skip_schedule_change)
            modify_flags &= ~SVN_WC__ENTRY_MODIFY_SCHEDULE;
        }
    }

  /* Yay! Our "modify" function can actually "create". Bleah.  */
  if (cur_entry == NULL)
    cur_entry = alloc_entry(subpool);

  /* Fold in the changes, and write them out.  */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_SCHEDULE)
    cur_entry->schedule = new_schedule;
  SVN_ERR(fold_entry(cur_entry, name, modify_flags, entry_mods, parent_entry,
                     subpool));

  err = write_one_entry(db, adm_abspath,
                        parent_entry,
                        cur_entry,
                        subpool);

  svn_pool_destroy(subpool); /* Close wc.db handles */

  return svn_error_return(err);
}


svn_error_t *
svn_wc__entry_modify(svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_node_kind_t kind,
                     const svn_wc_entry_t *entry,
                     int modify_flags,
                     apr_pool_t *scratch_pool)
{
  return svn_error_return(entry_modify(db, local_abspath, kind, FALSE,
                                       entry, modify_flags, scratch_pool));
}


svn_error_t *
svn_wc__entry_modify_stub(svn_wc__db_t *db,
                          const char *local_abspath,
                          const svn_wc_entry_t *entry,
                          int modify_flags,
                          apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT((modify_flags & ~(
                    /* from adm_ops.c  */
                    SVN_WC__ENTRY_MODIFY_SCHEDULE
                    | SVN_WC__ENTRY_MODIFY_KIND
                    | SVN_WC__ENTRY_MODIFY_REVISION
                    | SVN_WC__ENTRY_MODIFY_COPYFROM_URL
                    | SVN_WC__ENTRY_MODIFY_COPYFROM_REV
                    | SVN_WC__ENTRY_MODIFY_COPIED
                    | SVN_WC__ENTRY_MODIFY_CHECKSUM

                    /* from entries.c  */
                    | SVN_WC__ENTRY_MODIFY_URL
                    | SVN_WC__ENTRY_MODIFY_REVISION

                    | SVN_WC__ENTRY_MODIFY_DELETED
                    | SVN_WC__ENTRY_MODIFY_SCHEDULE
                    | SVN_WC__ENTRY_MODIFY_FORCE

                    /* from update_editor.c  */
                    | SVN_WC__ENTRY_MODIFY_KIND
                    | SVN_WC__ENTRY_MODIFY_DELETED
                    | SVN_WC__ENTRY_MODIFY_ABSENT
                    | SVN_WC__ENTRY_MODIFY_SCHEDULE
                    | SVN_WC__ENTRY_MODIFY_FORCE

                    /* from workqueue.c  */
                    | SVN_WC__ENTRY_MODIFY_COPIED
                    | SVN_WC__ENTRY_MODIFY_COPYFROM_URL
                    | SVN_WC__ENTRY_MODIFY_COPYFROM_REV
                    | SVN_WC__ENTRY_MODIFY_SCHEDULE
                                   )) == 0);
  return svn_error_return(entry_modify(db, local_abspath,
                                       svn_node_dir, TRUE,
                                       entry, modify_flags, scratch_pool));
}


svn_wc_entry_t *
svn_wc_entry_dup(const svn_wc_entry_t *entry, apr_pool_t *pool)
{
  svn_wc_entry_t *dupentry = apr_palloc(pool, sizeof(*dupentry));

  /* Perform a trivial copy ... */
  *dupentry = *entry;

  /* ...and then re-copy stuff that needs to be duped into our pool. */
  if (entry->name)
    dupentry->name = apr_pstrdup(pool, entry->name);
  if (entry->url)
    dupentry->url = apr_pstrdup(pool, entry->url);
  if (entry->repos)
    dupentry->repos = apr_pstrdup(pool, entry->repos);
  if (entry->uuid)
    dupentry->uuid = apr_pstrdup(pool, entry->uuid);
  if (entry->copyfrom_url)
    dupentry->copyfrom_url = apr_pstrdup(pool, entry->copyfrom_url);
  if (entry->conflict_old)
    dupentry->conflict_old = apr_pstrdup(pool, entry->conflict_old);
  if (entry->conflict_new)
    dupentry->conflict_new = apr_pstrdup(pool, entry->conflict_new);
  if (entry->conflict_wrk)
    dupentry->conflict_wrk = apr_pstrdup(pool, entry->conflict_wrk);
  if (entry->prejfile)
    dupentry->prejfile = apr_pstrdup(pool, entry->prejfile);
  if (entry->checksum)
    dupentry->checksum = apr_pstrdup(pool, entry->checksum);
  if (entry->cmt_author)
    dupentry->cmt_author = apr_pstrdup(pool, entry->cmt_author);
  if (entry->lock_token)
    dupentry->lock_token = apr_pstrdup(pool, entry->lock_token);
  if (entry->lock_owner)
    dupentry->lock_owner = apr_pstrdup(pool, entry->lock_owner);
  if (entry->lock_comment)
    dupentry->lock_comment = apr_pstrdup(pool, entry->lock_comment);
  if (entry->changelist)
    dupentry->changelist = apr_pstrdup(pool, entry->changelist);

  /* NOTE: we do not dup cachable_props or present_props since they
     are deprecated. Use "" to indicate "nothing cachable or cached". */
  dupentry->cachable_props = "";
  dupentry->present_props = "";

  if (entry->tree_conflict_data)
    dupentry->tree_conflict_data = apr_pstrdup(pool,
                                               entry->tree_conflict_data);
  if (entry->file_external_path)
    dupentry->file_external_path = apr_pstrdup(pool,
                                               entry->file_external_path);
  return dupentry;
}


/*** Generic Entry Walker */

/* A recursive entry-walker, helper for svn_wc_walk_entries3().
 *
 * For this directory (DIRPATH, ADM_ACCESS), call the "found_entry" callback
 * in WALK_CALLBACKS, passing WALK_BATON to it. Then, for each versioned
 * entry in this directory, call the "found entry" callback and then recurse
 * (if it is a directory and if DEPTH allows).
 *
 * If SHOW_HIDDEN is true, include entries that are in a 'deleted' or
 * 'absent' state (and not scheduled for re-addition), else skip them.
 *
 * Call CANCEL_FUNC with CANCEL_BATON to allow cancellation.
 */
static svn_error_t *
walker_helper(const char *dirpath,
              svn_wc_adm_access_t *adm_access,
              const svn_wc_entry_callbacks2_t *walk_callbacks,
              void *walk_baton,
              svn_depth_t depth,
              svn_boolean_t show_hidden,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_wc_entry_t *dot_entry;
  svn_error_t *err;
  svn_wc__db_t *db = svn_wc__adm_get_db(adm_access);

  err = svn_wc_entries_read(&entries, adm_access, show_hidden, pool);

  if (err)
    SVN_ERR(walk_callbacks->handle_error(dirpath, err, walk_baton, pool));

  /* As promised, always return the '.' entry first. */
  dot_entry = apr_hash_get(entries, SVN_WC_ENTRY_THIS_DIR,
                           APR_HASH_KEY_STRING);
  if (! dot_entry)
    return walk_callbacks->handle_error
      (dirpath, svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                                  _("Directory '%s' has no THIS_DIR entry"),
                                  svn_dirent_local_style(dirpath, pool)),
       walk_baton, pool);

  /* Call the "found entry" callback for this directory as a "this dir"
   * entry. Note that if this directory has been reached by recursion, this
   * is the second visit as it will already have been visited once as a
   * child entry of its parent. */

  err = walk_callbacks->found_entry(dirpath, dot_entry, walk_baton, subpool);


  if(err)
    SVN_ERR(walk_callbacks->handle_error(dirpath, err, walk_baton, pool));

  if (depth == svn_depth_empty)
    return SVN_NO_ERROR;

  /* Loop over each of the other entries. */
  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);
      const svn_wc_entry_t *current_entry = svn__apr_hash_index_val(hi);
      const char *entrypath;
      const char *entry_abspath;
      svn_boolean_t hidden;

      svn_pool_clear(subpool);

      /* See if someone wants to cancel this operation. */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      /* Skip the "this dir" entry. */
      if (strcmp(current_entry->name, SVN_WC_ENTRY_THIS_DIR) == 0)
        continue;

      entrypath = svn_dirent_join(dirpath, name, subpool);
      SVN_ERR(svn_wc__entry_is_hidden(&hidden, current_entry));
      SVN_ERR(svn_dirent_get_absolute(&entry_abspath, entrypath, subpool));

      /* Call the "found entry" callback for this entry. (For a directory,
       * this is the first visit: as a child.) */
      if (current_entry->kind == svn_node_file
          || depth >= svn_depth_immediates)
        {
          err = walk_callbacks->found_entry(entrypath, current_entry,
                                            walk_baton, subpool);

          if (err)
            SVN_ERR(walk_callbacks->handle_error(entrypath, err,
                                                 walk_baton, pool));
        }

      /* Recurse into this entry if appropriate. */
      if (current_entry->kind == svn_node_dir
          && !hidden
          && depth >= svn_depth_immediates)
        {
          svn_wc_adm_access_t *entry_access;
          svn_depth_t depth_below_here = depth;

          if (depth == svn_depth_immediates)
            depth_below_here = svn_depth_empty;

          entry_access = svn_wc__adm_retrieve_internal2(db, entry_abspath,
                                                        subpool);

          if (entry_access)
            SVN_ERR(walker_helper(entrypath, entry_access,
                                  walk_callbacks, walk_baton,
                                  depth_below_here, show_hidden,
                                  cancel_func, cancel_baton,
                                  subpool));
        }
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__walker_default_error_handler(const char *path,
                                     svn_error_t *err,
                                     void *walk_baton,
                                     apr_pool_t *pool)
{
  /* Note: don't trace this. We don't want to insert a false "stack frame"
     onto an error generated elsewhere.  */
  return svn_error_return(err);
}


/* The public API. */
svn_error_t *
svn_wc_walk_entries3(const char *path,
                     svn_wc_adm_access_t *adm_access,
                     const svn_wc_entry_callbacks2_t *walk_callbacks,
                     void *walk_baton,
                     svn_depth_t walk_depth,
                     svn_boolean_t show_hidden,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_t *db = svn_wc__adm_get_db(adm_access);
  svn_error_t *err;
  svn_wc__db_kind_t kind;
  svn_depth_t depth;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  err = svn_wc__db_read_info(NULL, &kind, NULL,
                             NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             NULL, &depth,
                             NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL,
                             db, local_abspath,
                             pool, pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);
      /* Remap into SVN_ERR_UNVERSIONED_RESOURCE.  */
      svn_error_clear(err);
      return walk_callbacks->handle_error(
        path, svn_error_createf(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                                _("'%s' is not under version control"),
                                svn_dirent_local_style(local_abspath, pool)),
        walk_baton, pool);
    }

  if (kind == svn_wc__db_kind_file || depth == svn_depth_exclude)
    {
      const svn_wc_entry_t *entry;

      /* ### we should stop passing out entry structures.
         ###
         ### we should not call handle_error for an error the *callback*
         ###   gave us. let it deal with the problem before returning.  */

      if (!show_hidden)
        {
          svn_boolean_t hidden;
          SVN_ERR(svn_wc__db_node_hidden(&hidden, db, local_abspath, pool));

          if (hidden)
            {
              /* The fool asked to walk a "hidden" node. Report the node as
                 unversioned.

                 ### this is incorrect behavior. see depth_test 36. the walk
                 ### API will be revamped to avoid entry structures. we should
                 ### be able to solve the problem with the new API. (since we
                 ### shouldn't return a hidden entry here)  */
              return walk_callbacks->handle_error(
                               path, svn_error_createf(
                                  SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                                  _("'%s' is not under version control"),
                                  svn_dirent_local_style(local_abspath, pool)),
                               walk_baton, pool);
            }
        }

      SVN_ERR(svn_wc__get_entry(&entry, db, local_abspath, FALSE,
                                svn_node_file, FALSE, pool, pool));

      err = walk_callbacks->found_entry(path, entry, walk_baton, pool);
      if (err)
        return walk_callbacks->handle_error(path, err, walk_baton, pool);

      return SVN_NO_ERROR;
    }

  if (kind == svn_wc__db_kind_dir)
    return walker_helper(path, adm_access, walk_callbacks, walk_baton,
                         walk_depth, show_hidden, cancel_func, cancel_baton,
                         pool);

  return walk_callbacks->handle_error(
       path, svn_error_createf(SVN_ERR_NODE_UNKNOWN_KIND, NULL,
                               _("'%s' has an unrecognized node kind"),
                               svn_dirent_local_style(local_abspath, pool)),
       walk_baton, pool);
}

svn_error_t *
svn_wc__temp_mark_missing_not_present(const char *local_abspath,
                                      svn_wc_context_t *wc_ctx,
                                      apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR(svn_wc__db_read_info(&status, &kind,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  if (kind == svn_wc__db_kind_dir
      && status == svn_wc__db_status_obstructed_delete)
    {
      svn_wc_entry_t tmp_entry;

      tmp_entry.deleted = TRUE;
      tmp_entry.schedule = svn_wc_schedule_normal;

      SVN_ERR(svn_wc__entry_modify_stub(wc_ctx->db, local_abspath,
                                        &tmp_entry,
                                        (SVN_WC__ENTRY_MODIFY_DELETED
                                         | SVN_WC__ENTRY_MODIFY_SCHEDULE
                                         | SVN_WC__ENTRY_MODIFY_FORCE),
                                        scratch_pool));
      return SVN_NO_ERROR;
    }

  return svn_error_createf(SVN_ERR_WC_PATH_FOUND, NULL,
                           _("Unexpectedly found '%s': "
                             "path is marked 'missing'"),
                           svn_dirent_local_style(local_abspath, scratch_pool));
}

svn_error_t *
svn_wc_mark_missing_deleted(const char *path,
                            svn_wc_adm_access_t *parent,
                            apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL,
                                         svn_wc__adm_get_db(parent), pool));

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  SVN_ERR(svn_wc__temp_mark_missing_not_present(local_abspath, wc_ctx, pool));

  SVN_ERR(svn_wc_context_destroy(wc_ctx));

  return SVN_NO_ERROR;
}
