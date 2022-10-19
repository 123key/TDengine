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
#include "streamInc.h"
#include "tcommon.h"
#include "ttimer.h"

// todo refactor
typedef struct SStateKey {
  SWinKey key;
  int64_t opNum;
} SStateKey;

typedef struct SStateSessionKey {
  SSessionKey key;
  int64_t     opNum;
} SStateSessionKey;

static inline int sessionKeyCmpr(const SSessionKey* pWin1, const SSessionKey* pWin2) {
  if (pWin1->groupId > pWin2->groupId) {
    return 1;
  } else if (pWin1->groupId < pWin2->groupId) {
    return -1;
  }

  if (pWin1->win.skey > pWin2->win.ekey) {
    return 1;
  } else if (pWin1->win.ekey < pWin2->win.skey) {
    return -1;
  }

  return 0;
}

static inline int stateSessionKeyCmpr(const void* pKey1, int kLen1, const void* pKey2, int kLen2) {
  SStateSessionKey* pWin1 = (SStateSessionKey*)pKey1;
  SStateSessionKey* pWin2 = (SStateSessionKey*)pKey2;

  if (pWin1->opNum > pWin2->opNum) {
    return 1;
  } else if (pWin1->opNum < pWin2->opNum) {
    return -1;
  }

  return sessionKeyCmpr(&pWin1->key, &pWin2->key);
}

static inline int stateKeyCmpr(const void* pKey1, int kLen1, const void* pKey2, int kLen2) {
  SStateKey* pWin1 = (SStateKey*)pKey1;
  SStateKey* pWin2 = (SStateKey*)pKey2;

  if (pWin1->opNum > pWin2->opNum) {
    return 1;
  } else if (pWin1->opNum < pWin2->opNum) {
    return -1;
  }

  if (pWin1->key.ts > pWin2->key.ts) {
    return 1;
  } else if (pWin1->key.ts < pWin2->key.ts) {
    return -1;
  }

  if (pWin1->key.groupId > pWin2->key.groupId) {
    return 1;
  } else if (pWin1->key.groupId < pWin2->key.groupId) {
    return -1;
  }

  return 0;
}

SStreamState* streamStateOpen(char* path, SStreamTask* pTask, bool specPath, int32_t szPage, int32_t pages) {
  szPage = szPage < 0 ? 4096 : szPage;
  pages = pages < 0 ? 256 : pages;
  SStreamState* pState = taosMemoryCalloc(1, sizeof(SStreamState));
  if (pState == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  char statePath[300];
  if (!specPath) {
    sprintf(statePath, "%s/%d", path, pTask->taskId);
  } else {
    memset(statePath, 0, 300);
    tstrncpy(statePath, path, 300);
  }
  if (tdbOpen(statePath, szPage, pages, &pState->db) < 0) {
    goto _err;
  }

  // open state storage backend
  if (tdbTbOpen("state.db", sizeof(SStateKey), -1, stateKeyCmpr, pState->db, &pState->pStateDb) < 0) {
    goto _err;
  }

  // todo refactor
  if (tdbTbOpen("fill.state.db", sizeof(SWinKey), -1, winKeyCmpr, pState->db, &pState->pFillStateDb) < 0) {
    goto _err;
  }

  if (tdbTbOpen("session.state.db", sizeof(SStateSessionKey), -1, stateSessionKeyCmpr, pState->db,
                &pState->pSessionStateDb) < 0) {
    goto _err;
  }

  if (tdbTbOpen("func.state.db", sizeof(STupleKey), -1, STupleKeyCmpr, pState->db, &pState->pFuncStateDb) < 0) {
    goto _err;
  }

  if (streamStateBegin(pState) < 0) {
    goto _err;
  }

  pState->pOwner = pTask;

  return pState;

_err:
  tdbTbClose(pState->pStateDb);
  tdbTbClose(pState->pFuncStateDb);
  tdbTbClose(pState->pFillStateDb);
  tdbTbClose(pState->pSessionStateDb);
  tdbClose(pState->db);
  taosMemoryFree(pState);
  return NULL;
}

void streamStateClose(SStreamState* pState) {
  tdbCommit(pState->db, &pState->txn);
  tdbTbClose(pState->pStateDb);
  tdbTbClose(pState->pFuncStateDb);
  tdbTbClose(pState->pFillStateDb);
  tdbTbClose(pState->pSessionStateDb);
  tdbClose(pState->db);

  taosMemoryFree(pState);
}

int32_t streamStateBegin(SStreamState* pState) {
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }

  if (tdbBegin(pState->db, &pState->txn) < 0) {
    tdbTxnClose(&pState->txn);
    return -1;
  }
  return 0;
}

int32_t streamStateCommit(SStreamState* pState) {
  if (tdbCommit(pState->db, &pState->txn) < 0) {
    return -1;
  }
  memset(&pState->txn, 0, sizeof(TXN));
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }
  if (tdbBegin(pState->db, &pState->txn) < 0) {
    return -1;
  }
  return 0;
}

int32_t streamStateAbort(SStreamState* pState) {
  if (tdbAbort(pState->db, &pState->txn) < 0) {
    return -1;
  }
  memset(&pState->txn, 0, sizeof(TXN));
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }
  if (tdbBegin(pState->db, &pState->txn) < 0) {
    return -1;
  }
  return 0;
}

int32_t streamStateFuncPut(SStreamState* pState, const STupleKey* key, const void* value, int32_t vLen) {
  return tdbTbUpsert(pState->pFuncStateDb, key, sizeof(STupleKey), value, vLen, &pState->txn);
}
int32_t streamStateFuncGet(SStreamState* pState, const STupleKey* key, void** pVal, int32_t* pVLen) {
  return tdbTbGet(pState->pFuncStateDb, key, sizeof(STupleKey), pVal, pVLen);
}

int32_t streamStateFuncDel(SStreamState* pState, const STupleKey* key) {
  return tdbTbDelete(pState->pFuncStateDb, key, sizeof(STupleKey), &pState->txn);
}

// todo refactor
int32_t streamStatePut(SStreamState* pState, const SWinKey* key, const void* value, int32_t vLen) {
  SStateKey sKey = {.key = *key, .opNum = pState->number};
  return tdbTbUpsert(pState->pStateDb, &sKey, sizeof(SStateKey), value, vLen, &pState->txn);
}

// todo refactor
int32_t streamStateFillPut(SStreamState* pState, const SWinKey* key, const void* value, int32_t vLen) {
  return tdbTbUpsert(pState->pFillStateDb, key, sizeof(SWinKey), value, vLen, &pState->txn);
}

// todo refactor
int32_t streamStateGet(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  SStateKey sKey = {.key = *key, .opNum = pState->number};
  return tdbTbGet(pState->pStateDb, &sKey, sizeof(SStateKey), pVal, pVLen);
}

// todo refactor
int32_t streamStateFillGet(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  return tdbTbGet(pState->pFillStateDb, key, sizeof(SWinKey), pVal, pVLen);
}

// todo refactor
int32_t streamStateDel(SStreamState* pState, const SWinKey* key) {
  SStateKey sKey = {.key = *key, .opNum = pState->number};
  return tdbTbDelete(pState->pStateDb, &sKey, sizeof(SStateKey), &pState->txn);
}

int32_t streamStateClear(SStreamState* pState) {
  SWinKey key = {.ts = 0, .groupId = 0};
  streamStatePut(pState, &key, NULL, 0);
  while (1) {
    SStreamStateCur* pCur = streamStateSeekKeyNext(pState, &key);
    SWinKey          delKey = {0};
    int32_t          code = streamStateGetKVByCur(pCur, &delKey, NULL, 0);
    streamStateFreeCur(pCur);
    if (code == 0) {
      streamStateDel(pState, &delKey);
    } else {
      break;
    }
  }
  return 0;
}

void streamStateSetNumber(SStreamState* pState, int32_t number) { pState->number = number; }

// todo refactor
int32_t streamStateFillDel(SStreamState* pState, const SWinKey* key) {
  return tdbTbDelete(pState->pFillStateDb, key, sizeof(SWinKey), &pState->txn);
}

int32_t streamStateAddIfNotExist(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  // todo refactor
  int32_t size = *pVLen;
  if (streamStateGet(pState, key, pVal, pVLen) == 0) {
    return 0;
  }
  *pVal = tdbRealloc(NULL, size);
  memset(*pVal, 0, size);
  return 0;
}

int32_t streamStateReleaseBuf(SStreamState* pState, const SWinKey* key, void* pVal) {
  // todo refactor
  if (!pVal) {
    return 0;
  }
  streamFreeVal(pVal);
  return 0;
}

SStreamStateCur* streamStateGetCur(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) return NULL;
  tdbTbcOpen(pState->pStateDb, &pCur->pCur, NULL);

  int32_t   c = 0;
  SStateKey sKey = {.key = *key, .opNum = pState->number};
  tdbTbcMoveTo(pCur->pCur, &sKey, sizeof(SStateKey), &c);
  if (c != 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }
  pCur->number = pState->number;
  return pCur;
}

SStreamStateCur* streamStateFillGetCur(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) return NULL;
  tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL);

  int32_t c = 0;
  tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c);
  if (c != 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }
  return pCur;
}

SStreamStateCur* streamStateGetAndCheckCur(SStreamState* pState, SWinKey* key) {
  SStreamStateCur* pCur = streamStateFillGetCur(pState, key);
  if (pCur) {
    int32_t code = streamStateGetGroupKVByCur(pCur, key, NULL, 0);
    if (code == 0) {
      return pCur;
    }
    streamStateFreeCur(pCur);
  }
  return NULL;
}

int32_t streamStateGetKVByCur(SStreamStateCur* pCur, SWinKey* pKey, const void** pVal, int32_t* pVLen) {
  if (!pCur) {
    return -1;
  }
  const SStateKey* pKTmp = NULL;
  int32_t          kLen;
  if (tdbTbcGet(pCur->pCur, (const void**)&pKTmp, &kLen, pVal, pVLen) < 0) {
    return -1;
  }
  if (pKTmp->opNum != pCur->number) {
    return -1;
  }
  *pKey = pKTmp->key;
  return 0;
}

int32_t streamStateFillGetKVByCur(SStreamStateCur* pCur, SWinKey* pKey, const void** pVal, int32_t* pVLen) {
  if (!pCur) {
    return -1;
  }
  const SWinKey* pKTmp = NULL;
  int32_t        kLen;
  if (tdbTbcGet(pCur->pCur, (const void**)&pKTmp, &kLen, pVal, pVLen) < 0) {
    return -1;
  }
  *pKey = *pKTmp;
  return 0;
}

int32_t streamStateGetGroupKVByCur(SStreamStateCur* pCur, SWinKey* pKey, const void** pVal, int32_t* pVLen) {
  if (!pCur) {
    return -1;
  }
  uint64_t groupId = pKey->groupId;
  int32_t  code = streamStateFillGetKVByCur(pCur, pKey, pVal, pVLen);
  if (code == 0) {
    if (pKey->groupId == groupId) {
      return 0;
    }
  }
  return -1;
}

int32_t streamStateGetFirst(SStreamState* pState, SWinKey* key) {
  // todo refactor
  SWinKey tmp = {.ts = 0, .groupId = 0};
  streamStatePut(pState, &tmp, NULL, 0);
  SStreamStateCur* pCur = streamStateSeekKeyNext(pState, &tmp);
  int32_t          code = streamStateGetKVByCur(pCur, key, NULL, 0);
  streamStateDel(pState, &tmp);
  return code;
}

int32_t streamStateSeekFirst(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToFirst(pCur->pCur);
}

int32_t streamStateSeekLast(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToLast(pCur->pCur);
}

SStreamStateCur* streamStateSeekKeyNext(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  pCur->number = pState->number;
  if (tdbTbcOpen(pState->pStateDb, &pCur->pCur, NULL) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  SStateKey sKey = {.key = *key, .opNum = pState->number};
  int32_t   c = 0;
  if (tdbTbcMoveTo(pCur->pCur, &sKey, sizeof(SStateKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    streamStateFreeCur(pCur);
    return NULL;
  }
  if (c > 0) return pCur;

  if (tdbTbcMoveToNext(pCur->pCur) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  return pCur;
}

SStreamStateCur* streamStateFillSeekKeyNext(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (!pCur) {
    return NULL;
  }
  if (tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  int32_t c = 0;
  if (tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    streamStateFreeCur(pCur);
    return NULL;
  }
  if (c > 0) return pCur;

  if (tdbTbcMoveToNext(pCur->pCur) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  return pCur;
}

SStreamStateCur* streamStateFillSeekKeyPrev(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  if (tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  int32_t c = 0;
  if (tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    streamStateFreeCur(pCur);
    return NULL;
  }
  if (c < 0) return pCur;

  if (tdbTbcMoveToPrev(pCur->pCur) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  return pCur;
}

int32_t streamStateCurNext(SStreamState* pState, SStreamStateCur* pCur) {
  if (!pCur) {
    return -1;
  }
  //
  return tdbTbcMoveToNext(pCur->pCur);
}

int32_t streamStateCurPrev(SStreamState* pState, SStreamStateCur* pCur) {
  //
  if (!pCur) {
    return -1;
  }
  return tdbTbcMoveToPrev(pCur->pCur);
}
void streamStateFreeCur(SStreamStateCur* pCur) {
  if (!pCur) {
    return;
  }
  tdbTbcClose(pCur->pCur);
  taosMemoryFree(pCur);
}

void streamFreeVal(void* val) { tdbFree(val); }

int32_t streamStateSessionPut(SStreamState* pState, const SSessionKey* key, const void* value, int32_t vLen) {
  SStateSessionKey sKey = {.key = *key, .opNum = pState->number};
  return tdbTbUpsert(pState->pSessionStateDb, &sKey, sizeof(SStateSessionKey), value, vLen, &pState->txn);
}

SStreamStateCur* streamStateSessionGetRanomCur(SStreamState* pState, const SSessionKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) return NULL;
  tdbTbcOpen(pState->pSessionStateDb, &pCur->pCur, NULL);

  int32_t          c = 0;
  SStateSessionKey sKey = {.key = *key, .opNum = pState->number};
  tdbTbcMoveTo(pCur->pCur, &sKey, sizeof(SStateSessionKey), &c);
  if (c != 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }
  pCur->number = pState->number;
  return pCur;
}

int32_t streamStateSessionGet(SStreamState* pState, SSessionKey* key, void** pVal, int32_t* pVLen) {
  SStreamStateCur* pCur = streamStateSessionGetRanomCur(pState, key);
  void*            tmp = NULL;
  if (streamStateSessionGetKVByCur(pCur, key, (const void**)&tmp, pVLen) == 0) {
    *pVal = tdbRealloc(NULL, *pVLen);
    memcpy(*pVal, tmp, *pVLen);
    streamStateFreeCur(pCur);
    return 0;
  }
  streamStateFreeCur(pCur);
  return -1;
}

int32_t streamStateSessionDel(SStreamState* pState, const SSessionKey* key) {
  SStateSessionKey sKey = {.key = *key, .opNum = pState->number};
  return tdbTbDelete(pState->pSessionStateDb, &sKey, sizeof(SStateSessionKey), &pState->txn);
}

SStreamStateCur* streamStateSessionSeekKeyPrev(SStreamState* pState, const SSessionKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  pCur->number = pState->number;
  if (tdbTbcOpen(pState->pSessionStateDb, &pCur->pCur, NULL) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  SStateSessionKey sKey = {.key = *key, .opNum = pState->number};
  int32_t          c = 0;
  if (tdbTbcMoveTo(pCur->pCur, &sKey, sizeof(SStateSessionKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    streamStateFreeCur(pCur);
    return NULL;
  }
  if (c > 0) return pCur;

  if (tdbTbcMoveToPrev(pCur->pCur) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  return pCur;
}

SStreamStateCur* streamStateSessionSeekKeyNext(SStreamState* pState, const SSessionKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  pCur->number = pState->number;
  if (tdbTbcOpen(pState->pSessionStateDb, &pCur->pCur, NULL) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  SStateSessionKey sKey = {.key = *key, .opNum = pState->number};
  int32_t          c = 0;
  if (tdbTbcMoveTo(pCur->pCur, &sKey, sizeof(SStateSessionKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    streamStateFreeCur(pCur);
    return NULL;
  }
  if (c > 0) return pCur;

  if (tdbTbcMoveToNext(pCur->pCur) < 0) {
    streamStateFreeCur(pCur);
    return NULL;
  }

  return pCur;
}

int32_t streamStateSessionGetKVByCur(SStreamStateCur* pCur, SSessionKey* pKey, const void** pVal, int32_t* pVLen) {
  if (!pCur) {
    return -1;
  }
  const SStateSessionKey* pKTmp = NULL;
  int32_t                 kLen;
  if (tdbTbcGet(pCur->pCur, (const void**)&pKTmp, &kLen, pVal, pVLen) < 0) {
    return -1;
  }
  if (pKTmp->opNum != pCur->number) {
    return -1;
  }
  if (pKey->groupId != 0 && pKey->groupId != pKTmp->key.groupId) {
    return -1;
  }
  *pKey = pKTmp->key;
  return 0;
}

int32_t streamStateSessionClear(SStreamState* pState) {
  SSessionKey key = {.win.skey = 0, .win.ekey = 0, .groupId = 0};
  streamStateSessionPut(pState, &key, NULL, 0);
  SStreamStateCur* pCur = streamStateSessionSeekKeyNext(pState, &key);
  while (1) {
    SSessionKey delKey = {0};
    void*       buf = NULL;
    int32_t     size = 0;
    int32_t     code = streamStateSessionGetKVByCur(pCur, &delKey, buf, &size);
    if (code == 0) {
      memset(buf, 0, size);
      streamStateSessionPut(pState, &delKey, buf, size);
    } else {
      break;
    }
    streamStateCurNext(pState, pCur);
  }
  streamStateFreeCur(pCur);
  streamStateSessionDel(pState, &key);
  return 0;
}

SStreamStateCur* streamStateSessionGetCur(SStreamState* pState, const SSessionKey* key) {
  SStreamStateCur* pCur = streamStateSessionGetRanomCur(pState, key);
  SSessionKey      resKey = *key;
  while (1) {
    streamStateCurPrev(pState, pCur);
    SSessionKey tmpKey = *key;
    int32_t     code = streamStateSessionGetKVByCur(pCur, &tmpKey, NULL, 0);
    if (code == TSDB_CODE_SUCCESS && sessionKeyCmpr(key, &tmpKey) == 0) {
      resKey = tmpKey;
    } else {
      break;
    }
  }
  streamStateFreeCur(pCur);
  return streamStateSessionGetRanomCur(pState, &resKey);
}

int32_t streamStateSessionAddIfNotExist(SStreamState* pState, SSessionKey* key, void** pVal, int32_t* pVLen) {
  // todo refactor
  SStreamStateCur* pCur = streamStateSessionGetCur(pState, key);
  int32_t          size = *pVLen;
  void*            tmp = NULL;
  *pVal = tdbRealloc(NULL, size);
  memset(*pVal, 0, size);
  if (streamStateSessionGetKVByCur(pCur, key, (const void**)&tmp, pVLen) == 0) {
    memcpy(*pVal, tmp, *pVLen);
    streamStateFreeCur(pCur);
    return 0;
  }
  streamStateFreeCur(pCur);
  return 1;
}

int32_t streamStateStateAddIfNotExist(SStreamState* pState, SSessionKey* key, char* pKeyData, int32_t keyDataLen,
                                      state_key_cmpr_fn fn, void** pVal, int32_t* pVLen) {
  // todo refactor
  int32_t     res = TSDB_CODE_SUCCESS;
  SSessionKey tmpKey = *key;
  int32_t     valSize = *pVLen;
  void*       tmp = tdbRealloc(NULL, valSize);
  if (!tmp) {
    return -1;
  }

  SStreamStateCur* pCur = streamStateSessionGetRanomCur(pState, key);
  int32_t          code = streamStateSessionGetKVByCur(pCur, key, (const void**)pVal, pVLen);
  if (code == TSDB_CODE_SUCCESS) {
    memcpy(tmp, *pVal, valSize);
    *pVal = tmp;
    streamStateFreeCur(pCur);
    return res;
  }
  streamStateFreeCur(pCur);

  streamStateSessionPut(pState, key, NULL, 0);
  pCur = streamStateSessionGetRanomCur(pState, key);
  streamStateCurPrev(pState, pCur);
  code = streamStateSessionGetKVByCur(pCur, key, (const void**)pVal, pVLen);
  if (code == TSDB_CODE_SUCCESS) {
    void* stateKey = (char*)(*pVal) + (valSize - keyDataLen);
    if (fn(pKeyData, stateKey) == true) {
      memcpy(tmp, *pVal, valSize);
      goto _end;
    }
  }

  streamStateFreeCur(pCur);
  *key = tmpKey;
  pCur = streamStateSessionSeekKeyNext(pState, key);
  code = streamStateSessionGetKVByCur(pCur, key, (const void**)pVal, pVLen);
  if (code == TSDB_CODE_SUCCESS) {
    void* stateKey = (char*)(*pVal) + (valSize - keyDataLen);
    if (fn(pKeyData, stateKey) == true) {
      memcpy(tmp, *pVal, valSize);
      goto _end;
    }
  }

  *key = tmpKey;
  res = 1;
  memset(tmp, 0, valSize);

_end:

  *pVal = tmp;
  streamStateSessionDel(pState, &tmpKey);
  streamStateFreeCur(pCur);
  return res;
}
