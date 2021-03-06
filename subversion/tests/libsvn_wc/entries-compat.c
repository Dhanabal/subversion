/*
 * entries-compat.c :  test backwards compatibility issues for entries
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
#include <apr_general.h>

#include "svn_types.h"

/* Make sure SVN_DEPRECATED is defined as empty before including svn_io.h.
   We don't want to trigger deprecation warnings.  */
#ifdef SVN_DEPRECATED
#undef SVN_DEPRECATED
#endif
#define SVN_DEPRECATED
#include "svn_io.h"

#include "svn_dirent_uri.h"
#include "svn_pools.h"
#include "svn_wc.h"

#include "../../include/private/svn_sqlite.h"

#include "../../libsvn_wc/wc.h"
#include "../../libsvn_wc/wc-queries.h"
#include "../../libsvn_wc/wc_db.h"

#include "../svn_test.h"


/* NOTE: these must be canonical!  */
#define ROOT_ONE "http://example.com/one"
#define ROOT_TWO "http://example.com/two"
#define ROOT_THREE "http://example.com/three"

#define UUID_ONE "uuid1"
#define UUID_TWO "uuid2"
#define UUID_THREE "uuid3"

#define TIME_1 1235142208
#define TIME_2 1235142268
#define TIME_3 1235142328

#define TIME_1s APR_STRINGIFY(TIME_1) "000000"
#define TIME_2s APR_STRINGIFY(TIME_2) "000000"
#define TIME_3s APR_STRINGIFY(TIME_3) "000000"

#define TIME_1a apr_time_from_sec(TIME_1)
#define TIME_2a apr_time_from_sec(TIME_2)
#define TIME_3a apr_time_from_sec(TIME_3)

#define AUTHOR_1 "johndoe"
#define AUTHOR_2 "janedoe"

/* Stick to MD5 values. We don't want to trigger SHA1->MD5 lookups.  */
#define MD5_1 "2d18c5e57e84c5b8a5e9a6e13fa394dc"
#define MD5_2 "5d41402abc4b2a76b9719d911017c592"

#define I_TC_DATA "((conflict F file update edited deleted (version 23 " ROOT_ONE " 1 2 branch1/ft/F none) (version 23 " ROOT_ONE " 1 3 branch1/ft/F file)) (conflict G file update edited deleted (version 23 " ROOT_ONE " 1 2 branch1/ft/F none) (version 23 " ROOT_ONE " 1 3 branch1/ft/F file)) )"

static const char * const TESTING_DATA = (
   /* Load our test data.

      Note: do not use named-column insertions. This allows us to test
      the column count in the schema matches our expectation here. */

   "insert into repository values (1, '" ROOT_ONE "', '" UUID_ONE "'); "
   "insert into repository values (2, '" ROOT_TWO "', '" UUID_TWO "'); "
   "insert into wcroot values (1, null); "

   /* ### The file_externals column in BASE_NODE is temporary, and will be
      ### removed.  However, to keep the tests passing, we need to add it
      ### to the following insert statements.  *Be sure to remove it*. */
   "insert into base_node values ("
   "  1, '', 1, '', null, 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', 'infinity', null, null, '()', null, 0, "
   "  null); "
   "insert into base_node values ("
   "  1, 'A', null, null, '', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 10, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'B', null, null, '', 'excluded', 'symlink', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'C', null, null, '', 'absent', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'D', null, null, '', 'not-present', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'E', null, null, '', 'incomplete', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'F', null, null, '', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'G', 2, 'G-alt', '', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'H', null, null, '', 'normal', 'symlink', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, 'H-target', null, '()', null, "
   "  null, null); "
   "insert into base_node values ("
   "  1, 'I', null, null, '', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J', null, null, '', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-e', null, null, 'J', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-e/J-e-a', null, null, 'J/J-e', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-e/J-e-b', null, null, 'J/J-e', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-e/J-e-b/Jeba', null, null, 'J/J-e/J-e-b', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-f', null, null, 'J', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'J/J-f/J-f-a', null, null, 'J/J-f', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'K', null, null, '', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'K/K-a', null, null, 'K', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into base_node values ("
   "  1, 'K/K-b', null, null, 'K', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   " "
   "insert into working_node values ("
   "  1, 'I', '', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'immediates', null, "
   "  2, 'some/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J', '', 'normal', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-a', 'J', 'normal', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b', 'J', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'infinity', null, "
   "  2, 'some/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b/J-b-a', 'J/J-b', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'infinity', null, "
   "  2, 'another/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b/J-b-b', 'J/J-b', 'normal', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-c', 'J', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-c/J-c-a', 'J/J-c', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-d', 'J', 'normal', 'file', "
   "  '$md5 $" MD5_1 "', 10, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', null, null, "
   "  2, 'moved/file', 2, 1, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e', 'J', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, 'other/place', null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e/J-e-a', 'J/J-e', 'not-present', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e/J-e-b', 'J/J-e', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e/J-e-b/Jeba', 'J/J-e/J-e-b', 'base-deleted', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-f', 'J', 'normal', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-f/J-f-a', 'J/J-f', 'base-deleted', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'K', '', 'base-deleted', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'K/K-a', 'K', 'base-deleted', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'K/K-b', 'K', 'base-deleted', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, 'moved/away', null, '()', 0); "
   "insert into working_node values ("
   "  1, 'L', '', 'normal', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'L/L-a', 'L', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'L/L-a/L-a-a', 'L', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   " "
   "insert into actual_node values ("
   "  1, 'I', '', null, null, null, null, null, 'changelist', null, "
   "'" I_TC_DATA "', null, null, null, null);"
   "  "
   "insert into base_node values ("
   "  1, 'M', null, null, '', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null, null, "
   "  null); "
   "insert into working_node values ("
   "  1, 'M/M-a', 'M', 'not-present', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   );


static const char * const M_TESTING_DATA = (
   /* Load our test data.

      Note: do not use named-column insertions. This allows us to test
      the column count in the schema matches our expectation here. */

   "insert into repository values (1, '" ROOT_ONE "', '" UUID_ONE "'); "
   "insert into repository values (2, '" ROOT_TWO "', '" UUID_TWO "'); "
   "insert into wcroot values (1, null); "

   "insert into base_node values ("
   "  1, '', 1, 'M', null, 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', 'infinity', null, null, '()', null, 0, "
   "  null); "
   );

WC_QUERIES_SQL_DECLARE_STATEMENTS(statements);


static svn_error_t *
make_one_db(const char *dirpath,
            const char * const my_statements[],
            apr_pool_t *scratch_pool)
{
  const char *dbpath = svn_dirent_join(dirpath, "wc.db", scratch_pool);
  svn_sqlite__db_t *sdb;

  /* Create fake-wc/SUBDIR/.svn/ for placing the metadata. */
  SVN_ERR(svn_io_make_dir_recursively(dirpath, scratch_pool));

  svn_error_clear(svn_io_remove_file(dbpath, scratch_pool));
  SVN_ERR(svn_sqlite__open(&sdb, dbpath, svn_sqlite__mode_rwcreate,
                           my_statements,
                           0, NULL,
                           scratch_pool, scratch_pool));

  /* Create the database's schema.  */
  SVN_ERR(svn_sqlite__exec_statements(sdb, /* my_statements[] */ 0));

  /* Throw our extra data into the database.  */
  SVN_ERR(svn_sqlite__exec_statements(sdb, /* my_statements[] */ 1));

  return SVN_NO_ERROR;
}


static svn_error_t *
create_fake_wc(const char *subdir, int format, apr_pool_t *scratch_pool)
{
  const char *dirpath;
  const char * const my_statements[] = {
    statements[STMT_CREATE_SCHEMA],
    TESTING_DATA,
    NULL
  };
  const char * const M_statements[] = {
    statements[STMT_CREATE_SCHEMA],
    M_TESTING_DATA,
    NULL
  };

  dirpath = svn_dirent_join_many(scratch_pool,
                                 "fake-wc", subdir, ".svn", NULL);
  SVN_ERR(make_one_db(dirpath, my_statements, scratch_pool));

  dirpath = svn_dirent_join_many(scratch_pool,
                                 "fake-wc", subdir, "M", ".svn", NULL);
  SVN_ERR(make_one_db(dirpath, M_statements, scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
create_open(svn_wc__db_t **db,
            const char **local_abspath,
            const char *subdir,
            apr_pool_t *pool)
{
  SVN_ERR(create_fake_wc(subdir, SVN_WC__VERSION, pool));

  SVN_ERR(svn_dirent_get_absolute(local_abspath,
                                  svn_dirent_join("fake-wc", subdir, pool),
                                  pool));
  SVN_ERR(svn_wc__db_open(db, svn_wc__db_openmode_readwrite,
                          NULL /* config */,
                          TRUE /* auto_upgrade */,
                          TRUE /* enforce_empty_wq */,
                          pool, pool));
  
  return SVN_NO_ERROR;
}


static svn_error_t *
test_entries_alloc(apr_pool_t *pool)
{
  svn_wc__db_t *db;
  const char *local_abspath;
  svn_wc_adm_access_t *adm_access;
  apr_hash_t *entries;
  const svn_wc_entry_t *entry;
  const char *local_relpath;

#undef WC_NAME
#define WC_NAME "test_entries_alloc"

  SVN_ERR(create_open(&db, &local_abspath, WC_NAME, pool));

  SVN_ERR(svn_wc_adm_open3(&adm_access,
                           NULL /* associated */,
                           svn_dirent_join("fake-wc", WC_NAME, pool),
                           FALSE /* write_lock */,
                           0 /* levels_to_lock */,
                           NULL /* cancel_func */,
                           NULL /* cancel_baton */,
                           pool));
  SVN_ERR(svn_wc_entries_read(&entries, adm_access, TRUE /* show_hidden */,
                              pool));

  /* The wcroot has 12 BASE children + 1 WORKING child + "this dir".  */
  SVN_TEST_ASSERT(apr_hash_count(entries) == 14);

  /* The "D" entry in the entries hash should be what we get from the
     svn_wc_entry() entrypoint.  */
  local_relpath = svn_dirent_join_many(pool,
                                       "fake-wc",
                                       WC_NAME,
                                       "D",
                                       NULL);
  SVN_ERR(svn_wc_entry(&entry, local_relpath, adm_access, TRUE, pool));
  SVN_TEST_ASSERT(entry == apr_hash_get(entries, "D", APR_HASH_KEY_STRING));

  /* This entry should be missing.  */
  SVN_ERR(svn_wc_entry(&entry, "missing", adm_access, TRUE, pool));
  SVN_TEST_ASSERT(entry == NULL);

  return SVN_NO_ERROR;
}


static svn_error_t *
test_stubs(apr_pool_t *pool)
{
  svn_wc__db_t *db;
  const char *local_abspath;
  const char *local_relpath;
  svn_wc_adm_access_t *adm_access;
  svn_wc_adm_access_t *subdir_access;
  const svn_wc_entry_t *stub_entry;
  const svn_wc_entry_t *entry;
  const svn_wc_entry_t *test_entry;
  apr_hash_t *entries;

#undef WC_NAME
#define WC_NAME "test_stubs"

  SVN_ERR(create_open(&db, &local_abspath, WC_NAME, pool));

  /* The "M" entry is a subdir. Let's ensure we can reach its stub,
     and the actual contents.  */
  local_relpath = svn_dirent_join_many(pool,
                                       "fake-wc",
                                       WC_NAME,
                                       "M",
                                       NULL);

  SVN_ERR(svn_wc_adm_open3(&adm_access,
                           NULL /* associated */,
                           svn_dirent_join("fake-wc", WC_NAME, pool),
                           FALSE /* write_lock */,
                           0 /* levels_to_lock */,
                           NULL /* cancel_func */,
                           NULL /* cancel_baton */,
                           pool));

  /* Ensure we get the stub. NOTE: do this before we have associated the
     subdir baton with ADM_ACCESS.  */
  SVN_ERR(svn_wc_entry(&stub_entry, local_relpath, adm_access, TRUE, pool));
  SVN_TEST_STRING_ASSERT(stub_entry->name, "M");

  SVN_ERR(svn_wc_adm_open3(&subdir_access,
                           adm_access,
                           local_relpath,
                           FALSE /* write_lock */,
                           0 /* levels_to_lock */,
                           NULL /* cancel_func */,
                           NULL /* cancel_baton */,
                           pool));

  /* Ensure we get the real entry.  */
  SVN_ERR(svn_wc_entry(&entry, local_relpath, subdir_access, TRUE, pool));
  SVN_TEST_STRING_ASSERT(entry->name, "");

  /* Ensure that we get the SAME entry, even using the parent baton.  */
  SVN_ERR(svn_wc_entry(&test_entry, local_relpath, adm_access, TRUE, pool));
  SVN_TEST_ASSERT(test_entry == entry);

  /* Ensure we get the stub when reading entries with ADM_ACCESS.  */
  SVN_ERR(svn_wc_entries_read(&entries, adm_access, TRUE /* show_hidden */,
                              pool));
  SVN_TEST_ASSERT(stub_entry
                  == apr_hash_get(entries, "M", APR_HASH_KEY_STRING));

  /* Ensure we get the real entry when reading entries with SUBDIR_ACCESS.  */
  SVN_ERR(svn_wc_entries_read(&entries, subdir_access, TRUE /* show_hidden */,
                              pool));
  SVN_TEST_ASSERT(entry
                  == apr_hash_get(entries, "", APR_HASH_KEY_STRING));

  return SVN_NO_ERROR;
}


struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_entries_alloc,
                   "entries are allocated in access baton"),
    SVN_TEST_PASS2(test_stubs,
                   "access baton mojo can return stubs"),
    SVN_TEST_NULL
  };
