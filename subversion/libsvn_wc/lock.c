/*
 * lock.c:  routines for locking working copy subdirectories.
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
#include <apr_time.h>

#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_sorts.h"
#include "svn_hash.h"
#include "svn_types.h"

#include "wc.h"
#include "adm_files.h"
#include "lock.h"
#include "props.h"
#include "log.h"
#include "entries.h"
#include "wc_db.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"




struct svn_wc_adm_access_t
{
  /* PATH to directory which contains the administrative area */
  const char *path;

  /* And the absolute form of the path.  */
  const char *abspath;

  /* Indicates that the baton has been closed. */
  svn_boolean_t closed;

  /* Handle to the administrative database. */
  svn_wc__db_t *db;

  /* Was the DB provided to us? If so, then we'll never close it.  */
  svn_boolean_t db_provided;

  /* ENTRIES_HIDDEN is all cached entries including those in
     state deleted or state absent. It may be NULL. */
  apr_hash_t *entries_all;

  /* POOL is used to allocate cached items, they need to persist for the
     lifetime of this access baton */
  apr_pool_t *pool;

};


/* This is a placeholder used in the set hash to represent missing
   directories.  Only its address is important, it contains no useful
   data. */
static const svn_wc_adm_access_t missing;
#define IS_MISSING(lock) ((lock) == &missing)

/* ### hack for now. future functionality coming in a future revision.  */
#define svn_wc__db_is_closed(db) FALSE



/* ### these functions are here for forward references. generally, they're
   ### here to avoid the code churn from moving the definitions.  */

static svn_error_t *
do_close(svn_wc_adm_access_t *adm_access, svn_boolean_t preserve_lock,
         apr_pool_t *scratch_pool);

static svn_error_t *
add_to_shared(svn_wc_adm_access_t *lock, apr_pool_t *scratch_pool);

static svn_error_t *
close_single(svn_wc_adm_access_t *adm_access,
             svn_boolean_t preserve_lock,
             apr_pool_t *scratch_pool);

static svn_error_t *
alloc_db(svn_wc__db_t **db,
         svn_config_t *config,
         apr_pool_t *result_pool,
         apr_pool_t *scratch_pool);


svn_error_t *
svn_wc__internal_check_wc(int *wc_format,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  err = svn_wc__db_temp_get_format(wc_format, db, local_abspath, scratch_pool);
  if (err)
    {
      svn_node_kind_t kind;

      if (err->apr_err != SVN_ERR_WC_MISSING)
        return svn_error_return(err);
      svn_error_clear(err);

      /* ### the stuff below seems to be redundant. get_format() probably
         ### does all this.
         ###
         ### investigate all callers. DEFINITELY keep in mind the
         ### svn_wc_check_wc() entrypoint.
      */

      /* If the format file does not exist or path not directory, then for
         our purposes this is not a working copy, so return 0. */
      *wc_format = 0;

      /* Check path itself exists. */
      SVN_ERR(svn_io_check_path(local_abspath, &kind, scratch_pool));
      if (kind == svn_node_none)
        {
          return svn_error_createf(APR_ENOENT, NULL, _("'%s' does not exist"),
                                   svn_dirent_local_style(local_abspath,
                                                          scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_check_wc2(int *wc_format,
                 svn_wc_context_t *wc_ctx,
                 const char *local_abspath,
                 apr_pool_t *pool)
{
  return svn_error_return(
    svn_wc__internal_check_wc(wc_format, wc_ctx->db, local_abspath, pool));
}


/* Cleanup for a locked access baton.

   This handles closing access batons when their pool gets destroyed.
   The physical locks associated with such batons remain in the working
   copy if they are protecting work items in the workqueue.  */
static apr_status_t
pool_cleanup_locked(void *p)
{
  svn_wc_adm_access_t *lock = p;
  apr_uint64_t id;
  svn_skel_t *work_item;
  svn_error_t *err;

  if (lock->closed)
    return APR_SUCCESS;

  /* If the DB is closed, then we have a bunch of extra work to do.  */
  if (svn_wc__db_is_closed(lock->db))
    {
      apr_pool_t *scratch_pool;
      svn_wc__db_t *db;

      lock->closed = TRUE;

      /* If there is no ADM area, then we definitely have no work items
         or physical locks to worry about. Bail out.  */
      if (!svn_wc__adm_area_exists(lock->abspath, lock->pool))
        return APR_SUCCESS;

      /* Creating a subpool is safe within a pool cleanup, as long as
         we're absolutely sure to destroy it before we exit this function.

         We avoid using LOCK->POOL to keep the following functions from
         hanging cleanups or subpools from it. (the cleanups *might* get
         run, but the subpools will NOT be destroyed)  */
      scratch_pool = svn_pool_create(lock->pool);

      err = alloc_db(&db, NULL /* config */, scratch_pool, scratch_pool);
      if (!err)
        {
          err = svn_wc__db_wq_fetch(&id, &work_item, db, lock->abspath,
                                    scratch_pool, scratch_pool);
          if (!err && work_item == NULL)
            {
              /* There is no remaining work, so we're good to remove any
                 potential "physical" lock.  */
              err = svn_wc__db_wclock_remove(db, lock->abspath, scratch_pool);
            }
        }
      svn_error_clear(err);

      /* Closes the DB, too.  */
      svn_pool_destroy(scratch_pool);

      return APR_SUCCESS;
    }

  /* ### should we create an API that just looks, but doesn't return?  */
  err = svn_wc__db_wq_fetch(&id, &work_item, lock->db, lock->abspath,
                            lock->pool, lock->pool);

  /* Close just this access baton. The pool cleanup will close the rest.  */
  if (!err)
    err = close_single(lock,
                       work_item != NULL /* preserve_lock */,
                       lock->pool);

  if (err)
    {
      apr_status_t apr_err = err->apr_err;
      svn_error_clear(err);
      return apr_err;
    }

  return APR_SUCCESS;
}


/* Cleanup for a readonly access baton.  */
static apr_status_t
pool_cleanup_readonly(void *data)
{
  svn_wc_adm_access_t *lock = data;
  svn_error_t *err;

  if (lock->closed)
    return APR_SUCCESS;

  /* If the DB is closed, then we have nothing to do. There are no
     "physical" locks to remove, and we don't care whether this baton
     is registered with the DB.  */
  if (svn_wc__db_is_closed(lock->db))
    return APR_SUCCESS;

  /* Close this baton. No lock to preserve. Since this is part of the
     pool cleanup, we don't need to close children -- the cleanup process
     will close all children.  */
  err = close_single(lock, FALSE /* preserve_lock */, lock->pool);
  if (err)
    {
      apr_status_t result = err->apr_err;
      svn_error_clear(err);
      return result;
    }

  return APR_SUCCESS;
}


/* An APR pool cleanup handler.  This is a child handler, it removes the
   main pool handler. */
static apr_status_t
pool_cleanup_child(void *p)
{
  svn_wc_adm_access_t *lock = p;

  apr_pool_cleanup_kill(lock->pool, lock, pool_cleanup_locked);
  apr_pool_cleanup_kill(lock->pool, lock, pool_cleanup_readonly);

  return APR_SUCCESS;
}


/* Allocate from POOL, initialise and return an access baton. TYPE and PATH
   are used to initialise the baton.  If STEAL_LOCK, steal the lock if path
   is already locked */
static svn_error_t *
adm_access_alloc(svn_wc_adm_access_t **adm_access,
                 const char *path,
                 svn_wc__db_t *db,
                 svn_boolean_t db_provided,
                 svn_boolean_t write_lock,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_wc_adm_access_t *lock = apr_palloc(result_pool, sizeof(*lock));

  lock->closed = FALSE;
  lock->entries_all = NULL;
  lock->db = db;
  lock->db_provided = db_provided;
  lock->path = apr_pstrdup(result_pool, path);
  lock->pool = result_pool;

  SVN_ERR(svn_dirent_get_absolute(&lock->abspath, path, result_pool));

  *adm_access = lock;

  if (write_lock)
    {
      SVN_ERR(svn_wc__db_wclock_set(db, lock->abspath, 0, scratch_pool));
      SVN_ERR(svn_wc__db_temp_mark_locked(db, lock->abspath, scratch_pool));
    }

  err = add_to_shared(lock, scratch_pool);

  if (err)
    return svn_error_compose_create(
                err,
                svn_wc__db_wclock_remove(db, lock->abspath, scratch_pool));

  /* ### does this utf8 thing really/still apply??  */
  /* It's important that the cleanup handler is registered *after* at least
     one UTF8 conversion has been done, since such a conversion may create
     the apr_xlate_t object in the pool, and that object must be around
     when the cleanup handler runs.  If the apr_xlate_t cleanup handler
     were to run *before* the access baton cleanup handler, then the access
     baton's handler won't work. */

  /* Register an appropriate cleanup handler, based on the whether this
     access baton is locked or not.  */
  apr_pool_cleanup_register(lock->pool, lock,
                            write_lock
                              ? pool_cleanup_locked
                              : pool_cleanup_readonly,
                            pool_cleanup_child);

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
alloc_db(svn_wc__db_t **db,
         svn_config_t *config,
         apr_pool_t *result_pool,
         apr_pool_t *scratch_pool)
{
  svn_wc__db_openmode_t mode;

  /* ### need to determine MODE based on callers' needs.  */
  mode = svn_wc__db_openmode_default;
  SVN_ERR(svn_wc__db_open(db, mode, config, TRUE, TRUE,
                          result_pool, scratch_pool));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
add_to_shared(svn_wc_adm_access_t *lock, apr_pool_t *scratch_pool)
{
  /* ### sometimes we replace &missing with a now-valid lock.  */
  {
    svn_wc_adm_access_t *prior = svn_wc__db_temp_get_access(lock->db,
                                                            lock->abspath,
                                                            scratch_pool);
    if (IS_MISSING(prior))
      SVN_ERR(svn_wc__db_temp_close_access(lock->db, lock->abspath,
                                           prior, scratch_pool));
  }

  svn_wc__db_temp_set_access(lock->db, lock->abspath, lock,
                             scratch_pool);

  return SVN_NO_ERROR;
}


/* */
static svn_wc_adm_access_t *
get_from_shared(const char *abspath,
                svn_wc__db_t *db,
                apr_pool_t *scratch_pool)
{
  /* We closed the DB when it became empty. ABSPATH is not present.  */
  if (db == NULL)
    return NULL;
  return svn_wc__db_temp_get_access(db, abspath, scratch_pool);
}


/* */
static svn_error_t *
probe(svn_wc__db_t *db,
      const char **dir,
      const char *path,
      apr_pool_t *pool)
{
  svn_node_kind_t kind;
  int wc_format = 0;

  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind == svn_node_dir)
    {
      const char *local_abspath;

      SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
      SVN_ERR(svn_wc__internal_check_wc(&wc_format, db, local_abspath, pool));
    }

  /* a "version" of 0 means a non-wc directory */
  if (kind != svn_node_dir || wc_format == 0)
    {
      /* Passing a path ending in "." or ".." to svn_dirent_dirname() is
         probably always a bad idea; certainly it is in this case.
         Unfortunately, svn_dirent_dirname()'s current signature can't
         return an error, so we have to insert the protection in this
         caller, ideally the API needs a change.  See issue #1617. */
      const char *base_name = svn_dirent_basename(path, pool);
      if ((strcmp(base_name, "..") == 0)
          || (strcmp(base_name, ".") == 0))
        {
          return svn_error_createf
            (SVN_ERR_WC_BAD_PATH, NULL,
             _("Path '%s' ends in '%s', "
               "which is unsupported for this operation"),
             svn_dirent_local_style(path, pool), base_name);
        }

      *dir = svn_dirent_dirname(path, pool);
    }
  else
    *dir = path;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_single(svn_wc_adm_access_t **adm_access,
            const char *path,
            svn_boolean_t write_lock,
            svn_wc__db_t *db,
            svn_boolean_t db_provided,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  int wc_format = 0;
  svn_error_t *err;
  svn_wc_adm_access_t *lock;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, scratch_pool));
  err = svn_wc__internal_check_wc(&wc_format, db, local_abspath, scratch_pool);
  if (wc_format == 0 || (err && APR_STATUS_IS_ENOENT(err->apr_err)))
    {
      return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, err,
                               _("'%s' is not a working copy"),
                               svn_dirent_local_style(path, scratch_pool));
    }
  SVN_ERR(err);

  /* The format version must match exactly. Note that wc_db will perform
     an auto-upgrade if allowed. If it does *not*, then it has decided a
     manual upgrade is required.

     Note: if it decided on a manual upgrade, then we "should" never even
     reach this code. An error should have been raised earlier.  */
  if (wc_format != SVN_WC__VERSION)
    {
      return svn_error_createf(SVN_ERR_WC_UPGRADE_REQUIRED, NULL,
                               _("Working copy format of '%s' is too old (%d); "
                                 "please run 'svn upgrade'"),
                               svn_dirent_local_style(path, scratch_pool),
                               wc_format);
    }

  /* Need to create a new lock */
  SVN_ERR(adm_access_alloc(&lock, path, db, db_provided, write_lock,
                           result_pool, scratch_pool));

  /* ### recurse was here */
  *adm_access = lock;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
close_single(svn_wc_adm_access_t *adm_access,
             svn_boolean_t preserve_lock,
             apr_pool_t *scratch_pool)
{
  svn_boolean_t locked;

  if (adm_access->closed)
    return SVN_NO_ERROR;

  /* Physically unlock if required */
  SVN_ERR(svn_wc__db_temp_own_lock(&locked, adm_access->db,
                                   adm_access->abspath, scratch_pool));
  if (locked)
    {
      if (!preserve_lock)
        {
          /* Remove the physical lock in the admin directory for
             PATH. It is acceptable for the administrative area to
             have disappeared, such as when the directory is removed
             from the working copy.  It is an error for the lock to
             have disappeared if the administrative area still exists. */

          svn_error_t *err = svn_wc__db_wclock_remove(adm_access->db,
                                                      adm_access->abspath,
                                                      scratch_pool);
          if (err)
            {
              if (svn_wc__adm_area_exists(adm_access->abspath, scratch_pool))
                return err;
              svn_error_clear(err);
            }
        }
    }

  /* Reset to prevent further use of the lock. */
  adm_access->closed = TRUE;

  /* Detach from set */
  SVN_ERR(svn_wc__db_temp_close_access(adm_access->db, adm_access->abspath,
                                       adm_access, scratch_pool));

  /* Possibly close the underlying wc_db. */
  if (!adm_access->db_provided)
    {
      apr_hash_t *opened = svn_wc__db_temp_get_all_access(adm_access->db,
                                                          scratch_pool);
      if (apr_hash_count(opened) == 0)
        {
          SVN_ERR(svn_wc__db_close(adm_access->db));
          adm_access->db = NULL;
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__adm_available(svn_boolean_t *available,
                      svn_wc__db_kind_t *kind,
                      svn_boolean_t *obstructed,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_depth_t depth;

  if (kind)
    *kind = svn_wc__db_kind_unknown;

  SVN_ERR(svn_wc__db_read_info(&status, kind, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, &depth, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  if (obstructed)
    *obstructed = (status == svn_wc__db_status_obstructed ||
                   status == svn_wc__db_status_obstructed_add ||
                   status == svn_wc__db_status_obstructed_delete);

  *available = !(status == svn_wc__db_status_obstructed ||
                 status == svn_wc__db_status_obstructed_add ||
                 status == svn_wc__db_status_obstructed_delete ||
                 status == svn_wc__db_status_absent ||
                 status == svn_wc__db_status_excluded ||
                 status == svn_wc__db_status_not_present ||
                 depth == svn_depth_exclude);

  return SVN_NO_ERROR;
}
/* This is essentially the guts of svn_wc_adm_open3.
 *
 * If the working copy is already locked, return SVN_ERR_WC_LOCKED; if
 * it is not a versioned directory, return SVN_ERR_WC_NOT_WORKING_COPY.
 */
static svn_error_t *
do_open(svn_wc_adm_access_t **adm_access,
        const char *path,
        svn_wc__db_t *db,
        svn_boolean_t db_provided,
        apr_array_header_t *rollback,
        svn_boolean_t write_lock,
        int levels_to_lock,
        svn_cancel_func_t cancel_func,
        void *cancel_baton,
        apr_pool_t *result_pool,
        apr_pool_t *scratch_pool)
{
  svn_wc_adm_access_t *lock;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(open_single(&lock, path, write_lock, db, db_provided,
                      result_pool, iterpool));

  /* Add self to the rollback list in case of error.  */
  APR_ARRAY_PUSH(rollback, svn_wc_adm_access_t *) = lock;

  if (levels_to_lock != 0)
    {
      const apr_array_header_t *children;
      const char *local_abspath = svn_wc__adm_access_abspath(lock);
      int i;

      /* Reduce levels_to_lock since we are about to recurse */
      if (levels_to_lock > 0)
        levels_to_lock--;

      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       scratch_pool, iterpool));

      /* Open the tree */
      for (i = 0; i < children->nelts; i++)
        {
          const char *node_abspath;
          svn_wc__db_kind_t kind;
          svn_boolean_t available, obstructed;
          const char *name = APR_ARRAY_IDX(children, i, const char *);

          svn_pool_clear(iterpool);

          /* See if someone wants to cancel this operation. */
          if (cancel_func)
            SVN_ERR(cancel_func(cancel_baton));

          node_abspath = svn_dirent_join(local_abspath, name, iterpool);

          SVN_ERR(svn_wc__adm_available(&available,
                                        &kind,
                                        &obstructed,
                                        db,
                                        node_abspath,
                                        scratch_pool));

          if (kind != svn_wc__db_kind_dir)
            continue;

          if (available)
            {
              const char *node_path = svn_dirent_join(path, name, iterpool);
              svn_wc_adm_access_t *node_access;

              SVN_ERR(do_open(&node_access, node_path, db, db_provided,
                              rollback, write_lock, levels_to_lock,
                              cancel_func, cancel_baton,
                              lock->pool, iterpool));
              /* node_access has been registered in DB, so we don't need
                 to do anything with it.  */
            }
          else if (obstructed)
            {
              svn_wc__db_temp_set_access(lock->db, node_abspath,
                                         (svn_wc_adm_access_t *)&missing,
                                         iterpool);
            }
        }
    }
  svn_pool_destroy(iterpool);

  *adm_access = lock;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_all(svn_wc_adm_access_t **adm_access,
         const char *path,
         svn_wc__db_t *db,
         svn_boolean_t db_provided,
         svn_boolean_t write_lock,
         int levels_to_lock,
         svn_cancel_func_t cancel_func,
         void *cancel_baton,
         apr_pool_t *pool)
{
  apr_array_header_t *rollback;
  svn_error_t *err;

  rollback = apr_array_make(pool, 10, sizeof(svn_wc_adm_access_t *));

  err = do_open(adm_access, path, db, db_provided, rollback,
                write_lock, levels_to_lock,
                cancel_func, cancel_baton, pool, pool);
  if (err)
    {
      int i;

      for (i = rollback->nelts; i--; )
        {
          svn_wc_adm_access_t *lock = APR_ARRAY_IDX(rollback, i,
                                                    svn_wc_adm_access_t *);
          SVN_ERR_ASSERT(!IS_MISSING(lock));

          svn_error_clear(close_single(lock, FALSE /* preserve_lock */, pool));
        }
    }

  return svn_error_return(err);
}


svn_error_t *
svn_wc_adm_open3(svn_wc_adm_access_t **adm_access,
                 svn_wc_adm_access_t *associated,
                 const char *path,
                 svn_boolean_t write_lock,
                 int levels_to_lock,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *pool)
{
  svn_wc__db_t *db;
  svn_boolean_t db_provided;

  /* Make sure that ASSOCIATED has a set of access batons, so that we can
     glom a reference to self into it. */
  if (associated)
    {
      const char *abspath;
      svn_wc_adm_access_t *lock;

      SVN_ERR(svn_dirent_get_absolute(&abspath, path, pool));
      lock = get_from_shared(abspath, associated->db, pool);
      if (lock && !IS_MISSING(lock))
        /* Already locked.  The reason we don't return the existing baton
           here is that the user is supposed to know whether a directory is
           locked: if it's not locked call svn_wc_adm_open, if it is locked
           call svn_wc_adm_retrieve.  */
        return svn_error_createf(SVN_ERR_WC_LOCKED, NULL,
                                 _("Working copy '%s' locked"),
                                 svn_dirent_local_style(path, pool));
      db = associated->db;
      db_provided = associated->db_provided;
    }
  else
    {
      /* Any baton creation is going to need a shared structure for holding
         data across the entire set. The caller isn't providing one, so we
         do it here.  */
      /* ### we could optimize around levels_to_lock==0, but much of this
         ### is going to be simplified soon anyways.  */
      SVN_ERR(alloc_db(&db, NULL /* ### config. need! */, pool, pool));
      db_provided = FALSE;
    }

  return svn_error_return(open_all(adm_access, path, db, db_provided,
                                   write_lock, levels_to_lock,
                                   cancel_func, cancel_baton, pool));
}


svn_error_t *
svn_wc_adm_probe_open3(svn_wc_adm_access_t **adm_access,
                       svn_wc_adm_access_t *associated,
                       const char *path,
                       svn_boolean_t write_lock,
                       int levels_to_lock,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *pool)
{
  svn_error_t *err;
  const char *dir;

  if (associated == NULL)
    {
      svn_wc__db_t *db;

      /* Ugh. Too bad about having to open a DB.  */
      SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readonly,
                              NULL /* ### config */, TRUE, TRUE, pool, pool));
      err = probe(db, &dir, path, pool);
      svn_error_clear(svn_wc__db_close(db));
      SVN_ERR(err);
    }
  else
    {
      SVN_ERR(probe(associated->db, &dir, path, pool));
    }

  /* If we moved up a directory, then the path is not a directory, or it
     is not under version control. In either case, the notion of
     levels_to_lock does not apply to the provided path.  Disable it so
     that we don't end up trying to lock more than we need.  */
  if (dir != path)
    levels_to_lock = 0;

  err = svn_wc_adm_open3(adm_access, associated, dir, write_lock,
                         levels_to_lock, cancel_func, cancel_baton, pool);
  if (err)
    {
      svn_error_t *err2;

      /* If we got an error on the parent dir, that means we failed to
         get an access baton for the child in the first place.  And if
         the reason we couldn't get the child access baton is that the
         child is not a versioned directory, then return an error
         about the child, not the parent. */
      svn_node_kind_t child_kind;
      if ((err2 = svn_io_check_path(path, &child_kind, pool)))
        {
          svn_error_compose(err, err2);
          return err;
        }

      if ((dir != path)
          && (child_kind == svn_node_dir)
          && (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY))
        {
          svn_error_clear(err);
          return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                                   _("'%s' is not a working copy"),
                                   svn_dirent_local_style(path, pool));
        }

      return err;
    }

  return SVN_NO_ERROR;
}


svn_wc_adm_access_t *
svn_wc__adm_retrieve_internal2(svn_wc__db_t *db,
                               const char *abspath,
                               apr_pool_t *scratch_pool)
{
  svn_wc_adm_access_t *adm_access = get_from_shared(abspath, db, scratch_pool);

  /* If the entry is marked as "missing", then return nothing.  */
  if (IS_MISSING(adm_access))
    adm_access = NULL;

  return adm_access;
}


/* SVN_DEPRECATED */
svn_error_t *
svn_wc_adm_retrieve(svn_wc_adm_access_t **adm_access,
                    svn_wc_adm_access_t *associated,
                    const char *path,
                    apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_kind_t kind = svn_wc__db_kind_unknown;
  svn_node_kind_t wckind;
  svn_error_t *err;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  if (strcmp(associated->path, path) == 0)
    *adm_access = associated;
  else
    *adm_access = svn_wc__adm_retrieve_internal2(associated->db, local_abspath,
                                                 pool);

  /* We found what we're looking for, so bail. */
  if (*adm_access)
    return SVN_NO_ERROR;

  /* Most of the code expects access batons to exist, so returning an error
     generally makes the calling code simpler as it doesn't need to check
     for NULL batons. */
  /* We are going to send a SVN_ERR_WC_NOT_LOCKED, but let's provide
     a bit more information to our caller */

  err = svn_io_check_path(path, &wckind, pool);

  /* If we can't check the path, we can't make a good error message.  */
  if (err)
    {
      return svn_error_createf(SVN_ERR_WC_NOT_LOCKED, err,
                               _("Unable to check path existence for '%s'"),
                               svn_dirent_local_style(path, pool));
    }

  if (associated)
    {
      err = svn_wc__db_read_kind(&kind, svn_wc__adm_get_db(associated),
                                 local_abspath, TRUE, pool);

      if (err)
        {
          kind = svn_wc__db_kind_unknown;
          svn_error_clear(err);
        }
    }

  if (kind == svn_wc__db_kind_dir && wckind == svn_node_file)
    {
      err = svn_error_createf(
               SVN_ERR_WC_NOT_WORKING_COPY, NULL,
               _("Expected '%s' to be a directory but found a file"),
               svn_dirent_local_style(path, pool));

      return svn_error_create(SVN_ERR_WC_NOT_LOCKED, err, err->message);
    }

  if (kind != svn_wc__db_kind_dir && kind != svn_wc__db_kind_unknown)
    {
      err = svn_error_createf(
               SVN_ERR_WC_NOT_WORKING_COPY, NULL,
               _("Can't retrieve an access baton for non-directory '%s'"),
               svn_dirent_local_style(path, pool));

      return svn_error_create(SVN_ERR_WC_NOT_LOCKED, err, err->message);
    }

  if (kind == svn_wc__db_kind_unknown || wckind == svn_node_none)
    {
      err = svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                              _("Directory '%s' is missing"),
                              svn_dirent_local_style(path, pool));

      return svn_error_create(SVN_ERR_WC_NOT_LOCKED, err, err->message);
    }

  /* If all else fails, return our useless generic error.  */
  return svn_error_createf(SVN_ERR_WC_NOT_LOCKED, NULL,
                           _("Working copy '%s' is not locked"),
                           svn_dirent_local_style(path, pool));
}


/* SVN_DEPRECATED */
svn_error_t *
svn_wc_adm_probe_retrieve(svn_wc_adm_access_t **adm_access,
                          svn_wc_adm_access_t *associated,
                          const char *path,
                          apr_pool_t *pool)
{
  const char *dir;
  const char *local_abspath;
  svn_wc__db_kind_t kind;
  svn_error_t *err;

  SVN_ERR_ASSERT(associated != NULL);

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__db_read_kind(&kind, associated->db, local_abspath, TRUE, pool));

  if (kind == svn_wc__db_kind_dir)
    dir = path;
  else if (kind != svn_wc__db_kind_unknown)
    dir = svn_dirent_dirname(path, pool);
  else
    /* Not a versioned item, probe it */
    SVN_ERR(probe(associated->db, &dir, path, pool));

  err = svn_wc_adm_retrieve(adm_access, associated, dir, pool);
  if (err && err->apr_err == SVN_ERR_WC_NOT_LOCKED)
    {
      /* We'll receive a NOT LOCKED error for various reasons,
         including the reason we'll actually want to test for:
         The path is a versioned directory, but missing, in which case
         we want its parent's adm_access (which holds minimal data
         on the child) */
      svn_error_clear(err);
      SVN_ERR(probe(associated->db, &dir, path, pool));
      SVN_ERR(svn_wc_adm_retrieve(adm_access, associated, dir, pool));
    }
  else
    return svn_error_return(err);

  return SVN_NO_ERROR;
}


/* SVN_DEPRECATED */
svn_error_t *
svn_wc_adm_probe_try3(svn_wc_adm_access_t **adm_access,
                      svn_wc_adm_access_t *associated,
                      const char *path,
                      svn_boolean_t write_lock,
                      int levels_to_lock,
                      svn_cancel_func_t cancel_func,
                      void *cancel_baton,
                      apr_pool_t *pool)
{
  svn_error_t *err;

  err = svn_wc_adm_probe_retrieve(adm_access, associated, path, pool);

  /* SVN_ERR_WC_NOT_LOCKED would mean there was no access baton for
     path in associated, in which case we want to open an access
     baton and add it to associated. */
  if (err && (err->apr_err == SVN_ERR_WC_NOT_LOCKED))
    {
      svn_error_clear(err);
      err = svn_wc_adm_probe_open3(adm_access, associated,
                                   path, write_lock, levels_to_lock,
                                   cancel_func, cancel_baton,
                                   svn_wc_adm_access_pool(associated));

      /* If the path is not a versioned directory, we just return a
         null access baton with no error.  Note that of the errors we
         do report, the most important (and probably most likely) is
         SVN_ERR_WC_LOCKED.  That error would mean that someone else
         has this area locked, and we definitely want to bail in that
         case. */
      if (err && (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY))
        {
          svn_error_clear(err);
          *adm_access = NULL;
          err = NULL;
        }
    }

  return err;
}


/* */
static svn_error_t *
child_is_disjoint(svn_boolean_t *disjoint,
                  svn_wc__db_t *db,
                  const char *local_abspath,
                  apr_pool_t *scratch_pool)
{
  const char *node_repos_root, *node_repos_relpath, *node_repos_uuid;
  const char *parent_repos_root, *parent_repos_relpath, *parent_repos_uuid;
  svn_wc__db_status_t parent_status;
  const apr_array_header_t *children;
  const char *parent_abspath, *base;
  svn_error_t *err;
  svn_boolean_t found_in_parent = FALSE;
  int i;

  svn_dirent_split(local_abspath, &parent_abspath, &base, scratch_pool);

  /* Check if the parent directory knows about this node */
  err = svn_wc__db_read_children(&children, db, parent_abspath, scratch_pool,
                                 scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_NOT_DIRECTORY)
    {
      svn_error_clear(err);
      *disjoint = TRUE;
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);

      if (strcmp(name, base) == 0)
        {
          found_in_parent = TRUE;
          break;
        }
    }

  if (!found_in_parent)
    {
      *disjoint = TRUE;
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, &node_repos_relpath,
                               &node_repos_root, &node_repos_uuid, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));

  /* If the node does not have its own relpath, its value is inherited
     which tells us that it is not disjoint. */
  if (node_repos_relpath == NULL)
    {
      *disjoint = FALSE;
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_wc__db_read_info(&parent_status, NULL, NULL,
                               &parent_repos_relpath, &parent_repos_root,
                               &parent_repos_uuid, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               db, parent_abspath,
                               scratch_pool, scratch_pool));

  if (parent_repos_relpath == NULL)
    {
      if (parent_status == svn_wc__db_status_added)
        SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, &parent_repos_relpath,
                                         &parent_repos_root,
                                         &parent_repos_uuid,
                                         NULL, NULL, NULL, NULL,
                                         db, parent_abspath,
                                         scratch_pool, scratch_pool));
      else
        SVN_ERR(svn_wc__db_scan_base_repos(&parent_repos_relpath,
                                           &parent_repos_root,
                                           &parent_repos_uuid,
                                           db, parent_abspath,
                                           scratch_pool, scratch_pool));
    }

  if (strcmp(parent_repos_root, node_repos_root) != 0 ||
      strcmp(parent_repos_uuid, node_repos_uuid) != 0 ||
      strcmp(svn_relpath_join(parent_repos_relpath, base, scratch_pool),
             node_repos_relpath) != 0)
    {
      *disjoint = TRUE;
    }
  else
    *disjoint = FALSE;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_anchor(svn_wc_adm_access_t **anchor_access,
            svn_wc_adm_access_t **target_access,
            const char **target,
            svn_wc__db_t *db,
            svn_boolean_t db_provided,
            const char *path,
            svn_boolean_t write_lock,
            int levels_to_lock,
            svn_cancel_func_t cancel_func,
            void *cancel_baton,
            apr_pool_t *pool)
{
  const char *base_name = svn_dirent_basename(path, pool);

  /* Any baton creation is going to need a shared structure for holding
     data across the entire set. The caller isn't providing one, so we
     do it here.  */
  /* ### we could maybe skip the shared struct for levels_to_lock==0, but
     ### given that we need DB for format detection, may as well keep this.
     ### in any case, much of this is going to be simplified soon anyways.  */
  if (!db_provided)
    SVN_ERR(alloc_db(&db, NULL /* ### config. need! */, pool, pool));

  if (svn_path_is_empty(path)
      || svn_dirent_is_root(path, strlen(path))
      || ! strcmp(base_name, ".."))
    {
      SVN_ERR(open_all(anchor_access, path, db, db_provided,
                       write_lock, levels_to_lock,
                       cancel_func, cancel_baton, pool));
      *target_access = *anchor_access;
      *target = "";
    }
  else
    {
      svn_error_t *err;
      svn_wc_adm_access_t *p_access = NULL;
      svn_wc_adm_access_t *t_access = NULL;
      const char *parent = svn_dirent_dirname(path, pool);
      const char *local_abspath;
      svn_error_t *p_access_err = SVN_NO_ERROR;

      SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

      /* Try to open parent of PATH to setup P_ACCESS */
      err = open_single(&p_access, parent, write_lock, db, db_provided,
                        pool, pool);
      if (err)
        {
          const char *abspath = svn_dirent_dirname(local_abspath, pool);
          svn_wc_adm_access_t *existing_adm = svn_wc__db_temp_get_access(db, abspath, pool);

          if (IS_MISSING(existing_adm))
            svn_wc__db_temp_clear_access(db, abspath, pool);
          else
            SVN_ERR_ASSERT(existing_adm == NULL);

          if (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY)
            {
              svn_error_clear(err);
              p_access = NULL;
            }
          else if (write_lock && (err->apr_err == SVN_ERR_WC_LOCKED
                                  || APR_STATUS_IS_EACCES(err->apr_err)))
            {
              /* If P_ACCESS isn't to be returned then a read-only baton
                 will do for now, but keep the error in case we need it. */
              svn_error_t *err2 = open_single(&p_access, parent, FALSE,
                                              db, db_provided, pool, pool);
              if (err2)
                {
                  svn_error_clear(err2);
                  return err;
                }
              p_access_err = err;
            }
          else
            return err;
        }

      /* Try to open PATH to setup T_ACCESS */
      err = open_all(&t_access, path, db, db_provided, write_lock,
                     levels_to_lock, cancel_func, cancel_baton, pool);
      if (err)
        {
          if (p_access == NULL)
            {
              /* Couldn't open the parent or the target. Bail out.  */
              svn_error_clear(p_access_err);
              return svn_error_return(err);
            }

          if (err->apr_err != SVN_ERR_WC_NOT_WORKING_COPY)
            {
              if (p_access)
                svn_error_clear(svn_wc_adm_close2(p_access, pool));
              svn_error_clear(p_access_err);
              return svn_error_return(err);
            }

          /* This directory is not under version control. Ignore it.  */
          svn_error_clear(err);
          t_access = NULL;
        }

      /* At this stage might have P_ACCESS, T_ACCESS or both */

      /* Check for switched or disjoint P_ACCESS and T_ACCESS */
      if (p_access && t_access)
        {
          svn_boolean_t disjoint;

          err = child_is_disjoint(&disjoint, db, local_abspath, pool);
          if (err)
            {
              svn_error_clear(p_access_err);
              svn_error_clear(svn_wc_adm_close2(p_access, pool));
              svn_error_clear(svn_wc_adm_close2(t_access, pool));
              return svn_error_return(err);
            }

          if (disjoint)
            {
              /* Switched or disjoint, so drop P_ACCESS. Don't close any
                 descendents, or we might blast the child.  */
              err = close_single(p_access, FALSE /* preserve_lock */, pool);
              if (err)
                {
                  svn_error_clear(p_access_err);
                  svn_error_clear(svn_wc_adm_close2(t_access, pool));
                  return svn_error_return(err);
                }
              p_access = NULL;
            }
        }

      /* We have a parent baton *and* we have an error related to opening
         the baton. That means we have a readonly baton, but that isn't
         going to work for us. (p_access would have been set to NULL if
         a writable parent baton is not required)  */
      if (p_access && p_access_err)
        {
          if (t_access)
            svn_error_clear(svn_wc_adm_close2(t_access, pool));
          svn_error_clear(svn_wc_adm_close2(p_access, pool));
          return svn_error_return(p_access_err);
        }
      svn_error_clear(p_access_err);

      if (! t_access)
        {
          svn_boolean_t available, obstructed;
          svn_wc__db_kind_t kind;

          err = svn_wc__adm_available(&available, &kind, &obstructed,
                                      db, local_abspath, pool);

          if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
            svn_error_clear(err);
          else if (err)
            {
              svn_error_clear(svn_wc_adm_close2(p_access, pool));
              return svn_error_return(err);
            }
          if (obstructed && kind == svn_wc__db_kind_dir)
            {
              /* Child PATH is missing.  */
              svn_wc__db_temp_set_access(db, local_abspath,
                                         (svn_wc_adm_access_t *)&missing,
                                         pool);
            }
        }

      *anchor_access = p_access ? p_access : t_access;
      *target_access = t_access ? t_access : p_access;

      if (! p_access)
        *target = "";
      else
        *target = base_name;
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_adm_open_anchor(svn_wc_adm_access_t **anchor_access,
                       svn_wc_adm_access_t **target_access,
                       const char **target,
                       const char *path,
                       svn_boolean_t write_lock,
                       int levels_to_lock,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *pool)
{
  return svn_error_return(open_anchor(anchor_access, target_access, target,
                                      NULL, FALSE, path, write_lock,
                                      levels_to_lock, cancel_func,
                                      cancel_baton, pool));
}


/* Does the work of closing the access baton ADM_ACCESS.  Any physical
   locks are removed from the working copy if PRESERVE_LOCK is FALSE, or
   are left if PRESERVE_LOCK is TRUE.  Any associated access batons that
   are direct descendants will also be closed.
 */
static svn_error_t *
do_close(svn_wc_adm_access_t *adm_access,
         svn_boolean_t preserve_lock,
         apr_pool_t *scratch_pool)
{
  svn_wc_adm_access_t *look;

  if (adm_access->closed)
    return SVN_NO_ERROR;

  /* If we are part of the shared set, then close descendant batons.  */
  look = get_from_shared(adm_access->abspath, adm_access->db, scratch_pool);
  if (look != NULL)
    {
      apr_hash_t *opened;
      apr_hash_index_t *hi;

      /* Gather all the opened access batons from the DB.  */
      opened = svn_wc__db_temp_get_all_access(adm_access->db, scratch_pool);

      /* Close any that are descendents of this baton.  */
      for (hi = apr_hash_first(scratch_pool, opened);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *abspath = svn__apr_hash_index_key(hi);
          svn_wc_adm_access_t *child = svn__apr_hash_index_val(hi);
          const char *path = child->path;

          if (IS_MISSING(child))
            {
              /* We don't close the missing entry, but get rid of it from
                 the set. */
              svn_wc__db_temp_clear_access(adm_access->db, abspath,
                                           scratch_pool);
              continue;
            }

          if (! svn_dirent_is_ancestor(adm_access->path, path)
              || strcmp(adm_access->path, path) == 0)
            continue;

          SVN_ERR(close_single(child, preserve_lock, scratch_pool));
        }
    }

  return svn_error_return(close_single(adm_access, preserve_lock,
                                       scratch_pool));
}


/* SVN_DEPRECATED */
svn_error_t *
svn_wc_adm_close2(svn_wc_adm_access_t *adm_access, apr_pool_t *scratch_pool)
{
  return svn_error_return(do_close(adm_access, FALSE, scratch_pool));
}


/* SVN_DEPRECATED */
svn_boolean_t
svn_wc_adm_locked(const svn_wc_adm_access_t *adm_access)
{
  svn_boolean_t locked;
  apr_pool_t *subpool = svn_pool_create(adm_access->pool);
  svn_error_t *err = svn_wc__db_temp_own_lock(&locked, adm_access->db,
                                              adm_access->abspath,
                                              subpool);
  svn_pool_destroy(subpool);

  if (err)
    {
      svn_error_clear(err);
      /* ### is this right? */
      return FALSE;
    }

  return locked;
}

svn_error_t *
svn_wc__write_check(svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *scratch_pool)
{
  svn_boolean_t locked;

  SVN_ERR(svn_wc__db_temp_own_lock(&locked, db, local_abspath, scratch_pool));
  if (!locked)
    return svn_error_createf(SVN_ERR_WC_NOT_LOCKED, NULL,
                             _("No write-lock in '%s'"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_locked2(svn_boolean_t *locked_here,
               svn_boolean_t *locked,
               svn_wc_context_t *wc_ctx,
               const char *local_abspath,
               apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  if (locked_here != NULL)
    SVN_ERR(svn_wc__db_temp_own_lock(locked_here, wc_ctx->db, local_abspath,
                                     scratch_pool));
  if (locked != NULL)
    SVN_ERR(svn_wc__db_wclocked(locked, wc_ctx->db, local_abspath,
                                scratch_pool));

  return SVN_NO_ERROR;
}


/* SVN_DEPRECATED */
const char *
svn_wc_adm_access_path(const svn_wc_adm_access_t *adm_access)
{
  return adm_access->path;
}


const char *
svn_wc__adm_access_abspath(const svn_wc_adm_access_t *adm_access)
{
  return adm_access->abspath;
}


/* SVN_DEPRECATED */
apr_pool_t *
svn_wc_adm_access_pool(const svn_wc_adm_access_t *adm_access)
{
  return adm_access->pool;
}


void
svn_wc__adm_access_set_entries(svn_wc_adm_access_t *adm_access,
                               apr_hash_t *entries)
{
  adm_access->entries_all = entries;
}


apr_hash_t *
svn_wc__adm_access_entries(svn_wc_adm_access_t *adm_access)
{
  /* Compile with -DSVN_DISABLE_ENTRY_CACHE to disable the in-memory
     entry caching. As of 2010-03-18 (r924708) merge_tests 34 and 134
     fail during "make check".  */
#ifdef SVN_DISABLE_ENTRY_CACHE
  return NULL;
#else
  return adm_access->entries_all;
#endif
}


svn_wc__db_t *
svn_wc__adm_get_db(const svn_wc_adm_access_t *adm_access)
{
  return adm_access->db;
}


svn_boolean_t
svn_wc__adm_missing(svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *scratch_pool)
{
  const svn_wc_adm_access_t *look;
  svn_boolean_t available, obstructed;
  svn_wc__db_kind_t kind;

  look = get_from_shared(local_abspath, db, scratch_pool);

  if (look != NULL)
    return IS_MISSING(look);

  /* When we switch to a single database an access baton can't be
     missing, but until then it can. But if there are no access batons we
     would always return FALSE.
     For this case we check if an access baton could be opened

*/

  /* This check must match the check in do_open() */
  svn_error_clear(svn_wc__adm_available(&available, &kind, &obstructed,
                                        db, local_abspath,
                                        scratch_pool));

  return (kind == svn_wc__db_kind_dir) && !available && obstructed;
}


svn_error_t *
svn_wc__acquire_write_lock(const char **anchor_abspath,
                           svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  apr_pool_t *iterpool;
  const apr_array_header_t *children;
  int format, i;
  svn_error_t *err;

  SVN_ERR(svn_wc__db_read_kind(&kind, wc_ctx->db, local_abspath, TRUE,
                               scratch_pool));

  if (anchor_abspath)
    {
      const char *parent_abspath;
      svn_wc__db_kind_t parent_kind;

      parent_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
      err = svn_wc__db_read_kind(&parent_kind, wc_ctx->db, parent_abspath, TRUE,
                                 scratch_pool);
      if (err && err->apr_err == SVN_ERR_WC_NOT_DIRECTORY)
        {
          svn_error_clear(err);
          parent_kind = svn_wc__db_kind_unknown;
        }
      else
        SVN_ERR(err);

      if (kind == svn_wc__db_kind_dir && parent_kind == svn_wc__db_kind_dir)
        {
          svn_boolean_t disjoint;
          SVN_ERR(child_is_disjoint(&disjoint, wc_ctx->db, local_abspath,
                                    scratch_pool));
          if (!disjoint)
            local_abspath = parent_abspath;
        }
      else if (parent_kind == svn_wc__db_kind_dir)
        local_abspath = parent_abspath;
      else if (kind != svn_wc__db_kind_dir)
        return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                                 _("'%s' is not a working copy"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));

      *anchor_abspath = apr_pstrdup(result_pool, local_abspath);
    }
  else if (kind != svn_wc__db_kind_dir)
    local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);

  SVN_ERR(svn_wc__db_read_children(&children, wc_ctx->db, local_abspath,
                                   scratch_pool, scratch_pool));

  /* The current lock paradigm is that each directory holds a lock for itself,
     and there are no inherited locks.  In the eventual wc-ng paradigm, a
     lock on a directory, would imply a infinite-depth lock on the children.
     But since we aren't quite there yet, we do the infinite locking
     manually (and be sure to release them in svn_wc__release_write_lock(). */

  iterpool = svn_pool_create(scratch_pool);
  for (i = 0; i < children->nelts; i ++)
    {
      const char *child_relpath = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;

      svn_pool_clear(iterpool);
      child_abspath = svn_dirent_join(local_abspath, child_relpath, iterpool);

      SVN_ERR(svn_wc__db_read_kind(&kind, wc_ctx->db, child_abspath, FALSE,
                                   iterpool));
      if (kind == svn_wc__db_kind_dir)
        {
          err = svn_wc__acquire_write_lock(NULL, wc_ctx, child_abspath, NULL,
                                           iterpool);
          if (err && err->apr_err == SVN_ERR_WC_LOCKED)
            {
              while(i >= 0)
                {
                  svn_error_t *err2;
                  svn_pool_clear(iterpool);
                  child_relpath = APR_ARRAY_IDX(children, i, const char *);
                  child_abspath = svn_dirent_join(local_abspath, child_relpath,
                                                  iterpool);
                   err2 = svn_wc__release_write_lock(wc_ctx, child_abspath,
                                                     iterpool);
                   if (err2)
                     svn_error_compose(err, err2);
                   --i;
                }
              return svn_error_return(err);
            }
        }
    }

  /* We don't want to try and lock an unversioned directory that
     obstructs a versioned directory. */
  err = svn_wc__internal_check_wc(&format, wc_ctx->db, local_abspath, iterpool);
  if (!err && format)
    {
      SVN_ERR(svn_wc__db_wclock_set(wc_ctx->db, local_abspath, 0, iterpool));
      SVN_ERR(svn_wc__db_temp_mark_locked(wc_ctx->db, local_abspath, iterpool));
    }
  svn_error_clear(err);

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__release_write_lock(svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  apr_pool_t *iterpool;
  const apr_array_header_t *children;
  apr_uint64_t id;
  svn_skel_t *work_item;
  svn_boolean_t locked_here;
  int i;

  SVN_ERR(svn_wc__db_read_kind(&kind, wc_ctx->db, local_abspath, TRUE,
                               scratch_pool));
  if (kind != svn_wc__db_kind_dir)
    local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);

  SVN_ERR(svn_wc__db_wq_fetch(&id, &work_item, wc_ctx->db, local_abspath,
                              scratch_pool, scratch_pool));
  if (work_item)
    {
      /* Do not release locks (here or below) if there is work to do.  */
      return SVN_NO_ERROR;
    }

  /* We need to recursively remove locks (see comment in
     svn_wc__acquire_write_lock(). */

  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_wc__db_read_children(&children, wc_ctx->db, local_abspath,
                                   scratch_pool, iterpool));
  for (i = 0; i < children->nelts; i ++)
    {
      const char *child_relpath = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;

      svn_pool_clear(iterpool);
      child_abspath = svn_dirent_join(local_abspath, child_relpath, iterpool);

      SVN_ERR(svn_wc__db_read_kind(&kind, wc_ctx->db, child_abspath, FALSE,
                                   iterpool));
      if (kind == svn_wc__db_kind_dir)
        SVN_ERR(svn_wc__release_write_lock(wc_ctx, child_abspath, iterpool));
    }

  SVN_ERR(svn_wc__db_temp_own_lock(&locked_here, wc_ctx->db, local_abspath,
                                   iterpool));
  if (locked_here)
    SVN_ERR(svn_wc__db_wclock_remove(wc_ctx->db, local_abspath, iterpool));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__call_with_write_lock(svn_wc__with_write_lock_func_t func,
                             void *baton,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  svn_error_t *err1, *err2;
  SVN_ERR(svn_wc__acquire_write_lock(NULL, wc_ctx, local_abspath,
                                     scratch_pool, scratch_pool));
  err1 = func(baton, result_pool, scratch_pool);
  err2 = svn_wc__release_write_lock(wc_ctx, local_abspath, scratch_pool);
  return svn_error_compose_create(err1, err2);
}

svn_error_t *
svn_wc__path_switched(svn_boolean_t *switched,
                      svn_wc_context_t *wc_ctx,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool)
{
  svn_boolean_t wc_root;

  return svn_wc__check_wc_root(&wc_root, NULL, switched, wc_ctx->db,
                               local_abspath, scratch_pool);
}


