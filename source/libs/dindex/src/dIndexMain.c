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


#include "os.h"
#include "taoserror.h"

// write block into file
int32_t writeBlock(void* pData, int16_t type, int16_t bytes, int32_t colId, int32_t rows, int32_t tid, int32_t blockid, int32_t fileid, char* indexName) {

    int32_t hash = 0;
    // write into idx file
    char * p = (char*)pData;
    for(int i=0; i<rows; i++) {
        p += i * bytes;
        // write hash
    }

    // write body
}
