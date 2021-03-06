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
 * @file svn_dirent_uri.h
 * @brief A library to manipulate URIs, relative paths and directory entries.
 *
 * This library makes a clear distinction between several path formats:
 *
 *  - a dirent is a path on (local) disc or a UNC path (Windows) in either
 *    relative or absolute format
 *    Examples: "/foo/bar", "X:/temp", "//server/share" and on Windows "A:/"
 *    But not: "http://server"
 *
 *  - a URI is an absolute path that starts with a '/' or a schema definition.
 *    Examples: "/", "/foo", "http://server", "svn+ssh://user@host:123/file"
 *    But not: "file", "dir/file", "A:/dir"
 *    ### Currently the URI implementation still allows relpaths as valid
 *    uris, but this will change soon.
 *
 *  - a relative path (relpath) is an unrooted path that can be joined to
 *    any other relative path, uri or dirent. A relative path is never
 *    rooted/prefixed by a '/'.
 *    Examples: "file", "dir/file", "dir/subdir/../file"
 *    But not: "/file", "http://server/file"
 *
 * This distinction is needed because on Windows we have to handle some
 * dirents and URIs differently. Since it's not possible to determine from
 * the path string if it's a dirent or a URI, it's up to the API user to
 * make this choice. See also issue #2028.
 *
 * Nearly all the @c svn_dirent_xxx, @c svn_relpath_xxx and @c svn_uri_xxx
 * functions expect paths passed into them to be in canonical form.  The only
 * functions which do *not* have such expectations are:
 *
 *    - @c svn_dirent_canonicalize()
 *    - @c svn_dirent_is_canonical()
 *    - @c svn_dirent_internal_style()
 *    - @c svn_dirent_local_style()
 *    - @c svn_relpath_canonicalize()
 *    - @c svn_relpath_is_canonical()
 *    - @c svn_relpath_internal_style()
 *    - @c svn_relpath_local_style()
 *    - @c svn_uri_canonicalize()
 *    - @c svn_uri_is_canonical()
 *
 * Code that works with repository paths should always use repository
 * root based relative paths (relpaths), or uris if it has to specify the
 * repository itself too.
 *
 * All code that works with local files, MUST USE the dirent apis.
 *
 * When translating between local paths (dirents) and uris code should
 * always go via the relative path format.
 * E.g.
 *  - by truncating a parent portion from a path with svn_*_skip_ancestor(),
 *  - or by converting portions to basenames and then joining to existing paths.
 *
 * SECURITY WARNING: If a path that is received from an untrusted source -like
 * from the network- is converted to a dirent it should be tested with
 * svn_dirent_is_under_root() before you can assume the path to be a safe local
 * path.
 */

#ifndef SVN_DIRENT_URI_H
#define SVN_DIRENT_URI_H

#include <apr.h>
#include <apr_pools.h>
#include <apr_tables.h>

#include "svn_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/** Convert @a dirent from the local style to the canonical internal style.
 *
 * @since New in 1.6.
 */
const char *
svn_dirent_internal_style(const char *dirent,
                          apr_pool_t *pool);

/** Convert @a dirent from the canonical internal style to the local style.
 *
 * @since New in 1.6.
 */
const char *
svn_dirent_local_style(const char *dirent,
                       apr_pool_t *pool);

/** Convert @a relpath from the local style to the canonical internal style.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_internal_style(const char *relpath,
                           apr_pool_t *pool);

/** Convert @a relpath from the canonical internal style to the local style.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_local_style(const char *relpath,
                        apr_pool_t *pool);


/** Join a base dirent (@a base) with a component (@a component), allocated in
 * @a pool.
 *
 * If either @a base or @a component is the empty string, then the other
 * argument will be copied and returned.  If both are the empty string then
 * empty string is returned.
 *
 * If the @a component is an absolute dirent, then it is copied and returned.
 * The platform specific rules for joining paths are used to join the components.
 *
 * This function is NOT appropriate for native (local) file
 * dirents. Only for "internal" canonicalized dirents, since it uses '/'
 * for the separator.
 *
 * @since New in 1.6.
 */
char *
svn_dirent_join(const char *base,
                const char *component,
                apr_pool_t *pool);

/** Join multiple components onto a @a base dirent, allocated in @a pool. The
 * components are terminated by a @c NULL.
 *
 * If any component is the empty string, it will be ignored.
 *
 * If any component is an absolute dirent, then it resets the base and
 * further components will be appended to it.
 *
 * See svn_dirent_join() for further notes about joining dirents.
 *
 * @since New in 1.6.
 */
char *
svn_dirent_join_many(apr_pool_t *pool,
                     const char *base,
                     ...);

/** Join a base relpath (@a base) with a component (@a component), allocating
 * the result in @a pool. @a component need not be a single component.
 *
 * If either @a base or @a component is the empty path, then the other
 * argument will be copied and returned.  If both are the empty path the
 * empty path is returned.
 *
 * @since New in 1.7.
 */
char *
svn_relpath_join(const char *base,
                 const char *component,
                 apr_pool_t *pool);

/** Join a valid base uri (@a base) with a relative path or uri
 * (@a component), allocating the result in @a pool. @a component need
 * not be a single component: it can be a relative path or a '/'
 * prefixed relative path to join component to the root path of @a base.
 *
 * If @a component is the empty path, then @a base will be copied and
 * returned.
 *
 * If the @a component is an absolute uri, then it is copied and returned.
 *
 * If @a component starts with a '/' and @a base contains a scheme, the
 * scheme defined joining rules are applied.
 *
 * @since New in 1.7.
 */
char *
svn_uri_join(const char *base,
             const char *component,
             apr_pool_t *pool);


/** Gets the name of the specified canonicalized @a dirent as it is known
 * within its parent directory. If the @a dirent is root, return "". The
 * returned value will not have slashes in it.
 *
 * Example: svn_dirent_basename("/foo/bar") -> "bar"
 *
 * The returned basename will be allocated in @a pool. If @a pool is NULL
 * a pointer to the basename in @a dirent is returned.
 *
 * @note If an empty string is passed, then an empty string will be returned.
 *
 * @since New in 1.7.
 */
const char *
svn_dirent_basename(const char *dirent,
                    apr_pool_t *pool);

/** Get the dirname of the specified canonicalized @a dirent, defined as
 * the dirent with its basename removed.
 *
 * If @a dirent is root  ("/", "X:/", "//server/share/") or "", it is returned
 * unchanged.
 *
 * The returned dirname will be allocated in @a pool.
 *
 * @since New in 1.6.
 */
char *
svn_dirent_dirname(const char *dirent,
                   apr_pool_t *pool);

/** Divide the canonicalized @a dirent into @a *dirpath and @a
 * *base_name, allocated in @a pool.
 *
 * If @a dirpath or @a base_name is NULL, then don't set that one.
 *
 * Either @a dirpath or @a base_name may be @a dirent's own address, but they
 * may not both be the same address, or the results are undefined.
 *
 * If @a dirent has two or more components, the separator between @a dirpath
 * and @a base_name is not included in either of the new names.
 *
 * Examples:
 *             - <pre>"/foo/bar/baz"  ==>  "/foo/bar" and "baz"</pre>
 *             - <pre>"/bar"          ==>  "/"  and "bar"</pre>
 *             - <pre>"/"             ==>  "/"  and ""</pre>
 *             - <pre>"bar"           ==>  ""   and "bar"</pre>
 *             - <pre>""              ==>  ""   and ""</pre>
 *  Windows:   - <pre>"X:/"           ==>  "X:/" and ""</pre>
 *             - <pre>"X:/foo"        ==>  "X:/" and "foo"</pre>
 *             - <pre>"X:foo"         ==>  "X:" and "foo"</pre>
 *  Posix:     - <pre>"X:foo"         ==>  ""   and "X:foo"</pre>
 *
 * @since New in 1.7.
 */
void
svn_dirent_split(const char *dirent,
                 const char **dirpath,
                 const char **base_name,
                 apr_pool_t *pool);

/** Divide the canonicalized @a relpath into @a *dirpath and @a
 * *base_name, allocated in @a pool.
 *
 * If @a dirpath or @a base_name is NULL, then don't set that one.
 *
 * Either @a dirpath or @a base_name may be @a relpaths's own address, but
 * they may not both be the same address, or the results are undefined.
 *
 * If @a relpath has two or more components, the separator between @a dirpath
 * and @a base_name is not included in either of the new names.
 *
 *   examples:
 *             - <pre>"foo/bar/baz"  ==>  "foo/bar" and "baz"</pre>
 *             - <pre>"bar"          ==>  ""  and "bar"</pre>
 *             - <pre>""              ==>  ""   and ""</pre>
 *
 * @since New in 1.7.
 */
void
svn_relpath_split(const char *relpath,
                  const char **dirpath,
                  const char **base_name,
                  apr_pool_t *pool);

/** Get the basename of the specified canonicalized @a relpath.  The
 * basename is defined as the last component of the relpath.  If the @a
 * relpath has only one component then that is returned. The returned
 * value will have no slashes in it.
 *
 * Example: svn_relpath_basename("/trunk/foo/bar") -> "bar"
 *
 * The returned basename will be allocated in @a pool. If @a
 * pool is NULL a pointer to the basename in @a relpath is returned.
 *
 * @note If an empty string is passed, then an empty string will be returned.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_basename(const char *uri,
                     apr_pool_t *pool);

/** Get the dirname of the specified canonicalized @a relpath, defined as
 * the relpath with its basename removed.
 *
 * If @a relpath is empty, "" is returned.
 *
 * The returned relpath will be allocated in @a pool.
 *
 * @since New in 1.7.
 */
char *
svn_relpath_dirname(const char *relpath,
                    apr_pool_t *pool);


/** Divide the canonicalized @a uri into @a *dirpath and @a
 * *base_name, allocated in @a pool.
 *
 * If @a dirpath or @a base_name is NULL, then don't set that one.
 *
 * Either @a dirpath or @a base_name may be @a dirent's own address, but they
 * may not both be the same address, or the results are undefined.
 *
 * If @a dirent has two or more components, the separator between @a dirpath
 * and @a base_name is not included in either of the new names.
 *
 *   examples:
 *             - <pre>"/foo/bar/baz"  ==>  "/foo/bar" and "baz"</pre>
 *             - <pre>"/bar"          ==>  "/"  and "bar"</pre>
 *             - <pre>"/"             ==>  "/"  and "/"</pre>
 *             - <pre>"bar"           ==>  ""   and "bar"</pre>
 *             - <pre>""              ==>  ""   and ""</pre>
 *
 * @since New in 1.7.
 */
void
svn_uri_split(const char *dirent,
              const char **dirpath,
              const char **base_name,
              apr_pool_t *pool);

/** Get the basename of the specified canonicalized @a uri.  The
 * basename is defined as the last component of the uri.  If the @a dirent
 * is root then that is returned. Otherwise, the returned value will have no
 * slashes in it.
 *
 * Example: svn_dirent_basename("http://server/foo/bar") -> "bar"
 *
 * The returned basename will be allocated in @a pool. If @a pool is NULL
 * a pointer to the basename in @a uri is returned.
 *
 * @note If an empty string is passed, then an empty string will be returned.
 *
 * @since New in 1.7.
 */
const char *
svn_uri_basename(const char *uri,
                 apr_pool_t *pool);

/** Get the dirname of the specified canonicalized @a uri, defined as
 * the dirent with its basename removed.
 *
 * If @a dirent is root  (e.g. "http://server"), it is returned
 * unchanged.
 *
 * The returned dirname will be allocated in @a pool.
 *
 * @since New in 1.7.
 */
char *
svn_uri_dirname(const char *dirent,
                apr_pool_t *pool);


/** Return TRUE if @a dirent is considered absolute on the platform at
 * hand. E.g. '/foo' on Posix platforms or 'X:/foo', '//server/share/foo'
 * on Windows.
 *
 * @since New in 1.6.
 */
svn_boolean_t
svn_dirent_is_absolute(const char *dirent);

/** Return TRUE if @a uri is considered absolute or is a URL.
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_uri_is_absolute(const char *uri);

/** Return TRUE if @a dirent is considered a root directory on the platform
 * at hand.
 * E.g.:
 *  On Posix:   '/'
 *  On Windows: '/', 'X:/', '//server/share', 'X:'
 *
 * Note that on Windows '/' and 'X:' are roots, but paths starting with this
 * root are not absolute.
 *
 * @since New in 1.5.
 */
svn_boolean_t
svn_dirent_is_root(const char *dirent,
                   apr_size_t len);

/** Return TRUE if @a uri is a root path, so starts with '/'.
 *
 * Do not use this function with URLs.
 *
 * @since New in 1.7
 */
svn_boolean_t
svn_uri_is_root(const char *uri,
                apr_size_t len);

/** Return a new dirent like @a dirent, but transformed such that some types
 * of dirent specification redundancies are removed.
 *
 * This involves collapsing redundant "/./" elements, removing
 * multiple adjacent separator characters, removing trailing
 * separator characters, and possibly other semantically inoperative
 * transformations.
 *
 * Convert the server name of UNC paths lowercase and drive letters to
 * upper case on Windows.
 *
 * The returned dirent may be statically allocated or allocated from @a pool.
 *
 * @since New in 1.6.
 */
const char *
svn_dirent_canonicalize(const char *dirent,
                        apr_pool_t *pool);


/** Return a new relpath like @a relpath, but transformed such that some types
 * of relpath specification redundancies are removed.
 *
 * This involves collapsing redundant "/./" elements, removing
 * multiple adjacent separator characters, removing trailing
 * separator characters, and possibly other semantically inoperative
 * transformations.
 *
 * This functions supports relpaths.
 *
 * The returned relpath may be statically allocated or allocated from @a
 * pool.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_canonicalize(const char *uri,
                         apr_pool_t *pool);


/** Return a new uri like @a uri, but transformed such that some types
 * of uri specification redundancies are removed.
 *
 * This involves collapsing redundant "/./" elements, removing
 * multiple adjacent separator characters, removing trailing
 * separator characters, and possibly other semantically inoperative
 * transformations.
 *
 * If @a uri starts with a schema, this function also normalizes the
 * escaping of the path component by unescaping characters that don't
 * need escaping and escaping characters that do need escaping but
 * weren't.
 *
 * This functions supports URLs.
 *
 * The returned uri may be statically allocated or allocated from @a pool.
 *
 * @since New in 1.7.
 */
const char *
svn_uri_canonicalize(const char *uri,
                     apr_pool_t *pool);

/** Return @c TRUE iff @a dirent is canonical.  Use @a pool for temporary
 * allocations.
 *
 * @note The test for canonicalization is currently defined as
 * "looks exactly the same as @c svn_dirent_canonicalize() would make
 * it look".
 *
 * @since New in 1.6.
 */
svn_boolean_t
svn_dirent_is_canonical(const char *dirent,
                        apr_pool_t *pool);

/** Return @c TRUE iff @a relpath is canonical.  Use @a scratch_pool for
 * temporary allocations.
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_relpath_is_canonical(const char *relpath,
                         apr_pool_t *scratch_pool);

/** Return @c TRUE iff @a uri is canonical.  Use @a pool for temporary
 * allocations.
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_uri_is_canonical(const char *uri,
                     apr_pool_t *pool);

/** Return the longest common dirent shared by two canonicalized dirents,
 * @a dirent1 and @a dirent2.  If there's no common ancestor, return the
 * empty path.
 *
 * @since New in 1.6.
 */
char *
svn_dirent_get_longest_ancestor(const char *dirent1,
                                const char *dirent2,
                                apr_pool_t *pool);

/** Return the longest common path shared by two relative paths,
 * @a relpath1 and @a relpath2.  If there's no common ancestor, return the
 * empty path.
 *
 * @since New in 1.7.
 */
char *
svn_relpath_get_longest_ancestor(const char *relpath1,
                                 const char *relpath2,
                                 apr_pool_t *pool);

/** Return the longest common path shared by two canonicalized uris,
 * @a uri1 and @a uri2.  If there's no common ancestor, return the
 * empty path.
 *
 * @a uri1 and @a uri2 may be URLs.  In order for two URLs to have
 * a common ancestor, they must (a) have the same protocol (since two URLs
 * with the same path but different protocols may point at completely
 * different resources), and (b) share a common ancestor in their path
 * component, i.e. 'protocol://' is not a sufficient ancestor.
 *
 * @since New in 1.7.
 */
char *
svn_uri_get_longest_ancestor(const char *uri1,
                             const char *uri2,
                             apr_pool_t *pool);

/** Convert @a relative canonicalized dirent to an absolute dirent and
 * return the results in @a *pabsolute, allocated in @a pool.
 * Raise SVN_ERR_BAD_FILENAME if the absolute dirent cannot be determined.
 *
 * @since New in 1.6.
 */
svn_error_t *
svn_dirent_get_absolute(const char **pabsolute,
                        const char *relative,
                        apr_pool_t *pool);

/** Test if @a uri2 is a child of @a uri1.
 * If not, return @c NULL.
 * If so, return a copy of the remainder uri, allocated in @a pool.
 * (The remainder is the component which, added to @a uri1, yields
 * @a uri2.  The remainder does not begin with a dir separator.)
 *
 * Both uris must be in canonical form, and must either be absolute,
 * or contain no ".." components.
 *
 * If @a uri2 is the same as @a uri1, it is not considered a child,
 * so the result is @c NULL; an empty string is never returned.
 *
 * If @a pool is @c NULL , a pointer into @a uri2 will be returned to
 *       identify the remainder uri.
 *
 * ### @todo the ".." restriction is unfortunate, and would ideally
 * be lifted by making the implementation smarter.  But this is not
 * trivial: if the uri is "../foo", how do you know whether or not
 * the current directory is named "foo" in its parent?
 *
 * @since New in 1.7.
 */
const char *
svn_uri_is_child(const char *uri1,
                 const char *uri2,
                 apr_pool_t *pool);

/**
 * This function is similar as svn_uri_is_child(), except that it supports
 * Windows dirents and UNC paths on Windows.
 *
 * ### @todo Makes no attempt to handle one absolute and one relative
 * dirent, and will simply return NULL.
 *
 * @since New in 1.6.
 */
const char *
svn_dirent_is_child(const char *dirent1,
                    const char *dirent2,
                    apr_pool_t *pool);

/**
 * This function is similar as svn_uri_is_child(), except that it supports
 * only relative paths.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_is_child(const char *relpath1,
                     const char *relpath2,
                     apr_pool_t *pool);

/** Return TRUE if @a dirent1 is an ancestor of @a dirent2 or the dirents are
 * equal and FALSE otherwise.
 *
 * @since New in 1.6.
 */
svn_boolean_t
svn_dirent_is_ancestor(const char *path1,
                       const char *path2);

/** Return TRUE if @a relpath1 is an ancestor of @a relpath2 or the relpaths
 * are equal and FALSE otherwise.
 *
 * This function supports only relative paths.
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_relpath_is_ancestor(const char *relpath1,
                        const char *relpath2);

/** Return TRUE if @a uri1 is an ancestor of @a uri2 or the uris are
 * equal and FALSE otherwise.
 *
 * This function supports URLs.
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_uri_is_ancestor(const char *uri1,
                    const char *uri2);


/** Returns the relative path part of @a dirent2 that is below @a dirent1,
 * or just "" iif @a dirent1 is equal to @a dirent2. If @a dirent2 is not
 * below @a path1, return @a dirent2 completely.
 *
 * This function assumes @a dirent1 and @a dirent2 are both absolute or
 * relative in the same way.
 *
 * @since New in 1.7.
 */
const char *
svn_dirent_skip_ancestor(const char *dirent1,
                         const char *dirent2);

/** Returns the relative path part of @a relpath2 that is below @a relpath1,
 * or just "" iif @a relpath1 is equal to @a relpath2. If @a relpath2 is not
 * below @a relpath1, return @a relpath2.
 *
 * @since New in 1.7.
 */
const char *
svn_relpath_skip_ancestor(const char *relpath1,
                          const char *relpath2);

/** Returns the relative path part of @a uri2 that is below @a uri1, or just
 * "" iif @a uri1 is equal to @a uri2. If @a uri2 is not below @a uri1,
 * return @a uri2.
 *
 * This function assumes @a uri1 and @a uri2 are both absolute or relative
 * in the same way.
 *
 * @since New in 1.7.
 */
const char *
svn_uri_skip_ancestor(const char *uri1,
                      const char *uri2);

/** Find the common prefix of the canonicalized dirents in @a targets
 * (an array of <tt>const char *</tt>'s), and remove redundant dirents if @a
 * remove_redundancies is TRUE.
 *
 *   - Set @a *pcommon to the absolute dirent of the dirent common to
 *     all of the targets.  If the targets have no common prefix (e.g.
 *     "C:/file" and "D:/file" on Windows), set @a *pcommon to the empty
 *     string.
 *
 *   - If @a pcondensed_targets is non-NULL, set @a *pcondensed_targets
 *     to an array of targets relative to @a *pcommon, and if
 *     @a remove_redundancies is TRUE, omit any dirents that are
 *     descendants of another dirent in @a targets.  If *pcommon
 *     is empty, @a *pcondensed_targets will contain absolute dirents;
 *     redundancies can still be removed.  If @a pcondensed_targets is NULL,
 *     leave it alone.
 *
 * Else if there is exactly one target, then
 *
 *   - Set @a *pcommon to that target, and
 *
 *   - If @a pcondensed_targets is non-NULL, set @a *pcondensed_targets
 *     to an array containing zero elements.  Else if
 *     @a pcondensed_targets is NULL, leave it alone.
 *
 * If there are no items in @a targets, set @a *pcommon and (if
 * applicable) @a *pcondensed_targets to @c NULL.
 *
 * Allocates @a *pcommon and @a *targets in @a result_pool. All
 * intermediate allocations will be performed in @a scratch_pool.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_dirent_condense_targets(const char **pcommon,
                            apr_array_header_t **pcondensed_targets,
                            const apr_array_header_t *targets,
                            svn_boolean_t remove_redundancies,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool);

/** Find the common prefix of the canonicalized uris in @a targets
 * (an array of <tt>const char *</tt>'s), and remove redundant uris if @a
 * remove_redundancies is TRUE.
 *
 *   - Set @a *pcommon to the common base uri of all of the targets.
 *     If the targets have no common prefix (e.g. "http://srv1/file"
 *     and "http://srv2/file"), set @a *pcommon to the empty
 *     string.
 *
 *   - If @a pcondensed_targets is non-NULL, set @a *pcondensed_targets
 *     to an array of targets relative to @a *pcommon, and if @a
 *     remove_redundancies is TRUE, omit any uris that are descendants of
 *     another uri in @a targets.  If *pcommon is empty, @a
 *     *pcondensed_targets will contain absolute dirents; redundancies
 *     can still be removed.  If @a pcondensed_targets is NULL, leave it
 *     alone.
 *
 * Else if there is exactly one target, then
 *
 *   - Set @a *pcommon to that target, and
 *
 *   - If @a pcondensed_targets is non-NULL, set @a *pcondensed_targets
 *     to an array containing zero elements.  Else if
 *     @a pcondensed_targets is NULL, leave it alone.
 *
 * If there are no items in @a targets, set @a *pcommon and (if
 * applicable) @a *pcondensed_targets to @c NULL.
 *
 * Allocates @a *pcommon and @a *targets in @a result_pool. Temporary
 * allocations will be performed in @a scratch_pool.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_uri_condense_targets(const char **pcommon,
                         apr_array_header_t **pcondensed_targets,
                         const apr_array_header_t *targets,
                         svn_boolean_t remove_redundancies,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);

/* Check that when @a path is joined to @a base_path, the resulting path
 * is still under BASE_PATH in the local filesystem. If not, return @c FALSE.
 * If @c TRUE is returned, @a *full_path will be set to the absolute path
 * of @a path, allocated in @a pool.
 *
 * Note: Use of this function is strongly encouraged. Do not roll your own.
 * (http://cve.mitre.org/cgi-bin/cvename.cgi?name=2007-3846)
 *
 * @since New in 1.7.
 */
svn_boolean_t
svn_dirent_is_under_root(char **full_path,
                         const char *base_path,
                         const char *path,
                         apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_DIRENT_URI_H */
