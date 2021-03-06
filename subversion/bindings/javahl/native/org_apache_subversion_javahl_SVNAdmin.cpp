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
 * @file org_apache_subversion_javahl_SVNAdmin.cpp
 * @brief Implementation of the native methods in the Java class SVNAdmin
 */

#include "../include/org_apache_subversion_javahl_SVNAdmin.h"
#include "JNIUtil.h"
#include "JNIStackElement.h"
#include "JNIStringHolder.h"
#include "JNIByteArray.h"
#include "SVNAdmin.h"
#include "Revision.h"
#include "InputStream.h"
#include "OutputStream.h"
#include "MessageReceiver.h"
#include "svn_props.h"
#include "svn_private_config.h"

JNIEXPORT jlong JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_ctNative
(JNIEnv *env, jobject jthis)
{
  JNIEntry(SVNAdmin, ctNative);
  SVNAdmin *obj = new SVNAdmin;
  return obj->getCppAddr();
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_dispose
(JNIEnv *env, jobject jthis)
{
  JNIEntry(SVNAdmin, dispose);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }
  cl->dispose(jthis);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_finalize
(JNIEnv *env, jobject jthis)
{
  JNIEntry(SVNAdmin, finalize);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl != NULL)
    cl->finalize();
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_create
(JNIEnv *env, jobject jthis, jstring jpath, jboolean jdisableFsyncCommit,
 jboolean jkeepLog, jstring jconfigpath, jstring jfstype)
{
  JNIEntry(SVNAdmin, create);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder configpath(jconfigpath);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder fstype(jfstype);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->create(path, jdisableFsyncCommit? true : false, jkeepLog? true : false,
             configpath, fstype);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_deltify
(JNIEnv *env, jobject jthis, jstring jpath, jobject jrevisionStart,
 jobject jrevisionStop)
{
  JNIEntry(SVNAdmin, deltify);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionStart(jrevisionStart);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionEnd(jrevisionStop);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->deltify(path, revisionStart, revisionEnd);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_dump
(JNIEnv *env, jobject jthis, jstring jpath, jobject jdataout,
 jobject jmessageout, jobject jrevisionStart, jobject jrevisionEnd,
 jboolean jincremental, jboolean juseDeltas)
{
  JNIEntry(SVNAdmin, dump);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  OutputStream dataOut(jdataout);
  if (JNIUtil::isExceptionThrown())
    return;

  OutputStream messageOut(jmessageout);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionStart(jrevisionStart);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionEnd(jrevisionEnd);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->dump(path, dataOut, messageOut, revisionStart, revisionEnd,
           jincremental ? true : false, juseDeltas ? true : false);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_hotcopy
(JNIEnv *env, jobject jthis, jstring jpath, jstring jtargetPath,
 jboolean jcleanLogs)
{
  JNIEntry(SVNAdmin, hotcopy);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder targetPath(jtargetPath);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->hotcopy(path, targetPath, jcleanLogs ? true : false);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_listDBLogs
(JNIEnv *env, jobject jthis, jstring jpath, jobject jreceiver)
{
  JNIEntry(SVNAdmin, listDBLogs);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  MessageReceiver mr(jreceiver);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->listDBLogs(path, mr);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_listUnusedDBLogs
(JNIEnv *env, jobject jthis, jstring jpath, jobject jreceiver)
{
  JNIEntry(SVNAdmin, listUnusedDBLogs);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  MessageReceiver mr(jreceiver);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->listUnusedDBLogs(path, mr);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_load
(JNIEnv *env, jobject jthis, jstring jpath, jobject jinputData,
 jobject joutputMsg, jboolean jignoreUUID, jboolean jforceUUID,
 jboolean jusePreCommitHook, jboolean jusePostCommitHook, jstring jrelativePath)
{
  JNIEntry(SVNAdmin, load);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  InputStream inputData(jinputData);
  if (JNIUtil::isExceptionThrown())
    return;


  OutputStream outputMsg(joutputMsg);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder relativePath(jrelativePath);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->load(path, inputData, outputMsg, jignoreUUID ? true : false,
           jforceUUID ? true : false, jusePreCommitHook ? true : false,
           jusePostCommitHook ? true : false, relativePath);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_lstxns
(JNIEnv *env, jobject jthis, jstring jpath, jobject jmessageReceiver)
{
  JNIEntry(SVNAdmin, lstxns);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  MessageReceiver mr(jmessageReceiver);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->lstxns(path, mr);
}

JNIEXPORT jlong JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_recover
(JNIEnv *env, jobject jthis, jstring jpath)
{
  JNIEntry(SVNAdmin, recover);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return -1;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return -1;

  return cl->recover(path);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_rmtxns
(JNIEnv *env, jobject jthis, jstring jpath, jobjectArray jtransactions)
{
  JNIEntry(SVNAdmin, rmtxns);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  StringArray transactions(jtransactions);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->rmtxns(path, transactions);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_setRevProp
(JNIEnv *env, jobject jthis, jstring jpath, jobject jrevision,
 jstring jpropName, jstring jpropValue, jboolean jusePreRevPropChangeHook,
 jboolean jusePostRevPropChangeHook)
{
  JNIEntry(SVNAdmin, setRevProp);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revision(jrevision);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder propName(jpropName);
  if (JNIUtil::isExceptionThrown())
    return;

  JNIStringHolder propValue(jpropValue);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->setRevProp(path, revision, propName, propValue,
                 jusePreRevPropChangeHook ? true : false,
                 jusePostRevPropChangeHook ? true : false);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_verify
(JNIEnv *env, jobject jthis, jstring jpath, jobject jmessageout,
 jobject jrevisionStart, jobject jrevisionEnd)
{
  JNIEntry(SVNAdmin, verify);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  OutputStream messageOut(jmessageout);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionStart(jrevisionStart);
  if (JNIUtil::isExceptionThrown())
    return;

  Revision revisionEnd(jrevisionEnd);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->verify(path, messageOut, revisionStart, revisionEnd);
}

JNIEXPORT jobject JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_lslocks
(JNIEnv *env, jobject jthis, jstring jpath)
{
  JNIEntry(SVNAdmin, lslocks);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return NULL;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return NULL;

  return cl->lslocks(path);
}

JNIEXPORT void JNICALL
Java_org_apache_subversion_javahl_SVNAdmin_rmlocks
(JNIEnv *env, jobject jthis, jstring jpath, jobjectArray jlocks)
{
  JNIEntry(SVNAdmin, rmlocks);
  SVNAdmin *cl = SVNAdmin::getCppObject(jthis);
  if (cl == NULL)
    {
      JNIUtil::throwError(_("bad C++ this"));
      return;
    }

  JNIStringHolder path(jpath);
  if (JNIUtil::isExceptionThrown())
    return;

  StringArray locks(jlocks);
  if (JNIUtil::isExceptionThrown())
    return;

  cl->rmlocks(path, locks);
}
