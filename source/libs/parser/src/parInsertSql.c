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

#include "parInsertUtil.h"
#include "parToken.h"
#include "tglobal.h"
#include "ttime.h"

#define NEXT_TOKEN_WITH_PREV(pSql, sToken)     \
  do {                                         \
    int32_t index = 0;                         \
    sToken = tStrGetToken(pSql, &index, true); \
    pSql += index;                             \
  } while (0)

#define NEXT_TOKEN_KEEP_SQL(pSql, sToken, index) \
  do {                                           \
    sToken = tStrGetToken(pSql, &index, false);  \
  } while (0)

#define NEXT_VALID_TOKEN(pSql, sToken)        \
  do {                                        \
    sToken.n = tGetToken(pSql, &sToken.type); \
    sToken.z = pSql;                          \
    pSql += sToken.n;                         \
  } while (TK_NK_SPACE == sToken.type)

typedef int32_t (*_row_append_fn_t)(SMsgBuf* pMsgBuf, const void* value, int32_t len, void* param);

static uint8_t TRUE_VALUE = (uint8_t)TSDB_TRUE;
static uint8_t FALSE_VALUE = (uint8_t)TSDB_FALSE;

static int32_t skipInsertInto(char** pSql, SMsgBuf* pMsg) {
  SToken sToken;
  NEXT_TOKEN(*pSql, sToken);
  if (TK_INSERT != sToken.type && TK_IMPORT != sToken.type) {
    return buildSyntaxErrMsg(pMsg, "keyword INSERT is expected", sToken.z);
  }
  NEXT_TOKEN(*pSql, sToken);
  if (TK_INTO != sToken.type) {
    return buildSyntaxErrMsg(pMsg, "keyword INTO is expected", sToken.z);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t checkAuth(SInsertParseContext* pCxt, char* pDbFname, bool* pPass) {
  SParseContext* pBasicCtx = pCxt->pComCxt;
  if (pBasicCtx->async) {
    return getUserAuthFromCache(pCxt->pMetaCache, pBasicCtx->pUser, pDbFname, AUTH_TYPE_WRITE, pPass);
  }
  SRequestConnInfo conn = {.pTrans = pBasicCtx->pTransporter,
                           .requestId = pBasicCtx->requestId,
                           .requestObjRefId = pBasicCtx->requestRid,
                           .mgmtEps = pBasicCtx->mgmtEpSet};

  return catalogChkAuth(pBasicCtx->pCatalog, &conn, pBasicCtx->pUser, pDbFname, AUTH_TYPE_WRITE, pPass);
}

static int32_t getTableSchema(SInsertParseContext* pCxt, int32_t tbNo, SName* pTbName, bool isStb,
                              STableMeta** pTableMeta) {
  SParseContext* pBasicCtx = pCxt->pComCxt;
  if (pBasicCtx->async) {
    return getTableMetaFromCacheForInsert(pBasicCtx->pTableMetaPos, pCxt->pMetaCache, tbNo, pTableMeta);
  }
  SRequestConnInfo conn = {.pTrans = pBasicCtx->pTransporter,
                           .requestId = pBasicCtx->requestId,
                           .requestObjRefId = pBasicCtx->requestRid,
                           .mgmtEps = pBasicCtx->mgmtEpSet};

  if (isStb) {
    return catalogGetSTableMeta(pBasicCtx->pCatalog, &conn, pTbName, pTableMeta);
  }
  return catalogGetTableMeta(pBasicCtx->pCatalog, &conn, pTbName, pTableMeta);
}

static int32_t getTableVgroup(SInsertParseContext* pCxt, int32_t tbNo, SName* pTbName, SVgroupInfo* pVg) {
  SParseContext* pBasicCtx = pCxt->pComCxt;
  if (pBasicCtx->async) {
    return getTableVgroupFromCacheForInsert(pBasicCtx->pTableVgroupPos, pCxt->pMetaCache, tbNo, pVg);
  }
  SRequestConnInfo conn = {.pTrans = pBasicCtx->pTransporter,
                           .requestId = pBasicCtx->requestId,
                           .requestObjRefId = pBasicCtx->requestRid,
                           .mgmtEps = pBasicCtx->mgmtEpSet};
  return catalogGetTableHashVgroup(pBasicCtx->pCatalog, &conn, pTbName, pVg);
}

static int32_t getTableMetaImpl(SInsertParseContext* pCxt, int32_t tbNo, SName* name, bool isStb) {
  CHECK_CODE(getTableSchema(pCxt, tbNo, name, isStb, &pCxt->pTableMeta));
  if (!isStb) {
    SVgroupInfo vg;
    CHECK_CODE(getTableVgroup(pCxt, tbNo, name, &vg));
    CHECK_CODE(taosHashPut(pCxt->pVgroupsHashObj, (const char*)&vg.vgId, sizeof(vg.vgId), (char*)&vg, sizeof(vg)));
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t getTableMeta(SInsertParseContext* pCxt, int32_t tbNo, SName* name) {
  return getTableMetaImpl(pCxt, tbNo, name, false);
}

static int32_t getSTableMeta(SInsertParseContext* pCxt, int32_t tbNo, SName* name) {
  return getTableMetaImpl(pCxt, tbNo, name, true);
}

static int32_t getDBCfg(SInsertParseContext* pCxt, const char* pDbFName, SDbCfgInfo* pInfo) {
  SParseContext* pBasicCtx = pCxt->pComCxt;
  if (pBasicCtx->async) {
    CHECK_CODE(getDbCfgFromCache(pCxt->pMetaCache, pDbFName, pInfo));
  } else {
    SRequestConnInfo conn = {.pTrans = pBasicCtx->pTransporter,
                             .requestId = pBasicCtx->requestId,
                             .requestObjRefId = pBasicCtx->requestRid,
                             .mgmtEps = pBasicCtx->mgmtEpSet};
    CHECK_CODE(catalogGetDBCfg(pBasicCtx->pCatalog, &conn, pDbFName, pInfo));
  }
  return TSDB_CODE_SUCCESS;
}

static int parseTime(char** end, SToken* pToken, int16_t timePrec, int64_t* time, SMsgBuf* pMsgBuf) {
  int32_t index = 0;
  SToken  sToken;
  int64_t interval;
  int64_t ts = 0;
  char*   pTokenEnd = *end;

  if (pToken->type == TK_NOW) {
    ts = taosGetTimestamp(timePrec);
  } else if (pToken->type == TK_TODAY) {
    ts = taosGetTimestampToday(timePrec);
  } else if (pToken->type == TK_NK_INTEGER) {
    if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &ts)) {
      return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp format", pToken->z);
    }
  } else {  // parse the RFC-3339/ISO-8601 timestamp format string
    if (taosParseTime(pToken->z, time, pToken->n, timePrec, tsDaylight) != TSDB_CODE_SUCCESS) {
      return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp format", pToken->z);
    }

    return TSDB_CODE_SUCCESS;
  }

  for (int k = pToken->n; pToken->z[k] != '\0'; k++) {
    if (pToken->z[k] == ' ' || pToken->z[k] == '\t') continue;
    if (pToken->z[k] == '(' && pToken->z[k + 1] == ')') {  // for insert NOW()/TODAY()
      *end = pTokenEnd = &pToken->z[k + 2];
      k++;
      continue;
    }
    if (pToken->z[k] == ',') {
      *end = pTokenEnd;
      *time = ts;
      return 0;
    }

    break;
  }

  /*
   * time expression:
   * e.g., now+12a, now-5h
   */
  SToken valueToken;
  index = 0;
  sToken = tStrGetToken(pTokenEnd, &index, false);
  pTokenEnd += index;

  if (sToken.type == TK_NK_MINUS || sToken.type == TK_NK_PLUS) {
    index = 0;
    valueToken = tStrGetToken(pTokenEnd, &index, false);
    pTokenEnd += index;

    if (valueToken.n < 2) {
      return buildSyntaxErrMsg(pMsgBuf, "value expected in timestamp", sToken.z);
    }

    char unit = 0;
    if (parseAbsoluteDuration(valueToken.z, valueToken.n, &interval, &unit, timePrec) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    if (sToken.type == TK_NK_PLUS) {
      ts += interval;
    } else {
      ts = ts - interval;
    }

    *end = pTokenEnd;
  }

  *time = ts;
  return TSDB_CODE_SUCCESS;
}

static FORCE_INLINE int32_t checkAndTrimValue(SToken* pToken, char* tmpTokenBuf, SMsgBuf* pMsgBuf) {
  if ((pToken->type != TK_NOW && pToken->type != TK_TODAY && pToken->type != TK_NK_INTEGER &&
       pToken->type != TK_NK_STRING && pToken->type != TK_NK_FLOAT && pToken->type != TK_NK_BOOL &&
       pToken->type != TK_NULL && pToken->type != TK_NK_HEX && pToken->type != TK_NK_OCT &&
       pToken->type != TK_NK_BIN) ||
      (pToken->n == 0) || (pToken->type == TK_NK_RP)) {
    return buildSyntaxErrMsg(pMsgBuf, "invalid data or symbol", pToken->z);
  }

  // Remove quotation marks
  if (TK_NK_STRING == pToken->type) {
    if (pToken->n >= TSDB_MAX_BYTES_PER_ROW) {
      return buildSyntaxErrMsg(pMsgBuf, "too long string", pToken->z);
    }

    int32_t len = trimString(pToken->z, pToken->n, tmpTokenBuf, TSDB_MAX_BYTES_PER_ROW);
    pToken->z = tmpTokenBuf;
    pToken->n = len;
  }

  return TSDB_CODE_SUCCESS;
}

static bool isNullStr(SToken* pToken) {
  return ((pToken->type == TK_NK_STRING) && (strlen(TSDB_DATA_NULL_STR_L) == pToken->n) &&
          (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0));
}

static bool isNullValue(int8_t dataType, SToken* pToken) {
  return TK_NULL == pToken->type || (!IS_STR_DATA_TYPE(dataType) && isNullStr(pToken));
}

static FORCE_INLINE int32_t toDouble(SToken* pToken, double* value, char** endPtr) {
  errno = 0;
  *value = taosStr2Double(pToken->z, endPtr);

  // not a valid integer number, return error
  if ((*endPtr - pToken->z) != pToken->n) {
    return TK_NK_ILLEGAL;
  }

  return pToken->type;
}

static int32_t parseValueToken(char** end, SToken* pToken, SSchema* pSchema, int16_t timePrec, char* tmpTokenBuf,
                               _row_append_fn_t func, void* param, SMsgBuf* pMsgBuf) {
  int64_t  iv;
  uint64_t uv;
  char*    endptr = NULL;

  int32_t code = checkAndTrimValue(pToken, tmpTokenBuf, pMsgBuf);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  if (isNullValue(pSchema->type, pToken)) {
    if (TSDB_DATA_TYPE_TIMESTAMP == pSchema->type && PRIMARYKEY_TIMESTAMP_COL_ID == pSchema->colId) {
      return buildSyntaxErrMsg(pMsgBuf, "primary timestamp should not be null", pToken->z);
    }

    return func(pMsgBuf, NULL, 0, param);
  }

  if (IS_NUMERIC_TYPE(pSchema->type) && pToken->n == 0) {
    return buildSyntaxErrMsg(pMsgBuf, "invalid numeric data", pToken->z);
  }

  switch (pSchema->type) {
    case TSDB_DATA_TYPE_BOOL: {
      if ((pToken->type == TK_NK_BOOL || pToken->type == TK_NK_STRING) && (pToken->n != 0)) {
        if (strncmp(pToken->z, "true", pToken->n) == 0) {
          return func(pMsgBuf, &TRUE_VALUE, pSchema->bytes, param);
        } else if (strncmp(pToken->z, "false", pToken->n) == 0) {
          return func(pMsgBuf, &FALSE_VALUE, pSchema->bytes, param);
        } else {
          return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
        }
      } else if (pToken->type == TK_NK_INTEGER) {
        return func(pMsgBuf, ((taosStr2Int64(pToken->z, NULL, 10) == 0) ? &FALSE_VALUE : &TRUE_VALUE), pSchema->bytes,
                    param);
      } else if (pToken->type == TK_NK_FLOAT) {
        return func(pMsgBuf, ((taosStr2Double(pToken->z, NULL) == 0) ? &FALSE_VALUE : &TRUE_VALUE), pSchema->bytes,
                    param);
      } else {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
      }
    }

    case TSDB_DATA_TYPE_TINYINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid tinyint data", pToken->z);
      } else if (!IS_VALID_TINYINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "tinyint data overflow", pToken->z);
      }

      uint8_t tmpVal = (uint8_t)iv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_UTINYINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned tinyint data", pToken->z);
      } else if (uv > UINT8_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned tinyint data overflow", pToken->z);
      }
      uint8_t tmpVal = (uint8_t)uv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_SMALLINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid smallint data", pToken->z);
      } else if (!IS_VALID_SMALLINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "smallint data overflow", pToken->z);
      }
      int16_t tmpVal = (int16_t)iv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_USMALLINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned smallint data", pToken->z);
      } else if (uv > UINT16_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned smallint data overflow", pToken->z);
      }
      uint16_t tmpVal = (uint16_t)uv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_INT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid int data", pToken->z);
      } else if (!IS_VALID_INT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "int data overflow", pToken->z);
      }
      int32_t tmpVal = (int32_t)iv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_UINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned int data", pToken->z);
      } else if (uv > UINT32_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned int data overflow", pToken->z);
      }
      uint32_t tmpVal = (uint32_t)uv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_BIGINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bigint data", pToken->z);
      }
      return func(pMsgBuf, &iv, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_UBIGINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned bigint data", pToken->z);
      }
      return func(pMsgBuf, &uv, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_FLOAT: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || dv > FLT_MAX || dv < -FLT_MAX || isinf(dv) ||
          isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      float tmpVal = (float)dv;
      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_DOUBLE: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || isinf(dv) || isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }
      return func(pMsgBuf, &dv, pSchema->bytes, param);
    }

    case TSDB_DATA_TYPE_BINARY: {
      // Too long values will raise the invalid sql error message
      if (pToken->n + VARSTR_HEADER_SIZE > pSchema->bytes) {
        return generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      }

      return func(pMsgBuf, pToken->z, pToken->n, param);
    }

    case TSDB_DATA_TYPE_NCHAR: {
      return func(pMsgBuf, pToken->z, pToken->n, param);
    }
    case TSDB_DATA_TYPE_JSON: {
      if (pToken->n > (TSDB_MAX_JSON_TAG_LEN - VARSTR_HEADER_SIZE) / TSDB_NCHAR_SIZE) {
        return buildSyntaxErrMsg(pMsgBuf, "json string too long than 4095", pToken->z);
      }
      return func(pMsgBuf, pToken->z, pToken->n, param);
    }
    case TSDB_DATA_TYPE_TIMESTAMP: {
      int64_t tmpVal;
      if (parseTime(end, pToken, timePrec, &tmpVal, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp", pToken->z);
      }

      return func(pMsgBuf, &tmpVal, pSchema->bytes, param);
    }
  }

  return TSDB_CODE_FAILED;
}

// pSql -> tag1_name, ...)
static int32_t parseBoundColumns(SInsertParseContext* pCxt, SParsedDataColInfo* pColList, SSchema* pSchema) {
  col_id_t nCols = pColList->numOfCols;

  pColList->numOfBound = 0;
  pColList->boundNullLen = 0;
  memset(pColList->boundColumns, 0, sizeof(col_id_t) * nCols);
  for (col_id_t i = 0; i < nCols; ++i) {
    pColList->cols[i].valStat = VAL_STAT_NONE;
  }

  SToken   sToken;
  bool     isOrdered = true;
  col_id_t lastColIdx = -1;  // last column found
  while (1) {
    NEXT_TOKEN(pCxt->pSql, sToken);

    if (TK_NK_RP == sToken.type) {
      break;
    }

    char tmpTokenBuf[TSDB_COL_NAME_LEN + 2] = {0};  // used for deleting Escape character backstick(`)
    strncpy(tmpTokenBuf, sToken.z, sToken.n);
    sToken.z = tmpTokenBuf;
    sToken.n = strdequote(sToken.z);

    col_id_t t = lastColIdx + 1;
    col_id_t index = insFindCol(&sToken, t, nCols, pSchema);
    if (index < 0 && t > 0) {
      index = insFindCol(&sToken, 0, t, pSchema);
      isOrdered = false;
    }
    if (index < 0) {
      return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMN, sToken.z);
    }
    if (pColList->cols[index].valStat == VAL_STAT_HAS) {
      return buildSyntaxErrMsg(&pCxt->msg, "duplicated column name", sToken.z);
    }
    lastColIdx = index;
    pColList->cols[index].valStat = VAL_STAT_HAS;
    pColList->boundColumns[pColList->numOfBound] = index;
    ++pColList->numOfBound;
    switch (pSchema[t].type) {
      case TSDB_DATA_TYPE_BINARY:
        pColList->boundNullLen += (sizeof(VarDataOffsetT) + VARSTR_HEADER_SIZE + CHAR_BYTES);
        break;
      case TSDB_DATA_TYPE_NCHAR:
        pColList->boundNullLen += (sizeof(VarDataOffsetT) + VARSTR_HEADER_SIZE + TSDB_NCHAR_SIZE);
        break;
      default:
        pColList->boundNullLen += TYPE_BYTES[pSchema[t].type];
        break;
    }
  }

  pColList->orderStatus = isOrdered ? ORDER_STATUS_ORDERED : ORDER_STATUS_DISORDERED;

  if (!isOrdered) {
    pColList->colIdxInfo = taosMemoryCalloc(pColList->numOfBound, sizeof(SBoundIdxInfo));
    if (NULL == pColList->colIdxInfo) {
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }
    SBoundIdxInfo* pColIdx = pColList->colIdxInfo;
    for (col_id_t i = 0; i < pColList->numOfBound; ++i) {
      pColIdx[i].schemaColIdx = pColList->boundColumns[i];
      pColIdx[i].boundIdx = i;
    }
    taosSort(pColIdx, pColList->numOfBound, sizeof(SBoundIdxInfo), insSchemaIdxCompar);
    for (col_id_t i = 0; i < pColList->numOfBound; ++i) {
      pColIdx[i].finalIdx = i;
    }
    taosSort(pColIdx, pColList->numOfBound, sizeof(SBoundIdxInfo), insBoundIdxCompar);
  }

  if (pColList->numOfCols > pColList->numOfBound) {
    memset(&pColList->boundColumns[pColList->numOfBound], 0,
           sizeof(col_id_t) * (pColList->numOfCols - pColList->numOfBound));
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t parseTagToken(char** end, SToken* pToken, SSchema* pSchema, int16_t timePrec, STagVal* val,
                             SMsgBuf* pMsgBuf) {
  int64_t  iv;
  uint64_t uv;
  char*    endptr = NULL;

  if (isNullValue(pSchema->type, pToken)) {
    if (TSDB_DATA_TYPE_TIMESTAMP == pSchema->type && PRIMARYKEY_TIMESTAMP_COL_ID == pSchema->colId) {
      return buildSyntaxErrMsg(pMsgBuf, "primary timestamp should not be null", pToken->z);
    }

    return TSDB_CODE_SUCCESS;
  }

  //  strcpy(val->colName, pSchema->name);
  val->cid = pSchema->colId;
  val->type = pSchema->type;

  switch (pSchema->type) {
    case TSDB_DATA_TYPE_BOOL: {
      if ((pToken->type == TK_NK_BOOL || pToken->type == TK_NK_STRING) && (pToken->n != 0)) {
        if (strncmp(pToken->z, "true", pToken->n) == 0) {
          *(int8_t*)(&val->i64) = TRUE_VALUE;
        } else if (strncmp(pToken->z, "false", pToken->n) == 0) {
          *(int8_t*)(&val->i64) = FALSE_VALUE;
        } else {
          return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
        }
      } else if (pToken->type == TK_NK_INTEGER) {
        *(int8_t*)(&val->i64) = ((taosStr2Int64(pToken->z, NULL, 10) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else if (pToken->type == TK_NK_FLOAT) {
        *(int8_t*)(&val->i64) = ((taosStr2Double(pToken->z, NULL) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
      }
      break;
    }

    case TSDB_DATA_TYPE_TINYINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid tinyint data", pToken->z);
      } else if (!IS_VALID_TINYINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "tinyint data overflow", pToken->z);
      }

      *(int8_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_UTINYINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned tinyint data", pToken->z);
      } else if (uv > UINT8_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned tinyint data overflow", pToken->z);
      }
      *(uint8_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_SMALLINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid smallint data", pToken->z);
      } else if (!IS_VALID_SMALLINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "smallint data overflow", pToken->z);
      }
      *(int16_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_USMALLINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned smallint data", pToken->z);
      } else if (uv > UINT16_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned smallint data overflow", pToken->z);
      }
      *(uint16_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_INT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid int data", pToken->z);
      } else if (!IS_VALID_INT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "int data overflow", pToken->z);
      }
      *(int32_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_UINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned int data", pToken->z);
      } else if (uv > UINT32_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned int data overflow", pToken->z);
      }
      *(uint32_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_BIGINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bigint data", pToken->z);
      }
      val->i64 = iv;
      break;
    }

    case TSDB_DATA_TYPE_UBIGINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned bigint data", pToken->z);
      }
      *(uint64_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_FLOAT: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || dv > FLT_MAX || dv < -FLT_MAX || isinf(dv) ||
          isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      *(float*)(&val->i64) = dv;
      break;
    }

    case TSDB_DATA_TYPE_DOUBLE: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || isinf(dv) || isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }

      *(double*)(&val->i64) = dv;
      break;
    }

    case TSDB_DATA_TYPE_BINARY: {
      // Too long values will raise the invalid sql error message
      if (pToken->n + VARSTR_HEADER_SIZE > pSchema->bytes) {
        return generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      }
      val->pData = strdup(pToken->z);
      val->nData = pToken->n;
      break;
    }

    case TSDB_DATA_TYPE_NCHAR: {
      int32_t output = 0;
      void*   p = taosMemoryCalloc(1, pSchema->bytes - VARSTR_HEADER_SIZE);
      if (p == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      if (!taosMbsToUcs4(pToken->z, pToken->n, (TdUcs4*)(p), pSchema->bytes - VARSTR_HEADER_SIZE, &output)) {
        if (errno == E2BIG) {
          taosMemoryFree(p);
          return generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
        }
        char buf[512] = {0};
        snprintf(buf, tListLen(buf), " taosMbsToUcs4 error:%s", strerror(errno));
        taosMemoryFree(p);
        return buildSyntaxErrMsg(pMsgBuf, buf, pToken->z);
      }
      val->pData = p;
      val->nData = output;
      break;
    }
    case TSDB_DATA_TYPE_TIMESTAMP: {
      if (parseTime(end, pToken, timePrec, &iv, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp", pToken->z);
      }

      val->i64 = iv;
      break;
    }
  }

  return TSDB_CODE_SUCCESS;
}

// pSql -> tag1_value, ...)
static int32_t parseTagsClause(SInsertParseContext* pCxt, SSchema* pSchema, uint8_t precision, const char* tName) {
  int32_t code = TSDB_CODE_SUCCESS;
  SArray* pTagVals = taosArrayInit(pCxt->tags.numOfBound, sizeof(STagVal));
  SArray* tagName = taosArrayInit(8, TSDB_COL_NAME_LEN);
  SToken  sToken;
  bool    isParseBindParam = false;
  bool    isJson = false;
  STag*   pTag = NULL;
  for (int i = 0; i < pCxt->tags.numOfBound; ++i) {
    NEXT_TOKEN_WITH_PREV(pCxt->pSql, sToken);

    if (sToken.type == TK_NK_QUESTION) {
      isParseBindParam = true;
      if (NULL == pCxt->pStmtCb) {
        code = buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", sToken.z);
        break;
      }

      continue;
    }

    if (isParseBindParam) {
      code = buildInvalidOperationMsg(&pCxt->msg, "no mix usage for ? and tag values");
      break;
    }

    SSchema* pTagSchema = &pSchema[pCxt->tags.boundColumns[i]];
    char     tmpTokenBuf[TSDB_MAX_BYTES_PER_ROW] = {0};  // todo this can be optimize with parse column
    code = checkAndTrimValue(&sToken, tmpTokenBuf, &pCxt->msg);
    if (TSDB_CODE_SUCCESS == code) {
      if (!isNullValue(pTagSchema->type, &sToken)) {
        taosArrayPush(tagName, pTagSchema->name);
      }
      if (pTagSchema->type == TSDB_DATA_TYPE_JSON) {
        isJson = true;
        if (sToken.n > (TSDB_MAX_JSON_TAG_LEN - VARSTR_HEADER_SIZE) / TSDB_NCHAR_SIZE) {
          code = buildSyntaxErrMsg(&pCxt->msg, "json string too long than 4095", sToken.z);
          break;
        }
        if (isNullValue(pTagSchema->type, &sToken)) {
          code = tTagNew(pTagVals, 1, true, &pTag);
        } else {
          code = parseJsontoTagData(sToken.z, pTagVals, &pTag, &pCxt->msg);
        }
      } else {
        STagVal val = {0};
        code = parseTagToken(&pCxt->pSql, &sToken, pTagSchema, precision, &val, &pCxt->msg);
        if (TSDB_CODE_SUCCESS == code) {
          taosArrayPush(pTagVals, &val);
        }
      }
    }
    if (TSDB_CODE_SUCCESS != code) {
      break;
    }
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam && !isJson) {
    code = tTagNew(pTagVals, 1, false, &pTag);
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam) {
    insBuildCreateTbReq(&pCxt->createTblReq, tName, pTag, pCxt->pTableMeta->suid, pCxt->sTableName, tagName,
                        pCxt->pTableMeta->tableInfo.numOfTags);
    pTag = NULL;
  }

  for (int i = 0; i < taosArrayGetSize(pTagVals); ++i) {
    STagVal* p = (STagVal*)taosArrayGet(pTagVals, i);
    if (IS_VAR_DATA_TYPE(p->type)) {
      taosMemoryFreeClear(p->pData);
    }
  }
  taosArrayDestroy(pTagVals);
  taosArrayDestroy(tagName);
  tTagFree(pTag);
  return code;
}

static int32_t storeTableMeta(SInsertParseContext* pCxt, SHashObj* pHash, int32_t tbNo, SName* pTableName,
                              const char* pName, int32_t len, STableMeta* pMeta) {
  SVgroupInfo vg;
  CHECK_CODE(getTableVgroup(pCxt, tbNo, pTableName, &vg));
  CHECK_CODE(taosHashPut(pCxt->pVgroupsHashObj, (const char*)&vg.vgId, sizeof(vg.vgId), (char*)&vg, sizeof(vg)));

  pMeta->uid = tbNo;
  pMeta->vgId = vg.vgId;
  pMeta->tableType = TSDB_CHILD_TABLE;

  STableMeta* pBackup = NULL;
  if (TSDB_CODE_SUCCESS != cloneTableMeta(pMeta, &pBackup)) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }
  return taosHashPut(pHash, pName, len, &pBackup, POINTER_BYTES);
}

static int32_t skipParentheses(SInsertParseSyntaxCxt* pCxt) {
  SToken  sToken;
  int32_t expectRightParenthesis = 1;
  while (1) {
    NEXT_TOKEN(pCxt->pSql, sToken);
    if (TK_NK_LP == sToken.type) {
      ++expectRightParenthesis;
    } else if (TK_NK_RP == sToken.type && 0 == --expectRightParenthesis) {
      break;
    }
    if (0 == sToken.n) {
      return buildSyntaxErrMsg(&pCxt->msg, ") expected", NULL);
    }
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t skipBoundColumns(SInsertParseSyntaxCxt* pCxt) { return skipParentheses(pCxt); }

static int32_t ignoreBoundColumns(SInsertParseContext* pCxt) {
  SInsertParseSyntaxCxt cxt = {.pComCxt = pCxt->pComCxt, .pSql = pCxt->pSql, .msg = pCxt->msg, .pMetaCache = NULL};
  int32_t               code = skipBoundColumns(&cxt);
  pCxt->pSql = cxt.pSql;
  return code;
}

static int32_t skipUsingClause(SInsertParseSyntaxCxt* pCxt);

// pSql -> stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)
static int32_t ignoreAutoCreateTableClause(SInsertParseContext* pCxt) {
  SToken sToken;
  NEXT_TOKEN(pCxt->pSql, sToken);
  SInsertParseSyntaxCxt cxt = {.pComCxt = pCxt->pComCxt, .pSql = pCxt->pSql, .msg = pCxt->msg, .pMetaCache = NULL};
  int32_t               code = skipUsingClause(&cxt);
  pCxt->pSql = cxt.pSql;
  return code;
}

static int32_t parseTableOptions(SInsertParseContext* pCxt) {
  do {
    int32_t index = 0;
    SToken  sToken;
    NEXT_TOKEN_KEEP_SQL(pCxt->pSql, sToken, index);
    if (TK_TTL == sToken.type) {
      pCxt->pSql += index;
      NEXT_TOKEN_WITH_PREV(pCxt->pSql, sToken);
      if (TK_NK_INTEGER != sToken.type) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option ttl", sToken.z);
      }
      pCxt->createTblReq.ttl = taosStr2Int32(sToken.z, NULL, 10);
      if (pCxt->createTblReq.ttl < 0) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option ttl", sToken.z);
      }
    } else if (TK_COMMENT == sToken.type) {
      pCxt->pSql += index;
      NEXT_TOKEN(pCxt->pSql, sToken);
      if (TK_NK_STRING != sToken.type) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option comment", sToken.z);
      }
      if (sToken.n >= TSDB_TB_COMMENT_LEN) {
        return buildSyntaxErrMsg(&pCxt->msg, "comment too long", sToken.z);
      }
      int32_t len = trimString(sToken.z, sToken.n, pCxt->tmpTokenBuf, TSDB_TB_COMMENT_LEN);
      pCxt->createTblReq.comment = strndup(pCxt->tmpTokenBuf, len);
      if (NULL == pCxt->createTblReq.comment) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      pCxt->createTblReq.commentLen = len;
    } else {
      break;
    }
  } while (1);
  return TSDB_CODE_SUCCESS;
}

// pSql -> stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)
static int32_t parseUsingClause(SInsertParseContext* pCxt, int32_t tbNo, SName* name, char* tbFName) {
  int32_t      len = strlen(tbFName);
  STableMeta** pMeta = taosHashGet(pCxt->pSubTableHashObj, tbFName, len);
  if (NULL != pMeta) {
    CHECK_CODE(ignoreAutoCreateTableClause(pCxt));
    return cloneTableMeta(*pMeta, &pCxt->pTableMeta);
  }

  SToken sToken;
  // pSql -> stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)
  NEXT_TOKEN(pCxt->pSql, sToken);

  SName sname;
  CHECK_CODE(insCreateSName(&sname, &sToken, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg));
  char dbFName[TSDB_DB_FNAME_LEN];
  tNameGetFullDbName(&sname, dbFName);
  strcpy(pCxt->sTableName, sname.tname);

  CHECK_CODE(getSTableMeta(pCxt, tbNo, &sname));
  if (TSDB_SUPER_TABLE != pCxt->pTableMeta->tableType) {
    return buildInvalidOperationMsg(&pCxt->msg, "create table only from super table is allowed");
  }
  CHECK_CODE(storeTableMeta(pCxt, pCxt->pSubTableHashObj, tbNo, name, tbFName, len, pCxt->pTableMeta));

  SSchema* pTagsSchema = getTableTagSchema(pCxt->pTableMeta);
  insSetBoundColumnInfo(&pCxt->tags, pTagsSchema, getNumOfTags(pCxt->pTableMeta));

  // pSql -> [(tag1_name, ...)] TAGS (tag1_value, ...)
  NEXT_TOKEN(pCxt->pSql, sToken);
  if (TK_NK_LP == sToken.type) {
    CHECK_CODE(parseBoundColumns(pCxt, &pCxt->tags, pTagsSchema));
    NEXT_TOKEN(pCxt->pSql, sToken);
  }

  if (TK_TAGS != sToken.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "TAGS is expected", sToken.z);
  }
  // pSql -> (tag1_value, ...)
  NEXT_TOKEN(pCxt->pSql, sToken);
  if (TK_NK_LP != sToken.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "( is expected", sToken.z);
  }
  CHECK_CODE(parseTagsClause(pCxt, pTagsSchema, getTableInfo(pCxt->pTableMeta).precision, name->tname));
  NEXT_VALID_TOKEN(pCxt->pSql, sToken);
  if (TK_NK_COMMA == sToken.type) {
    return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_TAGS_NOT_MATCHED);
  } else if (TK_NK_RP != sToken.type) {
    return buildSyntaxErrMsg(&pCxt->msg, ") is expected", sToken.z);
  }

  return parseTableOptions(pCxt);
}

static int parseOneRow(SInsertParseContext* pCxt, STableDataBlocks* pDataBlocks, int16_t timePrec, bool* gotRow,
                       char* tmpTokenBuf) {
  SParsedDataColInfo* spd = &pDataBlocks->boundColumnInfo;
  SRowBuilder*        pBuilder = &pDataBlocks->rowBuilder;
  STSRow*             row = (STSRow*)(pDataBlocks->pData + pDataBlocks->size);  // skip the SSubmitBlk header

  tdSRowResetBuf(pBuilder, row);

  bool      isParseBindParam = false;
  SSchema*  schema = getTableColumnSchema(pDataBlocks->pTableMeta);
  SMemParam param = {.rb = pBuilder};
  SToken    sToken = {0};
  // 1. set the parsed value from sql string
  for (int i = 0; i < spd->numOfBound; ++i) {
    NEXT_TOKEN_WITH_PREV(pCxt->pSql, sToken);
    SSchema* pSchema = &schema[spd->boundColumns[i]];

    if (sToken.type == TK_NK_QUESTION) {
      isParseBindParam = true;
      if (NULL == pCxt->pStmtCb) {
        return buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", sToken.z);
      }

      continue;
    }

    if (TK_NK_RP == sToken.type) {
      return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMNS_NUM);
    }

    if (isParseBindParam) {
      return buildInvalidOperationMsg(&pCxt->msg, "no mix usage for ? and values");
    }

    param.schema = pSchema;
    insGetSTSRowAppendInfo(pBuilder->rowType, spd, i, &param.toffset, &param.colIdx);
    CHECK_CODE(
        parseValueToken(&pCxt->pSql, &sToken, pSchema, timePrec, tmpTokenBuf, insMemRowAppend, &param, &pCxt->msg));

    if (i < spd->numOfBound - 1) {
      NEXT_VALID_TOKEN(pCxt->pSql, sToken);
      if (TK_NK_COMMA != sToken.type) {
        return buildSyntaxErrMsg(&pCxt->msg, ", expected", sToken.z);
      }
    }
  }

  TSKEY tsKey = TD_ROW_KEY(row);
  insCheckTimestamp(pDataBlocks, (const char*)&tsKey);

  if (!isParseBindParam) {
    // set the null value for the columns that do not assign values
    if ((spd->numOfBound < spd->numOfCols) && TD_IS_TP_ROW(row)) {
      pBuilder->hasNone = true;
    }

    tdSRowEnd(pBuilder);

    *gotRow = true;

#ifdef TD_DEBUG_PRINT_ROW
    STSchema* pSTSchema = tdGetSTSChemaFromSSChema(schema, spd->numOfCols, 1);
    tdSRowPrint(row, pSTSchema, __func__);
    taosMemoryFree(pSTSchema);
#endif
  }

  // *len = pBuilder->extendedRowSize;
  return TSDB_CODE_SUCCESS;
}

static int32_t allocateMemIfNeed(STableDataBlocks* pDataBlock, int32_t rowSize, int32_t* numOfRows) {
  size_t    remain = pDataBlock->nAllocSize - pDataBlock->size;
  const int factor = 5;
  uint32_t  nAllocSizeOld = pDataBlock->nAllocSize;

  // expand the allocated size
  if (remain < rowSize * factor) {
    while (remain < rowSize * factor) {
      pDataBlock->nAllocSize = (uint32_t)(pDataBlock->nAllocSize * 1.5);
      remain = pDataBlock->nAllocSize - pDataBlock->size;
    }

    char* tmp = taosMemoryRealloc(pDataBlock->pData, (size_t)pDataBlock->nAllocSize);
    if (tmp != NULL) {
      pDataBlock->pData = tmp;
      memset(pDataBlock->pData + pDataBlock->size, 0, pDataBlock->nAllocSize - pDataBlock->size);
    } else {
      // do nothing, if allocate more memory failed
      pDataBlock->nAllocSize = nAllocSizeOld;
      *numOfRows = (int32_t)(pDataBlock->nAllocSize - pDataBlock->headerSize) / rowSize;
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }
  }

  *numOfRows = (int32_t)(pDataBlock->nAllocSize - pDataBlock->headerSize) / rowSize;
  return TSDB_CODE_SUCCESS;
}

// pSql -> (field1_value, ...) [(field1_value2, ...) ...]
static int32_t parseValues(SInsertParseContext* pCxt, STableDataBlocks* pDataBlock, int maxRows, int32_t* numOfRows) {
  STableComInfo tinfo = getTableInfo(pDataBlock->pTableMeta);
  int32_t       extendedRowSize = insGetExtendedRowSize(pDataBlock);
  CHECK_CODE(
      insInitRowBuilder(&pDataBlock->rowBuilder, pDataBlock->pTableMeta->sversion, &pDataBlock->boundColumnInfo));

  (*numOfRows) = 0;
  // char   tmpTokenBuf[TSDB_MAX_BYTES_PER_ROW] = {0};  // used for deleting Escape character: \\, \', \"
  SToken sToken;
  while (1) {
    int32_t index = 0;
    NEXT_TOKEN_KEEP_SQL(pCxt->pSql, sToken, index);
    if (TK_NK_LP != sToken.type) {
      break;
    }
    pCxt->pSql += index;

    if ((*numOfRows) >= maxRows || pDataBlock->size + extendedRowSize >= pDataBlock->nAllocSize) {
      int32_t tSize;
      CHECK_CODE(allocateMemIfNeed(pDataBlock, extendedRowSize, &tSize));
      ASSERT(tSize >= maxRows);
      maxRows = tSize;
    }

    bool gotRow = false;
    CHECK_CODE(parseOneRow(pCxt, pDataBlock, tinfo.precision, &gotRow, pCxt->tmpTokenBuf));
    if (gotRow) {
      pDataBlock->size += extendedRowSize;  // len;
    }

    NEXT_VALID_TOKEN(pCxt->pSql, sToken);
    if (TK_NK_COMMA == sToken.type) {
      return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMNS_NUM);
    } else if (TK_NK_RP != sToken.type) {
      return buildSyntaxErrMsg(&pCxt->msg, ") expected", sToken.z);
    }

    if (gotRow) {
      (*numOfRows)++;
    }
  }

  if (0 == (*numOfRows) && (!TSDB_QUERY_HAS_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT))) {
    return buildSyntaxErrMsg(&pCxt->msg, "no any data points", NULL);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t parseValuesClause(SInsertParseContext* pCxt, STableDataBlocks* dataBuf) {
  int32_t maxNumOfRows;
  CHECK_CODE(allocateMemIfNeed(dataBuf, insGetExtendedRowSize(dataBuf), &maxNumOfRows));

  int32_t numOfRows = 0;
  CHECK_CODE(parseValues(pCxt, dataBuf, maxNumOfRows, &numOfRows));

  SSubmitBlk* pBlocks = (SSubmitBlk*)(dataBuf->pData);
  if (TSDB_CODE_SUCCESS != insSetBlockInfo(pBlocks, dataBuf, numOfRows)) {
    return buildInvalidOperationMsg(&pCxt->msg,
                                    "too many rows in sql, total number of rows should be less than INT32_MAX");
  }

  dataBuf->numOfTables = 1;
  pCxt->totalNum += numOfRows;
  return TSDB_CODE_SUCCESS;
}

static int32_t parseCsvFile(SInsertParseContext* pCxt, TdFilePtr fp, STableDataBlocks* pDataBlock, int maxRows,
                            int32_t* numOfRows) {
  STableComInfo tinfo = getTableInfo(pDataBlock->pTableMeta);
  int32_t       extendedRowSize = insGetExtendedRowSize(pDataBlock);
  CHECK_CODE(
      insInitRowBuilder(&pDataBlock->rowBuilder, pDataBlock->pTableMeta->sversion, &pDataBlock->boundColumnInfo));

  (*numOfRows) = 0;
  char    tmpTokenBuf[TSDB_MAX_BYTES_PER_ROW] = {0};  // used for deleting Escape character: \\, \', \"
  char*   pLine = NULL;
  int64_t readLen = 0;
  while ((readLen = taosGetLineFile(fp, &pLine)) != -1) {
    if (('\r' == pLine[readLen - 1]) || ('\n' == pLine[readLen - 1])) {
      pLine[--readLen] = '\0';
    }

    if (readLen == 0) {
      continue;
    }

    if ((*numOfRows) >= maxRows || pDataBlock->size + extendedRowSize >= pDataBlock->nAllocSize) {
      int32_t tSize;
      CHECK_CODE(allocateMemIfNeed(pDataBlock, extendedRowSize, &tSize));
      ASSERT(tSize >= maxRows);
      maxRows = tSize;
    }

    strtolower(pLine, pLine);
    char* pRawSql = pCxt->pSql;
    pCxt->pSql = pLine;
    bool    gotRow = false;
    int32_t code = parseOneRow(pCxt, pDataBlock, tinfo.precision, &gotRow, tmpTokenBuf);
    if (TSDB_CODE_SUCCESS != code) {
      pCxt->pSql = pRawSql;
      return code;
    }
    if (gotRow) {
      pDataBlock->size += extendedRowSize;  // len;
      (*numOfRows)++;
    }
    pCxt->pSql = pRawSql;

    if (pDataBlock->nAllocSize > tsMaxMemUsedByInsert * 1024 * 1024) {
      break;
    }
  }

  if (0 == (*numOfRows) && (!TSDB_QUERY_HAS_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT))) {
    return buildSyntaxErrMsg(&pCxt->msg, "no any data points", NULL);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t parseDataFromFileAgain(SInsertParseContext* pCxt, int16_t tableNo, const SName* pTableName,
                                      STableDataBlocks* dataBuf) {
  int32_t maxNumOfRows;
  CHECK_CODE(allocateMemIfNeed(dataBuf, insGetExtendedRowSize(dataBuf), &maxNumOfRows));

  int32_t numOfRows = 0;
  CHECK_CODE(parseCsvFile(pCxt, pCxt->pComCxt->csvCxt.fp, dataBuf, maxNumOfRows, &numOfRows));

  SSubmitBlk* pBlocks = (SSubmitBlk*)(dataBuf->pData);
  if (TSDB_CODE_SUCCESS != insSetBlockInfo(pBlocks, dataBuf, numOfRows)) {
    return buildInvalidOperationMsg(&pCxt->msg,
                                    "too many rows in sql, total number of rows should be less than INT32_MAX");
  }

  if (!taosEOFFile(pCxt->pComCxt->csvCxt.fp)) {
    pCxt->pComCxt->needMultiParse = true;
    pCxt->pComCxt->csvCxt.tableNo = tableNo;
    memcpy(&pCxt->pComCxt->csvCxt.tableName, pTableName, sizeof(SName));
    pCxt->pComCxt->csvCxt.pLastSqlPos = pCxt->pSql;
  }

  dataBuf->numOfTables = 1;
  pCxt->totalNum += numOfRows;
  return TSDB_CODE_SUCCESS;
}

static int32_t parseDataFromFile(SInsertParseContext* pCxt, int16_t tableNo, const SName* pTableName, SToken filePath,
                                 STableDataBlocks* dataBuf) {
  char filePathStr[TSDB_FILENAME_LEN] = {0};
  if (TK_NK_STRING == filePath.type) {
    trimString(filePath.z, filePath.n, filePathStr, sizeof(filePathStr));
  } else {
    strncpy(filePathStr, filePath.z, filePath.n);
  }
  pCxt->pComCxt->csvCxt.fp = taosOpenFile(filePathStr, TD_FILE_READ | TD_FILE_STREAM);
  if (NULL == pCxt->pComCxt->csvCxt.fp) {
    return TAOS_SYSTEM_ERROR(errno);
  }

  return parseDataFromFileAgain(pCxt, tableNo, pTableName, dataBuf);
}

static void destroyInsertParseContextForTable(SInsertParseContext* pCxt) {
  if (!pCxt->pComCxt->needMultiParse) {
    taosCloseFile(&pCxt->pComCxt->csvCxt.fp);
  }
  taosMemoryFreeClear(pCxt->pTableMeta);
  destroyBoundColumnInfo(&pCxt->tags);
  tdDestroySVCreateTbReq(&pCxt->createTblReq);
}

static void destroySubTableHashElem(void* p) { taosMemoryFree(*(STableMeta**)p); }

static void destroyInsertParseContext(SInsertParseContext* pCxt) {
  destroyInsertParseContextForTable(pCxt);
  taosHashCleanup(pCxt->pVgroupsHashObj);
  taosHashCleanup(pCxt->pSubTableHashObj);
  taosHashCleanup(pCxt->pTableNameHashObj);
  taosHashCleanup(pCxt->pDbFNameHashObj);

  insDestroyBlockHashmap(pCxt->pTableBlockHashObj);
  insDestroyBlockArrayList(pCxt->pVgDataBlocks);
}

static int32_t parseTableName(SInsertParseContext* pCxt, SToken* pTbnameToken, SName* pName, char* pDbFName,
                              char* pTbFName) {
  int32_t code = insCreateSName(pName, pTbnameToken, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg);
  if (TSDB_CODE_SUCCESS == code) {
    tNameExtractFullName(pName, pTbFName);
    code = taosHashPut(pCxt->pTableNameHashObj, pTbFName, strlen(pTbFName), pName, sizeof(SName));
  }
  if (TSDB_CODE_SUCCESS == code) {
    tNameGetFullDbName(pName, pDbFName);
    code = taosHashPut(pCxt->pDbFNameHashObj, pDbFName, strlen(pDbFName), pDbFName, TSDB_DB_FNAME_LEN);
  }
  return code;
}

//   tb_name
//       [USING stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)]
//       [(field1_name, ...)]
//       VALUES (field1_value, ...) [(field1_value2, ...) ...] | FILE csv_file_path
//   [...];
static int32_t parseInsertBody(SInsertParseContext* pCxt) {
  int32_t tbNum = 0;
  SName   name;
  char    tbFName[TSDB_TABLE_FNAME_LEN];
  char    dbFName[TSDB_DB_FNAME_LEN];
  bool    autoCreateTbl = false;

  // for each table
  while (1) {
    SToken sToken;
    char*  tbName = NULL;

    // pSql -> tb_name ...
    NEXT_TOKEN(pCxt->pSql, sToken);

    // no data in the sql string anymore.
    if (sToken.n == 0) {
      if (sToken.type && pCxt->pSql[0]) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid charactor in SQL", sToken.z);
      }

      if (0 == pCxt->totalNum && (!TSDB_QUERY_HAS_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT)) &&
          !pCxt->pComCxt->needMultiParse) {
        return buildInvalidOperationMsg(&pCxt->msg, "no data in sql");
      }
      break;
    }

    if (TSDB_QUERY_HAS_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT) && tbNum > 0) {
      return buildInvalidOperationMsg(&pCxt->msg, "single table allowed in one stmt");
    }

    destroyInsertParseContextForTable(pCxt);

    if (TK_NK_QUESTION == sToken.type) {
      if (pCxt->pStmtCb) {
        CHECK_CODE((*pCxt->pStmtCb->getTbNameFn)(pCxt->pStmtCb->pStmt, &tbName));

        sToken.z = tbName;
        sToken.n = strlen(tbName);
      } else {
        return buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", sToken.z);
      }
    }

    SToken tbnameToken = sToken;
    NEXT_TOKEN(pCxt->pSql, sToken);

    if (!pCxt->pComCxt->async || TK_USING == sToken.type) {
      CHECK_CODE(parseTableName(pCxt, &tbnameToken, &name, dbFName, tbFName));
    }

    bool existedUsing = false;
    // USING clause
    if (TK_USING == sToken.type) {
      existedUsing = true;
      CHECK_CODE(parseUsingClause(pCxt, tbNum, &name, tbFName));
      NEXT_TOKEN(pCxt->pSql, sToken);
      autoCreateTbl = true;
    }

    char* pBoundColsStart = NULL;
    if (TK_NK_LP == sToken.type) {
      // pSql -> field1_name, ...)
      pBoundColsStart = pCxt->pSql;
      CHECK_CODE(ignoreBoundColumns(pCxt));
      NEXT_TOKEN(pCxt->pSql, sToken);
    }

    if (TK_USING == sToken.type) {
      if (pCxt->pComCxt->async) {
        CHECK_CODE(parseTableName(pCxt, &tbnameToken, &name, dbFName, tbFName));
      }
      CHECK_CODE(parseUsingClause(pCxt, tbNum, &name, tbFName));
      NEXT_TOKEN(pCxt->pSql, sToken);
      autoCreateTbl = true;
    } else if (!existedUsing) {
      CHECK_CODE(getTableMeta(pCxt, tbNum, &name));
      if (TSDB_SUPER_TABLE == pCxt->pTableMeta->tableType) {
        return buildInvalidOperationMsg(&pCxt->msg, "insert data into super table is not supported");
      }
    }

    STableDataBlocks* dataBuf = NULL;
    if (pCxt->pComCxt->async) {
      CHECK_CODE(insGetDataBlockFromList(pCxt->pTableBlockHashObj, &pCxt->pTableMeta->uid,
                                         sizeof(pCxt->pTableMeta->uid), TSDB_DEFAULT_PAYLOAD_SIZE, sizeof(SSubmitBlk),
                                         getTableInfo(pCxt->pTableMeta).rowSize, pCxt->pTableMeta, &dataBuf, NULL,
                                         &pCxt->createTblReq));
    } else {
      CHECK_CODE(insGetDataBlockFromList(pCxt->pTableBlockHashObj, tbFName, strlen(tbFName), TSDB_DEFAULT_PAYLOAD_SIZE,
                                         sizeof(SSubmitBlk), getTableInfo(pCxt->pTableMeta).rowSize, pCxt->pTableMeta,
                                         &dataBuf, NULL, &pCxt->createTblReq));
    }

    if (NULL != pBoundColsStart) {
      char* pCurrPos = pCxt->pSql;
      pCxt->pSql = pBoundColsStart;
      CHECK_CODE(parseBoundColumns(pCxt, &dataBuf->boundColumnInfo, getTableColumnSchema(pCxt->pTableMeta)));
      pCxt->pSql = pCurrPos;
    }

    if (TK_VALUES == sToken.type) {
      // pSql -> (field1_value, ...) [(field1_value2, ...) ...]
      CHECK_CODE(parseValuesClause(pCxt, dataBuf));
      TSDB_QUERY_SET_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_INSERT);

      tbNum++;
      continue;
    }

    // FILE csv_file_path
    if (TK_FILE == sToken.type) {
      // pSql -> csv_file_path
      NEXT_TOKEN(pCxt->pSql, sToken);
      if (0 == sToken.n || (TK_NK_STRING != sToken.type && TK_NK_ID != sToken.type)) {
        return buildSyntaxErrMsg(&pCxt->msg, "file path is required following keyword FILE", sToken.z);
      }
      CHECK_CODE(parseDataFromFile(pCxt, tbNum, &name, sToken, dataBuf));
      pCxt->pOutput->insertType = TSDB_QUERY_TYPE_FILE_INSERT;

      tbNum++;
      if (!pCxt->pComCxt->needMultiParse) {
        continue;
      } else {
        parserDebug("0x%" PRIx64 " insert from csv. File is too large, do it in batches.", pCxt->pComCxt->requestId);
        break;
      }
    }

    return buildSyntaxErrMsg(&pCxt->msg, "keyword VALUES or FILE is expected", sToken.z);
  }

  parserDebug("0x%" PRIx64 " insert input rows: %d", pCxt->pComCxt->requestId, pCxt->totalNum);

  if (TSDB_QUERY_HAS_TYPE(pCxt->pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT)) {
    SParsedDataColInfo* tags = taosMemoryMalloc(sizeof(pCxt->tags));
    if (NULL == tags) {
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }
    memcpy(tags, &pCxt->tags, sizeof(pCxt->tags));
    (*pCxt->pStmtCb->setInfoFn)(pCxt->pStmtCb->pStmt, pCxt->pTableMeta, tags, tbFName, autoCreateTbl,
                                pCxt->pVgroupsHashObj, pCxt->pTableBlockHashObj, pCxt->sTableName);

    memset(&pCxt->tags, 0, sizeof(pCxt->tags));
    pCxt->pVgroupsHashObj = NULL;
    pCxt->pTableBlockHashObj = NULL;

    return TSDB_CODE_SUCCESS;
  }

  // merge according to vgId
  if (taosHashGetSize(pCxt->pTableBlockHashObj) > 0) {
    CHECK_CODE(insMergeTableDataBlocks(pCxt->pTableBlockHashObj, &pCxt->pVgDataBlocks));
  }
  return insBuildOutput(pCxt);
}

static int32_t parseInsertBodyAgain(SInsertParseContext* pCxt) {
  STableDataBlocks* dataBuf = NULL;
  CHECK_CODE(getTableMeta(pCxt, pCxt->pComCxt->csvCxt.tableNo, &pCxt->pComCxt->csvCxt.tableName));
  CHECK_CODE(insGetDataBlockFromList(pCxt->pTableBlockHashObj, &pCxt->pTableMeta->uid, sizeof(pCxt->pTableMeta->uid),
                                     TSDB_DEFAULT_PAYLOAD_SIZE, sizeof(SSubmitBlk),
                                     getTableInfo(pCxt->pTableMeta).rowSize, pCxt->pTableMeta, &dataBuf, NULL,
                                     &pCxt->createTblReq));
  CHECK_CODE(parseDataFromFileAgain(pCxt, pCxt->pComCxt->csvCxt.tableNo, &pCxt->pComCxt->csvCxt.tableName, dataBuf));
  if (taosEOFFile(pCxt->pComCxt->csvCxt.fp)) {
    CHECK_CODE(parseInsertBody(pCxt));
    pCxt->pComCxt->needMultiParse = false;
    return TSDB_CODE_SUCCESS;
  }
  parserDebug("0x%" PRIx64 " insert again input rows: %d", pCxt->pComCxt->requestId, pCxt->totalNum);
  // merge according to vgId
  if (taosHashGetSize(pCxt->pTableBlockHashObj) > 0) {
    CHECK_CODE(insMergeTableDataBlocks(pCxt->pTableBlockHashObj, &pCxt->pVgDataBlocks));
  }
  return insBuildOutput(pCxt);
}

// INSERT INTO
//   tb_name
//       [USING stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)]
//       [(field1_name, ...)]
//       VALUES (field1_value, ...) [(field1_value2, ...) ...] | FILE csv_file_path
//   [...];
int32_t parseInsertSql(SParseContext* pContext, SQuery** pQuery, SParseMetaCache* pMetaCache) {
  SInsertParseContext context = {
      .pComCxt = pContext,
      .pSql = pContext->needMultiParse ? (char*)pContext->csvCxt.pLastSqlPos : (char*)pContext->pSql,
      .msg = {.buf = pContext->pMsg, .len = pContext->msgLen},
      .pTableMeta = NULL,
      .createTblReq = {0},
      .pSubTableHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK),
      .pTableNameHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK),
      .pDbFNameHashObj = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK),
      .totalNum = 0,
      .pOutput = (SVnodeModifOpStmt*)nodesMakeNode(QUERY_NODE_VNODE_MODIF_STMT),
      .pStmtCb = pContext->pStmtCb,
      .pMetaCache = pMetaCache,
      .memElapsed = 0,
      .parRowElapsed = 0};

  if (pContext->pStmtCb && *pQuery) {
    (*pContext->pStmtCb->getExecInfoFn)(pContext->pStmtCb->pStmt, &context.pVgroupsHashObj,
                                        &context.pTableBlockHashObj);
    if (NULL == context.pVgroupsHashObj) {
      context.pVgroupsHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_NO_LOCK);
    }
    if (NULL == context.pTableBlockHashObj) {
      context.pTableBlockHashObj =
          taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_NO_LOCK);
    }
  } else {
    context.pVgroupsHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_NO_LOCK);
    context.pTableBlockHashObj =
        taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_NO_LOCK);
  }

  if (NULL == context.pVgroupsHashObj || NULL == context.pTableBlockHashObj || NULL == context.pSubTableHashObj ||
      NULL == context.pTableNameHashObj || NULL == context.pDbFNameHashObj || NULL == context.pOutput) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }
  taosHashSetFreeFp(context.pSubTableHashObj, destroySubTableHashElem);

  if (pContext->pStmtCb) {
    TSDB_QUERY_SET_TYPE(context.pOutput->insertType, TSDB_QUERY_TYPE_STMT_INSERT);
  }

  if (NULL == *pQuery) {
    *pQuery = (SQuery*)nodesMakeNode(QUERY_NODE_QUERY);
    if (NULL == *pQuery) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  } else {
    nodesDestroyNode((*pQuery)->pRoot);
  }

  (*pQuery)->execMode = QUERY_EXEC_MODE_SCHEDULE;
  (*pQuery)->haveResultSet = false;
  (*pQuery)->msgType = TDMT_VND_SUBMIT;
  (*pQuery)->pRoot = (SNode*)context.pOutput;

  if (NULL == (*pQuery)->pTableList) {
    (*pQuery)->pTableList = taosArrayInit(taosHashGetSize(context.pTableNameHashObj), sizeof(SName));
    if (NULL == (*pQuery)->pTableList) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  if (NULL == (*pQuery)->pDbList) {
    (*pQuery)->pDbList = taosArrayInit(taosHashGetSize(context.pDbFNameHashObj), TSDB_DB_FNAME_LEN);
    if (NULL == (*pQuery)->pDbList) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  int32_t code = TSDB_CODE_SUCCESS;
  if (!context.pComCxt->needMultiParse) {
    code = skipInsertInto(&context.pSql, &context.msg);
    if (TSDB_CODE_SUCCESS == code) {
      code = parseInsertBody(&context);
    }
  } else {
    code = parseInsertBodyAgain(&context);
  }

  if (TSDB_CODE_SUCCESS == code || NEED_CLIENT_HANDLE_ERROR(code)) {
    SName* pTable = taosHashIterate(context.pTableNameHashObj, NULL);
    while (NULL != pTable) {
      taosArrayPush((*pQuery)->pTableList, pTable);
      pTable = taosHashIterate(context.pTableNameHashObj, pTable);
    }

    char* pDb = taosHashIterate(context.pDbFNameHashObj, NULL);
    while (NULL != pDb) {
      taosArrayPush((*pQuery)->pDbList, pDb);
      pDb = taosHashIterate(context.pDbFNameHashObj, pDb);
    }
  }
  if (pContext->pStmtCb) {
    context.pVgroupsHashObj = NULL;
    context.pTableBlockHashObj = NULL;
  }
  destroyInsertParseContext(&context);
  return code;
}

// pSql -> (field1_value, ...) [(field1_value2, ...) ...]
static int32_t skipValuesClause(SInsertParseSyntaxCxt* pCxt) {
  int32_t numOfRows = 0;
  SToken  sToken;
  while (1) {
    int32_t index = 0;
    NEXT_TOKEN_KEEP_SQL(pCxt->pSql, sToken, index);
    if (TK_NK_LP != sToken.type) {
      break;
    }
    pCxt->pSql += index;

    CHECK_CODE(skipParentheses(pCxt));
    ++numOfRows;
  }
  if (0 == numOfRows) {
    return buildSyntaxErrMsg(&pCxt->msg, "no any data points", NULL);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t skipTagsClause(SInsertParseSyntaxCxt* pCxt) { return skipParentheses(pCxt); }

static int32_t skipTableOptions(SInsertParseSyntaxCxt* pCxt) {
  do {
    int32_t index = 0;
    SToken  sToken;
    NEXT_TOKEN_KEEP_SQL(pCxt->pSql, sToken, index);
    if (TK_TTL == sToken.type || TK_COMMENT == sToken.type) {
      pCxt->pSql += index;
      NEXT_TOKEN_WITH_PREV(pCxt->pSql, sToken);
    } else {
      break;
    }
  } while (1);
  return TSDB_CODE_SUCCESS;
}

// pSql -> [(tag1_name, ...)] TAGS (tag1_value, ...)
static int32_t skipUsingClause(SInsertParseSyntaxCxt* pCxt) {
  SToken sToken;
  NEXT_TOKEN(pCxt->pSql, sToken);
  if (TK_NK_LP == sToken.type) {
    CHECK_CODE(skipBoundColumns(pCxt));
    NEXT_TOKEN(pCxt->pSql, sToken);
  }

  if (TK_TAGS != sToken.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "TAGS is expected", sToken.z);
  }
  // pSql -> (tag1_value, ...)
  NEXT_TOKEN(pCxt->pSql, sToken);
  if (TK_NK_LP != sToken.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "( is expected", sToken.z);
  }
  CHECK_CODE(skipTagsClause(pCxt));
  CHECK_CODE(skipTableOptions(pCxt));

  return TSDB_CODE_SUCCESS;
}

static int32_t collectTableMetaKey(SInsertParseSyntaxCxt* pCxt, bool isStable, int32_t tableNo, SToken* pTbToken) {
  SName name = {0};
  CHECK_CODE(insCreateSName(&name, pTbToken, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg));
  CHECK_CODE(reserveTableMetaInCacheForInsert(&name, isStable ? CATALOG_REQ_TYPE_META : CATALOG_REQ_TYPE_BOTH, tableNo,
                                              pCxt->pMetaCache));
  return TSDB_CODE_SUCCESS;
}

static int32_t checkTableName(const char* pTableName, SMsgBuf* pMsgBuf) {
  if (NULL != strchr(pTableName, '.')) {
    return generateSyntaxErrMsgExt(pMsgBuf, TSDB_CODE_PAR_INVALID_IDENTIFIER_NAME, "The table name cannot contain '.'");
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t collectAutoCreateTableMetaKey(SInsertParseSyntaxCxt* pCxt, int32_t tableNo, SToken* pTbToken) {
  SName name = {0};
  CHECK_CODE(insCreateSName(&name, pTbToken, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg));
  CHECK_CODE(checkTableName(name.tname, &pCxt->msg));
  CHECK_CODE(reserveTableMetaInCacheForInsert(&name, CATALOG_REQ_TYPE_VGROUP, tableNo, pCxt->pMetaCache));
  return TSDB_CODE_SUCCESS;
}

static int32_t parseInsertBodySyntax(SInsertParseSyntaxCxt* pCxt) {
  bool    hasData = false;
  int32_t tableNo = 0;
  // for each table
  while (1) {
    SToken sToken;

    // pSql -> tb_name ...
    NEXT_TOKEN(pCxt->pSql, sToken);

    // no data in the sql string anymore.
    if (sToken.n == 0) {
      if (sToken.type && pCxt->pSql[0]) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid charactor in SQL", sToken.z);
      }

      if (!hasData) {
        return buildInvalidOperationMsg(&pCxt->msg, "no data in sql");
      }
      break;
    }

    hasData = false;

    SToken tbnameToken = sToken;
    NEXT_TOKEN(pCxt->pSql, sToken);

    bool existedUsing = false;
    // USING clause
    if (TK_USING == sToken.type) {
      existedUsing = true;
      CHECK_CODE(collectAutoCreateTableMetaKey(pCxt, tableNo, &tbnameToken));
      NEXT_TOKEN(pCxt->pSql, sToken);
      CHECK_CODE(collectTableMetaKey(pCxt, true, tableNo, &sToken));
      CHECK_CODE(skipUsingClause(pCxt));
      NEXT_TOKEN(pCxt->pSql, sToken);
    }

    if (TK_NK_LP == sToken.type) {
      // pSql -> field1_name, ...)
      CHECK_CODE(skipBoundColumns(pCxt));
      NEXT_TOKEN(pCxt->pSql, sToken);
    }

    if (TK_USING == sToken.type && !existedUsing) {
      existedUsing = true;
      CHECK_CODE(collectAutoCreateTableMetaKey(pCxt, tableNo, &tbnameToken));
      NEXT_TOKEN(pCxt->pSql, sToken);
      CHECK_CODE(collectTableMetaKey(pCxt, true, tableNo, &sToken));
      CHECK_CODE(skipUsingClause(pCxt));
      NEXT_TOKEN(pCxt->pSql, sToken);
    } else if (!existedUsing) {
      CHECK_CODE(collectTableMetaKey(pCxt, false, tableNo, &tbnameToken));
    }

    ++tableNo;

    if (TK_VALUES == sToken.type) {
      // pSql -> (field1_value, ...) [(field1_value2, ...) ...]
      CHECK_CODE(skipValuesClause(pCxt));
      hasData = true;
      continue;
    }

    // FILE csv_file_path
    if (TK_FILE == sToken.type) {
      // pSql -> csv_file_path
      NEXT_TOKEN(pCxt->pSql, sToken);
      if (0 == sToken.n || (TK_NK_STRING != sToken.type && TK_NK_ID != sToken.type)) {
        return buildSyntaxErrMsg(&pCxt->msg, "file path is required following keyword FILE", sToken.z);
      }
      hasData = true;
      continue;
    }

    return buildSyntaxErrMsg(&pCxt->msg, "keyword VALUES or FILE is expected", sToken.z);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t parseInsertSyntax(SParseContext* pContext, SQuery** pQuery, SParseMetaCache* pMetaCache) {
  SInsertParseSyntaxCxt context = {.pComCxt = pContext,
                                   .pSql = (char*)pContext->pSql,
                                   .msg = {.buf = pContext->pMsg, .len = pContext->msgLen},
                                   .pMetaCache = pMetaCache};
  int32_t               code = skipInsertInto(&context.pSql, &context.msg);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseInsertBodySyntax(&context);
  }
  if (TSDB_CODE_SUCCESS == code) {
    *pQuery = (SQuery*)nodesMakeNode(QUERY_NODE_QUERY);
    if (NULL == *pQuery) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }
  return code;
}
