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

#ifndef __TRIE__
#define __TRIE__

// 
// The prefix search tree is a efficient storage words and search words tree, it support 95 visible ascii code character
//
#define FIRST_ASCII 32   // first visiable char is space
#define LAST_ASCII  126  // last visilbe char is '~'

// capacity save char is 95
#define CHAR_CNT   (LAST_ASCII - FIRST_ASCII + 1)
#define MAX_WORD_LEN 256 // max insert word length

#define PTR_END  (STireNode* )(-1)

typedef struct STireNode {
    struct STireNode* d[CHAR_CNT];
}STireNode;

typedef struct STire {
    STireNode root;
    int count;      // all count 
    int ref;
}STire;

typedef struct SMatchNode {
    char word[MAX_WORD_LEN];
    struct SMatchNode* next;
}SMatchNode;


typedef struct SMatch {
    SMatchNode* head;
    SMatchNode* tail;  // append node to tail
    int count;
}SMatch;


// ----------- interface -------------

// create prefix search tree, return value call freeTrie to free 
STire* createTrie();

// destroy prefix search tree
void freeTrie(STire* tire);

// add a new word 
bool insertWord(STire* tire, char* word);

// match prefix words
SMatch* matchPrefix(STire* tire, char* prefix);

// free match result
void freeMatch(SMatch* match);

#endif
