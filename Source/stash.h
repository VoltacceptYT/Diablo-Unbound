#ifndef STASH_H
#define STASH_H

#include "diablo.h"

#define NUM_STASH_PAGES 100
#define STASH_GRID_SIZE 10
#define STASH_MAX_ITEMS 10000  // maximum number of items in stashList

// Stash button indices
#define STASH_BTN_PREV_10 0
#define STASH_BTN_PREV_1  1
#define STASH_BTN_WITHDRAW 2
#define STASH_BTN_NEXT_1  3
#define STASH_BTN_NEXT_10 4
#define NUM_STASH_BUTTONS 5

extern BOOL stashflag;
extern BOOL isWithdrawGoldOpen;
extern int pcurstashitem;

typedef struct StashStruct {
    int page;
    int gold;
    BOOL dirty;
    int stashGrid[NUM_STASH_PAGES][STASH_GRID_SIZE][STASH_GRID_SIZE];
    ItemStruct stashList[STASH_MAX_ITEMS];
    int stashListCount;
} StashStruct;

extern StashStruct Stash;

typedef struct StashSlotCoord {
    int X;
    int Y;
} StashSlotCoord;

StashSlotCoord GetStashSlotCoord(int x, int y);
void InitStash();
void FreeStashGFX();
void DrawStash();
void DrawGoldWithdraw();
void CheckStashItem(int mx, int my);
void CheckStashCut(int mx, int my);
void CheckStashButtonPress();
void CheckStashButtonRelease();
int CheckStashHLight(int mx, int my);
BOOL UseStashItem(int cii);
void StartGoldWithdraw();
void CloseGoldWithdraw();
BOOL AutoPlaceItemInStash(ItemStruct *pItem);
void TransferItemToInventory(int pnum, int itemId);
void RemoveStashItem(int iv);
void WithdrawGoldKeyPress(int vkey);

// Save/Load functions (called from pfile)
void pfile_write_stash();
BOOL pfile_read_stash(HANDLE archive);

#endif