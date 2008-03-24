/*
 * checksum.c:   checksum routines
 *
 * ====================================================================
 * Copyright (c) 2008 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */


#include "svn_checksum.h"
#include "svn_md5.h"
#include "svn_sha1.h"



/* A useful macro:  returns the greater of its arguments. */
#define MAX(x,y) ((x)>(y)?(x):(y))

svn_checksum_t *
svn_checksum_create(svn_checksum_kind_t kind,
                    apr_pool_t *pool)
{
  svn_checksum_t *checksum = apr_palloc(pool, sizeof(*checksum));

  switch (kind)
    {
      case svn_checksum_md5:
        checksum->digest = apr_palloc(pool, APR_MD5_DIGESTSIZE);
        break;

      case svn_checksum_sha1:
        checksum->digest = apr_palloc(pool, APR_SHA1_DIGESTSIZE);
        break;

      default:
        return NULL;
    }

  checksum->kind = kind;
  checksum->pool = pool;

  return checksum;
}

svn_boolean_t
svn_checksum_match(svn_checksum_t *d1,
                   svn_checksum_t *d2)
{
  if (d1->kind != d2->kind)
    return FALSE;

  switch (d1->kind)
    {
      case svn_checksum_md5:
        return svn_md5_digests_match(d1->digest, d2->digest);
      case svn_checksum_sha1:
        return svn_sha1_digests_match(d1->digest, d2->digest);
      default:
        /* We really shouldn't get here, but if we do... */
        return FALSE;
    }
}

const char *
svn_checksum_to_cstring_display(svn_checksum_t *checksum,
                                apr_pool_t *pool)
{
  switch (checksum->kind)
    {
      case svn_checksum_md5:
        return svn_md5_digest_to_cstring_display(checksum->digest, pool);
      case svn_checksum_sha1:
        return svn_sha1_digest_to_cstring_display(checksum->digest, pool);
      default:
        /* We really shouldn't get here, but if we do... */
        return NULL;
    }
}

const char *
svn_checksum_to_cstring(svn_checksum_t *checksum,
                        apr_pool_t *pool)
{
  switch (checksum->kind)
    {
      case svn_checksum_md5:
        return svn_md5_digest_to_cstring(checksum->digest, pool);
      case svn_checksum_sha1:
        return svn_sha1_digest_to_cstring(checksum->digest, pool);
      default:
        /* We really shouldn't get here, but if we do... */
        return NULL;
    }
}

svn_error_t *
svn_checksum_copy(svn_checksum_t *dest,
                  svn_checksum_t *src)
{
  dest->kind = src->kind;
  memcpy(dest->digest, src->digest, MAX(APR_MD5_DIGESTSIZE,
                                        APR_SHA1_DIGESTSIZE));

  return SVN_NO_ERROR;
}
