/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "executor.h"
#include "executorimpl.h"
#include "planner.h"
#include "tdatablock.h"
#include "tref.h"
#include "tudf.h"
#include "vnode.h"

static TdThreadOnce initPoolOnce = PTHREAD_ONCE_INIT;
int32_t             exchangeObjRefPool = -1;

static void initRefPool() { exchangeObjRefPool = taosOpenRef(1024, doDestroyExchangeOperatorInfo); }
static void cleanupRefPool() {
  int32_t ref = atomic_val_compare_exchange_32(&exchangeObjRefPool, exchangeObjRefPool, 0);
  taosCloseRef(ref);
}

static int32_t doSetSMABlock(SOperatorInfo* pOperator, void* input, size_t numOfBlocks, int32_t type, char* id) {
  ASSERT(pOperator != NULL);
  if (pOperator->operatorType != QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
    if (pOperator->numOfDownstream == 0) {
      qError("failed to find stream scan operator to set the input data block, %s" PRIx64, id);
      return TSDB_CODE_QRY_APP_ERROR;
    }

    if (pOperator->numOfDownstream > 1) {  // not handle this in join query
      qError("join not supported for stream block scan, %s" PRIx64, id);
      return TSDB_CODE_QRY_APP_ERROR;
    }
    pOperator->status = OP_NOT_OPENED;
    return doSetSMABlock(pOperator->pDownstream[0], input, numOfBlocks, type, id);
  } else {
    pOperator->status = OP_NOT_OPENED;

    SStreamScanInfo* pInfo = pOperator->info;

    if (type == STREAM_INPUT__MERGED_SUBMIT) {
      for (int32_t i = 0; i < numOfBlocks; i++) {
        SSubmitReq* pReq = *(void**)POINTER_SHIFT(input, i * sizeof(void*));
        taosArrayPush(pInfo->pBlockLists, &pReq);
      }
      pInfo->blockType = STREAM_INPUT__DATA_SUBMIT;
    } else if (type == STREAM_INPUT__DATA_SUBMIT) {
      taosArrayPush(pInfo->pBlockLists, &input);
      pInfo->blockType = STREAM_INPUT__DATA_SUBMIT;
    } else if (type == STREAM_INPUT__DATA_BLOCK) {
      for (int32_t i = 0; i < numOfBlocks; ++i) {
        SSDataBlock* pDataBlock = &((SSDataBlock*)input)[i];
        taosArrayPush(pInfo->pBlockLists, &pDataBlock);
      }
      pInfo->blockType = STREAM_INPUT__DATA_BLOCK;
    }

    return TSDB_CODE_SUCCESS;
  }
}

static int32_t doSetStreamOpOpen(SOperatorInfo* pOperator, char* id) {
  {
    ASSERT(pOperator != NULL);
    if (pOperator->operatorType != QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
      if (pOperator->numOfDownstream == 0) {
        qError("failed to find stream scan operator to set the input data block, %s" PRIx64, id);
        return TSDB_CODE_QRY_APP_ERROR;
      }

      if (pOperator->numOfDownstream > 1) {  // not handle this in join query
        qError("join not supported for stream block scan, %s" PRIx64, id);
        return TSDB_CODE_QRY_APP_ERROR;
      }
      pOperator->status = OP_NOT_OPENED;
      return doSetStreamOpOpen(pOperator->pDownstream[0], id);
    }
  }
  return 0;
}

static int32_t doSetStreamBlock(SOperatorInfo* pOperator, void* input, size_t numOfBlocks, int32_t type, char* id) {
  ASSERT(pOperator != NULL);
  if (pOperator->operatorType != QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
    if (pOperator->numOfDownstream == 0) {
      qError("failed to find stream scan operator to set the input data block, %s" PRIx64, id);
      return TSDB_CODE_QRY_APP_ERROR;
    }

    if (pOperator->numOfDownstream > 1) {  // not handle this in join query
      qError("join not supported for stream block scan, %s" PRIx64, id);
      return TSDB_CODE_QRY_APP_ERROR;
    }
    pOperator->status = OP_NOT_OPENED;
    return doSetStreamBlock(pOperator->pDownstream[0], input, numOfBlocks, type, id);
  } else {
    pOperator->status = OP_NOT_OPENED;

    SStreamScanInfo* pInfo = pOperator->info;

    ASSERT(pInfo->validBlockIndex == 0);
    ASSERT(taosArrayGetSize(pInfo->pBlockLists) == 0);

    if (type == STREAM_INPUT__MERGED_SUBMIT) {
      // ASSERT(numOfBlocks > 1);
      for (int32_t i = 0; i < numOfBlocks; i++) {
        SSubmitReq* pReq = *(void**)POINTER_SHIFT(input, i * sizeof(void*));
        taosArrayPush(pInfo->pBlockLists, &pReq);
      }
      pInfo->blockType = STREAM_INPUT__DATA_SUBMIT;
    } else if (type == STREAM_INPUT__DATA_SUBMIT) {
      ASSERT(numOfBlocks == 1);
      taosArrayPush(pInfo->pBlockLists, &input);
      pInfo->blockType = STREAM_INPUT__DATA_SUBMIT;
    } else if (type == STREAM_INPUT__DATA_BLOCK) {
      for (int32_t i = 0; i < numOfBlocks; ++i) {
        SSDataBlock* pDataBlock = &((SSDataBlock*)input)[i];
        taosArrayPush(pInfo->pBlockLists, &pDataBlock);
      }
      pInfo->blockType = STREAM_INPUT__DATA_BLOCK;
    } else {
      ASSERT(0);
    }

    return TSDB_CODE_SUCCESS;
  }
}

int32_t qSetStreamOpOpen(qTaskInfo_t tinfo) {
  if (tinfo == NULL) {
    return TSDB_CODE_QRY_APP_ERROR;
  }

  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  int32_t code = doSetStreamOpOpen(pTaskInfo->pRoot, GET_TASKID(pTaskInfo));
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed to set the stream block data", GET_TASKID(pTaskInfo));
  } else {
    qDebug("%s set the stream block successfully", GET_TASKID(pTaskInfo));
  }

  return code;
}

int32_t qSetMultiStreamInput(qTaskInfo_t tinfo, const void* pBlocks, size_t numOfBlocks, int32_t type) {
  if (tinfo == NULL) {
    return TSDB_CODE_QRY_APP_ERROR;
  }

  if (pBlocks == NULL || numOfBlocks == 0) {
    return TSDB_CODE_SUCCESS;
  }

  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  int32_t code = doSetStreamBlock(pTaskInfo->pRoot, (void*)pBlocks, numOfBlocks, type, GET_TASKID(pTaskInfo));
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed to set the stream block data", GET_TASKID(pTaskInfo));
  } else {
    qDebug("%s set the stream block successfully", GET_TASKID(pTaskInfo));
  }

  return code;
}

int32_t qSetSMAInput(qTaskInfo_t tinfo, const void* pBlocks, size_t numOfBlocks, int32_t type) {
  if (tinfo == NULL) {
    return TSDB_CODE_QRY_APP_ERROR;
  }

  if (pBlocks == NULL || numOfBlocks == 0) {
    return TSDB_CODE_SUCCESS;
  }

  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  int32_t code = doSetSMABlock(pTaskInfo->pRoot, (void*)pBlocks, numOfBlocks, type, GET_TASKID(pTaskInfo));
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed to set the sma block data", GET_TASKID(pTaskInfo));
  } else {
    qDebug("%s set the sma block successfully", GET_TASKID(pTaskInfo));
  }

  return code;
}

qTaskInfo_t qCreateQueueExecTaskInfo(void* msg, SReadHandle* readers, int32_t* numOfCols, SSchemaWrapper** pSchema) {
  if (msg == NULL) {
    // create raw scan

    SExecTaskInfo* pTaskInfo = taosMemoryCalloc(1, sizeof(SExecTaskInfo));
    if (NULL == pTaskInfo) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      return NULL;
    }
    setTaskStatus(pTaskInfo, TASK_NOT_COMPLETED);

    pTaskInfo->cost.created = taosGetTimestampMs();
    pTaskInfo->execModel = OPTR_EXEC_MODEL_QUEUE;
    pTaskInfo->pRoot = createRawScanOperatorInfo(readers, pTaskInfo);
    if (NULL == pTaskInfo->pRoot) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      taosMemoryFree(pTaskInfo);
      return NULL;
    }
    return pTaskInfo;
  }

  struct SSubplan* pPlan = NULL;
  int32_t          code = qStringToSubplan(msg, &pPlan);
  if (code != TSDB_CODE_SUCCESS) {
    terrno = code;
    return NULL;
  }

  qTaskInfo_t pTaskInfo = NULL;
  code = qCreateExecTask(readers, 0, 0, pPlan, &pTaskInfo, NULL, NULL, OPTR_EXEC_MODEL_QUEUE);
  if (code != TSDB_CODE_SUCCESS) {
    nodesDestroyNode((SNode*)pPlan);
    qDestroyTask(pTaskInfo);
    terrno = code;
    return NULL;
  }

  // extract the number of output columns
  SDataBlockDescNode* pDescNode = pPlan->pNode->pOutputDataBlockDesc;
  *numOfCols = 0;

  SNode* pNode;
  FOREACH(pNode, pDescNode->pSlots) {
    SSlotDescNode* pSlotDesc = (SSlotDescNode*)pNode;
    if (pSlotDesc->output) {
      ++(*numOfCols);
    }
  }

  *pSchema = tCloneSSchemaWrapper(((SExecTaskInfo*)pTaskInfo)->schemaInfo.qsw);
  return pTaskInfo;
}

qTaskInfo_t qCreateStreamExecTaskInfo(void* msg, SReadHandle* readers) {
  if (msg == NULL) {
    return NULL;
  }

  /*qDebugL("stream task string %s", (const char*)msg);*/

  struct SSubplan* pPlan = NULL;
  int32_t          code = qStringToSubplan(msg, &pPlan);
  if (code != TSDB_CODE_SUCCESS) {
    terrno = code;
    return NULL;
  }

  qTaskInfo_t pTaskInfo = NULL;
  code = qCreateExecTask(readers, 0, 0, pPlan, &pTaskInfo, NULL, NULL, OPTR_EXEC_MODEL_STREAM);
  if (code != TSDB_CODE_SUCCESS) {
    nodesDestroyNode((SNode*)pPlan);
    qDestroyTask(pTaskInfo);
    terrno = code;
    return NULL;
  }

  return pTaskInfo;
}

static SArray* filterUnqualifiedTables(const SStreamScanInfo* pScanInfo, const SArray* tableIdList, const char* idstr) {
  SArray* qa = taosArrayInit(4, sizeof(tb_uid_t));

  // let's discard the tables those are not created according to the queried super table.
  SMetaReader mr = {0};
  metaReaderInit(&mr, pScanInfo->readHandle.meta, 0);
  for (int32_t i = 0; i < taosArrayGetSize(tableIdList); ++i) {
    uint64_t* id = (uint64_t*)taosArrayGet(tableIdList, i);

    int32_t code = metaGetTableEntryByUid(&mr, *id);
    if (code != TSDB_CODE_SUCCESS) {
      qError("failed to get table meta, uid:%" PRIu64 " code:%s, %s", *id, tstrerror(terrno), idstr);
      continue;
    }

    tDecoderClear(&mr.coder);

    // TODO handle ntb case
    if (mr.me.type != TSDB_CHILD_TABLE || mr.me.ctbEntry.suid != pScanInfo->tableUid) {
      continue;
    }

    if (pScanInfo->pTagCond != NULL) {
      bool          qualified = false;
      STableKeyInfo info = {.groupId = 0, .uid = mr.me.uid};
      code = isQualifiedTable(&info, pScanInfo->pTagCond, pScanInfo->readHandle.meta, &qualified);
      if (code != TSDB_CODE_SUCCESS) {
        qError("failed to filter new table, uid:0x%" PRIx64 ", %s", info.uid, idstr);
        continue;
      }

      if (!qualified) {
        continue;
      }
    }

    // handle multiple partition
    taosArrayPush(qa, id);
  }

  metaReaderClear(&mr);
  return qa;
}

int32_t qUpdateQualifiedTableId(qTaskInfo_t tinfo, const SArray* tableIdList, bool isAdd) {
  SExecTaskInfo*  pTaskInfo = (SExecTaskInfo*)tinfo;

  if (isAdd) {
    qDebug("add %d tables id into query list, %s", (int32_t)taosArrayGetSize(tableIdList), pTaskInfo->id.str);
  }

  // traverse to the stream scanner node to add this table id
  SOperatorInfo* pInfo = pTaskInfo->pRoot;
  while (pInfo->operatorType != QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
    pInfo = pInfo->pDownstream[0];
  }

  int32_t          code = 0;
  SStreamScanInfo* pScanInfo = pInfo->info;
  if (isAdd) {  // add new table id
    SArray* qa = filterUnqualifiedTables(pScanInfo, tableIdList, GET_TASKID(pTaskInfo));
    int32_t numOfQualifiedTables = taosArrayGetSize(qa);

    qDebug(" %d qualified child tables added into stream scanner", numOfQualifiedTables);

    code = tqReaderAddTbUidList(pScanInfo->tqReader, qa);
    if (code != TSDB_CODE_SUCCESS) {
      taosArrayDestroy(qa);
      return code;
    }

    // todo refactor STableList
    bool   assignUid = false;
    size_t bufLen = (pScanInfo->pGroupTags != NULL) ? getTableTagsBufLen(pScanInfo->pGroupTags) : 0;
    char*  keyBuf = NULL;
    if (bufLen > 0) {
      assignUid = groupbyTbname(pScanInfo->pGroupTags);
      keyBuf = taosMemoryMalloc(bufLen);
      if (keyBuf == NULL) {
        taosArrayDestroy(qa);
        return TSDB_CODE_OUT_OF_MEMORY;
      }
    }

    STableListInfo* pTableListInfo = pTaskInfo->pTableInfoList;

    for (int32_t i = 0; i < numOfQualifiedTables; ++i) {
      uint64_t*     uid = taosArrayGet(qa, i);
      STableKeyInfo keyInfo = {.uid = *uid, .groupId = 0};

      if (bufLen > 0) {
        if (assignUid) {
          keyInfo.groupId = keyInfo.uid;
        } else {
          code = getGroupIdFromTagsVal(pScanInfo->readHandle.meta, keyInfo.uid, pScanInfo->pGroupTags, keyBuf,
                                       &keyInfo.groupId);
          if (code != TSDB_CODE_SUCCESS) {
            taosMemoryFree(keyBuf);
            taosArrayDestroy(qa);
            return code;
          }
        }
      }

#if 0
      bool exists = false;
      for (int32_t k = 0; k < taosArrayGetSize(pListInfo->pTableList); ++k) {
        STableKeyInfo* pKeyInfo = taosArrayGet(pListInfo->pTableList, k);
        if (pKeyInfo->uid == keyInfo.uid) {
          qWarn("ignore duplicated query table uid:%" PRIu64 " added, %s", pKeyInfo->uid, pTaskInfo->id.str);
          exists = true;
        }
      }

      if (!exists) {
#endif

      tableListAddTableInfo(pTableListInfo, keyInfo.uid, keyInfo.groupId);
    }

    if (keyBuf != NULL) {
      taosMemoryFree(keyBuf);
    }

    taosArrayDestroy(qa);
  } else {  // remove the table id in current list
    qDebug(" %d remove child tables from the stream scanner", (int32_t)taosArrayGetSize(tableIdList));
    code = tqReaderRemoveTbUidList(pScanInfo->tqReader, tableIdList);
  }

  return code;
}

int32_t qGetQueryTableSchemaVersion(qTaskInfo_t tinfo, char* dbName, char* tableName, int32_t* sversion,
                                    int32_t* tversion) {
  ASSERT(tinfo != NULL && dbName != NULL && tableName != NULL);
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  if (pTaskInfo->schemaInfo.sw == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  *sversion = pTaskInfo->schemaInfo.sw->version;
  *tversion = pTaskInfo->schemaInfo.tversion;
  if (pTaskInfo->schemaInfo.dbname) {
    strcpy(dbName, pTaskInfo->schemaInfo.dbname);
  } else {
    dbName[0] = 0;
  }
  if (pTaskInfo->schemaInfo.tablename) {
    strcpy(tableName, pTaskInfo->schemaInfo.tablename);
  } else {
    tableName[0] = 0;
  }

  return 0;
}

int32_t qCreateExecTask(SReadHandle* readHandle, int32_t vgId, uint64_t taskId, SSubplan* pSubplan,
                        qTaskInfo_t* pTaskInfo, DataSinkHandle* handle, char* sql, EOPTR_EXEC_MODEL model) {
  assert(pSubplan != NULL);
  SExecTaskInfo** pTask = (SExecTaskInfo**)pTaskInfo;

  taosThreadOnce(&initPoolOnce, initRefPool);
  atexit(cleanupRefPool);

  qDebug("start to create subplan task, TID:0x%" PRIx64 " QID:0x%" PRIx64, taskId, pSubplan->id.queryId);

  int32_t code = createExecTaskInfoImpl(pSubplan, pTask, readHandle, taskId, sql, model);
  if (code != TSDB_CODE_SUCCESS) {
    qError("failed to createExecTaskInfoImpl, code: %s", tstrerror(code));
    goto _error;
  }

  SDataSinkMgtCfg cfg = {.maxDataBlockNum = 500, .maxDataBlockNumPerQuery = 50};
  code = dsDataSinkMgtInit(&cfg);
  if (code != TSDB_CODE_SUCCESS) {
    qError("failed to dsDataSinkMgtInit, code:%s, %s", tstrerror(code), (*pTask)->id.str);
    goto _error;
  }

  if (handle) {
    void* pSinkParam = NULL;
    code = createDataSinkParam(pSubplan->pDataSink, &pSinkParam, pTaskInfo, readHandle);
    if (code != TSDB_CODE_SUCCESS) {
      qError("failed to createDataSinkParam, vgId:%d, code:%s, %s", vgId, tstrerror(code), (*pTask)->id.str);
      goto _error;
    }

    code = dsCreateDataSinker(pSubplan->pDataSink, handle, pSinkParam, (*pTask)->id.str);
    if (code != TSDB_CODE_SUCCESS) {
      taosMemoryFreeClear(pSinkParam);
    }
  }

  qDebug("subplan task create completed, TID:0x%" PRIx64 " QID:0x%" PRIx64, taskId, pSubplan->id.queryId);

  _error:
  // if failed to add ref for all tables in this query, abort current query
  return code;
}

#ifdef TEST_IMPL
// wait moment
int waitMoment(SQInfo* pQInfo) {
  if (pQInfo->sql) {
    int   ms = 0;
    char* pcnt = strstr(pQInfo->sql, " count(*)");
    if (pcnt) return 0;

    char* pos = strstr(pQInfo->sql, " t_");
    if (pos) {
      pos += 3;
      ms = atoi(pos);
      while (*pos >= '0' && *pos <= '9') {
        pos++;
      }
      char unit_char = *pos;
      if (unit_char == 'h') {
        ms *= 3600 * 1000;
      } else if (unit_char == 'm') {
        ms *= 60 * 1000;
      } else if (unit_char == 's') {
        ms *= 1000;
      }
    }
    if (ms == 0) return 0;
    printf("test wait sleep %dms. sql=%s ...\n", ms, pQInfo->sql);

    if (ms < 1000) {
      taosMsleep(ms);
    } else {
      int used_ms = 0;
      while (used_ms < ms) {
        taosMsleep(1000);
        used_ms += 1000;
        if (isTaskKilled(pQInfo)) {
          printf("test check query is canceled, sleep break.%s\n", pQInfo->sql);
          break;
        }
      }
    }
  }
  return 1;
}
#endif

static void freeBlock(void* param) {
  SSDataBlock* pBlock = *(SSDataBlock**)param;
  blockDataDestroy(pBlock);
}

int32_t qExecTaskOpt(qTaskInfo_t tinfo, SArray* pResList, uint64_t* useconds, bool* hasMore, SLocalFetch* pLocal) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  int64_t        threadId = taosGetSelfPthreadId();

  if (pLocal) {
    memcpy(&pTaskInfo->localFetch, pLocal, sizeof(*pLocal));
  }

  taosArrayClearEx(pResList, freeBlock);

  int64_t curOwner = 0;
  if ((curOwner = atomic_val_compare_exchange_64(&pTaskInfo->owner, 0, threadId)) != 0) {
    qError("%s-%p execTask is now executed by thread:%p", GET_TASKID(pTaskInfo), pTaskInfo, (void*)curOwner);
    pTaskInfo->code = TSDB_CODE_QRY_IN_EXEC;
    return pTaskInfo->code;
  }

  if (pTaskInfo->cost.start == 0) {
    pTaskInfo->cost.start = taosGetTimestampMs();
  }

  if (isTaskKilled(pTaskInfo)) {
    atomic_store_64(&pTaskInfo->owner, 0);
    qDebug("%s already killed, abort", GET_TASKID(pTaskInfo));
    return TSDB_CODE_SUCCESS;
  }

  // error occurs, record the error code and return to client
  int32_t ret = setjmp(pTaskInfo->env);
  if (ret != TSDB_CODE_SUCCESS) {
    pTaskInfo->code = ret;
    cleanUpUdfs();

    qDebug("%s task abort due to error/cancel occurs, code:%s", GET_TASKID(pTaskInfo), tstrerror(pTaskInfo->code));
    atomic_store_64(&pTaskInfo->owner, 0);

    return pTaskInfo->code;
  }

  qDebug("%s execTask is launched", GET_TASKID(pTaskInfo));

  int32_t      current = 0;
  SSDataBlock* pRes = NULL;

  int64_t st = taosGetTimestampUs();

  while ((pRes = pTaskInfo->pRoot->fpSet.getNextFn(pTaskInfo->pRoot)) != NULL) {
    SSDataBlock* p = createOneDataBlock(pRes, true);
    current += p->info.rows;
    ASSERT(p->info.rows > 0);
    taosArrayPush(pResList, &p);

    if (current >= 4096) {
      break;
    }
  }

  *hasMore = (pRes != NULL);
  uint64_t el = (taosGetTimestampUs() - st);

  pTaskInfo->cost.elapsedTime += el;
  if (NULL == pRes) {
    *useconds = pTaskInfo->cost.elapsedTime;
  }

  cleanUpUdfs();

  uint64_t total = pTaskInfo->pRoot->resultInfo.totalRows;
  qDebug("%s task suspended, %d rows in %d blocks returned, total:%" PRId64 " rows, in sinkNode:%d, elapsed:%.2f ms",
         GET_TASKID(pTaskInfo), current, (int32_t)taosArrayGetSize(pResList), total, 0, el / 1000.0);

  atomic_store_64(&pTaskInfo->owner, 0);
  return pTaskInfo->code;
}

int32_t qExecTask(qTaskInfo_t tinfo, SSDataBlock** pRes, uint64_t* useconds) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  int64_t        threadId = taosGetSelfPthreadId();

  *pRes = NULL;
  int64_t curOwner = 0;
  if ((curOwner = atomic_val_compare_exchange_64(&pTaskInfo->owner, 0, threadId)) != 0) {
    qError("%s-%p execTask is now executed by thread:%p", GET_TASKID(pTaskInfo), pTaskInfo, (void*)curOwner);
    pTaskInfo->code = TSDB_CODE_QRY_IN_EXEC;
    return pTaskInfo->code;
  }

  if (pTaskInfo->cost.start == 0) {
    pTaskInfo->cost.start = taosGetTimestampMs();
  }

  if (isTaskKilled(pTaskInfo)) {
    atomic_store_64(&pTaskInfo->owner, 0);
    qDebug("%s already killed, abort", GET_TASKID(pTaskInfo));
    return TSDB_CODE_SUCCESS;
  }

  // error occurs, record the error code and return to client
  int32_t ret = setjmp(pTaskInfo->env);
  if (ret != TSDB_CODE_SUCCESS) {
    pTaskInfo->code = ret;
    cleanUpUdfs();
    qDebug("%s task abort due to error/cancel occurs, code:%s", GET_TASKID(pTaskInfo), tstrerror(pTaskInfo->code));
    atomic_store_64(&pTaskInfo->owner, 0);
    return pTaskInfo->code;
  }

  qDebug("%s execTask is launched", GET_TASKID(pTaskInfo));

  int64_t st = taosGetTimestampUs();

  *pRes = pTaskInfo->pRoot->fpSet.getNextFn(pTaskInfo->pRoot);
  uint64_t el = (taosGetTimestampUs() - st);

  pTaskInfo->cost.elapsedTime += el;
  if (NULL == *pRes) {
    *useconds = pTaskInfo->cost.elapsedTime;
  }

  cleanUpUdfs();

  int32_t  current = (*pRes != NULL) ? (*pRes)->info.rows : 0;
  uint64_t total = pTaskInfo->pRoot->resultInfo.totalRows;

  qDebug("%s task suspended, %d rows returned, total:%" PRId64 " rows, in sinkNode:%d, elapsed:%.2f ms",
         GET_TASKID(pTaskInfo), current, total, 0, el / 1000.0);

  atomic_store_64(&pTaskInfo->owner, 0);
  return pTaskInfo->code;
}

int32_t qAsyncKillTask(qTaskInfo_t qinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)qinfo;

  if (pTaskInfo == NULL) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  qDebug("%s execTask async killed", GET_TASKID(pTaskInfo));
  setTaskKilled(pTaskInfo);
  return TSDB_CODE_SUCCESS;
}

void qDestroyTask(qTaskInfo_t qTaskHandle) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)qTaskHandle;
  if (pTaskInfo == NULL) {
    return;
  }

  qDebug("%s execTask completed, numOfRows:%" PRId64, GET_TASKID(pTaskInfo), pTaskInfo->pRoot->resultInfo.totalRows);

  queryCostStatis(pTaskInfo);  // print the query cost summary
  doDestroyTask(pTaskInfo);
}

int32_t qGetExplainExecInfo(qTaskInfo_t tinfo, SArray* pExecInfoList) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  return getOperatorExplainExecInfo(pTaskInfo->pRoot, pExecInfoList);
}

int32_t qSerializeTaskStatus(qTaskInfo_t tinfo, char** pOutput, int32_t* len) {
  SExecTaskInfo* pTaskInfo = (struct SExecTaskInfo*)tinfo;
  if (pTaskInfo->pRoot == NULL) {
    return TSDB_CODE_INVALID_PARA;
  }

  int32_t nOptrWithVal = 0;
  int32_t code = encodeOperator(pTaskInfo->pRoot, pOutput, len, &nOptrWithVal);
  if ((code == TSDB_CODE_SUCCESS) && (nOptrWithVal == 0)) {
    taosMemoryFreeClear(*pOutput);
    *len = 0;
  }
  return code;
}

int32_t qDeserializeTaskStatus(qTaskInfo_t tinfo, const char* pInput, int32_t len) {
  SExecTaskInfo* pTaskInfo = (struct SExecTaskInfo*)tinfo;

  if (pTaskInfo == NULL || pInput == NULL || len == 0) {
    return TSDB_CODE_INVALID_PARA;
  }

  return decodeOperator(pTaskInfo->pRoot, pInput, len);
}

int32_t qExtractStreamScanner(qTaskInfo_t tinfo, void** scanner) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;

  while (1) {
    uint16_t type = pOperator->operatorType;
    if (type == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
      *scanner = pOperator->info;
      return 0;
    } else {
      ASSERT(pOperator->numOfDownstream == 1);
      pOperator = pOperator->pDownstream[0];
    }
  }
}

#if 0
int32_t qStreamInput(qTaskInfo_t tinfo, void* pItem) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_STREAM);
  taosWriteQitem(pTaskInfo->streamInfo.inputQueue->queue, pItem);
  return 0;
}
#endif

int32_t qStreamSourceRecoverStep1(qTaskInfo_t tinfo, int64_t ver) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_STREAM);
  pTaskInfo->streamInfo.fillHistoryVer1 = ver;
  pTaskInfo->streamInfo.recoverStep = STREAM_RECOVER_STEP__PREPARE1;
  return 0;
}

int32_t qStreamSourceRecoverStep2(qTaskInfo_t tinfo, int64_t ver) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_STREAM);
  pTaskInfo->streamInfo.fillHistoryVer2 = ver;
  pTaskInfo->streamInfo.recoverStep = STREAM_RECOVER_STEP__PREPARE2;
  return 0;
}

int32_t qStreamRecoverFinish(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_STREAM);
  pTaskInfo->streamInfo.recoverStep = STREAM_RECOVER_STEP__NONE;
  return 0;
}

int32_t qStreamSetParamForRecover(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;

  while (1) {
    if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_INTERVAL ||
        pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SEMI_INTERVAL ||
        pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_INTERVAL) {
      SStreamIntervalOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      ASSERT(pInfo->twAggSup.calTriggerSaved == 0);
      ASSERT(pInfo->twAggSup.deleteMarkSaved == 0);

      qInfo("save stream param for interval: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);

      pInfo->twAggSup.calTriggerSaved = pInfo->twAggSup.calTrigger;
      pInfo->twAggSup.deleteMarkSaved = pInfo->twAggSup.deleteMark;
      pInfo->twAggSup.calTrigger = STREAM_TRIGGER_AT_ONCE;
      pInfo->twAggSup.deleteMark = INT64_MAX;

    } else if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SESSION ||
               pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SEMI_SESSION ||
               pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_SESSION) {
      SStreamSessionAggOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      ASSERT(pInfo->twAggSup.calTriggerSaved == 0);
      ASSERT(pInfo->twAggSup.deleteMarkSaved == 0);

      qInfo("save stream param for session: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);

      pInfo->twAggSup.calTriggerSaved = pInfo->twAggSup.calTrigger;
      pInfo->twAggSup.deleteMarkSaved = pInfo->twAggSup.deleteMark;
      pInfo->twAggSup.calTrigger = STREAM_TRIGGER_AT_ONCE;
      pInfo->twAggSup.deleteMark = INT64_MAX;
    } else if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_STATE) {
      SStreamStateAggOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      ASSERT(pInfo->twAggSup.calTriggerSaved == 0);
      ASSERT(pInfo->twAggSup.deleteMarkSaved == 0);

      qInfo("save stream param for state: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);

      pInfo->twAggSup.calTriggerSaved = pInfo->twAggSup.calTrigger;
      pInfo->twAggSup.deleteMarkSaved = pInfo->twAggSup.deleteMark;
      pInfo->twAggSup.calTrigger = STREAM_TRIGGER_AT_ONCE;
      pInfo->twAggSup.deleteMark = INT64_MAX;
    }

    // iterate operator tree
    if (pOperator->numOfDownstream != 1 || pOperator->pDownstream[0] == NULL) {
      if (pOperator->numOfDownstream > 1) {
        qError("unexpected stream, multiple downstream");
        ASSERT(0);
        return -1;
      }
      return 0;
    } else {
      pOperator = pOperator->pDownstream[0];
    }
  }

  return 0;
}

int32_t qStreamRestoreParam(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;

  while (1) {
    if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_INTERVAL ||
        pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SEMI_INTERVAL ||
        pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_INTERVAL) {
      SStreamIntervalOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE);
      ASSERT(pInfo->twAggSup.deleteMark == INT64_MAX);

      pInfo->twAggSup.calTrigger = pInfo->twAggSup.calTriggerSaved;
      pInfo->twAggSup.deleteMark = pInfo->twAggSup.deleteMarkSaved;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      qInfo("restore stream param for interval: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);
    } else if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SESSION ||
               pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SEMI_SESSION ||
               pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_SESSION) {
      SStreamSessionAggOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE);
      ASSERT(pInfo->twAggSup.deleteMark == INT64_MAX);

      pInfo->twAggSup.calTrigger = pInfo->twAggSup.calTriggerSaved;
      pInfo->twAggSup.deleteMark = pInfo->twAggSup.deleteMarkSaved;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      qInfo("restore stream param for session: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);
    } else if (pOperator->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_STATE) {
      SStreamStateAggOperatorInfo* pInfo = pOperator->info;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE);
      ASSERT(pInfo->twAggSup.deleteMark == INT64_MAX);

      pInfo->twAggSup.calTrigger = pInfo->twAggSup.calTriggerSaved;
      pInfo->twAggSup.deleteMark = pInfo->twAggSup.deleteMarkSaved;
      ASSERT(pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE ||
             pInfo->twAggSup.calTrigger == STREAM_TRIGGER_WINDOW_CLOSE);
      qInfo("restore stream param for state: %d,  %" PRId64, pInfo->twAggSup.calTrigger, pInfo->twAggSup.deleteMark);
    }

    // iterate operator tree
    if (pOperator->numOfDownstream != 1 || pOperator->pDownstream[0] == NULL) {
      if (pOperator->numOfDownstream > 1) {
        qError("unexpected stream, multiple downstream");
        ASSERT(0);
        return -1;
      }
      return 0;
    } else {
      pOperator = pOperator->pDownstream[0];
    }
  }
  return 0;
}

void* qExtractReaderFromStreamScanner(void* scanner) {
  SStreamScanInfo* pInfo = scanner;
  return (void*)pInfo->tqReader;
}

const SSchemaWrapper* qExtractSchemaFromTask(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  return pTaskInfo->streamInfo.schema;
}

const char* qExtractTbnameFromTask(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  return pTaskInfo->streamInfo.tbName;
}

SMqMetaRsp* qStreamExtractMetaMsg(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  return &pTaskInfo->streamInfo.metaRsp;
}

int64_t qStreamExtractPrepareUid(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  return pTaskInfo->streamInfo.prepareStatus.uid;
}

int32_t qStreamExtractOffset(qTaskInfo_t tinfo, STqOffsetVal* pOffset) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  memcpy(pOffset, &pTaskInfo->streamInfo.lastStatus, sizeof(STqOffsetVal));
  return 0;
}

int32_t initQueryTableDataCondForTmq(SQueryTableDataCond* pCond, SSnapContext* sContext, SMetaTableInfo* pMtInfo) {
  memset(pCond, 0, sizeof(SQueryTableDataCond));
  pCond->order = TSDB_ORDER_ASC;
  pCond->numOfCols = pMtInfo->schema->nCols;
  pCond->colList = taosMemoryCalloc(pCond->numOfCols, sizeof(SColumnInfo));
  if (pCond->colList == NULL) {
    terrno = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return terrno;
  }

  pCond->twindows = (STimeWindow){.skey = INT64_MIN, .ekey = INT64_MAX};
  pCond->suid = pMtInfo->suid;
  pCond->type = TIMEWINDOW_RANGE_CONTAINED;
  pCond->startVersion = -1;
  pCond->endVersion = sContext->snapVersion;

  for (int32_t i = 0; i < pCond->numOfCols; ++i) {
    pCond->colList[i].type = pMtInfo->schema->pSchema[i].type;
    pCond->colList[i].bytes = pMtInfo->schema->pSchema[i].bytes;
    pCond->colList[i].colId = pMtInfo->schema->pSchema[i].colId;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t qStreamScanMemData(qTaskInfo_t tinfo, const SSubmitReq* pReq) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  ASSERT(pTaskInfo->streamInfo.pReq == NULL);
  pTaskInfo->streamInfo.pReq = pReq;
  return 0;
}

int32_t qStreamPrepareScan(qTaskInfo_t tinfo, STqOffsetVal* pOffset, int8_t subType) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  pTaskInfo->streamInfo.prepareStatus = *pOffset;
  pTaskInfo->streamInfo.returned = 0;
  if (tOffsetEqual(pOffset, &pTaskInfo->streamInfo.lastStatus)) {
    return 0;
  }
  if (subType == TOPIC_SUB_TYPE__COLUMN) {
    uint16_t type = pOperator->operatorType;
    pOperator->status = OP_OPENED;
    // TODO add more check
    if (type != QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
      ASSERT(pOperator->numOfDownstream == 1);
      pOperator = pOperator->pDownstream[0];
    }

    SStreamScanInfo* pInfo = pOperator->info;
    if (pOffset->type == TMQ_OFFSET__LOG) {
      STableScanInfo* pTSInfo = pInfo->pTableScanOp->info;
      tsdbReaderClose(pTSInfo->dataReader);
      pTSInfo->dataReader = NULL;
#if 0
      if (tOffsetEqual(pOffset, &pTaskInfo->streamInfo.lastStatus) &&
          pInfo->tqReader->pWalReader->curVersion != pOffset->version) {
        qError("prepare scan ver %" PRId64 " actual ver %" PRId64 ", last %" PRId64, pOffset->version,
               pInfo->tqReader->pWalReader->curVersion, pTaskInfo->streamInfo.lastStatus.version);
        ASSERT(0);
      }
#endif
      if (tqSeekVer(pInfo->tqReader, pOffset->version + 1) < 0) {
        return -1;
      }
      ASSERT(pInfo->tqReader->pWalReader->curVersion == pOffset->version + 1);
    } else if (pOffset->type == TMQ_OFFSET__SNAPSHOT_DATA) {
      /*pInfo->blockType = STREAM_INPUT__TABLE_SCAN;*/
      int64_t uid = pOffset->uid;
      int64_t ts = pOffset->ts;

      if (uid == 0) {
        if (tableListGetSize(pTaskInfo->pTableInfoList) != 0) {
          STableKeyInfo* pTableInfo = tableListGetInfo(pTaskInfo->pTableInfoList, 0);
          uid = pTableInfo->uid;
          ts = INT64_MIN;
        } else {
          return -1;
        }
      }

      /*if (pTaskInfo->streamInfo.lastStatus.type != TMQ_OFFSET__SNAPSHOT_DATA ||*/
      /*pTaskInfo->streamInfo.lastStatus.uid != uid || pTaskInfo->streamInfo.lastStatus.ts != ts) {*/
      STableScanInfo* pTableScanInfo = pInfo->pTableScanOp->info;
      int32_t         numOfTables = tableListGetSize(pTaskInfo->pTableInfoList);

#ifndef NDEBUG
      qDebug("switch to next table %" PRId64 " (cursor %d), %" PRId64 " rows returned", uid,
             pTableScanInfo->currentTable, pInfo->pTableScanOp->resultInfo.totalRows);
      pInfo->pTableScanOp->resultInfo.totalRows = 0;
#endif

      bool found = false;
      for (int32_t i = 0; i < numOfTables; i++) {
        STableKeyInfo* pTableInfo = tableListGetInfo(pTaskInfo->pTableInfoList, i);
        if (pTableInfo->uid == uid) {
          found = true;
          pTableScanInfo->currentTable = i;
          break;
        }
      }

      // TODO after dropping table, table may not found
      ASSERT(found);

      if (pTableScanInfo->dataReader == NULL) {
        STableKeyInfo* pList = tableListGetInfo(pTaskInfo->pTableInfoList, 0);
        int32_t num = tableListGetSize(pTaskInfo->pTableInfoList);

        if (tsdbReaderOpen(pTableScanInfo->readHandle.vnode, &pTableScanInfo->cond, pList, num,
                           &pTableScanInfo->dataReader, NULL) < 0 || pTableScanInfo->dataReader == NULL) {
          ASSERT(0);
        }
      }

      STableKeyInfo tki = {.uid = uid};
      tsdbSetTableList(pTableScanInfo->dataReader, &tki, 1);
      int64_t oldSkey = pTableScanInfo->cond.twindows.skey;
      pTableScanInfo->cond.twindows.skey = ts + 1;
      tsdbReaderReset(pTableScanInfo->dataReader, &pTableScanInfo->cond);
      pTableScanInfo->cond.twindows.skey = oldSkey;
      pTableScanInfo->scanTimes = 0;

      qDebug("tsdb reader offset seek to uid %" PRId64 " ts %" PRId64 ", table cur set to %d , all table num %d", uid,
             ts, pTableScanInfo->currentTable, numOfTables);
      /*}*/
    } else {
      ASSERT(0);
    }
  } else if (pOffset->type == TMQ_OFFSET__SNAPSHOT_DATA) {
    SStreamRawScanInfo* pInfo = pOperator->info;
    SSnapContext*       sContext = pInfo->sContext;
    if (setForSnapShot(sContext, pOffset->uid) != 0) {
      qError("setDataForSnapShot error. uid:%" PRIi64, pOffset->uid);
      return -1;
    }

    SMetaTableInfo mtInfo = getUidfromSnapShot(sContext);
    tsdbReaderClose(pInfo->dataReader);
    pInfo->dataReader = NULL;

    cleanupQueryTableDataCond(&pTaskInfo->streamInfo.tableCond);
    tableListClear(pTaskInfo->pTableInfoList);

    if (mtInfo.uid == 0) {
      return 0;  // no data
    }

    initQueryTableDataCondForTmq(&pTaskInfo->streamInfo.tableCond, sContext, &mtInfo);
    pTaskInfo->streamInfo.tableCond.twindows.skey = pOffset->ts;

    if (pTaskInfo->pTableInfoList == NULL)  {
      pTaskInfo->pTableInfoList = tableListCreate();
    }

    tableListAddTableInfo(pTaskInfo->pTableInfoList, mtInfo.uid, 0);

    STableKeyInfo* pList = tableListGetInfo(pTaskInfo->pTableInfoList, 0);
    int32_t size = tableListGetSize(pTaskInfo->pTableInfoList);
    ASSERT(size == 1);

    tsdbReaderOpen(pInfo->vnode, &pTaskInfo->streamInfo.tableCond, pList, size, &pInfo->dataReader, NULL);

    cleanupQueryTableDataCond(&pTaskInfo->streamInfo.tableCond);
    strcpy(pTaskInfo->streamInfo.tbName, mtInfo.tbName);
    tDeleteSSchemaWrapper(pTaskInfo->streamInfo.schema);
    pTaskInfo->streamInfo.schema = mtInfo.schema;

    qDebug("tmqsnap qStreamPrepareScan snapshot data uid %" PRId64 " ts %" PRId64, mtInfo.uid, pOffset->ts);
  } else if (pOffset->type == TMQ_OFFSET__SNAPSHOT_META) {
    SStreamRawScanInfo* pInfo = pOperator->info;
    SSnapContext*       sContext = pInfo->sContext;
    if (setForSnapShot(sContext, pOffset->uid) != 0) {
      qError("setForSnapShot error. uid:%" PRIu64 " ,version:%" PRId64, pOffset->uid, pOffset->version);
      return -1;
    }
    qDebug("tmqsnap qStreamPrepareScan snapshot meta uid %" PRId64 " ts %" PRId64, pOffset->uid, pOffset->ts);
  } else if (pOffset->type == TMQ_OFFSET__LOG) {
    SStreamRawScanInfo* pInfo = pOperator->info;
    tsdbReaderClose(pInfo->dataReader);
    pInfo->dataReader = NULL;
    qDebug("tmqsnap qStreamPrepareScan snapshot log");
  }
  return 0;
}
