//HEADER_GOES_HERE
#include "diablo.h"

// ---------------------------------------------------------------------
// Single page 10x10 stash.
//
// This mirrors the "unequippable" (general storage) branch of the
// inventory system in inv.cpp: StashList/StashGrid work exactly like
// InvList/InvGrid (positive value = anchor slot holding StashList[v-1],
// negative value = an occupied-but-not-anchor slot belonging to
// abs(v)-1, 0 = empty), which means multi-cell items and gold-pile
// merging behave identically to the normal inventory.
//
// Panel graphics: Data\stash.cel, a single 320x352 frame (matches
// PANEL_TOP, i.e. it fills the screen from the top down to where the
// control panel starts) containing a small header area followed by a
// 10x10 grid of 28px slots on a 29px pitch (1px grid line).
//
// The stash is positioned exactly like the character panel (left half
// of the screen, x=0..320) rather than as a centered standalone dialog,
// and opening it also opens the inventory on the right half (x=320..640)
// so items can be dragged directly between the two, mirroring the
// character/inventory pairing.
//
// If your stash.cel differs from that layout, only the constants in the
// "panel & grid geometry" block below need to change.
// ---------------------------------------------------------------------

BOOL stashflag;
BYTE *pStashCels;
int pcursstashitem = -1;

/* panel & grid geometry (all in mouse/screen space, 0-640 x 0-480) */
#define STASH_PANEL_X 0 // panel occupies the left half, like the character panel
#define STASH_PANEL_Y 0
#define STASH_PANEL_WIDTH 320
#define STASH_PANEL_HEIGHT 352 // == PANEL_TOP

#define STASH_GRID_ORIGIN_X (STASH_PANEL_X + 16) // left edge of column 0
#define STASH_GRID_ORIGIN_Y (STASH_PANEL_Y + 47) // top edge of row 0
#define STASH_GRID_PITCH (INV_SLOT_SIZE_PX + 1)  // 29px: 28px slot + 1px gridline

/* buffer-space offsets used when calling the CEL draw primitives, which
 * expect coordinates relative to the render buffer's top-left corner and
 * a bottom-left anchor point for the Y axis (see InvRect's "+64"/"+159"
 * idiom in inv.cpp - these are just SCREEN_X and SCREEN_Y-1). */
#define STASH_X_OFFSET SCREEN_X
#define STASH_Y_OFFSET (SCREEN_Y - 1)

InvXY StashRect[NUM_STASH_GRID_ELEM];

static void InitStashRect()
{
	int row, col;

	for (row = 0; row < 10; row++) {
		for (col = 0; col < 10; col++) {
			StashRect[row * 10 + col].X = STASH_GRID_ORIGIN_X + col * STASH_GRID_PITCH;
			// Y is stored as the BOTTOM edge of the slot, matching the
			// convention used by InvRect throughout inv.cpp.
			StashRect[row * 10 + col].Y = STASH_GRID_ORIGIN_Y + (row + 1) * STASH_GRID_PITCH;
		}
	}
}

void FreeStashGFX()
{
	MemFreeDbg(pStashCels);
}

void InitStash()
{
	pStashCels = LoadFileInMem("Data\\stash.CEL", NULL);
	InitStashRect();
	stashflag = FALSE;
	pcursstashitem = -1;
}

void OpenStash()
{
	chrflag = FALSE;
	sbookflag = FALSE;
	questlog = FALSE;
	stashflag = TRUE;
	invflag = TRUE; // open the inventory alongside the stash, like chr+inv
}

void CloseStash()
{
	stashflag = FALSE;
}

void ToggleStash()
{
	if (stashflag) {
		CloseStash();
	} else {
		OpenStash();
	}
}

void DrawStash()
{
	int j, ii, frame, frame_width, colour;
	int dx, dy;

	CelDecodeOnly(
	    STASH_PANEL_X + STASH_X_OFFSET,
	    STASH_PANEL_Y + STASH_PANEL_HEIGHT - 1 + SCREEN_Y,
	    pStashCels, 1, STASH_PANEL_WIDTH);

	for (j = 0; j < NUM_STASH_GRID_ELEM; j++) {
		if (plr[myplr].StashGrid[j] != 0) {
			InvDrawSlotBack(
			    StashRect[j].X + STASH_X_OFFSET,
			    StashRect[j].Y + STASH_Y_OFFSET,
			    INV_SLOT_SIZE_PX,
			    INV_SLOT_SIZE_PX);
		}
	}

	for (j = 0; j < NUM_STASH_GRID_ELEM; j++) {
		if (plr[myplr].StashGrid[j] <= 0) {
			continue; // not an anchor slot
		}

		ii = plr[myplr].StashGrid[j] - 1;

		frame = plr[myplr].StashList[ii]._iCurs + CURSOR_FIRSTITEM;
		frame_width = InvItemWidth[frame];

		dx = StashRect[j].X + STASH_X_OFFSET;
		dy = StashRect[j].Y + STASH_Y_OFFSET;

		if (pcursstashitem == ii) {
			colour = ICOL_WHITE;
			if (plr[myplr].StashList[ii]._iMagical != ITEM_QUALITY_NORMAL) {
				colour = ICOL_BLUE;
			}
			if (!plr[myplr].StashList[ii]._iStatFlag) {
				colour = ICOL_RED;
			}
			CelDecodeClr(colour, dx, dy, pCursCels, frame, frame_width, 0, 8);
		}

		if (plr[myplr].StashList[ii]._iStatFlag) {
			CelDrawHdrOnly(dx, dy, pCursCels, frame, frame_width, 0, 8);
		} else {
			CelDrawHdrLightRed(dx, dy, pCursCels, frame, frame_width, 0, 8, 1);
		}
	}
}

// Locates the stash slot (if any) under mouse-space coordinates (mx,my),
// exactly like the hit-test loops in CheckInvCut/CheckInvPaste.
static BOOL FindStashSlot(int mx, int my, int *outSlot)
{
	int r;

	for (r = 0; r < NUM_STASH_GRID_ELEM; r++) {
		if (mx >= StashRect[r].X && mx < StashRect[r].X + STASH_GRID_PITCH
		    && my >= StashRect[r].Y - STASH_GRID_PITCH && my < StashRect[r].Y) {
			*outSlot = r;
			return TRUE;
		}
	}
	return FALSE;
}

void CheckStashPaste(int pnum, int mx, int my)
{
	int i, j, xx, yy;
	int sx, sy;
	int r, it, iv, il, gt, ig;
	int cn;
	BOOL done;

	SetICursor(plr[pnum].HoldItem._iCurs + CURSOR_FIRSTITEM);
	i = mx + (icursW >> 1);
	j = my + (icursH >> 1);
	sx = icursW28;
	sy = icursH28;

	if (!FindStashSlot(i, j, &r)) {
		return;
	}

	it = 0;
	done = TRUE;

	if (plr[pnum].HoldItem._itype == ITYPE_GOLD) {
		yy = 10 * (r / 10);
		xx = r % 10;
		if (plr[pnum].StashGrid[xx + yy] != 0) {
			iv = plr[pnum].StashGrid[xx + yy];
			if (iv > 0) {
				if (plr[pnum].StashList[iv - 1]._itype != ITYPE_GOLD) {
					it = iv;
				}
			} else {
				it = -iv;
			}
		}
	} else {
		yy = 10 * ((r / 10) - ((sy - 1) >> 1));
		if (yy < 0) {
			yy = 0;
		}
		for (j = 0; j < sy && done; j++) {
			if (yy >= NUM_STASH_GRID_ELEM) {
				done = FALSE;
				break;
			}
			xx = (r % 10) - ((sx - 1) >> 1);
			if (xx < 0) {
				xx = 0;
			}
			for (i = 0; i < sx && done; i++) {
				if (xx >= 10) {
					done = FALSE;
				} else {
					if (plr[pnum].StashGrid[xx + yy] != 0) {
						iv = plr[pnum].StashGrid[xx + yy];
						if (iv < 0) {
							iv = -iv;
						}
						if (it != 0) {
							if (it != iv) {
								done = FALSE;
							}
						} else {
							it = iv;
						}
					}
				}
				xx++;
			}
			yy += 10;
		}
	}

	if (!done) {
		return;
	}

	if (pnum == myplr) {
		PlaySFX(ItemInvSnds[ItemCAnimTbl[plr[pnum].HoldItem._iCurs]]);
	}

	cn = CURSOR_HAND;

	if (plr[pnum].HoldItem._itype == ITYPE_GOLD) {
		yy = 10 * (r / 10);
		xx = r % 10;
		if (plr[pnum].StashGrid[yy + xx] > 0) {
			il = plr[pnum].StashGrid[yy + xx] - 1;
			gt = plr[pnum].StashList[il]._ivalue;
			ig = plr[pnum].HoldItem._ivalue + gt;
			if (ig <= GOLD_MAX_LIMIT) {
				plr[pnum].StashList[il]._ivalue = ig;
				if (ig >= GOLD_MEDIUM_LIMIT) {
					plr[pnum].StashList[il]._iCurs = ICURS_GOLD_LARGE;
				} else if (ig <= GOLD_SMALL_LIMIT) {
					plr[pnum].StashList[il]._iCurs = ICURS_GOLD_SMALL;
				} else {
					plr[pnum].StashList[il]._iCurs = ICURS_GOLD_MEDIUM;
				}
			} else {
				ig = GOLD_MAX_LIMIT - gt;
				plr[pnum].HoldItem._ivalue -= ig;
				plr[pnum].StashList[il]._ivalue = GOLD_MAX_LIMIT;
				plr[pnum].StashList[il]._iCurs = ICURS_GOLD_LARGE;
				// same literal cursor ids used by CheckInvPaste's gold-split path
				if (plr[pnum].HoldItem._ivalue >= GOLD_MEDIUM_LIMIT) {
					cn = 18;
				} else if (plr[pnum].HoldItem._ivalue <= GOLD_SMALL_LIMIT) {
					cn = 16;
				} else {
					cn = 17;
				}
			}
		} else {
			if (plr[pnum]._pNumStash >= NUM_STASH_GRID_ELEM) {
				return;
			}
			il = plr[pnum]._pNumStash;
			plr[pnum].StashList[il] = plr[pnum].HoldItem;
			plr[pnum]._pNumStash++;
			plr[pnum].StashGrid[yy + xx] = plr[pnum]._pNumStash;
		}
	} else {
		if (it == 0) {
			if (plr[pnum]._pNumStash >= NUM_STASH_GRID_ELEM) {
				return;
			}
			plr[pnum].StashList[plr[pnum]._pNumStash] = plr[pnum].HoldItem;
			plr[pnum]._pNumStash++;
			it = plr[pnum]._pNumStash;
		} else {
			il = it - 1;
			cn = SwapItem(&plr[pnum].StashList[il], &plr[pnum].HoldItem);
		}

		yy = 10 * (r / 10 - ((sy - 1) >> 1));
		if (yy < 0) {
			yy = 0;
		}
		for (j = 0; j < sy; j++) {
			xx = (r % 10) - ((sx - 1) >> 1);
			if (xx < 0) {
				xx = 0;
			}
			for (i = 0; i < sx; i++) {
				if (i != 0 || j != sy - 1) {
					plr[pnum].StashGrid[xx + yy] = -it;
				} else {
					plr[pnum].StashGrid[xx + yy] = it;
				}
				xx++;
			}
			yy += 10;
		}
	}

	if (pnum == myplr) {
		if (cn == CURSOR_HAND) {
			_SetCursorPos(MouseX + (cursW >> 1), MouseY + (cursH >> 1));
		}
		SetCursor_(cn);
	}
}

void CheckStashCut(int pnum, int mx, int my)
{
	int r, ii, iv, i, j;

	if (plr[pnum]._pmode > PM_WALK3) {
		return;
	}

	if (dropGoldFlag) {
		dropGoldFlag = FALSE;
		dropGoldValue = 0;
	}

	if (!FindStashSlot(mx, my, &r)) {
		return;
	}

	plr[pnum].HoldItem._itype = ITYPE_NONE;

	ii = plr[pnum].StashGrid[r];
	if (ii != 0) {
		iv = ii;
		if (iv < 0) {
			iv = -iv;
		}

		for (i = 0; i < NUM_STASH_GRID_ELEM; i++) {
			if (plr[pnum].StashGrid[i] == iv || plr[pnum].StashGrid[i] == -iv) {
				plr[pnum].StashGrid[i] = 0;
			}
		}

		iv--;

		plr[pnum].HoldItem = plr[pnum].StashList[iv];
		plr[pnum]._pNumStash--;

		if (plr[pnum]._pNumStash > 0 && plr[pnum]._pNumStash != iv) {
			plr[pnum].StashList[iv] = plr[pnum].StashList[plr[pnum]._pNumStash];

			for (j = 0; j < NUM_STASH_GRID_ELEM; j++) {
				if (plr[pnum].StashGrid[j] == plr[pnum]._pNumStash + 1) {
					plr[pnum].StashGrid[j] = iv + 1;
				}
				if (plr[pnum].StashGrid[j] == -(plr[pnum]._pNumStash + 1)) {
					plr[pnum].StashGrid[j] = -iv - 1;
				}
			}
		}
	}

	if (plr[pnum].HoldItem._itype != ITYPE_NONE) {
		if (pnum == myplr) {
			PlaySFX(IS_IGRAB);
			SetCursor_(plr[pnum].HoldItem._iCurs + CURSOR_FIRSTITEM);
			_SetCursorPos(mx - (cursW >> 1), MouseY - (cursH >> 1));
		}
	}
}

void CheckStashItem()
{
	if (pcurs >= CURSOR_FIRSTITEM) {
		CheckStashPaste(myplr, MouseX, MouseY);
	} else {
		CheckStashCut(myplr, MouseX, MouseY);
	}
}

char CheckStashHLight()
{
	int r, ii, nGold;
	ItemStruct *pi;

	if (!FindStashSlot(MouseX, MouseY, &r)) {
		return -1;
	}

	ii = plr[myplr].StashGrid[r];
	if (ii == 0) {
		return -1;
	}
	if (ii < 0) {
		ii = -ii;
	}
	ii--;

	pi = &plr[myplr].StashList[ii];
	if (pi->_itype == ITYPE_NONE) {
		return -1;
	}

	infoclr = COL_WHITE;
	ClearPanel();

	if (pi->_itype == ITYPE_GOLD) {
		nGold = pi->_ivalue;
		sprintf(infostr, "%i gold %s", nGold, get_pieces_str(nGold));
	} else {
		if (pi->_iMagical == ITEM_QUALITY_MAGIC) {
			infoclr = COL_BLUE;
		} else if (pi->_iMagical == ITEM_QUALITY_UNIQUE) {
			infoclr = COL_GOLD;
		}
		strcpy(infostr, pi->_iName);
		if (pi->_iIdentified) {
			strcpy(infostr, pi->_iIName);
			PrintItemDetails(pi);
		} else {
			PrintItemDur(pi);
		}
	}

	return ii;
}
