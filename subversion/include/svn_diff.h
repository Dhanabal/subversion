/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_diff.h
 * @brief Contextual diffing.
 *
 * This is an internalized library for performing contextual diffs
 * between sources of data.
 *
 * @note This is different than Subversion's binary-diffing engine.
 * That API lives in @c svn_delta.h -- see the "text deltas" section.  A
 * "text delta" is way of representing precise binary diffs between
 * strings of data.  The Subversion client and server send text deltas
 * to one another during updates and commits.
 *
 * This API, however, is (or will be) used for performing *contextual*
 * merges between files in the working copy.  During an update or
 * merge, 3-way file merging is needed.  And 'svn diff' needs to show
 * the differences between 2 files.
 *
 * The nice thing about this API is that it's very general.  It
 * operates on any source of data (a "datasource") and calculates
 * contextual differences on "tokens" within the data.  In our
 * particular usage, the datasources are files and the tokens are
 * lines.  But the possibilities are endless.
 */


#ifndef SVN_DIFF_H
#define SVN_DIFF_H

#include <apr.h>
#include <apr_pools.h>
#include <apr_tables.h>   /* for apr_array_header_t */

#include "svn_types.h"
#include "svn_io.h"       /* for svn_stream_t */
#include "svn_version.h"
#include "svn_string.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/**
 * Get libsvn_diff version information.
 *
 * @since New in 1.1.
 */
const svn_version_t *
svn_diff_version(void);


/* Diffs. */

/** An opaque type that represents a difference between either two or
 * three datasources.   This object is returned by svn_diff_diff(),
 * svn_diff_diff3() and svn_diff_diff4(), and consumed by a number of
 * other routines.
 */
typedef struct svn_diff_t svn_diff_t;

/**
 * There are four types of datasources.  In GNU diff3 terminology,
 * the first three types correspond to the phrases "older", "mine",
 * and "yours".
 */
typedef enum svn_diff_datasource_e
{
  /** The oldest form of the data. */
  svn_diff_datasource_original,

  /** The same data, but potentially changed by the user. */
  svn_diff_datasource_modified,

  /** The latest version of the data, possibly different than the
   * user's modified version.
   */
  svn_diff_datasource_latest,

  /** The common ancestor of original and modified. */
  svn_diff_datasource_ancestor

} svn_diff_datasource_e;


/** A vtable for reading data from the three datasources. */
typedef struct svn_diff_fns_t
{
  /** Open the datasource of type @a datasource. */
  svn_error_t *(*datasource_open)(void *diff_baton,
                                  svn_diff_datasource_e datasource);

  /** Close the datasource of type @a datasource. */
  svn_error_t *(*datasource_close)(void *diff_baton,
                                   svn_diff_datasource_e datasource);

  /** Get the next "token" from the datasource of type @a datasource.
   *  Return a "token" in @a *token.   Return a hash of "token" in @a *hash.
   *  Leave @a token and @a hash untouched when the datasource is exhausted.
   */
  svn_error_t *(*datasource_get_next_token)(apr_uint32_t *hash, void **token,
                                            void *diff_baton,
                                            svn_diff_datasource_e datasource);

  /** A function for ordering the tokens, resembling 'strcmp' in functionality.
   * @a compare should contain the return value of the comparison:
   * If @a ltoken and @a rtoken are "equal", return 0.  If @a ltoken is
   * "less than" @a rtoken, return a number < 0.  If @a ltoken  is
   * "greater than" @a rtoken, return a number > 0.
   */
  svn_error_t *(*token_compare)(void *diff_baton,
                                void *ltoken,
                                void *rtoken,
                                int *compare);

  /** Free @a token from memory, the diff algorithm is done with it. */
  void (*token_discard)(void *diff_baton,
                        void *token);

  /** Free *all* tokens from memory, they're no longer needed. */
  void (*token_discard_all)(void *diff_baton);
} svn_diff_fns_t;


/* The Main Events */

/** Given a vtable of @a diff_fns/@a diff_baton for reading datasources,
 * return a diff object in @a *diff that represents a difference between
 * an "original" and "modified" datasource.  Do all allocation in @a pool.
 */
svn_error_t *
svn_diff_diff(svn_diff_t **diff,
              void *diff_baton,
              const svn_diff_fns_t *diff_fns,
              apr_pool_t *pool);

/** Given a vtable of @a diff_fns/@a diff_baton for reading datasources,
 * return a diff object in @a *diff that represents a difference between
 * three datasources: "original", "modified", and "latest".  Do all
 * allocation in @a pool.
 */
svn_error_t *
svn_diff_diff3(svn_diff_t **diff,
               void *diff_baton,
               const svn_diff_fns_t *diff_fns,
               apr_pool_t *pool);

/** Given a vtable of @a diff_fns/@a diff_baton for reading datasources,
 * return a diff object in @a *diff that represents a difference between
 * two datasources: "original" and "latest", adjusted to become a full
 * difference between "original", "modified" and "latest" using "ancestor".
 * Do all allocation in @a pool.
 */
svn_error_t *
svn_diff_diff4(svn_diff_t **diff,
               void *diff_baton,
               const svn_diff_fns_t *diff_fns,
               apr_pool_t *pool);


/* Utility functions */

/** Determine if a diff object contains conflicts.  If it does, return
 * @c TRUE, else return @c FALSE.
 */
svn_boolean_t
svn_diff_contains_conflicts(svn_diff_t *diff);


/** Determine if a diff object contains actual differences between the
 * datasources.  If so, return @c TRUE, else return @c FALSE.
 */
svn_boolean_t
svn_diff_contains_diffs(svn_diff_t *diff);




/* Displaying Diffs */

/** A vtable for displaying (or consuming) differences between datasources.
 *
 * Differences, similarities, and conflicts are described by lining up
 * "ranges" of data.
 *
 * @note These callbacks describe data ranges in units of "tokens".
 * A "token" is whatever you've defined it to be in your datasource
 * @c svn_diff_fns_t vtable.
 */
typedef struct svn_diff_output_fns_t
{
  /* Two-way and three-way diffs both call the first two output functions: */

  /**
   * If doing a two-way diff, then an *identical* data range was found
   * between the "original" and "modified" datasources.  Specifically,
   * the match starts at @a original_start and goes for @a original_length
   * tokens in the original data, and at @a modified_start for
   * @a modified_length tokens in the modified data.
   *
   * If doing a three-way diff, then all three datasources have
   * matching data ranges.  The range @a latest_start, @a latest_length in
   * the "latest" datasource is identical to the range @a original_start,
   * @a original_length in the original data, and is also identical to
   * the range @a modified_start, @a modified_length in the modified data.
   */
  svn_error_t *(*output_common)(void *output_baton,
                                apr_off_t original_start,
                                apr_off_t original_length,
                                apr_off_t modified_start,
                                apr_off_t modified_length,
                                apr_off_t latest_start,
                                apr_off_t latest_length);

  /**
   * If doing a two-way diff, then an *conflicting* data range was found
   * between the "original" and "modified" datasources.  Specifically,
   * the conflict starts at @a original_start and goes for @a original_length
   * tokens in the original data, and at @a modified_start for
   * @a modified_length tokens in the modified data.
   *
   * If doing a three-way diff, then an identical data range was discovered
   * between the "original" and "latest" datasources, but this conflicts with
   * a range in the "modified" datasource.
   */
  svn_error_t *(*output_diff_modified)(void *output_baton,
                                       apr_off_t original_start,
                                       apr_off_t original_length,
                                       apr_off_t modified_start,
                                       apr_off_t modified_length,
                                       apr_off_t latest_start,
                                       apr_off_t latest_length);

  /* ------ The following callbacks are used by three-way diffs only --- */

  /** An identical data range was discovered between the "original" and
   * "modified" datasources, but this conflicts with a range in the
   * "latest" datasource.
   */
  svn_error_t *(*output_diff_latest)(void *output_baton,
                                     apr_off_t original_start,
                                     apr_off_t original_length,
                                     apr_off_t modified_start,
                                     apr_off_t modified_length,
                                     apr_off_t latest_start,
                                     apr_off_t latest_length);

  /** An identical data range was discovered between the "modified" and
   * "latest" datasources, but this conflicts with a range in the
   * "original" datasource.
   */
  svn_error_t *(*output_diff_common)(void *output_baton,
                                     apr_off_t original_start,
                                     apr_off_t original_length,
                                     apr_off_t modified_start,
                                     apr_off_t modified_length,
                                     apr_off_t latest_start,
                                     apr_off_t latest_length);

  /** All three datasources have conflicting data ranges.  The range
   * @a latest_start, @a latest_length in the "latest" datasource conflicts
   * with the range @a original_start, @a original_length in the "original"
   * datasource, and also conflicts with the range @a modified_start,
   * @a modified_length in the "modified" datasource.
   * If there are common ranges in the "modified" and "latest" datasources
   * in this conflicting range, @a resolved_diff will contain a diff
   * which can be used to retrieve the common and conflicting ranges.
   */
  svn_error_t *(*output_conflict)(void *output_baton,
                                  apr_off_t original_start,
                                  apr_off_t original_length,
                                  apr_off_t modified_start,
                                  apr_off_t modified_length,
                                  apr_off_t latest_start,
                                  apr_off_t latest_length,
                                  svn_diff_t *resolved_diff);
} svn_diff_output_fns_t;

/** Style for displaying conflicts during diff3 output.
 *
 * @since New in 1.6.
 */
typedef enum svn_diff_conflict_display_style_t
{
  /** Display modified and latest, with conflict markers. */
  svn_diff_conflict_display_modified_latest,

  /** Like svn_diff_conflict_display_modified_latest, but with an
      extra effort to identify common sequences between modified and
      latest. */
  svn_diff_conflict_display_resolved_modified_latest,

  /** Display modified, original, and latest, with conflict
      markers. */
  svn_diff_conflict_display_modified_original_latest,

  /** Just display modified, with no markers. */
  svn_diff_conflict_display_modified,

  /** Just display latest, with no markers. */
  svn_diff_conflict_display_latest,

  /** Like svn_diff_conflict_display_modified_original_latest, but
      *only* showing conflicts. */
  svn_diff_conflict_display_only_conflicts
} svn_diff_conflict_display_style_t;


/** Given a vtable of @a output_fns/@a output_baton for consuming
 * differences, output the differences in @a diff.
 */
svn_error_t *
svn_diff_output(svn_diff_t *diff,
                void *output_baton,
                const svn_diff_output_fns_t *output_fns);



/* Diffs on files */

/** To what extent whitespace should be ignored when comparing lines.
 *
 * @since New in 1.4.
 */
typedef enum svn_diff_file_ignore_space_t
{
  /** Ignore no whitespace. */
  svn_diff_file_ignore_space_none,

  /** Ignore changes in sequences of whitespace characters, treating each
   * sequence of whitespace characters as a single space. */
  svn_diff_file_ignore_space_change,

  /** Ignore all whitespace characters. */
  svn_diff_file_ignore_space_all
} svn_diff_file_ignore_space_t;

/** Options to control the behaviour of the file diff routines.
 *
 * @since New in 1.4.
 *
 * @note This structure may be extended in the future, so to preserve binary
 * compatibility, users must not allocate structs of this type themselves.
 * @see svn_diff_file_options_create().
 *
 * @note Although its name suggests otherwise, this structure is used to
 *       pass options to file as well as in-memory diff functions.
 */
typedef struct svn_diff_file_options_t
{
  /** To what extent whitespace should be ignored when comparing lines.
   * The default is @c svn_diff_file_ignore_space_none. */
  svn_diff_file_ignore_space_t ignore_space;
  /** Whether to treat all end-of-line markers the same when comparing lines.
   * The default is @c FALSE. */
  svn_boolean_t ignore_eol_style;
  /** Whether the '@@' lines of the unified diff output should include a prefix
    * of the nearest preceding line that starts with a character that might be
    * the initial character of a C language identifier.  The default is
    * @c FALSE.
    */
  svn_boolean_t show_c_function;
} svn_diff_file_options_t;

/** Allocate a @c svn_diff_file_options_t structure in @a pool, initializing
 * it with default values.
 *
 * @since New in 1.4.
 */
svn_diff_file_options_t *
svn_diff_file_options_create(apr_pool_t *pool);

/**
 * Parse @a args, an array of <tt>const char *</tt> command line switches
 * and adjust @a options accordingly.  @a options is assumed to be initialized
 * with default values.  @a pool is used for temporary allocation.
 *
 * @since New in 1.4.
 *
 * The following options are supported:
 * - --ignore-space-change, -b
 * - --ignore-all-space, -w
 * - --ignore-eol-style
 * - --unified, -u (for compatibility, does nothing).
 */
svn_error_t *
svn_diff_file_options_parse(svn_diff_file_options_t *options,
                            const apr_array_header_t *args,
                            apr_pool_t *pool);


/** A convenience function to produce a diff between two files.
 *
 * @since New in 1.4.
 *
 * Return a diff object in @a *diff (allocated from @a pool) that represents
 * the difference between an @a original file and @a modified file.
 * (The file arguments must be full paths to the files.)
 *
 * Compare lines according to the relevant fields of @a options.
 */
svn_error_t *
svn_diff_file_diff_2(svn_diff_t **diff,
                     const char *original,
                     const char *modified,
                     const svn_diff_file_options_t *options,
                     apr_pool_t *pool);

/** Similar to svn_file_diff_2(), but with @a options set to a struct with
 * default options.
 *
 * @deprecated Provided for backwards compatibility with the 1.3 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_diff(svn_diff_t **diff,
                   const char *original,
                   const char *modified,
                   apr_pool_t *pool);

/** A convenience function to produce a diff between three files.
 *
 * @since New in 1.4.
 *
 * Return a diff object in @a *diff (allocated from @a pool) that represents
 * the difference between an @a original file, @a modified file, and @a latest
 * file.
 *
 * Compare lines according to the relevant fields of @a options.
 */
svn_error_t *
svn_diff_file_diff3_2(svn_diff_t **diff,
                      const char *original,
                      const char *modified,
                      const char *latest,
                      const svn_diff_file_options_t *options,
                      apr_pool_t *pool);

/** Similar to svn_diff_file_diff3_2(), but with @a options set to a struct
 * with default options.
 *
 * @deprecated Provided for backwards compatibility with the 1.3 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_diff3(svn_diff_t **diff,
                    const char *original,
                    const char *modified,
                    const char *latest,
                    apr_pool_t *pool);

/** A convenience function to produce a diff between four files.
 *
 * @since New in 1.4.
 *
 * Return a diff object in @a *diff (allocated from @a pool) that represents
 * the difference between an @a original file, @a modified file, @a latest
 * and @a ancestor file. (The file arguments must be full paths to the files.)
 *
 * Compare lines according to the relevant fields of @a options.
 */
svn_error_t *
svn_diff_file_diff4_2(svn_diff_t **diff,
                      const char *original,
                      const char *modified,
                      const char *latest,
                      const char *ancestor,
                      const svn_diff_file_options_t *options,
                      apr_pool_t *pool);

/** Simliar to svn_file_diff4_2(), but with @a options set to a struct with
 * default options.
 *
 * @deprecated Provided for backwards compatibility with the 1.3 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_diff4(svn_diff_t **diff,
                    const char *original,
                    const char *modified,
                    const char *latest,
                    const char *ancestor,
                    apr_pool_t *pool);

/** A convenience function to produce unified diff output from the
 * diff generated by svn_diff_file_diff().
 *
 * @since New in 1.5.
 *
 * Output a @a diff between @a original_path and @a modified_path in unified
 * context diff format to @a output_stream.  Optionally supply
 * @a original_header and/or @a modified_header to be displayed in the header
 * of the output.  If @a original_header or @a modified_header is @c NULL, a
 * default header will be displayed, consisting of path and last modified time.
 * Output all headers and markers in @a header_encoding.  If @a relative_to_dir
 * is not @c NULL, the @a original_path and @a modified_path will have the
 * @a relative_to_dir stripped from the front of the respective paths.  If
 * @a relative_to_dir is @c NULL, paths will be not be modified.  If
 * @a relative_to_dir is not @c NULL but @a relative_to_dir is not a parent
 * path of the target, an error is returned. Finally, if @a relative_to_dir
 * is a URL, an error will be returned.
 */
svn_error_t *
svn_diff_file_output_unified3(svn_stream_t *output_stream,
                              svn_diff_t *diff,
                              const char *original_path,
                              const char *modified_path,
                              const char *original_header,
                              const char *modified_header,
                              const char *header_encoding,
                              const char *relative_to_dir,
                              svn_boolean_t show_c_function,
                              apr_pool_t *pool);

/** Similar to svn_diff_file_output_unified3(), but with @a relative_to_dir
 * set to NULL and @a show_c_function to false.
 *
 * @deprecated Provided for backwards compatibility with the 1.3 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_output_unified2(svn_stream_t *output_stream,
                              svn_diff_t *diff,
                              const char *original_path,
                              const char *modified_path,
                              const char *original_header,
                              const char *modified_header,
                              const char *header_encoding,
                              apr_pool_t *pool);

/** Similar to svn_diff_file_output_unified2(), but with @a header_encoding
 * set to @c APR_LOCALE_CHARSET.
 *
 * @deprecated Provided for backward compatibility with the 1.2 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_output_unified(svn_stream_t *output_stream,
                             svn_diff_t *diff,
                             const char *original_path,
                             const char *modified_path,
                             const char *original_header,
                             const char *modified_header,
                             apr_pool_t *pool);


/** A convenience function to produce diff3 output from the
 * diff generated by svn_diff_file_diff3().
 *
 * Output a @a diff between @a original_path, @a modified_path and
 * @a latest_path in merged format to @a output_stream.  Optionally supply
 * @a conflict_modified, @a conflict_original, @a conflict_separator and/or
 * @a conflict_latest to be displayed as conflict markers in the output.
 * If @a conflict_original, @a conflict_modified, @a conflict_latest and/or
 * @a conflict_separator is @c NULL, a default marker will be displayed.
 * @a conflict_style dictates how conflicts are displayed.
 *
 * @since New in 1.6.
 */
svn_error_t *
svn_diff_file_output_merge2(svn_stream_t *output_stream,
                            svn_diff_t *diff,
                            const char *original_path,
                            const char *modified_path,
                            const char *latest_path,
                            const char *conflict_original,
                            const char *conflict_modified,
                            const char *conflict_latest,
                            const char *conflict_separator,
                            svn_diff_conflict_display_style_t conflict_style,
                            apr_pool_t *pool);


/** Similar to svn_diff_file_output_merge2, but with @a
 * display_original_in_conflict and @a display_resolved_conflicts
 * booleans instead of the @a conflict_style enum.
 *
 * If both booleans are false, acts like
 * svn_diff_conflict_display_modified_latest; if @a
 * display_original_in_conflict is true, acts like
 * svn_diff_conflict_display_modified_original_latest; if @a
 * display_resolved_conflicts is true, acts like
 * svn_diff_conflict_display_resolved_modified_latest.  The booleans
 * may not both be true.
 *
 * @deprecated Provided for backward compatibility with the 1.5 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_file_output_merge(svn_stream_t *output_stream,
                           svn_diff_t *diff,
                           const char *original_path,
                           const char *modified_path,
                           const char *latest_path,
                           const char *conflict_original,
                           const char *conflict_modified,
                           const char *conflict_latest,
                           const char *conflict_separator,
                           svn_boolean_t display_original_in_conflict,
                           svn_boolean_t display_resolved_conflicts,
                           apr_pool_t *pool);



/* Diffs on in-memory structures */

/** Generate @a diff output from the @a original and @a modified
 * in-memory strings.  @a diff will be allocated from @a pool.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_diff_mem_string_diff(svn_diff_t **diff,
                         const svn_string_t *original,
                         const svn_string_t *modified,
                         const svn_diff_file_options_t *options,
                         apr_pool_t *pool);


/** Generate @a diff output from the @a orginal, @a modified and @a latest
 * in-memory strings.  @a diff will be allocated in @a pool.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_diff_mem_string_diff3(svn_diff_t **diff,
                          const svn_string_t *original,
                          const svn_string_t *modified,
                          const svn_string_t *latest,
                          const svn_diff_file_options_t *options,
                          apr_pool_t *pool);


/** Generate @a diff output from the @a original, @a modified and @a latest
 * in-memory strings, using @a ancestor.  @a diff will be allocated in @a pool.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_diff_mem_string_diff4(svn_diff_t **diff,
                          const svn_string_t *original,
                          const svn_string_t *modified,
                          const svn_string_t *latest,
                          const svn_string_t *ancestor,
                          const svn_diff_file_options_t *options,
                          apr_pool_t *pool);

/** Outputs the @a diff object generated by svn_diff_mem_string_diff()
 * in unified diff format on @a output_stream, using @a original
 * and @a modified for the text in the output.
 * A diff header is only written to the output if @a with_diff_header
 * is TRUE.
 *
 * Outputs the header and hunk delimiters in @a header_encoding.
 * A @a hunk_delimiter can optionally be specified.
 * If @a hunk_delimiter is NULL, use the default hunk delimiter "@@".
 *
 * @a original_header and @a modified header are
 * used to fill the field after the "---" and "+++" header markers.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_diff_mem_string_output_unified2(svn_stream_t *output_stream,
                                    svn_diff_t *diff,
                                    svn_boolean_t with_diff_header,
                                    const char *hunk_delimiter,
                                    const char *original_header,
                                    const char *modified_header,
                                    const char *header_encoding,
                                    const svn_string_t *original,
                                    const svn_string_t *modified,
                                    apr_pool_t *pool);

/** Similar to svn_diff_mem_string_output_unified2() but with
 * @a with_diff_header always set to TRUE and @a hunk_delimiter always
 * set to NULL.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_diff_mem_string_output_unified(svn_stream_t *output_stream,
                                   svn_diff_t *diff,
                                   const char *original_header,
                                   const char *modified_header,
                                   const char *header_encoding,
                                   const svn_string_t *original,
                                   const svn_string_t *modified,
                                   apr_pool_t *pool);

/** Output the @a diff generated by svn_diff_mem_string_diff3() in diff3
 * format on @a output_stream, using @a original, @a modified and @a latest
 * for content changes.
 *
 * Use the conflict markers @a conflict_original, @a conflict_modified,
 * @a conflict_latest and @a conflict_separator or the default one for
 * each of these if @c NULL is passed.
 *
 * @a conflict_style dictates how conflicts are displayed.
 *
 * @since New in 1.6.
 */
svn_error_t *
svn_diff_mem_string_output_merge2(svn_stream_t *output_stream,
                                  svn_diff_t *diff,
                                  const svn_string_t *original,
                                  const svn_string_t *modified,
                                  const svn_string_t *latest,
                                  const char *conflict_original,
                                  const char *conflict_modified,
                                  const char *conflict_latest,
                                  const char *conflict_separator,
                                  svn_diff_conflict_display_style_t style,
                                  apr_pool_t *pool);

/** Similar to svn_diff_mem_string_output_merge2, but with @a
 * display_original_in_conflict and @a display_resolved_conflicts
 * booleans instead of the @a conflict_style enum.
 *
 * If both booleans are false, acts like
 * svn_diff_conflict_display_modified_latest; if @a
 * display_original_in_conflict is true, acts like
 * svn_diff_conflict_display_modified_original_latest; if @a
 * display_resolved_conflicts is true, acts like
 * svn_diff_conflict_display_resolved_modified_latest.  The booleans
 * may not both be true.
 *
 * @deprecated Provided for backward compatibility with the 1.5 API.
 */
SVN_DEPRECATED
svn_error_t *
svn_diff_mem_string_output_merge(svn_stream_t *output_stream,
                                 svn_diff_t *diff,
                                 const svn_string_t *original,
                                 const svn_string_t *modified,
                                 const svn_string_t *latest,
                                 const char *conflict_original,
                                 const char *conflict_modified,
                                 const char *conflict_latest,
                                 const char *conflict_separator,
                                 svn_boolean_t display_original_in_conflict,
                                 svn_boolean_t display_resolved_conflicts,
                                 apr_pool_t *pool);




/* Diff parsing. If you want to apply a patch to a working copy
 * rather than parse it, see svn_client_patch(). */

/**
 * A single hunk inside a patch
 *
 * @since New in 1.7. */
typedef struct svn_hunk_t {
  /** The hunk's unidiff text as it appeared in the patch file,
   *  without range information. */
  svn_stream_t *diff_text;

  /**
   * The original and modified texts in the hunk range.
   * Derived from the diff text.
   *
   * For example, consider a hunk such as:
   *   @@ -1,5 +1,5 @@
   *    #include <stdio.h>
   *    int main(int argc, char *argv[])
   *    {
   *   -        printf("Hello World!\n");
   *   +        printf("I like Subversion!\n");
   *    }
   *
   * Then, the original text described by the hunk is:
   *   #include <stdio.h>
   *   int main(int argc, char *argv[])
   *   {
   *           printf("Hello World!\n");
   *   }
   *
   * And the modified text described by the hunk is:
   *   #include <stdio.h>
   *   int main(int argc, char *argv[])
   *   {
   *           printf("I like Subversion!\n");
   *   }
   *
   * Because these streams make use of line filtering and transformation,
   * they should only be read line-by-line with svn_stream_readline().
   * Reading them with svn_stream_read() will not yield the expected result,
   * because it will return the unidiff text from the patch file unmodified.
   * The streams support resetting.
   */
  svn_stream_t *original_text;
  svn_stream_t *modified_text;

  /**
   * Hunk ranges as they appeared in the patch file.
   * All numbers are lines, not bytes. */
  svn_linenum_t original_start;
  svn_linenum_t original_length;
  svn_linenum_t modified_start;
  svn_linenum_t modified_length;

  /** Number of lines starting with ' ' before first '+' or '-'. */
  svn_linenum_t leading_context;

  /** Number of lines starting with ' ' after last '+' or '-'. */
  svn_linenum_t trailing_context;
} svn_hunk_t;

/**
 * Data type to manage parsing of patches.
 *
 * @since New in 1.7. */
typedef struct svn_patch_t {
  /** Path to the patch file. */
  const char *path;

  /** The patch file itself. */
  apr_file_t *patch_file;

  /**
   * The old and new file names as retrieved from the patch file.
   * These paths are UTF-8 encoded and canonicalized, but otherwise
   * left unchanged from how they appeared in the patch file. */
  const char *old_filename;
  const char *new_filename;

  /**
   * An array containing an svn_hunk_t object for each hunk parsed
   * from the patch. */
  apr_array_header_t *hunks;
} svn_patch_t;

/**
 * Return the next @a *patch in @a patch_file.
 * If no patch can be found, set @a *patch to NULL.
 * If @a reverse is TRUE, invert the patch while parsing it.
 * If @a ignore_whitespace is TRUE, allow patches with no leading
 * whitespace to be parsed.
 * Allocate results in @a result_pool.
 * Use @a scratch_pool for all other allocations.
 * 
 * @since New in 1.7. */
svn_error_t *
svn_diff_parse_next_patch(svn_patch_t **patch,
                          apr_file_t *patch_file,
                          svn_boolean_t reverse,
                          svn_boolean_t ignore_whitespace,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);

/**
 * Dispose of @a patch, closing any streams used by it.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_diff_close_patch(const svn_patch_t *patch);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_DIFF_H */
