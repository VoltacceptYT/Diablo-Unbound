//HEADER_GOES_HERE
#ifndef __STASH_H__
#define __STASH_H__

// Single-page 10x10 player stash.
//
// Modeled directly on the general-storage ("unequippable") slice of the
// belt/body/inventory system in inv.h/inv.cpp - a stash slot behaves just
// like an inventory grid slot (supports multi-cell items and gold
// stacking/merging) but the 100 slots are not tied to a body location and
// are drawn in their own panel using Data\stash.cel.
//
// NOTE ON COORDINATES: STASH_PANEL_X/Y below position the panel exactly
// like the character panel - flush against the left edge of the screen
// (mouse-space 0,0 - 320,352) - matching the size of the supplied
// stash.cel (320x352, i.e. exactly PANEL_TOP tall). Opening the stash
// also opens the inventory on the right half, so the two sit side by
// side like the character/inventory pair. If your stash.cel differs in
// size/layout, adjust STASH_PANEL_X/Y and the grid origin constants
// (STASH_GRID_ORIGIN_X/Y) to match - see stash.cpp.

extern BOOL stashflag;
extern int pcursstashitem;

void FreeStashGFX();
void InitStash();
void DrawStash();
void CheckStashItem();
void CheckStashPaste(int pnum, int mx, int my);
void CheckStashCut(int pnum, int mx, int my);
char CheckStashHLight();
void OpenStash();
void CloseStash();
void ToggleStash();

// Gold banking: lets the player stash/withdraw gold as a single running
// total (plr._pStashGold) instead of storing gold-pile items on the grid,
// via a clickable "gold" button in the stash header.
BOOL CheckStashGoldButton(int mx, int my);
void StashDepositGold(int pnum);
void StashStartGoldWithdraw();

/* data */

extern InvXY StashRect[NUM_STASH_GRID_ELEM];

#endif /* __STASH_H__ */
