/*
 * commit-cmd.c -- Check changes into the repository.
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

/* ==================================================================== */



/*** Includes. ***/

#include <apr_general.h>

#include "svn_error.h"
#include "svn_error_codes.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_config.h"
#include "cl.h"



/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__commit(apr_getopt_t *os,
               void *baton,
               apr_pool_t *pool)
{
  svn_error_t *err;
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  apr_array_header_t *condensed_targets;
  const char *base_dir;
  svn_config_t *cfg;
  svn_boolean_t no_unlock = FALSE;
  svn_commit_info_t *commit_info = NULL;
  int i;

  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, pool));

  /* Check that no targets are URLs */
  for (i = 0; i < targets->nelts; i++)
    {
      const char *target = APR_ARRAY_IDX(targets, i, const char *);
      if (svn_path_is_url(target))
        return svn_error_create(SVN_ERR_WC_BAD_PATH, NULL,
                                "Must give local path (not URL) as the "
                                "target of a commit");
    }

  /* Add "." if user passed 0 arguments. */
  svn_opt_push_implicit_dot_target(targets, pool);

  SVN_ERR(svn_opt_eat_peg_revisions(&targets, targets, pool));

  /* Condense the targets (like commit does)... */
  SVN_ERR(svn_path_condense_targets(&base_dir,
                                    &condensed_targets,
                                    targets,
                                    TRUE,
                                    pool));

  if ((! condensed_targets) || (! condensed_targets->nelts))
    {
      const char *parent_dir, *base_name;

      SVN_ERR(svn_wc_get_actual_target2(&parent_dir, &base_name, ctx->wc_ctx,
                                        base_dir, pool, pool));
      if (*base_name)
        base_dir = apr_pstrdup(pool, parent_dir);
    }

  if (! opt_state->quiet)
    SVN_ERR(svn_cl__get_notifier(&ctx->notify_func2, &ctx->notify_baton2,
                                 FALSE, FALSE, FALSE, pool));

  if (opt_state->depth == svn_depth_unknown)
    opt_state->depth = svn_depth_infinity;

  cfg = apr_hash_get(ctx->config, SVN_CONFIG_CATEGORY_CONFIG,
                     APR_HASH_KEY_STRING);
  if (cfg)
    SVN_ERR(svn_config_get_bool(cfg, &no_unlock,
                                SVN_CONFIG_SECTION_MISCELLANY,
                                SVN_CONFIG_OPTION_NO_UNLOCK, FALSE));

  /* We're creating a new log message baton because we can use our base_dir
     to store the temp file, instead of the current working directory.  The
     client might not have write access to their working directory, but they
     better have write access to the directory they're committing.  */
  SVN_ERR(svn_cl__make_log_msg_baton(&(ctx->log_msg_baton3),
                                     opt_state, base_dir,
                                     ctx->config, pool));

  /* Commit. */
  err = svn_client_commit4(&commit_info,
                           targets,
                           opt_state->depth,
                           no_unlock,
                           opt_state->keep_changelists,
                           opt_state->changelists,
                           opt_state->revprop_table,
                           ctx,
                           pool);
  if (err)
    {
      svn_error_t *root_err = svn_error_root_cause(err);
      if (root_err->apr_err == SVN_ERR_UNKNOWN_CHANGELIST)
        {
          /* Strip any errors wrapped around this root cause.  Note
             that this handling differs from that of any other
             commands, because of the way 'commit' internally harvests
             its list of committables. */
          root_err = svn_error_dup(root_err);
          svn_error_clear(err);
          err = root_err;
        }
    }
  SVN_ERR(svn_cl__cleanup_log_msg(ctx->log_msg_baton3, err, pool));
  if (! err && ! opt_state->quiet)
    SVN_ERR(svn_cl__print_commit_info(commit_info, pool));

  return SVN_NO_ERROR;
}
