#include "diablo.h"
#include "stash.h"

// ============================================================================
// Globals
// ============================================================================

BOOL stashflag = FALSE;
BOOL isWithdrawGoldOpen = FALSE;
int pcurstashitem = -1;

StashStruct Stash = {0};

BYTE *pStashCels = NULL;
BYTE *pStashNavCels = NULL;

char goldWithdrawText[21] = {0};
int goldWithdrawValue = 0;
DWORD goldWithdrawLastTick = 0;

const int StashButtonX[NUM_STASH_BUTTONS] = { 19, 56, 93, 242, 279 };
const int StashButtonY[NUM_STASH_BUTTONS] = { 19, 19, 19, 19, 19 };
const int StashButtonW = 27;
const int StashButtonH = 16;

int stashButtonPressed = -1;

// Helper to draw a string at (x,y) using CPrintString per character
static void DrawString(int x, int y, const char *str, char col)
{
    int off = x + PitchTbl[y];
    for (int i = 0; str[i]; i++) {
        BYTE c = fontframe[gbFontTransTbl[(BYTE)str[i]]];
        if (c) {
            CPrintString(off, c, col);
        }
        off += fontkern[c] + 1;
    }
}

// ============================================================================
// Graphics Helpers
// ============================================================================

StashSlotCoord GetStashSlotCoord(int x, int y)
{
    StashSlotCoord coord;
    coord.X = 384 + 17 + x * (INV_SLOT_SIZE_PX + 1);
    coord.Y = 511 + 48 + y * (INV_SLOT_SIZE_PX + 1);
    return coord;
}

// ============================================================================
// Init / Free
// ============================================================================

void InitStash()
{
    pStashCels = LoadFileInMem("data\\stash\\panel.cel", NULL);
    Stash.page = 0;
    Stash.gold = 0;
    Stash.dirty = TRUE;
    Stash.stashListCount = 0;
    memset(Stash.stashGrid, 0, sizeof(Stash.stashGrid));
    stashflag = FALSE;
    isWithdrawGoldOpen = FALSE;
    pcurstashitem = -1;
}

void FreeStashGFX()
{
    MemFreeDbg(pStashCels);
    MemFreeDbg(pStashNavCels);
    pStashCels = NULL;
    pStashNavCels = NULL;
}

// ============================================================================
// Grid Helpers
// ============================================================================

static int GetItemIdAtPosition(int x, int y)
{
    int val = Stash.stashGrid[Stash.page][x][y];
    if (val == 0) return -1;
    if (val > 0) return val - 1;          // positive = anchor
    return -1;
}

static BOOL IsGridSlotFree(int page, int x, int y)
{
    if (x < 0 || x >= STASH_GRID_SIZE || y < 0 || y >= STASH_GRID_SIZE) return FALSE;
    return Stash.stashGrid[page][x][y] == 0;
}

static void AddItemToStashGrid(int page, int x, int y, int sx, int sy, int itemIndex)
{
    int anchorX = x;
    int anchorY = y + sy - 1;
    for (int j = 0; j < sy; j++) {
        for (int i = 0; i < sx; i++) {
            int gx = x + i;
            int gy = y + j;
            if (gx < STASH_GRID_SIZE && gy < STASH_GRID_SIZE) {
                if (gx == anchorX && gy == anchorY)
                    Stash.stashGrid[page][gx][gy] = itemIndex + 1;      // positive = anchor
                else
                    Stash.stashGrid[page][gx][gy] = -(itemIndex + 1);   // negative = occupied
            }
        }
    }
}

static void ClearItemFromGrid(int page, int itemIndex)
{
    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            int val = Stash.stashGrid[page][x][y];
            if (val == itemIndex + 1 || val == -(itemIndex + 1)) {
                Stash.stashGrid[page][x][y] = 0;
            }
        }
    }
}

// ============================================================================
// Auto-Place
// ============================================================================

BOOL AutoPlaceItemInStash(ItemStruct *pItem)
{
    if (pItem->_itype == ITYPE_NONE) return FALSE;

    if (pItem->_itype == ITYPE_GOLD) {
        if (Stash.gold > 2000000000 - pItem->_ivalue) return FALSE;
        Stash.gold += pItem->_ivalue;
        Stash.dirty = TRUE;
        return TRUE;
    }

    int frame = pItem->_iCurs + CURSOR_FIRSTITEM;
    int sx = InvItemWidth[frame] / INV_SLOT_SIZE_PX;
    int sy = InvItemHeight[frame] / INV_SLOT_SIZE_PX;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    for (int p = 0; p < NUM_STASH_PAGES; p++) {
        int page = (Stash.page + p) % NUM_STASH_PAGES;
        for (int y = 0; y <= STASH_GRID_SIZE - sy; y++) {
            for (int x = 0; x <= STASH_GRID_SIZE - sx; x++) {
                BOOL free = TRUE;
                for (int j = 0; j < sy && free; j++) {
                    for (int i = 0; i < sx && free; i++) {
                        if (!IsGridSlotFree(page, x + i, y + j)) free = FALSE;
                    }
                }
                if (free) {
                    int idx = Stash.stashListCount;
                    Stash.stashList[idx] = *pItem;
                    Stash.stashListCount++;
                    AddItemToStashGrid(page, x, y, sx, sy, idx);
                    Stash.dirty = TRUE;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

// ============================================================================
// Remove / Transfer
// ============================================================================

void RemoveStashItem(int iv)
{
    if (iv < 0 || iv >= Stash.stashListCount) return;
    for (int p = 0; p < NUM_STASH_PAGES; p++) {
        ClearItemFromGrid(p, iv);
    }
    if (iv == Stash.stashListCount - 1) {
        Stash.stashListCount--;
        Stash.dirty = TRUE;
        return;
    }
    Stash.stashList[iv] = Stash.stashList[Stash.stashListCount - 1];
    int movedIndex = Stash.stashListCount - 1;
    for (int p = 0; p < NUM_STASH_PAGES; p++) {
        for (int y = 0; y < STASH_GRID_SIZE; y++) {
            for (int x = 0; x < STASH_GRID_SIZE; x++) {
                int val = Stash.stashGrid[p][x][y];
                if (val == movedIndex + 1) {
                    Stash.stashGrid[p][x][y] = iv + 1;
                } else if (val == -(movedIndex + 1)) {
                    Stash.stashGrid[p][x][y] = -(iv + 1);
                }
            }
        }
    }
    Stash.stashListCount--;
    Stash.dirty = TRUE;
}

void TransferItemToInventory(int pnum, int itemId)
{
    if (itemId < 0 || itemId >= Stash.stashListCount) return;
    ItemStruct tempItem = Stash.stashList[itemId];

    if (tempItem._itype == ITYPE_GOLD) {
        if (GoldAutoPlace(pnum)) {
            RemoveStashItem(itemId);
            return;
        }
        for (int i = 0; i < MAXBELTITEMS; i++) {
            if (plr[pnum].SpdList[i]._itype == ITYPE_NONE) {
                plr[pnum].SpdList[i] = tempItem;
                plr[pnum]._pGold += tempItem._ivalue;
                RemoveStashItem(itemId);
                drawsbarflag = TRUE;
                return;
            }
        }
        return;
    }

    int frame = tempItem._iCurs + CURSOR_FIRSTITEM;
    int sx = InvItemWidth[frame] / INV_SLOT_SIZE_PX;
    int sy = InvItemHeight[frame] / INV_SLOT_SIZE_PX;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    for (int i = 0; i < NUM_INV_GRID_ELEM; i++) {
        if (AutoPlace(pnum, i, sx, sy, FALSE)) {
            plr[pnum].HoldItem = tempItem;
            AutoPlace(pnum, i, sx, sy, TRUE);
            RemoveStashItem(itemId);
            CalcPlrInv(pnum, TRUE);
            PlaySFX(ItemInvSnds[ItemCAnimTbl[tempItem._iCurs]]);
            return;
        }
    }

    if (AllItemsList[tempItem.IDidx].iUsable && tempItem._iStatFlag) {
        for (int i = 0; i < MAXBELTITEMS; i++) {
            if (plr[pnum].SpdList[i]._itype == ITYPE_NONE) {
                plr[pnum].SpdList[i] = tempItem;
                RemoveStashItem(itemId);
                drawsbarflag = TRUE;
                return;
            }
        }
    }

    if (pnum == myplr) {
        if (plr[pnum]._pClass == PC_WARRIOR)
            PlaySFX(PS_WARR14);
        else if (plr[pnum]._pClass == PC_ROGUE)
            PlaySFX(PS_ROGUE14);
        else if (plr[pnum]._pClass == PC_SORCERER)
            PlaySFX(PS_MAGE14);
    }
}

// ============================================================================
// Stash Paste
// ============================================================================

static void CheckStashPaste(int pnum)
{
    PlayerStruct *p = &plr[pnum];
    if (p->HoldItem._itype == ITYPE_NONE) return;

    int mouseX = MouseX;
    int mouseY = MouseY;

    int targetX = -1, targetY = -1;
    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            StashSlotCoord coord = GetStashSlotCoord(x, y);
            if (mouseX >= coord.X && mouseX < coord.X + INV_SLOT_SIZE_PX &&
                mouseY >= coord.Y && mouseY < coord.Y + INV_SLOT_SIZE_PX) {
                targetX = x;
                targetY = y;
                break;
            }
        }
        if (targetX != -1) break;
    }
    if (targetX == -1) return;

    int frame = p->HoldItem._iCurs + CURSOR_FIRSTITEM;
    int sx = InvItemWidth[frame] / INV_SLOT_SIZE_PX;
    int sy = InvItemHeight[frame] / INV_SLOT_SIZE_PX;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    int startX = targetX - (sx - 1) / 2;
    int startY = targetY - (sy - 1) / 2;
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;
    if (startX + sx > STASH_GRID_SIZE) startX = STASH_GRID_SIZE - sx;
    if (startY + sy > STASH_GRID_SIZE) startY = STASH_GRID_SIZE - sy;

    int existingItem = -1;
    BOOL free = TRUE;
    for (int j = 0; j < sy && free; j++) {
        for (int i = 0; i < sx && free; i++) {
            int id = GetItemIdAtPosition(startX + i, startY + j);
            if (id != -1) {
                if (existingItem == -1) {
                    existingItem = id;
                } else if (existingItem != id) {
                    free = FALSE;
                }
            }
        }
    }
    if (!free) return;

    if (pnum == myplr) {
        PlaySFX(ItemInvSnds[ItemCAnimTbl[p->HoldItem._iCurs]]);
    }

    if (existingItem == -1) {
        int idx = Stash.stashListCount;
        Stash.stashList[idx] = p->HoldItem;
        Stash.stashListCount++;
        AddItemToStashGrid(Stash.page, startX, startY, sx, sy, idx);
        p->HoldItem._itype = ITYPE_NONE;
        Stash.dirty = TRUE;
    } else {
        ItemStruct temp = Stash.stashList[existingItem];
        Stash.stashList[existingItem] = p->HoldItem;
        p->HoldItem = temp;
        ClearItemFromGrid(Stash.page, existingItem);
        AddItemToStashGrid(Stash.page, startX, startY, sx, sy, existingItem);
        Stash.dirty = TRUE;
        if (pnum == myplr) {
            SetCursor_(p->HoldItem._iCurs + CURSOR_FIRSTITEM);
        }
    }
    CalcPlrInv(pnum, TRUE);
}

// ============================================================================
// Stash Cut (pickup)
// ============================================================================

void CheckStashCut(int mx, int my)
{
    if (!stashflag) return;
    PlayerStruct *p = &plr[myplr];
    if (p->HoldItem._itype != ITYPE_NONE) return;

    int targetX = -1, targetY = -1;
    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            StashSlotCoord coord = GetStashSlotCoord(x, y);
            if (mx >= coord.X && mx < coord.X + INV_SLOT_SIZE_PX &&
                my >= coord.Y && my < coord.Y + INV_SLOT_SIZE_PX) {
                targetX = x;
                targetY = y;
                break;
            }
        }
        if (targetX != -1) break;
    }
    if (targetX == -1) return;

    int itemId = GetItemIdAtPosition(targetX, targetY);
    if (itemId == -1) return;

    p->HoldItem = Stash.stashList[itemId];
    RemoveStashItem(itemId);
    if (myplr == myplr) {
        PlaySFX(IS_IGRAB);
        SetCursor_(p->HoldItem._iCurs + CURSOR_FIRSTITEM);
    }
    CalcPlrInv(myplr, TRUE);
}

void CheckStashItem(int mx, int my)
{
    if (!stashflag) return;
    PlayerStruct *p = &plr[myplr];
    if (p->HoldItem._itype != ITYPE_NONE) {
        CheckStashPaste(myplr);
    } else {
        CheckStashCut(mx, my);
    }
}

// ============================================================================
// Highlight
// ============================================================================

int CheckStashHLight(int mx, int my)
{
    if (!stashflag) return -1;

    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            StashSlotCoord coord = GetStashSlotCoord(x, y);
            if (mx >= coord.X && mx < coord.X + INV_SLOT_SIZE_PX &&
                my >= coord.Y && my < coord.Y + INV_SLOT_SIZE_PX) {
                int itemId = GetItemIdAtPosition(x, y);
                if (itemId != -1) {
                    ItemStruct *pi = &Stash.stashList[itemId];
                    infoclr = COL_WHITE;
                    if (pi->_iMagical == ITEM_QUALITY_MAGIC) infoclr = COL_BLUE;
                    else if (pi->_iMagical == ITEM_QUALITY_UNIQUE) infoclr = COL_GOLD;
                    if (pi->_itype == ITYPE_GOLD) {
                        sprintf(infostr, "%i gold", pi->_ivalue);
                    } else {
                        strcpy(infostr, pi->_iName);
                        if (pi->_iIdentified) {
                            PrintItemDetails(pi);
                        } else {
                            PrintItemDur(pi);
                        }
                    }
                    return itemId;
                }
                return -1;
            }
        }
    }
    return -1;
}

// ============================================================================
// Draw Stash
// ============================================================================

void DrawStash()
{
    if (!stashflag || !pStashCels) return;

    CelDecodeOnly(384, 511, pStashCels, 1, 320);

    // Draw slot backgrounds for anchors
    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            int val = Stash.stashGrid[Stash.page][x][y];
            if (val <= 0) continue;
            StashSlotCoord coord = GetStashSlotCoord(x, y);
            InvDrawSlotBack(coord.X, coord.Y - INV_SLOT_SIZE_PX + 1,
                            INV_SLOT_SIZE_PX, INV_SLOT_SIZE_PX);
        }
    }

    // Draw items
    for (int y = 0; y < STASH_GRID_SIZE; y++) {
        for (int x = 0; x < STASH_GRID_SIZE; x++) {
            int val = Stash.stashGrid[Stash.page][x][y];
            if (val <= 0) continue;
            int itemId = val - 1;
            ItemStruct *pi = &Stash.stashList[itemId];

            int frame = pi->_iCurs + CURSOR_FIRSTITEM;
            int frame_width = InvItemWidth[frame];
            StashSlotCoord coord = GetStashSlotCoord(x, y);
            int drawX = coord.X;
            int drawY = coord.Y - INV_SLOT_SIZE_PX + 1;

            int sx = InvItemWidth[frame] / INV_SLOT_SIZE_PX;
            int sy = InvItemHeight[frame] / INV_SLOT_SIZE_PX;
            if (sx < 1) sx = 1;
            if (sy < 1) sy = 1;
            if (sx < 2) drawX += (INV_SLOT_SIZE_PX - InvItemWidth[frame]) / 2;
            if (sy < 2) drawY += (INV_SLOT_SIZE_PX - InvItemHeight[frame]) / 2;

            if (pcurstashitem == itemId) {
                int colour = ICOL_WHITE;
                if (pi->_iMagical != ITEM_QUALITY_NORMAL) colour = ICOL_BLUE;
                if (!pi->_iStatFlag) colour = ICOL_RED;
                CelDecodeClr(colour, drawX, drawY, pCursCels, frame, frame_width, 0, 8);
            }

            if (pi->_iStatFlag) {
                CelDrawHdrOnly(drawX, drawY, pCursCels, frame, frame_width, 0, 8);
            } else {
                CelDrawHdrLightRed(drawX, drawY, pCursCels, frame, frame_width, 0, 8, 1);
            }
        }
    }

    // Page number
    char pageStr[16];
    sprintf(pageStr, "%d / %d", Stash.page + 1, NUM_STASH_PAGES);
    DrawString(384 + 200, 511 + 15, pageStr, COL_WHITE);

    // Gold display
    char goldStr[32];
    sprintf(goldStr, "Gold: %d", Stash.gold);
    DrawString(384 + 100, 511 + 15, goldStr, COL_WHITE);

    // Navigation buttons
    DrawPanelBox(384 + 19, 511 + 19, 27, 16, 269, 517);
    DrawPanelBox(384 + 56, 511 + 19, 27, 16, 269, 517);
    DrawPanelBox(384 + 93, 511 + 19, 27, 16, 269, 517);
    DrawPanelBox(384 + 242, 511 + 19, 27, 16, 269, 517);
    DrawPanelBox(384 + 279, 511 + 19, 27, 16, 269, 517);
    DrawString(384 + 19, 511 + 19, "<<", COL_WHITE);
    DrawString(384 + 56, 511 + 19, "<", COL_WHITE);
    DrawString(384 + 93, 511 + 19, "$", COL_WHITE);
    DrawString(384 + 242, 511 + 19, ">", COL_WHITE);
    DrawString(384 + 279, 511 + 19, ">>", COL_WHITE);
}

// ============================================================================
// Gold Withdraw
// ============================================================================

void StartGoldWithdraw()
{
    if (!stashflag || Stash.gold <= 0) return;
    isWithdrawGoldOpen = TRUE;
    goldWithdrawValue = 0;
    goldWithdrawText[0] = '\0';
    goldWithdrawLastTick = _GetTickCount();
}

void CloseGoldWithdraw()
{
    isWithdrawGoldOpen = FALSE;
}

void DrawGoldWithdraw()
{
    if (!isWithdrawGoldOpen) return;
    DrawPanelBox(384 + 30, 511 + 75, 200, 50, 269, 517);
    DrawString(384 + 35, 511 + 80, "How much gold to withdraw?", COL_WHITE);
    char displayStr[32];
    if (goldWithdrawText[0] == '\0') {
        sprintf(displayStr, "0");
    } else {
        sprintf(displayStr, "%s", goldWithdrawText);
    }
    DrawString(384 + 35, 511 + 105, displayStr, COL_WHITE);
}

void WithdrawGoldKeyPress(int vkey)
{
    if (!isWithdrawGoldOpen) return;

    if (vkey == 13 || vkey == 10) { // Enter
        int amount = atoi(goldWithdrawText);
        if (amount > 0 && amount <= Stash.gold) {
            plr[myplr].HoldItem._itype = ITYPE_GOLD;
            plr[myplr].HoldItem._ivalue = amount;
            if (GoldAutoPlace(myplr)) {
                Stash.gold -= amount;
                Stash.dirty = TRUE;
                PlaySFX(IS_IGRAB);
            } else {
                plr[myplr].HoldItem._itype = ITYPE_NONE;
            }
        }
        CloseGoldWithdraw();
        return;
    }

    if (vkey == 27) { // Escape
        CloseGoldWithdraw();
        return;
    }

    if (vkey == 8) { // Backspace
        int len = strlen(goldWithdrawText);
        if (len > 0) goldWithdrawText[len - 1] = '\0';
        return;
    }

    if (vkey >= '0' && vkey <= '9') {
        int len = strlen(goldWithdrawText);
        if (len < 20) {
            goldWithdrawText[len] = (char)vkey;
            goldWithdrawText[len + 1] = '\0';
        }
    }
}

// ============================================================================
// Stash Buttons
// ============================================================================

void CheckStashButtonPress()
{
    if (!stashflag) return;
    int mouseX = MouseX;
    int mouseY = MouseY;

    for (int i = 0; i < NUM_STASH_BUTTONS; i++) {
        int bx = 384 + StashButtonX[i];
        int by = 511 + StashButtonY[i];
        if (mouseX >= bx && mouseX < bx + StashButtonW &&
            mouseY >= by && mouseY < by + StashButtonH) {
            stashButtonPressed = i;
            return;
        }
    }
    stashButtonPressed = -1;
}

void CheckStashButtonRelease()
{
    if (!stashflag || stashButtonPressed == -1) return;
    int mouseX = MouseX;
    int mouseY = MouseY;

    int i = stashButtonPressed;
    int bx = 384 + StashButtonX[i];
    int by = 511 + StashButtonY[i];

    if (mouseX >= bx && mouseX < bx + StashButtonW &&
        mouseY >= by && mouseY < by + StashButtonH) {
        switch (i) {
            case STASH_BTN_PREV_10:
                if (Stash.page >= 10) Stash.page -= 10;
                else Stash.page = 0;
                Stash.dirty = TRUE;
                break;
            case STASH_BTN_PREV_1:
                if (Stash.page > 0) Stash.page--;
                Stash.dirty = TRUE;
                break;
            case STASH_BTN_WITHDRAW:
                StartGoldWithdraw();
                break;
            case STASH_BTN_NEXT_1:
                if (Stash.page < NUM_STASH_PAGES - 1) Stash.page++;
                Stash.dirty = TRUE;
                break;
            case STASH_BTN_NEXT_10:
                if (Stash.page < NUM_STASH_PAGES - 10) Stash.page += 10;
                else Stash.page = NUM_STASH_PAGES - 1;
                Stash.dirty = TRUE;
                break;
        }
    }
    stashButtonPressed = -1;
}

// ============================================================================
// Use Stash Item
// ============================================================================

BOOL UseStashItem(int cii)
{
    if (cii < 0 || cii >= Stash.stashListCount) return FALSE;
    if (pcurs != CURSOR_HAND) return TRUE;

    ItemStruct *pItem = &Stash.stashList[cii];

    if (!AllItemsList[pItem->IDidx].iUsable) return FALSE;

    if (!pItem->_iStatFlag) {
        if (plr[myplr]._pClass == PC_WARRIOR)
            PlaySFX(PS_WARR13);
        else if (plr[myplr]._pClass == PC_ROGUE)
            PlaySFX(PS_ROGUE13);
        else if (plr[myplr]._pClass == PC_SORCERER)
            PlaySFX(PS_MAGE13);
        return TRUE;
    }

    if ((pItem->_iMiscId == IMISC_SCROLL || pItem->_iMiscId == IMISC_SCROLLT) &&
        leveltype == DTYPE_TOWN && !spelldata[pItem->_iSpell].sTownSpell) {
        return TRUE;
    }

    if (pItem->_iMiscId == IMISC_BOOK) {
        PlaySFX(IS_RBOOK);
    } else {
        PlaySFX(ItemInvSnds[ItemCAnimTbl[pItem->_iCurs]]);
    }

    UseItem(myplr, pItem->_iMiscId, pItem->_iSpell);

    if (pItem->_iMiscId != IMISC_MAPOFDOOM) {
        RemoveStashItem(cii);
    }
    return TRUE;
}
