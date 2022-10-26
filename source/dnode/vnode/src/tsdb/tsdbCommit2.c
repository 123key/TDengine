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

#include "tsdb.h"

// FLUSH MEMTABLE TO FILE SYSTEM ===================================
typedef struct {
  // data
} STsdbCommitter;

int32_t tsdbFlushMemTable(STsdb *pTsdb) {
  int32_t code = 0;
  int32_t lino = 0;
  // TODO
_exit:
  return code;
}

// MERGE MULTIPLE STT ===================================
typedef struct {
  // data
} STsdbMerger;

int32_t tsdbMerge(STsdb *pTsdb) {
  int32_t code = 0;
  int32_t lino = 0;
  // TODO
_exit:
  tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  return code;
}

// TRANSACTION CONTROL ===================================