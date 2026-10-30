/***********************************************************************************************

	Sega System C/C2 Driver
	-----------------------
	Version 0.54 - 02 Feb 2003
	(for changes see drivers\segac2.c)

	The video of the System C/C2, Mega-Tech and Mega Play boards and of the
	Mega Drive based bootlegs: the VDP itself is in vidhrdw/segavdp.c.

***********************************************************************************************/

#include "driver.h"
#include "segac2.h"

/* in vidhrdw/segasyse.c */
int start_megatech_video_normal(void);
void update_megaplay_video_normal(struct mame_bitmap *bitmap, const struct rectangle *cliprect );



/******************************************************************************
	Video Start Functions
*******************************************************************************

	The palette RAM is shared by the VDP (CRAM writes) and the board.

******************************************************************************/

static int start_vdp(int sp_palbase, int display_enable, int scanbase, int line_scale, int set_visible_area, int shadow_base)
{
	struct segavdp_interface intf;

	/* reset the palettes */
	memset(paletteram16, 0, 0x800 * sizeof(data16_t));

	intf.cram_mode        = SEGAVDP_CRAM_PALETTERAM;
	intf.cram_base        = 0;
	intf.bg_palbase       = 0x000;
	intf.sp_palbase       = sp_palbase;
	intf.scanbase         = scanbase;
	intf.line_scale       = line_scale;
	intf.display_enable   = display_enable;
	intf.set_visible_area = set_visible_area;
	intf.irq_update       = megadriv_vdp_irq_update;
	intf.shadow_base      = shadow_base;
	intf.hilite_base      = shadow_base * 2;
	intf.black_pen        = -1;
	return segavdp_start(&intf);
}


VIDEO_START( segac2 )
{
	paletteram16 = auto_malloc(0x800 * sizeof(data16_t));

	/* the display is enabled by the board (control_w); the shadow and
	   highlight colors follow the 0x800 colors of the board (palette_w); the
	   last palette entry is black, for the lines the board blanks */
	if (start_vdp(0x100, 0, 0, 1, 1, 0x800))
		return 1;
	palette_set_color(SEGAC2_BLACK_PEN, 0, 0, 0);
	segavdp_set_black_pen(SEGAC2_BLACK_PEN);
	return 0;
}


VIDEO_START( megadriv )
{
	paletteram16 = auto_malloc(0x800 * sizeof(data16_t));

	/* same palettes for sprites and bg; the shadow and highlight colors
	   after the 64 VDP colors */
	return start_vdp(0x000, 1, 0, 1, 1, 0x40);
}


VIDEO_START( megatech )
{
	paletteram16 = auto_malloc(0x800 * sizeof(data16_t));

	/* same palettes for sprites and bg; the screen keeps its width, it has
	   the BIOS display too (drivers/megatech.c centres the H32 mode) */
	if (start_vdp(0x000, 1, 224, 2, 0, 0x40))
		return 1;

	return start_megatech_video_normal();
}


VIDEO_START( megaplay )
{
	paletteram16 = auto_malloc(0x800 * sizeof(data16_t));

	/* same palettes for sprites and bg */
	if (start_vdp(0x000, 1, 0, 1, 1, 0x40))
		return 1;

	return start_megatech_video_normal();
}



/* end-of-frame callback to mark the start of VBLANK */
VIDEO_EOF( segac2 )
{
	segavdp_eof();
}



/******************************************************************************
	Screen Refresh Functions
******************************************************************************/

/* core refresh: computes the final screen */
VIDEO_UPDATE( segac2 )
{
	int y;

	/* generate the final screen */
	for (y = cliprect->min_y; y <= cliprect->max_y; y++)
		segavdp_draw_line((UINT16 *)bitmap->line[y], y, 0);
}


/* System C/C2: the screen is drawn in RGB a few lines at a time, with the
   colors of the moment, as current MAME draws each line: the games change
   the palette RAM while the screen is displayed (drivers/segac2.c draws the
   lines before the change first) */
VIDEO_UPDATE( segac2_rgb )
{
	UINT16 buf[8 + 320 + 8], *line = buf + 8;
	int x, y;

	for (y = cliprect->min_y; y <= cliprect->max_y; y++)
	{
		UINT32 *dest = (UINT32 *)bitmap->line[y];
		segavdp_draw_line(line, y, 0);
		for (x = cliprect->min_x; x <= cliprect->max_x; x++)
			dest[x] = Machine->pens[line[x]];
	}
}


/* megaplay, draws either Genesis or SMS (single screen display) */
VIDEO_UPDATE( megaplay )
{
	int y;

	/* generate the final screen */
	for (y = cliprect->min_y; y <= cliprect->max_y; y++)
		segavdp_draw_line((UINT16 *)bitmap->line[y], y, 0);

	update_megaplay_video_normal(bitmap, cliprect);
}
