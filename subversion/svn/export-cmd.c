/*
 * export-cmd.c -- Subversion export command
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

#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "cl.h"

#include "svn_private_config.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__export(apr_getopt_t *os,
               void *baton,
               apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *from, *to;
  apr_array_header_t *targets;
  svn_error_t *err;
  svn_opt_revision_t peg_revision;
  const char *truefrom;

  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, pool));

  /* We want exactly 1 or 2 targets for this subcommand. */
  if (targets->nelts < 1)
    return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, 0, NULL);
  if (targets->nelts > 2)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

  /* The first target is the `from' path. */
  from = APR_ARRAY_IDX(targets, 0, const char *);

  /* Get the peg revision if present. */
  SVN_ERR(svn_opt_parse_path(&peg_revision, &truefrom, from, pool));

  /* If only one target was given, split off the basename to use as
     the `to' path.  Else, a `to' path was supplied. */
  if (targets->nelts == 1)
    {
      to = svn_path_uri_decode(svn_uri_basename(truefrom, pool), pool);
    }
  else
    {
      to = APR_ARRAY_IDX(targets, 1, const char *);

      /* If given the cwd, pretend we weren't given anything. */
      if (strcmp("", to) == 0)
        to = svn_path_uri_decode(svn_uri_basename(truefrom, pool), pool);
    }

  if (! opt_state->quiet)
    SVN_ERR(svn_cl__get_notifier(&ctx->notify_func2, &ctx->notify_baton2,
                                 FALSE, TRUE, FALSE, pool));

  if (opt_state->depth == svn_depth_unknown)
    opt_state->depth = svn_depth_infinity;

  /* Do the export. */
  err = svn_client_export5(NULL, truefrom, to, &peg_revision,
                           &(opt_state->start_revision),
                           opt_state->force, opt_state->ignore_externals,
                           opt_state->ignore_keywords, opt_state->depth,
                           opt_state->native_eol, ctx, pool);
  if (err && err->apr_err == SVN_ERR_WC_OBSTRUCTED_UPDATE && !opt_state->force)
    SVN_ERR_W(err,
              _("Destination directory exists; please remove "
                "the directory or use --force to overwrite"));

  return svn_error_return(err);
}
