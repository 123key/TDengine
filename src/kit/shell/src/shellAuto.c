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

#define __USE_XOPEN
#include "os.h"
#include "tglobal.h"
#include "shell.h"
#include "shellCommand.h"
#include "tkey.h"
#include "tulog.h"
#include "shellAuto.h"
#include "tire.h"
#include "tthread.h"

// extern function
void insertChar(Command *cmd, char *c, int size);


typedef struct SAutoPtr {
  STire* p;
  int ref;
}SAutoPtr;

typedef struct SWord{
  int type ; // word type , see WT_ define
  const char * word;
  int32_t len;
  struct SWord * next;
}SWord;

typedef struct {
  const char * source;
  int32_t source_len; // valid data length in source
  int32_t count;
  SWord*  head;
  // matched information
  int32_t matchIndex;    // matched word index in words
  int32_t matchLen;     // matched length at matched word
}SWords;


SWords shellCommands[] = {
  {"alter database <dbname> [blocks] [cachelast] [comp] [keep] [replica] [quorum]", 0, 0, NULL},
  {"alter dnote <dnodeid> balance column", 0, 0, NULL},
  {"alter table <tbname> [add column|modify column|drop column|change tag]", 0, 0, NULL},
  {"alter table modify column", 0, 0, NULL},
  {"alter topic", 0, 0, NULL},
  {"alter user <username> pass", 0, 0, NULL},
  {"alter user <username> privilege [read|write]", 0, 0, NULL},
  {"create dnode", 0, 0, NULL},
  {"create function", 0, 0, NULL},
  {"create table <...> tags", 0, 0, NULL},
  {"create table <...> as", 0, 0, NULL},
  {"create topic", 0, 0, NULL},
  {"create user <...> pass", 0, 0, NULL},
  {"compact vnode in", 0, 0, NULL},
  {"describe", 0, 0, NULL},
#ifdef TD_ENTERPRISE
  {"delete from <tbname> where", 0, 0, NULL},
#endif
  {"drop database <dbname>", 0, 0, NULL},
  {"drop dnode <dnodeid>", 0, 0, NULL},
  {"drop function", 0, 0, NULL},
  {"drop topic", 0, 0, NULL},
  {"drop table <tbname>", 0, 0, NULL},
  {"drop user <username>", 0, 0, NULL},
  {"kill connection", 0, 0, NULL},
  {"kill query", 0, 0, NULL},
  {"kill stream", 0, 0, NULL},
  {"set max_binary_display_width", 0, 0, NULL},
  {"select <expr> from <tbname> where <keyword> [group by] [order by] [asc|desc] [limit] [offset] [slimit] [soffset] [interval] [sliding] [fill] [session] [state]", 0, 0, NULL},
  {"select <expr> union all select <expr>", 0, 0, NULL},
  {"select _block_dst() from <tbname>", 0, 0, NULL},
  {"select client_version() from <tbname>", 0, 0, NULL},
  {"select current_user();", 0, 0, NULL},
  {"select database;", 0, 0, NULL},
  {"select database;", 0, 0, NULL},
  {"select server_version();", 0, 0, NULL},
  {"show create database <dbname>", 0, 0, NULL},
  {"show create stable <stbname>", 0, 0, NULL},
  {"show create table <tbname>", 0, 0, NULL},
  {"show connections;", 0, 0, NULL},
  {"show databases;", 0, 0, NULL},
  {"show dnodes;", 0, 0, NULL},
  {"show functions;", 0, 0, NULL},
  {"show modules;", 0, 0, NULL},
  {"show mondes;", 0, 0, NULL},
  {"show queries;", 0, 0, NULL},
  {"show stables;", 0, 0, NULL},
  {"show stables like", 0, 0, NULL},
  {"show streams;", 0, 0, NULL},
  {"show scores;", 0, 0, NULL},
  {"show tables;", 0, 0, NULL},
  {"show tables like", 0, 0, NULL},
  {"show users;", 0, 0, NULL},
  {"show variables;", 0, 0, NULL},
  {"show vgroups;", 0, 0, NULL},
  {"insert into <tbname> values", 0, 0, NULL},
  {"use <dbname>", 0, 0, NULL}
};

//
//  ------- gobal variant define ---------
//
int32_t firstMatchIndex = -1; // first match shellCommands index
int32_t lastMatchIndex  = -1; // last match shellCommands index
int32_t curMatchIndex   = -1; // current match shellCommands index



//
//   ----------- global var array define -----------
//
#define WT_VAR_DBNAME 0
#define WT_VAR_STABLE 1
#define WT_VAR_TABLE  2
#define WT_VAR_CNT    3

#define WT_TEXT       0xFF

// tire array
STire* tires[WT_VAR_CNT];
pthread_mutex_t tiresMutex;
//save thread handle obtain var name from db server
pthread_t* threads[WT_VAR_CNT];
// obtain var name  with sql from server
char varSqls[WT_VAR_CNT][64] = {
  "show databases;",
  "show stables;",
  "show table;"
};

// var words current cursor, if user press any one key except tab, cursorVar can be reset to -1
int cursorVar = -1;

// define buffer for SWord->word for variant name used
char varName[1024] = "";



//
//  -------------------  parse words --------------------------
//

#define SHELL_COMMAND_COUNT() (sizeof(shellCommands) / sizeof(SWords))

// get at
SWord * atWord(SWords * command, int32_t index) {
  SWord * word = command->head;
  for (int32_t i = 0; i < index; i++) {
    if (word == NULL)
      return NULL;
    word = word->next;
  }

  return word;
}

#define MATCH_WORD(x) atWord(x, x->matchIndex)

int wordType(const char* p, int32_t len) {
  if (strncmp(p, "<db_name>") == 0)
     return WT_VAR_DBNAME;
  else if (strncmp(p, "<stb_name>") == 0) 
     return WT_VAR_STABLE;
  else if (strncmp(p, "<tb_name>") == 0) 
     return WT_VAR_TABLE;
  else 
     return WT_TEXT;
}

// add word
SWord * addWord(const char* p, int32_t len, bool pattern) {
  SWord* word = (SWord *) malloc(sizeof(SWord));
  memset(word, 0, sizeof(SWord));
  word->word = p;
  word->len  = len;

  // check format
  if (pattern) {
    word->type = wordType(p, len);
  } else {
    word->type = WT_TEXT;
  }
     
  
  return word;
}

// parse one command
void parseCommand(SWords * command, bool pattern) {
  const char * p = command->source;
  int32_t start = 0;
  int32_t size  = command->source_len > 0 ? command->source_len : strlen(p);

  bool lastBlank = false;
  for (int i = 0; i <= size; i++) {
    if (p[i] == ' ' || i == size) {
      // check continue blank like '    '
      if(p[i] == ' ') {
        if (lastBlank) {
          start ++;
          continue;
        }
        if (i == 0) { // first blank
          lastBlank = true;
          start ++;
          continue;
        }
        lastBlank = true;
      } 

      // found split or string end , append word
      if (command->head == NULL) {
        command->head = addWord(p + start, i - start, pattern);
        command->count = 1;
      } else {
        SWord * word = command->head;
        while(word->next) {
          word = word->next;
        }
        word->next = addWord(p + start, i - start, pattern);
        command->count ++;
      }
      start = i + 1;
    } else {
      lastBlank = false;
    }
  }
}

// free Command
void freeCommand(SWords * command) {
  SWord * word = command->head;
  if (word == NULL) {
    return ;
  }

  // loop 
  while (word->next) {
    SWord * tmp = word;
    word = word->next;
    free(tmp);
  }
  free(word);
}

//
//  -------------------- shell auto ----------------
//

// init shell auto funciton , shell start call once 
bool shellAutoInit() {
  // command
  int32_t count = SHELL_COMMAND_COUNT();
  for (int32_t i = 0; i < count; i ++) {
    parseCommand(shellCommands + i, true);
  }

  // tires
  memset(tires, 0, sizeof(STire*) * WT_VAR_CNT);
  pthread_mutex_init(&tiresMutex, NULL);

  // threads
  memset(threads, 0, sizeof(pthread_t*) * WT_VAR_CNT);

  return true;
}

// exit shell auto funciton, shell exit call once
void shellAutoExit() {
  // free command
  int32_t count = SHELL_COMMAND_COUNT();
  for (int32_t i = 0; i < count; i ++) {
    freeCommand(shellCommands + i);
  }

  // free tires
  pthread_mutex_lock(&tiresMutex);
  for(int32_t i = 0; i < WT_VAR_CNT; i++) {
    if (tires[i]) {
      freeTrie(tires[i]);
      tires[i] = NULL;
    } 
  }
  pthread_mutex_unlock(&tiresMutex);
  // destory
  pthread_mutex_destroy(&tiresMutex);

  // free threads
  for(int32_t i = 0; i < WT_VAR_CNT; i++) {
    if (threads[i]) {
      taosDestoryThread(threads[i]);
      threads[i] = NULL;
    } 
  }
}

//
//  -------------------  auto ptr for tires --------------------------
//
bool setNewAuotPtr(int type, STire* pNew) {
  if (pNew == NULL)
    return false;

  pthread_mutex_lock(&tiresMutex);
  STire* pOld = tires[type];
  if (pOld != NULL) {
    // previous have value, release self ref count
    if(--pOld->ref == 0) {
      freeTrie(pOld);
    }
  }

  // set new
  tires[type] = pNew;
  tires[type]->ref = 1;
  pthread_mutex_unlock(&tiresMutex);

  return true;
}

// get ptr
STire* getAutoPtr(int type) {
  if(tires[type] == NULL) {
    return NULL;
  }

  pthread_mutex_lock(&tiresMutex);
  tires[type]->ref++;
  pthread_mutex_unlock(&tiresMutex);

  return tires[type];
}

// put back tire to tires[type], if tire not equal tires[type].p, need free tire
void putBackAutoPtr(int type, STire* tire) {
  if(tire == NULL) {
    return false;
  }
  bool needFree = false;

  pthread_mutex_lock(&tiresMutex);
  if(tires[type] != tire) {
    //update by out,  can't put back , so free
    if (--tire->ref == 1) {
      // support multi thread getAuotPtr
      freeTrie(tire);
    }
    
  } else {
    tires[type]->ref--;
    assert(tires[type]->ref > 0);
  }
  pthread_mutex_unlock(&tiresMutex);

  if (needFree) {
    freeTrie(tire);
  }
  return ;
}



//
//  -------------------  var Word --------------------------
//

void varObtainThread(void* param)   {

}

// only match next one word from all match words
bool matchNextPrefix(STire* tire, char* pre, char* output) {
  bool ret = false;
  SMatch* match = matchPrefix(tire, pre);
  if (match == NULL || match->head == NULL) {
    // no one matched
    return false;
  }

  if(cursorVar == -1) {
    // first
    strcpy(output, match->head->word);
    cursorVar = 0;
    freeMatch(match);
    return true;    
  }

  // according to cursorVar , calculate next one
  int i = 0;
  SMatchNode* item = match->head;
  while (item) {
    if(i == cursorVar + 1) {
      // found next position ok
      strcpy(output, item->word);
      ret = true;
      if(item->next == NULL) {
        // match last item, reset cursorVar to head
        cursorVar = -1;
      } else {
        cursorVar = i;
      }

      break;
    }

    // check end item
    if(item->next == NULL) {
      // if cursorVar > var list count, return last and reset cursorVar
      strcpy(output, item->word);
      ret = true;
      cursorVar = -1;
    }

    // move next
    item = item->next;
    i++;
  }
  freeMatch(match);

  return ret;
}

// search pre word from tire tree, return value is global buffer,so need not free
char* tireSearchWord(int type, char* pre) {
  if (type == WT_TEXT) {
    return NULL;
  }

  pthread_mutex_lock(&tiresMutex);

  // check need obtain from server
  if (tires[type] == NULL) {
    // need async obtain var names from db sever
    if(threads[type] != NULL) {
      if(taosThreadRunning(threads[type])) {
        // thread running , need not obtain again, return 
        pthread_mutex_unlock(&tiresMutex);
        return NULL;
      }
      // destroy previous thread handle for new create thread handle
      taosDestoryThread(threads[type]);
      threads[type] = NULL;
    }
  
    // create new
    threads[type] = taosCreateThread(varObtainThread, varSqls[type]);
    pthread_mutex_unlock(&tiresMutex);
    return NULL;
  }
  pthread_mutex_unlock(&tiresMutex);

  // can obtain var names from local
  STire* tire = getAutoPtr(type);
  if (tire == NULL) {
    return NULL;
  }

  // auto tab function is single thread operate, so here set global varName is appropriate
  bool ret = matchNextPrefix(tire, pre, varName);
  // used finish, put back pointer to autoptr array
  putBackAutoPtr(type, tire);

  if (ret) {
    return varName;
  }

  return NULL;
}

// match var word, word1 is pattern , word2 is input from shell 
bool matchVarWord(SWord* word1, SWord* word2) {
  // search input word from tire tree 
  char* word = tireSearchWord(word1->type, word2->word);
  if (word == NULL) {
    // not found or word1->type variable list not obtain from server, return not match
    return false;
  }

  // save
  word1->word = word;
  word1->len  = strlen(word);
  
  return true;
}

//
//  -------------------  match words --------------------------
//


// compare command cmd1 come from shellCommands , cmd2 come from user input
int32_t compareCommand(SWords * cmd1, SWords * cmd2) {
  SWord * word1 = cmd1->head;
  SWord * word2 = cmd2->head;

  if(word1 == NULL || word2 == NULL) {
    return -1;
  }

  for (int32_t i = 0; i < cmd1->count; i++) {
    if(word1->type == WT_TEXT) {
      // WT_TEXT match
      if(word1->len == word2->len) {
        if (strncasecmp(word1->word, word2->word, word1->len) != 0)
          return -1; 
      } else if (word1->len < word2->len) {
        return -1;
      } else {
        // word1->len > word2->len
        if (strncasecmp(word1->word, word2->word, word2->len) == 0) {
          cmd1->matchIndex = i;
          cmd1->matchLen = word2->len;
          return i;
        } else {
          return -1;
        }
      }
    } else {
      // WT_VAR auto match any one word
      if (word2->next == NULL) { // input words last one
        if (matchVarWord(word1, word2)) {
          return i;
        }
        return -1;
      }
    }

    // move next
    word1 = word1->next;
    word2 = word2->next;
    if(word1 == NULL || word2 == NULL) {
      return -1;
    }
  }

  return -1;
}

// match command
SWords * matchCommand(SWords * input, bool checkLast) {
  int32_t count = SHELL_COMMAND_COUNT();
  for (int32_t i = 0; i < count; i ++) {
    SWords * shellCommand = shellCommands + i;
    if (checkLast && lastMatchIndex != -1 && i <= lastMatchIndex) {
      // new match must greate than lastMatchIndex
      continue;
    }

    // command is large
    if(input->count > shellCommand->count ) {
      continue;
    }

    // compare
    int32_t index = compareCommand(shellCommand, input);
    if (index != -1) {
      if (firstMatchIndex == -1)
        firstMatchIndex = i;
      curMatchIndex = i;
      return &shellCommands[i];
    }
  }

  // not match
  return NULL;
}

//
//  -------------------  print screen --------------------------
//


// show screen
void printScreen(TAOS * con, Command * cmd, SWords * match) {
  // modify Command
  if (firstMatchIndex == -1 || curMatchIndex == -1) {
    // no match
    return ;
  }

  // first tab press 
  const char * str = NULL;
  int strLen = 0; 

  if (firstMatchIndex == curMatchIndex && lastMatchIndex == -1) {
    // first press tab
    SWord * word = MATCH_WORD(match);
    str = word->word + match->matchLen;
    strLen = word->len - match->matchLen;
    lastMatchIndex = firstMatchIndex;
  } else {
    if (lastMatchIndex == -1)
      return ;
    // continue to press tab any times
    SWords * last = &shellCommands[lastMatchIndex];
    int count = MATCH_WORD(last)->len;

    // delete last match word
    int size = 0;
    int width = 0;
    clearScreen(cmd->endOffset + prompt_size, cmd->screenOffset + prompt_size);
    while(--count >= 0 && cmd->cursorOffset > 0) {
      getPrevCharSize(cmd->command, cmd->cursorOffset, &size, &width);
      memmove(cmd->command + cmd->cursorOffset - size, cmd->command + cmd->cursorOffset,
              cmd->commandSize - cmd->cursorOffset);
      cmd->commandSize -= size;
      cmd->cursorOffset -= size;
      cmd->screenOffset -= width;
      cmd->endOffset -= width;
    }

   SWord * word = MATCH_WORD(match);
    str = word->word;
    strLen = word->len;
    // set current to last
    lastMatchIndex = curMatchIndex;
  }
  
  // insert new
  insertChar(cmd, (char *)str, strLen);
}

// show help
void showHelp(TAOS * con, Command * cmd) {
  
}

void searchWord(char* pre) {
  STire* tire  = createTrie();

  insertWord(tire, "a");
  insertWord(tire, "b");
  insertWord(tire, "c");

  insertWord(tire, "box");
  insertWord(tire, "boxa");
  insertWord(tire, "boxb");

  insertWord(tire, "hello");
  insertWord(tire, "hello world");
  insertWord(tire, "hello me");

  insertWord(tire, "showstring");

  printf(" insert tire count=%d\n", tire->count);

  SMatch* match = matchPrefix(tire, pre);

  if(match){
    printf(" match pre=%s  matched string count=%d.\n", pre, match->count);
    SMatchNode* node = match->head;
    int i=0;
    while(node) {
      printf(" i=%d word=%s \n", ++i, node->word);
      node = node->next;
    }

    freeMatch(match);
    match = NULL;
  } else{
    printf(" match pre=%s not matched.\n", pre);
  }

  freeTrie(tire);
}

// main key press tab
void firstMatchCommand(TAOS * con, Command * cmd) {
  // parse command
  SWords* input = (SWords *)malloc(sizeof(SWords));
  memset(input, 0, sizeof(SWords));
  input->source = cmd->command;
  input->source_len = cmd->commandSize;
  parseCommand(input, false);

  // if have many , default match first, if press tab again , switch to next
  curMatchIndex  = -1;
  lastMatchIndex = -1;
  SWords * match = matchCommand(input, true);
  if (match == NULL) {
    // not match , nothing to do
    freeCommand(input);
    return ;
  }

  // print to screen
  printScreen(con, cmd, match);
  freeCommand(input);
}

// next Match Command
void nextMatchCommand(TAOS * con, Command * cmd, SWords * firstMatch) {
  SWords* input = (SWords *)malloc(sizeof(SWords));
  memset(input, 0, sizeof(SWords));

  // set source and source_len
  input->source = firstMatch->source;
  SWord * word = firstMatch->head;
  if (word == NULL)
    return ;
  for (int i = 0; i < firstMatch->matchIndex && word; i++) {
    input->source_len += word->len + 1; // 1 is blank
    word = word->next;
  }
  input->source_len += firstMatch->matchLen;
  
  parseCommand(input, false);

  // if have many , default match first, if press tab again , switch to next
  SWords * match = matchCommand(input, true);
  if (match == NULL) {
    // if not match , reset all index
    firstMatchIndex = -1;
    curMatchIndex   = -1;
    match = matchCommand(input, false);
    if(match == NULL) {
      freeCommand(input);
      return ;
    }
  }

  // print to screen
  printScreen(con, cmd, match);
  freeCommand(input);
}


// main key press tab
void pressTabKey(TAOS * con, Command * cmd) {
  // check 
  if(cmd->commandSize == 0) { 
    // empty
    showHelp(con, cmd);
    return ;
  } 

/*
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  strncpy(buf, cmd->command, cmd->commandSize);
  searchWord(buf);
  return ;
*/ 

  if (firstMatchIndex == -1) {
    firstMatchCommand(con, cmd);
  } else {
    nextMatchCommand(con, cmd, &shellCommands[firstMatchIndex]);
  }
}

// press othr key
void pressOtherKey(char c) {
  // reset global variant
  firstMatchIndex = -1;
  lastMatchIndex  = -1;
  curMatchIndex   = -1;
}
