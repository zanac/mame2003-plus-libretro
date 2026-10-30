/***********************************************************************************************

	Sega 315-5313 VDP (Mega Drive / Genesis video chip)
	---------------------------------------------------

	The VDP emulation of the Sega System C/C2 driver (see drivers/segac2.c for
	its history), as a module: the System C/C2, Mega-Tech and Mega Play boards,
	the Mega Drive based bootlegs and System 18 each start it with their own
	configuration (vidhrdw/segavdp.h), and all its state is reset at start.

***********************************************************************************************/

#include "driver.h"
#include "state.h"
#include "vidhrdw/segavdp.h"

/******************************************************************************
	Macros
******************************************************************************/

#define BITMAP_WIDTH		320
#define BITMAP_HEIGHT		240

#define VRAM_SIZE			0x10000
#define VRAM_MASK			(VRAM_SIZE - 1)
#define VSRAM_SIZE			0x80
#define VSRAM_MASK			(VSRAM_SIZE - 1)

#define VDP_VRAM_BYTE(x)	(vdp.vram[(x) & VRAM_MASK])
#define VDP_VSRAM_BYTE(x)	(vdp.vsram[(x) & VSRAM_MASK])
#define VDP_VRAM_WORD(x)	((VDP_VRAM_BYTE(x) << 8) | VDP_VRAM_BYTE((x) + 1))
#define VDP_VSRAM_WORD(x)	((VDP_VSRAM_BYTE(x) << 8) | VDP_VSRAM_BYTE((x) + 1))

#ifdef MSB_FIRST
#define EXTRACT_PIXEL(x,i)	(((x) >> (((i) ^ 7) * 4)) & 0x0f)
#else
#define EXTRACT_PIXEL(x,i)	(((x) >> (((i) ^ 1) * 4)) & 0x0f)
#endif



/******************************************************************************
	Function Prototypes
******************************************************************************/

static int  vdp_data_r(void);
static void vdp_data_w(int data);
static int  vdp_control_r(void);
static void vdp_control_w(int data);
static void vdp_register_w(int data);
static void vdp_control_dma(int data);
static void vdp_dma_68k(void);
static void vdp_dma_fill(int);
static void vdp_dma_copy(void);

static void drawline(UINT16 *bitmap, int line, int bgfill);
static void drawline_ste(UINT16 *bitmap, int line, int bgfill, UINT32 *scrolla_tiles, UINT32 *scrollb_tiles,
		UINT32 *window_tiles, int scrolla_offset, int scrollb_offset, int scrolla_lclip, int scrolla_rclip,
		int window_lclip, int window_rclip, int nsprites);
static void get_scroll_tiles(int line, int scrollnum, UINT32 scrollbase, UINT32 *tiles, int *offset);
static void get_window_tiles(int line, UINT32 scrollbase, UINT32 *tiles);
static void drawline_tiles(UINT32 *tiles, UINT16 *bmap, int pri, int offset, int lclip, int rclip);
static void drawline_sprite(int line, UINT16 *bmap, int priority, UINT8 *spritebase);



/******************************************************************************
	The VDP state
******************************************************************************/

static struct
{
	struct segavdp_interface intf;			/* the configuration of the user */

	UINT8		regs[32];					/* VDP registers */
	UINT8 *		vram;						/* VDP video RAM */
	UINT8 *		vsram;						/* VDP vertical scroll RAM */
	UINT16 *	transparent_lookup;			/* fast transparent mapping table */

	int			bg_palbase;					/* base of background palette */
	int			sp_palbase;					/* base of sprite palette */
	int			palbank;					/* global palette bank */
	UINT8		alt_palette;				/* System C/C2 alternate palette mode */
	int			bg_pal_lookup[4];			/* palette of each of the 4 bg palettes */
	int			sp_pal_lookup[4];			/* palette of each of the 4 sprite palettes */
	int			bd_pal_lookup[4];			/* the backdrop: without the background base */
	UINT8		display_enable;				/* is the display enabled (board control)? */
	UINT8		internal_vblank;			/* state of the VBLANK line */
	UINT8		iack_mode;					/* Mega Play interrupt acknowledge / HBLANK mode */
	UINT8		irq6_pending;				/* status bit 7: V-INT happened, cleared by its acknowledge */
	UINT8		sprite_collision;			/* status bit 5: two sprite pixels overlapped this frame */

	/* vram bases */
	UINT32		scrollabase;				/* base address of scroll A tiles */
	UINT32		scrollbbase;				/* base address of scroll B tiles */
	UINT32		windowbase;					/* base address of window tiles */
	UINT32		spritebase;					/* base address of sprite data */
	UINT32		hscrollbase;				/* base address of H scroll values */

	/* other vdp variables */
	int			hscrollmask;				/* mask for H scrolling */
	UINT32		hscrollsize;				/* size of active H scroll values */
	UINT8		vscrollmode;				/* current V scrolling mode */

	UINT8		cmdpart;					/* partial command flag */
	UINT8		code;						/* command code value */
	UINT32		address;					/* current I/O address */
	UINT8		dmafill;					/* DMA filling flag */
	UINT8		scrollheight;				/* height of the scroll area in tiles */
	UINT8		scrollwidth;				/* width of the scroll area in tiles */
	UINT8		bgcol;						/* current background color */
	UINT8		window_down;				/* window Y direction */
	UINT32		window_vpos;				/* window Y position */
	UINT8		window_right;				/* window X direction */
	UINT32		window_hpos;				/* window X position */
	UINT8		window_width;				/* window width */
} vdp;

/* shadow/highlight mode: the layers are drawn with the raw VDP colors
   (palette line * 16 + color, sprites with 0x40 low / 0x80 high priority)
   and the high priority tiles also mark the pixels they cover */
static int raw_mode;
static UINT8 *raw_cover;



/******************************************************************************
	Palette lookups
*******************************************************************************

	The palette entry of a pixel is the global palette bank, the base of the
	background or sprite palettes and the 4 palettes of the VDP. On System
	C/C2 the EPM5032 can also swizzle the colour RAM address bits (the
	alternate palette mode of Ribbit! and Twin Squash), as in current MAME.
	The backdrop colour doesn't take the background base (the title screen
	of Borench is black in MAME too).

******************************************************************************/

static void recompute_palette_lookups(void)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		int bgpal = vdp.bg_palbase + i * 0x10;
		int sppal = vdp.sp_palbase + i * 0x10;
		int bdpal = i * 0x10;

		if (!vdp.alt_palette)
		{
			vdp.bg_pal_lookup[i] = vdp.palbank + bgpal;
			vdp.sp_pal_lookup[i] = vdp.palbank + sppal;
			vdp.bd_pal_lookup[i] = vdp.palbank + bdpal;
		}
		else
		{
			vdp.bg_pal_lookup[i] = vdp.palbank + ((bgpal << 1) & 0x180) + ((~bgpal >> 2) & 0x40) + (bgpal & 0x30);
			vdp.sp_pal_lookup[i] = vdp.palbank + ((~sppal << 2) & 0x100) + ((sppal << 2) & 0x80) + ((~sppal >> 2) & 0x40) + ((sppal >> 2) & 0x20) + (sppal & 0x10);
			vdp.bd_pal_lookup[i] = vdp.palbank + ((bdpal << 1) & 0x180) + ((~bdpal >> 2) & 0x40) + (bdpal & 0x30);
		}
	}
}



/******************************************************************************
	Start
*******************************************************************************

	64kb of VRAM (multi-purpose, storing tiles, tilemaps, hscroll data,
					spritelist etc.)

	80bytes of VSRAM (used exclusively for storing Vertical Scroll values)

******************************************************************************/

int segavdp_start(const struct segavdp_interface *intf)
{
	static const UINT8 vdp_init[24] =
	{
		0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00,
		0x00, 0x00, 0xFF, 0x00, 0x01, 0x37, 0x00, 0x02,
		0x01, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x80,
	};
	int i;

	/* nothing survives from a previous game */
	memset(&vdp, 0, sizeof(vdp));
	vdp.intf = *intf;
	if (vdp.intf.line_scale < 1)
		vdp.intf.line_scale = 1;

	/* the shadow/highlight offsets must leave room for the 64 VDP colors */
	if (vdp.intf.shadow_base && (vdp.intf.shadow_base < 0x40 || vdp.intf.hilite_base < vdp.intf.shadow_base + 0x40))
		vdp.intf.shadow_base = vdp.intf.hilite_base = 0;

	/* allocate memory for the VDP and the lookup table */
	vdp.vram				= auto_malloc(VRAM_SIZE);
	vdp.vsram				= auto_malloc(VSRAM_SIZE);
	vdp.transparent_lookup	= auto_malloc(0x1000 * sizeof(UINT16));

	/* check for errors */
	if (!vdp.vram || !vdp.vsram || !vdp.transparent_lookup)
		return 1;

	/* clear the VDP memory, prevents corrupt tile in Puyo Puyo 2 */
	memset(vdp.vram, 0, VRAM_SIZE);
	memset(vdp.vsram, 0, VSRAM_SIZE);

	/* init transparency table */
	for (i = 0; i < 0x1000; i++)
	{
		int orig_color = i & 0x7ff;
		int half_bright = i & 0x800;

		if (orig_color & 0x100)
			vdp.transparent_lookup[i] = orig_color;
		else if (half_bright)
			vdp.transparent_lookup[i] = orig_color | 0x800;
		else
			vdp.transparent_lookup[i] = orig_color | 0x1000;
	}

	/* the palettes */
	vdp.bg_palbase = intf->bg_palbase;
	vdp.sp_palbase = intf->sp_palbase;
	vdp.palbank    = 0;
	vdp.alt_palette = 0;
	recompute_palette_lookups();
	vdp.display_enable = intf->display_enable;

	/* reset VDP (no interrupt callbacks yet: the board is not running) */
	vdp.internal_vblank = 1;
	vdp.intf.irq_update = NULL;
	for (i = 0; i < 24; i++)
		vdp_register_w(0x8000 | (i << 8) | vdp_init[i]);
	vdp.intf.irq_update = intf->irq_update;
	vdp.cmdpart = 0;
	vdp.code    = 0;
	vdp.address = 0;

	/* save states (the names of the System C/C2 driver, so the order is kept) */
	state_save_register_UINT8 ("C2_VDP", 0, "VDP Registers", vdp.regs, 32);
	state_save_register_UINT8 ("C2_VDP", 0, "V-INT pending", &vdp.irq6_pending, 1);
	state_save_register_UINT8 ("C2_VDP", 0, "Sprite collision", &vdp.sprite_collision, 1);
	state_save_register_UINT8 ("C2_VDP", 0, "VDP VRam", vdp.vram, 0x10000);
	state_save_register_UINT8 ("C2_VDP", 0, "VDP VSRam", vdp.vsram, 0x80);
	state_save_register_int("C2_Video", 0, "Palette Bank", &vdp.palbank);
	state_save_register_int("C2_Video", 0, "Background Pal Base",  &vdp.bg_palbase);
	state_save_register_int("C2_Video", 0, "Sprite Pal Base",  &vdp.sp_palbase);
	state_save_register_UINT8("C2_Video", 0, "Display Enabled",  &vdp.display_enable, 1);
	state_save_register_UINT32("C2_Video", 0, "Scroll A Base in VRAM",  &vdp.scrollabase, 1);
	state_save_register_UINT32("C2_Video", 0, "Scroll B Base in VRAM",  &vdp.scrollbbase, 1);
	state_save_register_UINT32("C2_Video", 0, "Window Base in VRAM",  &vdp.windowbase, 1);
	state_save_register_UINT32("C2_Video", 0, "Sprite Table Base in VRAM",  &vdp.spritebase, 1);
	state_save_register_UINT32("C2_Video", 0, "HScroll Data Base in VRAM",  &vdp.hscrollbase, 1);
	state_save_register_int("C2_Video", 0, "vdp_hscrollmask",  &vdp.hscrollmask);
	state_save_register_UINT32("C2_Video", 0, "vdp_hscrollsize",  &vdp.hscrollsize, 1);
	state_save_register_UINT8("C2_Video", 0, "vdp_vscrollmode",  &vdp.vscrollmode, 1);
	state_save_register_UINT8("C2_VDP", 0, "VDP Command Part",  &vdp.cmdpart, 1);
	state_save_register_UINT8("C2_VDP", 0, "VDP Current Code",  &vdp.code, 1);
	state_save_register_UINT32("C2_VDP", 0, "VDP Address",  &vdp.address, 1);
	state_save_register_UINT8("C2_VDP", 0, "VDP DMA Mode",  &vdp.dmafill, 1);
	state_save_register_UINT8("C2_Video", 0, "scrollheight",  &vdp.scrollheight, 1);
	state_save_register_UINT8("C2_Video", 0, "scrollwidth",  &vdp.scrollwidth, 1);
	state_save_register_UINT8("C2_Video", 0, "Background Colour",  &vdp.bgcol, 1);
	state_save_register_UINT8("C2_Video", 0, "Window Horz",  &vdp.window_down, 1);
	state_save_register_UINT32("C2_Video", 0, "Window Vert",  &vdp.window_vpos, 1);
	/* were missing: the window X position and width, the VBLANK flag */
	state_save_register_UINT8("C2_Video", 0, "Window Right",  &vdp.window_right, 1);
	state_save_register_UINT32("C2_Video", 0, "Window HPos",  &vdp.window_hpos, 1);
	state_save_register_UINT8("C2_Video", 0, "Window Width",  &vdp.window_width, 1);
	state_save_register_UINT8("C2_VDP", 0, "VBLANK",  &vdp.internal_vblank, 1);
	state_save_register_UINT8("C2_Video", 0, "Alt Palette",  &vdp.alt_palette, 1);
	state_save_register_func_postload(recompute_palette_lookups);

	return 0;
}



/******************************************************************************
	VBLANK routines
*******************************************************************************

	These callbacks are used to track the state of VBLANK. At the end of
	VBLANK, all the palette information is reset and updates to the cache
	bitmap are enabled.

******************************************************************************/

/* timer callback for the end of VBLANK */
static void vblank_end(int param)
{
	/* reset VBLANK flag */
	vdp.internal_vblank = 0;
}


/* Mega Play: raise the VBLANK status bit at the start of blanking, ahead of
   V-INT (VIDEO_EOF only runs after the screen update) */
void segavdp_set_vblank(void)
{
	vdp.internal_vblank = 1;
	vdp.irq6_pending = 1;
}

/* the 68000 acknowledges V-INT: as current MAME the pending flag (status bit
   7) only clears while V-INT is enabled in register 1 (System C/C2 takes
   V-INT from another line, so there the flag stays set) */
int segavdp_irq6_pending(void) { return vdp.irq6_pending; }
void segavdp_set_black_pen(int pen) { vdp.intf.black_pen = pen; }

void segavdp_irq6_ack(void)
{
	if (vdp.regs[1] & 0x20)
		vdp.irq6_pending = 0;
}


/* sprite collision for one visible line, as current MAME: the sprites of
   the line are walked in link order with the same sprite and pixel limits,
   and two opaque pixels on the same position (0-511) set status bit 5.
   The flag is cleared at the start of the frame (line 0). It is computed
   apart from the rendering, so it does not depend on the frameskip. */
void segavdp_sprite_line(int line)
{
	UINT8 used[512];
	int maxsprites, maxpixels, spritemask = 0, link = 0;

	if (line == 0)
		vdp.sprite_collision = 0;
	if (vdp.sprite_collision || line < 0 || line >= 224)
		return;

	if (vdp.regs[12] & 0x01)
		maxsprites = 80, maxpixels = 320;
	else
		maxsprites = 64, maxpixels = 256;
	memset(used, 0, sizeof(used));

	do
	{
		UINT8 *sb = &VDP_VRAM_BYTE(vdp.spritebase + 8 * link);
		int ypos = (((sb[0] & 0x01) << 8) | sb[1]) - 128;
		int height = (sb[2] & 0x03) + 1;
		int width = ((sb[2] & 0x0c) >> 2) + 1;
		int xpos = ((sb[6] & 0x01) << 8) | sb[7];

		link = sb[3] & 0x7f;
		if (ypos <= line && ypos + height * 8 > line)
		{
			int attr = (sb[4] << 8) | sb[5];
			int patno = attr & 0x7ff;
			int xflip = attr & 0x0800, yflip = attr & 0x1000;
			int yline = line - ypos, ytile = yline >> 3, xtile, xx;

			if (xpos == 0)
				spritemask |= 1;
			if (xpos > 0 && xpos < 0x40)
				spritemask |= 2;
			if (spritemask == 3)
				return;

			yline &= 7;
			if (yflip)
			{
				ytile = height - ytile - 1;
				yline = 7 - yline;
			}
			xx = xpos;
			for (xtile = 0; xtile < width; xtile++)
			{
				int code = (xflip ? width - xtile - 1 : xtile) * height + ytile;
				UINT32 tile = *(UINT32 *)&VDP_VRAM_BYTE(((patno + code) & 0x7ff) * 32 + 4 * yline);
				int i;

				for (i = 0; i < 8; i++)
				{
					if (EXTRACT_PIXEL(tile, xflip ? 7 - i : i))
					{
						if (used[xx])
						{
							vdp.sprite_collision = 1;
							return;
						}
						used[xx] = 1;
					}
					xx = (xx + 1) & 0x1ff;
					if (--maxpixels == 0)
						return;
				}
			}
		}
		maxsprites--;
	} while (maxsprites >= 0 && link != 0);
}


/* end-of-frame callback to mark the start of VBLANK */
void segavdp_eof(void)
{
	/* set VBLANK flag */
	vdp.internal_vblank = 1;
	vdp.irq6_pending = 1;

	/* set a timer for VBLANK off */
	timer_set(segavdp_scanlinetime(0), 0, vblank_end);
}


void segavdp_set_iack_mode(int enable)
{
	vdp.iack_mode = enable;
}



/******************************************************************************
	Board controls
******************************************************************************/

/* set the display enable bit */
void segavdp_enable_display(int enable)
{
	if (enable != vdp.display_enable)
		segavdp_update_before_line();
	vdp.display_enable = enable;
}

/* System C/C2 board controls (display enable, palette banks): current MAME
   draws each line at the start of the next one with the board state of that
   moment, so a change during a line already applies to that line. Draw the
   lines before it with the old state. */
void segavdp_update_before_line(void)
{
	int line = segavdp_drawn_line() + vdp.intf.scanbase - 1;
	if (line >= 0)
		force_partial_update(line);
}

void segavdp_set_palbases(int bg_palbase, int sp_palbase)
{
	vdp.bg_palbase = bg_palbase;
	vdp.sp_palbase = sp_palbase;
	recompute_palette_lookups();
}

int segavdp_get_bg_palbase(void) { return vdp.bg_palbase; }
int segavdp_get_sp_palbase(void) { return vdp.sp_palbase; }

void segavdp_set_palbank(int palbank)
{
	vdp.palbank = palbank;
	recompute_palette_lookups();
}

void segavdp_set_alt_palette(int enable)
{
	vdp.alt_palette = enable;
	recompute_palette_lookups();
}

int segavdp_get_palbank(void) { return vdp.palbank; }
int segavdp_get_reg(int regnum) { return vdp.regs[regnum & 0x1f]; }
int segavdp_get_scanbase(void) { return vdp.intf.scanbase; }

/* the VDP line timing: with a line_scale of 1 these are the cpu_getscanline()
   family as they are; the Mega-Tech screen has both displays, so a VDP line
   takes line_scale screen scanlines */
int segavdp_scanline(void)
{
	return cpu_getscanline() / vdp.intf.line_scale;
}

int segavdp_horzbeampos(void)
{
	if (vdp.intf.line_scale == 1)
		return cpu_gethorzbeampos();
	return ((cpu_getscanline() % vdp.intf.line_scale) * Machine->drv->screen_width + cpu_gethorzbeampos()) / vdp.intf.line_scale;
}

/* the last line already drawn, as in current MAME (each line is drawn at its
   start): on the boards of the 315-5313 H-INT and the V counter move to the
   next line at the end of the active display (machine/megadriv.c), so from
   there the next line counts as started and a change goes to the line after
   it */
int segavdp_drawn_line(void)
{
	int line = segavdp_scanline();
	if (vdp.intf.shadow_base && segavdp_horzbeampos() >= Machine->drv->screen_width * 320 / 342)
		line++;
	return line;
}

double segavdp_scanlinetime(int line)
{
	return cpu_getscanlinetime(line * vdp.intf.line_scale);
}

double segavdp_scanlineperiod(void)
{
	return cpu_getscanlineperiod() * vdp.intf.line_scale;
}

void segavdp_draw_line(UINT16 *dest, int line, int bgfill)
{
	drawline(dest, line, bgfill);
}



/******************************************************************************
	VDP Read & Write Handlers
*******************************************************************************

	The VDP is accessed via 8 & 16 bit reads / writes at the addresses 0xC00000 -
	0xC0001F.  Different locations have different functions in the following
	layout.  (based on Information in Charles MacDonalds Document)

	0xC00000 -   DATA port (8 or 16 bit, Read or Write)
	0xC00002 -   mirror of above
	0xC00004 -   CTRL port (8 or 16 bit, Read or Write)
	0xC00006 -   mirror of above
	0xC00008 -   HV counter (8 or 16 bit Read Only)
	0xC0000A -   mirror of above
	0xC0000C -   mirror of above
	0xC0000E -   mirror of above
	0xC00010 -   SN76489 (8 bit Write Only)
	0xC00012 -   mirror of above
	0xC00014 -   mirror of above
	0xC00016 -   mirror of above

	The SN76489 PSG Writes are intercepted before we get here and handled
	directly.

******************************************************************************/

READ16_HANDLER( segavdp_r )
{
	switch (offset)
	{
		case 0x00:	/* Read Data */
		case 0x01:
			return vdp_data_r();

		case 0x02:	/* Status Register */
		case 0x03:
			return vdp_control_r();

		case 0x04:	/* HV counter */
		case 0x05:
		case 0x06:
		case 0x07:
		{
			int xpos = segavdp_horzbeampos();
			int ypos = segavdp_scanline();

			/* the V counter moves to the next line at the end of the active
			   display, when H-INT is raised (machine/megadriv.c) */
			if (xpos >= Machine->drv->screen_width * 320 / 342)
				ypos = (ypos + 1) % 262;

			/* adjust for the weird counting rules */
			if (xpos > 0xe9) xpos -= (342 - 0x100);
			if (ypos > 0xea) ypos -= (262 - 0x100);

			/* the V counter is the line, as current MAME (H-INT is raised at
			   the start of its line, machine/megadriv.c): no more Ichidant-R
			   kludge (2 lines less, for the H-INT at the end of the next line) */
			return (ypos << 8) | xpos;
		}
	}
	return 0;
}


WRITE16_HANDLER( segavdp_w )
{
	switch (offset)
	{
		case 0x00:	/* Write data */
		case 0x01:
			if (mem_mask)
			{
				data &= ~mem_mask;
				 if (ACCESSING_MSB)
				 	data |= data >> 8;
				 else
				 	data |= data << 8;
			}
			vdp_data_w(data);
			break;

		case 0x02:	/* Control Write */
		case 0x03:
			if (mem_mask)
			{
				data &= ~mem_mask;
				 if (ACCESSING_MSB)
				 	data |= data >> 8;
				 else
				 	data |= data << 8;
			}
			vdp_control_w(data);
			break;
	}
}



/******************************************************************************
	VDP DATA Reads / Writes (Accesses to 0xC00000 - 0xC00003)
*******************************************************************************

	The functions here are called by the read / write handlers for the VDP.
	They deal with Data Reads and Writes to and from the VDP Chip.

	Read / write mode is set by Writes to VDP Control, Can't Read when in
	write mode or vice versa.

******************************************************************************/

/* Games needing Read to Work .. bloxeed (attract) .. puyo puyo .. probably more */
static int vdp_data_r(void)
{
	int read = 0;

	/* kill 2nd write pending flag */
	vdp.cmdpart = 0;

	/* which memory is based on the code */
	switch (vdp.code & 0x0f)
	{
		case 0x00:		/* VRAM read */
			read = VDP_VRAM_WORD(vdp.address & ~1);
			break;

		case 0x04:		/* VSRAM read */
			read = VDP_VSRAM_WORD(vdp.address & ~1);
			break;

		default:		/* Illegal read attempt */
			log_cb(RETRO_LOG_DEBUG, LOGPRE "%06x: VDP illegal read type %02x\n", activecpu_get_previouspc(), vdp.code);
			read = 0x00;
			break;
	}

	/* advance the address */
	vdp.address += vdp.regs[15];
	return read;
}


static void vdp_data_w(int data)
{
	/* kill 2nd write pending flag */
	vdp.cmdpart = 0;

	/* handle the fill case */
	if (vdp.dmafill)
	{
		if (!vdp.internal_vblank && vdp.intf.shadow_base)
			force_partial_update(segavdp_drawn_line() + vdp.intf.scanbase);
		vdp_dma_fill(data);
		vdp.dmafill = 0;
		return;
	}

	/* which memory is based on the code */
	switch (vdp.code & 0x0f)
	{
		case 0x01:		/* VRAM write */

			/* if the hscroll RAM is changing during screen refresh, force an
			   update; on the boards of the 315-5313 any VRAM change, as current
			   MAME draws each line at its start */
			if (!vdp.internal_vblank &&
				(vdp.intf.shadow_base ||
				 (vdp.address >= vdp.hscrollbase &&
				  vdp.address < vdp.hscrollbase + vdp.hscrollsize)))
				force_partial_update(segavdp_drawn_line() + vdp.intf.scanbase);

			/* write to VRAM */
			if (vdp.address & 1)
				data = ((data & 0xff) << 8) | ((data >> 8) & 0xff);
			VDP_VRAM_BYTE(vdp.address & ~1) = data >> 8;
			VDP_VRAM_BYTE(vdp.address |  1) = data;
			break;


		case 0x03:		/* Palette write - puckpkmn */
			if (vdp.intf.cram_mode == SEGAVDP_CRAM_DIRECT)
			{
				/* System 18: the 64 VDP colors live after the System 16 palette,
				   not in paletteram16 (only 0x800 entries there) */
				int r = data & 0x0f, g = (data >> 4) & 0x0f, b = (data >> 8) & 0x0f;
				palette_set_color(vdp.intf.cram_base + ((vdp.address / 2) & 0x3f), (r << 4) | r, (g << 4) | g, (b << 4) | b);
			}
			else
			{
				paletteram16_xxxxBBBBGGGGRRRR_word_w(vdp.address/2, data, 0);

				/* Mega Drive boards: the shadow and highlight colors of the
				   entry (half the 3-bit level, and half plus 7 of 14), as the
				   315-5313 of current MAME */
				if (vdp.intf.shadow_base && vdp.intf.shadow_base < 0x800)
				{
					int entry = (vdp.address / 2) & 0x3f;
					int r = (data >> 1) & 7, g = (data >> 5) & 7, b = (data >> 9) & 7;
					palette_set_color(entry + vdp.intf.shadow_base, r * 0x11, g * 0x11, b * 0x11);
					palette_set_color(entry + vdp.intf.hilite_base, (7 + r) * 0x11, (7 + g) * 0x11, (7 + b) * 0x11);
				}
			}
			break;


		case 0x05:		/* VSRAM write */

			/* if the vscroll RAM is changing during screen refresh, force an update */
			if (!vdp.internal_vblank)
				force_partial_update(segavdp_drawn_line() + vdp.intf.scanbase);

			/* write to VSRAM */
			if (vdp.address & 1)
				data = ((data & 0xff) << 8) | ((data >> 8) & 0xff);
			VDP_VSRAM_BYTE(vdp.address & ~1) = data >> 8;
			VDP_VSRAM_BYTE(vdp.address |  1) = data;
			break;

		default:		/* Illegal write attempt */
			log_cb(RETRO_LOG_DEBUG, LOGPRE "PC:%06x: VDP illegal write type %02x data %04x\n", activecpu_get_previouspc(), vdp.code, data);
			break;
	}

	/* advance the address */
	vdp.address += vdp.regs[15];
}



/******************************************************************************
	VDP CTRL Reads / Writes (Accesses to 0xC00004 - 0xC00007)
*******************************************************************************

	A Read from the Control Port will return the Status Register Value.
	16-bits are used to report the VDP status
	|  0     |  0     |  1     |  1     |  0     |  1     |  FIFE  |  FIFF  |
	|  VIP   |  SOF   |  SCL   |  ODD   |  VBLK  |  HBLK  |  DMA   |  PAL   |

		0,1 = Set Values
		FIFE = FIFO Empty
		FIFF = FIFO Full
		VIP  = Vertical Interrupt Pending
		SOF  = Sprite Overflow  (Not used in C2 afaik)
		SCL  = Sprite Collision (Not used in C2 afaik)
		ODD  = Odd Frame (Interlace Mode) (Not used in C2 afaik)
		VBLK = In Vertical Blank
		HBLK = In Horizontal Blank
		DMA  = DMA In Progress
		PAL  = Pal Mode Flag

	Control Writes are used for setting VDP Registers, setting up DMA Transfers
	etc.

	A Write to the Control port can consist of 2 16-bit Words.
	When the VDP _isn't_ expecting the 2nd part of a command the highest 2 bits
	of the data written will determine what it is we want to do.
	10xxxxxx xxxxxxxx will cause a register to be set
	anything else will trigger the 1st half of a 2 Part Mode Setting Command
	If the VDP is already expecting the 2nd half of a command the data written
	will be the 2nd half of the Mode setup.

******************************************************************************/

static int vdp_control_r(void)
{
	int beampos = segavdp_horzbeampos();
	int status = 0x3600; /* wwally needs fifo empty set */

	/* kill 2nd write pending flag */
	vdp.cmdpart = 0;

	/* set the VBLANK bit */
	if (vdp.internal_vblank)
		status |= 0x0008;

	/* V-INT happened (Puyo Puyo 2 keeps the status from its V-INT handler) */
	if (vdp.irq6_pending)
		status |= 0x0080;

	/* sprite collision */
	if (vdp.sprite_collision)
		status |= 0x0020;

	/* set the HBLANK bit */
	{
		if (vdp.iack_mode)
		{
			/* segavdp_horzbeampos() runs 0..screen_width-1 over the whole line, so in
			   H40 mode (visible area 0-319) the old test below was never true and
			   HBLANK never appeared. Mega Play: HBLANK for the last fifth of the line
			   (the same window the old test gave in H32 mode). Grand Slam waits on
			   HBLANK edges to send one long per line and hung forever. */
			if (beampos >= (Machine->drv->screen_width * 4) / 5)
				status |= 0x0004;
		}
		else if (beampos < Machine->visible_area.min_x || beampos > Machine->visible_area.max_x)
			status |= 0x0004;
	}

	return (status);
}


static void vdp_control_w(int data)
{
	/* case 1: we're not expecting the 2nd half of a command */
	if (!vdp.cmdpart)
	{
		/* if 10xxxxxx xxxxxxxx this is a register setting command */
		if ((data & 0xc000) == 0x8000)
			vdp_register_w(data);

		/* otherwise this is the First part of a mode setting command */
		else
		{
			vdp.code    = (vdp.code & 0x3c) | ((data >> 14) & 0x03);
			vdp.address = (vdp.address & 0xc000) | (data & 0x3fff);
			vdp.cmdpart = 1;
		}
	}

	/* case 2: this is the 2nd part of a mode setting command */
	else
	{
		vdp.code    = (vdp.code & 0x03) | ((data >> 2) & 0x3c);
		vdp.address = (vdp.address & 0x3fff) | ((data << 14) & 0xc000);
		vdp.cmdpart = 0;
		vdp_control_dma(data);
	}
}


static void vdp_register_w(int data)
{
	int scrwidth = 0;
	/* R1 (display enable) and R12 (shadow/highlight) are read directly by the
	   renderer, so a mid-screen write must flush the lines above too
	   (Gunstar Heroes switches the display on/off from H-INT for its letterbox) */
	static const UINT8 is_important[32] = { 0,1,1,1,1,1,0,1,0,0,0,1,1,1,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0 };

	UINT8 regnum = (data & 0x1f00) >> 8; /* ---R RRRR ---- ---- */
	UINT8 regdat = (data & 0x00ff);      /* ---- ---- DDDD DDDD */

	/* these are mostly important writes; force an update if they */
	/* are written during a screen refresh. This must happen BEFORE the
	   store, otherwise the lines already displayed get the new value */
	if (!vdp.internal_vblank && is_important[regnum] && vdp.regs[regnum] != regdat)
		force_partial_update(segavdp_drawn_line() + vdp.intf.scanbase);

	vdp.regs[regnum] = regdat;

	/* interrupt enables changed: re-evaluate the 68000 IRQ line (Mega Play) */
	if (regnum <= 1 && vdp.intf.irq_update)
		(*vdp.intf.irq_update)();

	/* For quite a few of the registers its a good idea to set a couple of variable based
	   upon the writes here */
	switch (regnum)
	{
		case 0x01: /* video modes */
			if (regdat & 8)
				usrintf_showmessage("Video height = 240!");
			break;

		case 0x02: /* Scroll A Name Table Base */
			vdp.scrollabase = (regdat & 0x38) << 10;
			break;

		case 0x03: /* Window Name Table Base */
			vdp.windowbase = (regdat & 0x3e) << 10;
			break;

		case 0x04: /* Scroll B Name Table Base */
			vdp.scrollbbase = (regdat & 0x07) << 13;
			break;

		case 0x05: /* Sprite Table Base: bit 0 (AT9) is used in H32 mode only */
			vdp.spritebase = (regdat & ((vdp.regs[12] & 0x01) ? 0x7e : 0x7f)) << 9;
			break;

		case 0x07: /* BG Colour */
			vdp.bgcol = regdat & 0x3f;
			break;

		case 0x0b: /* Scroll Modes */
		{
			static const UINT16 mask_table[4] = { 0x000, 0x007, 0xff8, 0xfff };
			vdp.vscrollmode = (regdat & 0x04) >> 2;

			vdp.hscrollmask = mask_table[regdat & 3];
			vdp.hscrollsize = 4 * ((vdp.hscrollmask < 224) ? (vdp.hscrollmask + 1) : 224);
			break;
		}

		case 0x0c: /* video modes */
			/* HDG: taken from mess/machine/genesis.c */
			switch (regdat&0x81)
			{
				case 0x00: /* 32 cell */
					scrwidth=32;
					vdp.window_width=32;
				break;
				case 0x01: /* 40 cell corrupted */
					scrwidth=40;
					vdp.window_width=64;
				break;
				case 0x80: /* illegal! */
					scrwidth=40;
					vdp.window_width=64;
				break;
				case 0x81: /* 40 cell */
					scrwidth=40;
					vdp.window_width=64;
				break;
			}
			if (vdp.intf.set_visible_area)
				set_visible_area(0, scrwidth*8-1,
				Machine->visible_area.min_y,
				Machine->visible_area.max_y);
			/* the sprite table base depends on the H40 / H32 mode */
			vdp.spritebase = (vdp.regs[5] & ((regdat & 0x01) ? 0x7e : 0x7f)) << 9;
			break;

		case 0x0d: /* HScroll Base */
			vdp.hscrollbase = (regdat & 0x3f) << 10;
			break;

		case 0x10: /* Scroll Size */
		{
			static const UINT8 size_table[4] = { 32, 64, 128, 128 };
			vdp.scrollwidth = size_table[regdat & 0x03];
			vdp.scrollheight = size_table[(regdat & 0x30) >> 4];
			break;
		}

		case 0x11: /* Window H Position .. Doesn't Matter for any of the C2 Games */
			vdp.window_right = regdat & 0x80;
			vdp.window_hpos = (regdat & 0x1f) << 4;
			break;

		case 0x12: /* Window V Position */
			vdp.window_down = regdat & 0x80;
			vdp.window_vpos = (regdat & 0x1f) << 3;
			break;
	}
}


static void vdp_control_dma(int data)
{
	if ((vdp.code & 0x20) && (vdp.regs[1] & 0x10))
	{
		switch(vdp.regs[23] & 0xc0)
		{
			case 0x00:
			case 0x40:		/* 68k -> VRAM Tranfser */
				vdp_dma_68k();
				break;

			case 0x80:		/* VRAM fill, can't be done here, requires data write */
				vdp.dmafill = 1;
				break;

			case 0xC0:		/* VRAM copy */
				if (!vdp.internal_vblank && vdp.intf.shadow_base)
					force_partial_update(segavdp_drawn_line() + vdp.intf.scanbase);
				vdp_dma_copy();
				break;
		}
	}
}



/******************************************************************************
	DMA handling
*******************************************************************************

	These are currently Pretty much directly from the C2 emu

******************************************************************************/

static void vdp_dma_68k(void)
{
	int length = vdp.regs[19] | (vdp.regs[20] << 8);
	int source = (vdp.regs[21] << 1) | (vdp.regs[22] << 9) | ((vdp.regs[23] & 0x7f) << 17);
	int count;

	/* length of 0 means 64k likely */
	if (!length)
		length = 0xffff;

	/* handle the DMA */
	for (count = 0; count < length; count++)
	{
		vdp_data_w(cpu_readmem24bew_word(source));
		source += 2;
	}
}


static void vdp_dma_copy(void)
{
	int length = vdp.regs[19] | (vdp.regs[20] << 8);
	int source = vdp.regs[21] | (vdp.regs[22] << 8);
	int count;

	/* length of 0 means 64k likely */
	if (!length)
		length = 0xffff;

	/* handle the DMA */
	for (count = 0; count < length; count++)
	{
		VDP_VRAM_BYTE(vdp.address) = VDP_VRAM_BYTE(source++);
		vdp.address += vdp.regs[15];
	}
}


static void vdp_dma_fill(int data)
{
	int length = vdp.regs[19] | (vdp.regs[20] << 8);
	int count;

	/* length of 0 means 64k likely */
	if (!length)
		length = 0xffff;

	/* handle the fill: length + 1 bytes, and the length registers are left
	   at 0, as current MAME */
	VDP_VRAM_BYTE(vdp.address) = data;
	data >>= 8;
	for(count = 0; count <= length; count++)
	{
		VDP_VRAM_BYTE(vdp.address ^ 1) = data;
		vdp.address += vdp.regs[15];
	}
	vdp.regs[19] = vdp.regs[20] = 0;
}



/******************************************************************************
	Scroll calculations
*******************************************************************************

	These special routines handle the transparency effects in sprites.

******************************************************************************/

/*** Useful Little Functions **************************************************/

/* Note: We Expect plane = 0 for Scroll A, plane = 2 for Scroll B */
static INLINE int vdp_gethscroll(int plane, int line)
{
	line &= vdp.hscrollmask;
	return 0x400 - (VDP_VRAM_WORD(vdp.hscrollbase + (4 * line) + plane) & 0x3ff);
}


/* Note: We expect plane = 0 for Scroll A, plane = 2 for Scroll B
   A Column is 8 Pixels Wide                                     */
static int vdp_getvscroll(int plane, int column)
{
	UINT32 vsramoffset;

	switch (vdp.vscrollmode)
	{
		case 0x00: /* Overall Scroll */
			return VDP_VSRAM_WORD(plane) & 0x7ff;

		case 0x01: /* Column Scroll */
			if (column == 40) column = 39; /* Fix Minor Innacuracy Only affects PotoPoto */
			vsramoffset = (4 * (column >> 1)) + plane;
			return VDP_VSRAM_WORD(vsramoffset) & 0x7ff;
	}
	return 0;
}



/******************************************************************************
	Drawing Functions
*******************************************************************************

	These are used by the Screen Refresh functions to do the actual rendering
	of a screenline to Machine->scrbitmap.

	Draw Planes in Order
		Scroll B Low
		Scroll A Low / Window Low
		Sprites Low
		Scroll B High
		Scroll A High / Window High
		Sprites High

	NOTE: Low Sprites _can_ overlap High sprites, however none of the C2
			games do this ever so its safe to draw them in this order.

******************************************************************************/

/* the sprites of a line as current MAME draws them: in link order, at most
   80 (H40) or 64 (H32) entries of the list and 320 or 256 pixels on the
   line; a sprite at X 0 followed by one at X 1-63 masks the rest of the
   line. limit[i] gets the pixels drawn of each sprite (0: none). */
static int sprite_draw_limit = -1;

static void sprite_line_limits(int line, UINT8 **list, int count, int *limit)
{
	int maxsprites = (vdp.regs[12] & 0x01) ? 80 : 64;
	int maxpixels = (vdp.regs[12] & 0x01) ? 320 : 256;
	int mask = 0, i;

	for (i = 0; i < count; i++)
		limit[i] = 0;
	for (i = 0; i < count && i <= maxsprites; i++)
	{
		UINT8 *sb = list[i];
		int ypos = (((sb[0] & 0x01) << 8) | sb[1]) - 0x80;
		int height = ((sb[2] & 0x03) + 1) * 8;
		int width = (((sb[2] & 0x0c) >> 2) + 1) * 8;
		int xpos = ((sb[6] & 0x01) << 8) | sb[7];

		if (line < ypos || line >= ypos + height)
			continue;
		if (xpos == 0)
			mask |= 1;
		if (xpos > 0 && xpos < 0x40)
			mask |= 2;
		if (mask == 3)
			return;
		if (maxpixels <= width)
		{
			limit[i] = maxpixels;
			return;
		}
		limit[i] = width;
		maxpixels -= width;
	}
}

/* shadow/highlight mode (register 12 bit 3), as the 315-5313 of current MAME:
   the pixels where only low priority layers show are shadowed, high priority
   tiles and sprites show normally; sprite colors 0x3e and 0x3f (palette 3,
   colors 14 and 15) are not drawn but highlight or shadow what is under
   them, and colors 0x0e/0x1e/0x2e always have normal intensity. As on System
   C/C2 in MAME the shadowed and highlighted pixels take the background
   palette. */
static void drawline_ste(UINT16 *bitmap, int line, int bgfill, UINT32 *scrolla_tiles, UINT32 *scrollb_tiles,
		UINT32 *window_tiles, int scrolla_offset, int scrollb_offset, int scrolla_lclip, int scrolla_rclip,
		int window_lclip, int window_rclip, int nsprites)
{
	UINT16 lo_buf[8 + BITMAP_WIDTH + 8], hi_buf[8 + BITMAP_WIDTH + 8], spr_buf[8 + BITMAP_WIDTH + 8];
	UINT8 cover[BITMAP_WIDTH];
	UINT16 *lo = lo_buf + 8, *hi = hi_buf + 8, *spr = spr_buf + 8;
	UINT8 *sprites[81];
	int limits[81];
	int count, link, sprite, x;
	int bgraw = vdp.bgcol & 0x3f;
	int shadow = vdp.regs[12] & 0x08;

	(void)nsprites;
	/* 0x100 marks the backdrop, which takes no background base */
	for (x = -8; x < BITMAP_WIDTH + 8; x++)
	{
		lo[x] = 0x100 | bgraw;
		hi[x] = 0;
		spr[x] = 0;
	}
	memset(cover, 0, sizeof(cover));

	raw_mode = 1;

	/* low priority B, A, window */
	drawline_tiles(scrollb_tiles, lo, 0, scrollb_offset, 0, BITMAP_WIDTH - 1);
	drawline_tiles(scrolla_tiles, lo, 0, scrolla_offset, scrolla_lclip, scrolla_rclip);
	drawline_tiles(window_tiles, lo, 0, 0, window_lclip, window_rclip);

	/* high priority B, A, window, and the pixels they cover */
	raw_cover = cover;
	drawline_tiles(scrollb_tiles, hi, 1, scrollb_offset, 0, BITMAP_WIDTH - 1);
	drawline_tiles(scrolla_tiles, hi, 1, scrolla_offset, scrolla_lclip, scrolla_rclip);
	drawline_tiles(window_tiles, hi, 1, 0, window_lclip, window_rclip);
	raw_cover = NULL;

	/* the sprites of the line in link order; the first one in the list wins */
	count = link = 0;
	for (sprite = 0; sprite < 80; sprite++)
	{
		UINT8 *spritebase = &VDP_VRAM_BYTE(vdp.spritebase + 8 * link);
		sprites[count++] = spritebase;
		link = spritebase[3] & 0x7F;
		if (!link)
			break;
	}
	sprite_line_limits(line, sprites, count, limits);
	for (sprite = count - 1; sprite >= 0; sprite--)
		if (limits[sprite])
		{
			sprite_draw_limit = limits[sprite];
			drawline_sprite(line, spr, (sprites[sprite][4] & 0x80) ? 1 : 0, sprites[sprite]);
		}
	sprite_draw_limit = -1;

	raw_mode = 0;

	/* normal mode: high priority sprites, then high priority tiles, then low
	   priority sprites, then the low priority tiles, as current MAME (the
	   first sprite of the list is the one in the sprite line buffer) */
	if (!shadow)
	{
		for (x = 0; x < BITMAP_WIDTH; x++)
		{
			int v = lo[x], s = spr[x], is_sprite = 0;
			const int *lookup;

			if (s & 0x40)
				v = s & 0x3f, is_sprite = 1;
			if (cover[x] && (hi[x] & 0x0f))
				v = hi[x] & 0x3f, is_sprite = 0;
			if (s & 0x80)
				v = s & 0x3f, is_sprite = 1;
			lookup = is_sprite ? vdp.sp_pal_lookup : (v & 0x100) ? vdp.bd_pal_lookup : vdp.bg_pal_lookup;
			bitmap[x] = (bgfill && (v & 0x100)) ? bgfill : (lookup[(v >> 4) & 3] + (v & 0x0f));
		}
		return;
	}

	/* combine, as current MAME */
	for (x = 0; x < BITMAP_WIDTH; x++)
	{
		int v = lo[x], flags = 0, is_sprite = 0, s = spr[x], idx;
		const int *lookup;

		/* low priority sprite */
		if (s & 0x40)
		{
			int sd = s & 0x3f;
			if (sd == 0x0e || sd == 0x1e || sd == 0x2e)
				v = sd, flags = 0x4000, is_sprite = 1;
			else if (sd == 0x3e)
				flags |= 0x8000;
			else if (sd == 0x3f)
				flags |= 0x2000;
			else
				v = sd, flags = 0, is_sprite = 1;
		}

		/* high priority tiles */
		if (cover[x])
		{
			if (hi[x] & 0x0f)
				v = hi[x] & 0x3f, flags = 0x4000, is_sprite = 0;
			else
				flags |= 0x4000;
		}

		/* high priority sprite */
		if (s & 0x80)
		{
			int sd = s & 0x3f;
			if (sd == 0x3e)
				flags |= 0x8000;
			else if (sd == 0x3f)
				flags |= 0x2000;
			else
				v = sd, flags = 0x4000, is_sprite = 1;
		}

		/* to the palette */
		lookup = is_sprite ? vdp.sp_pal_lookup : (v & 0x100) ? vdp.bd_pal_lookup : vdp.bg_pal_lookup;
		switch (flags)
		{
			case 0x0000: case 0x2000: case 0x6000:		/* shadow */
				idx = ((v & 0x100) ? vdp.bd_pal_lookup : vdp.bg_pal_lookup)[(v >> 4) & 3] + (v & 0x0f) + vdp.intf.shadow_base;
				break;
			case 0xc000:								/* highlight */
				idx = ((v & 0x100) ? vdp.bd_pal_lookup : vdp.bg_pal_lookup)[(v >> 4) & 3] + (v & 0x0f) + vdp.intf.hilite_base;
				break;
			default:									/* normal */
				idx = lookup[(v >> 4) & 3] + (v & 0x0f);
				break;
		}

		bitmap[x] = (bgfill && (v & 0x100)) ? bgfill : idx;
	}
}


static void drawline(UINT16 *bitmap, int line, int bgfill)
{
	int lowsprites, highsprites, link;
	UINT32 scrolla_tiles[41], scrollb_tiles[41], window_tiles[41];
	int scrolla_offset, scrollb_offset;
	UINT8 *lowlist[81], *highlist[81];
	int bgcolor = bgfill ? bgfill : ((vdp.bgcol & 0x0f) | vdp.bd_pal_lookup[(vdp.bgcol >> 4) & 3]);
	int window_lclip, window_rclip;
	int scrolla_lclip, scrolla_rclip;
	int column, sprite;

	/* the board display enable (System C/C2) blanks the line to black, as
	   current MAME; the VDP display enable (register 1) shows the backdrop */
	if (!vdp.display_enable)
		bgcolor = (vdp.intf.black_pen >= 0) ? vdp.intf.black_pen : get_black_pen();

	/* clear to the background color */
		for (column = 0; column < BITMAP_WIDTH; column++)
			bitmap[column] = bgcolor;

	/* if display is disabled, stop */
	if (!(vdp.regs[1] & 0x40) || !vdp.display_enable)
		return;

	/* Sprites need to be Drawn in Reverse order .. may as well sort them here */
	link = lowsprites = highsprites = 0;
	for (sprite = 0; sprite < 80; sprite++)
	{
		UINT8 *spritebase = &VDP_VRAM_BYTE(vdp.spritebase + 8 * link);

		/* sort into high/low priorities */
		if (spritebase[4] & 0x0080)
			highlist[++highsprites] = spritebase;
		else
			lowlist[++lowsprites] = spritebase;

		/* get the link; if 0, stop processing */
		link = spritebase[3] & 0x7F;
		if (!link)
			break;
	}

	/* get tiles for the B scroll layer */
	get_scroll_tiles(line, 2, vdp.scrollbbase, scrollb_tiles, &scrollb_offset);

	/* get tiles for the A scroll layer */
	get_scroll_tiles(line, 0, vdp.scrollabase, scrolla_tiles, &scrolla_offset);

	/* get tiles for the window layer */
	get_window_tiles(line, vdp.windowbase, window_tiles);

	/* compute the windowing for this line */
	if ((vdp.window_down && line >= vdp.window_vpos) || (!vdp.window_down && line < vdp.window_vpos))
		window_lclip = 0, window_rclip = BITMAP_WIDTH - 1;
	else if (vdp.window_right)
		window_lclip = vdp.window_hpos, window_rclip = BITMAP_WIDTH - 1;
	else
		window_lclip = 0, window_rclip = vdp.window_hpos - 1;

	/* compute the clipping of the scroll A layer */
	if (window_lclip == 0)
	{
		scrolla_lclip = window_rclip + 1;
		scrolla_rclip = BITMAP_WIDTH - 1;
	}
	else
	{
		scrolla_lclip = 0;
		scrolla_rclip = window_lclip - 1;
	}

	/* the boards of the 315-5313 (System C/C2, Mega Drive): composed per pixel
	   as current MAME, with its sprite limits and the shadow/highlight mode */
	if (vdp.intf.shadow_base)
	{
		drawline_ste(bitmap, line, bgfill, scrolla_tiles, scrollb_tiles, window_tiles,
				scrolla_offset, scrollb_offset, scrolla_lclip, scrolla_rclip, window_lclip, window_rclip, link);
		return;
	}

	/* Scroll B Low */
	drawline_tiles(scrollb_tiles, bitmap, 0, scrollb_offset, 0, BITMAP_WIDTH - 1);

	/* Scroll A Low */
	drawline_tiles(scrolla_tiles, bitmap, 0, scrolla_offset, scrolla_lclip, scrolla_rclip);

	/* Window Low */
	drawline_tiles(window_tiles, bitmap, 0, 0, window_lclip, window_rclip);

	/* Sprites Low */
	for (sprite = lowsprites; sprite > 0; sprite--)
		drawline_sprite(line, bitmap, 0, lowlist[sprite]);

	/* Scroll B High */
	drawline_tiles(scrollb_tiles, bitmap, 1, scrollb_offset, 0, BITMAP_WIDTH - 1);

	/* Scroll A High */
	drawline_tiles(scrolla_tiles, bitmap, 1, scrolla_offset, scrolla_lclip, scrolla_rclip);

	/* Window High */
	drawline_tiles(window_tiles, bitmap, 1, 0, window_lclip, window_rclip);

	/* Sprites High */
	for (sprite = highsprites; sprite > 0; sprite--)
		drawline_sprite(line, bitmap, 1, highlist[sprite]);
}



/******************************************************************************
	Background rendering
*******************************************************************************

	This code handles all the scroll calculations and rendering of the
	background layers.

******************************************************************************/

/* determine the tiles we will draw on a scrolling layer */
static void get_scroll_tiles(int line, int scrollnum, UINT32 scrollbase, UINT32 *tiles, int *offset)
{
	int linehscroll = vdp_gethscroll(scrollnum, line);
	int column;

	/* adjust for partial tiles and then pre-divide hscroll to get the tile offset */
	*offset = -(linehscroll % 8);
	linehscroll /= 8;

	/* loop over columns */
	for (column = 0; column < 41; column++)
	{
		int columnvscroll, temp, tilebase;

		/* column scroll, as current MAME: each 16 pixel column of the screen
		   takes the vertical scroll of the column before it for its first
		   (hscroll & 7) + 8 pixels when hscroll is not a multiple of 16; the
		   tiles never straddle the change */
		if (vdp.vscrollmode == 0x01)
		{
			int hs = (0x400 - (linehscroll * 8 + (-*offset))) & 0x3ff;
			int x0 = *offset + 8 * column, c, idx;
			if (x0 < 0)
				x0 = 0;
			c = x0 >> 4;
			idx = ((hs & 0x0f) && (x0 & 15) < (hs & 7) + 8) ? c - 1 : c;
			columnvscroll = (VDP_VSRAM_WORD((4 * idx + scrollnum) & 0x7e) & 0x7ff) + line;
		}
		else
			columnvscroll = vdp_getvscroll(scrollnum, column) + line;

		/* determine the base of the tilemap row */
		temp = ((columnvscroll / 8) & (vdp.scrollheight - 1)) * vdp.scrollwidth;
		tilebase = scrollbase + 2 * temp;

		/* offset into the tilemap based on the column */
		temp = (linehscroll + column) & (vdp.scrollwidth - 1);
		tilebase += 2 * temp;

		/* get the tile info */
		*tiles++ = ((columnvscroll % 8) << 16) | VDP_VRAM_WORD(tilebase);
	}
}


/* determine the tiles we will draw on a non-scrolling window layer */
static void get_window_tiles(int line, UINT32 scrollbase, UINT32 *tiles)
{
	int column;

	/* loop over columns */
	for (column = 0; column < 40; column++)
	{
		/* determine the base of the tilemap row */
		int temp = (line / 8) * vdp.window_width + column;
		int tilebase = scrollbase + 2 * temp;

		/* get the tile info */
		*tiles++ = ((line % 8) << 16) | VDP_VRAM_WORD(tilebase);
	}
}


/* draw a line of tiles */
static void drawline_tiles(UINT32 *tiles, UINT16 *bmap, int pri, int offset, int lclip, int rclip)
{
	/* adjust for the 8-pixel slop */
	bmap += offset;
	if (lclip > rclip)
		return;

	/* loop over columns */
	for ( ; offset < BITMAP_WIDTH; offset += 8, bmap += 8)
	{
		UINT32 tile = *tiles++;

		/* if the tile is the correct priority, draw it */
		if (((tile >> 15) & 1) == pri && offset < BITMAP_WIDTH)
		{
			int colbase = raw_mode ? ((tile >> 13) & 3) << 4 : vdp.bg_pal_lookup[(tile >> 13) & 3];
			UINT32 *tp = (UINT32 *)&VDP_VRAM_BYTE((tile & 0x7ff) * 32);
			UINT32 mytile;
			int col;

			/* shadow/highlight: a high priority tile covers its pixels, transparent too */
			if (raw_cover)
			{
				int x;
				for (x = offset; x < offset + 8; x++)
					if (x >= 0 && x < BITMAP_WIDTH && x >= lclip && x <= rclip)
						raw_cover[x] = 1;
			}

			/* vertical flipping */
			if (!(tile & 0x1000))
				mytile = tp[tile >> 16];
			else
				mytile = tp[(tile >> 16) ^ 7];

			/* skip if all-transparent */
			if (!mytile)
				continue;

			/* non-clipped */
			if (offset >= lclip && offset <= rclip - 7)
			{
				/* non-flipped */
				if (!(tile & 0x0800))
				{
					col = EXTRACT_PIXEL(mytile, 0); if (col) bmap[0] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 1); if (col) bmap[1] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 2); if (col) bmap[2] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 3); if (col) bmap[3] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 4); if (col) bmap[4] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 5); if (col) bmap[5] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 6); if (col) bmap[6] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 7); if (col) bmap[7] = colbase + col;
				}

				/* horizontal flip */
				else
				{
					col = EXTRACT_PIXEL(mytile, 7); if (col) bmap[0] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 6); if (col) bmap[1] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 5); if (col) bmap[2] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 4); if (col) bmap[3] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 3); if (col) bmap[4] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 2); if (col) bmap[5] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 1); if (col) bmap[6] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 0); if (col) bmap[7] = colbase + col;
				}
			}

			/* clipped */
			else if (offset >= lclip - 8 && offset <= rclip)
			{
				/* non-flipped */
				if (!(tile & 0x0800))
				{
					col = EXTRACT_PIXEL(mytile, 0); if (col && (offset + 0) >= lclip && (offset + 0) <= rclip) bmap[0] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 1); if (col && (offset + 1) >= lclip && (offset + 1) <= rclip) bmap[1] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 2); if (col && (offset + 2) >= lclip && (offset + 2) <= rclip) bmap[2] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 3); if (col && (offset + 3) >= lclip && (offset + 3) <= rclip) bmap[3] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 4); if (col && (offset + 4) >= lclip && (offset + 4) <= rclip) bmap[4] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 5); if (col && (offset + 5) >= lclip && (offset + 5) <= rclip) bmap[5] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 6); if (col && (offset + 6) >= lclip && (offset + 6) <= rclip) bmap[6] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 7); if (col && (offset + 7) >= lclip && (offset + 7) <= rclip) bmap[7] = colbase + col;
				}

				/* horizontal flip */
				else
				{
					col = EXTRACT_PIXEL(mytile, 7); if (col && (offset + 0) >= lclip && (offset + 0) <= rclip) bmap[0] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 6); if (col && (offset + 1) >= lclip && (offset + 1) <= rclip) bmap[1] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 5); if (col && (offset + 2) >= lclip && (offset + 2) <= rclip) bmap[2] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 4); if (col && (offset + 3) >= lclip && (offset + 3) <= rclip) bmap[3] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 3); if (col && (offset + 4) >= lclip && (offset + 4) <= rclip) bmap[4] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 2); if (col && (offset + 5) >= lclip && (offset + 5) <= rclip) bmap[5] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 1); if (col && (offset + 6) >= lclip && (offset + 6) <= rclip) bmap[6] = colbase + col;
					col = EXTRACT_PIXEL(mytile, 0); if (col && (offset + 7) >= lclip && (offset + 7) <= rclip) bmap[7] = colbase + col;
				}
			}
		}
	}
}



/******************************************************************************
	Sprite drawing
*******************************************************************************

	These special routines handle the transparency effects in sprites.

******************************************************************************/

/* draw a non-horizontally-flipped section of a sprite */
static INLINE void draw8pixs(UINT16 *bmap, int patno, int priority, int colbase, int patline)
{
	UINT32 tile = *(UINT32 *)&VDP_VRAM_BYTE(patno * 32 + 4 * patline);
	int col;

	/* skip if all-transparent */
	if (!tile)
		return;

	/* non-transparent */
	if (raw_mode || (colbase & 0x30) != 0x30 || !(vdp.regs[12] & 0x08))
	{
		col = EXTRACT_PIXEL(tile, 0); if (col) bmap[0] = colbase + col;
		col = EXTRACT_PIXEL(tile, 1); if (col) bmap[1] = colbase + col;
		col = EXTRACT_PIXEL(tile, 2); if (col) bmap[2] = colbase + col;
		col = EXTRACT_PIXEL(tile, 3); if (col) bmap[3] = colbase + col;
		col = EXTRACT_PIXEL(tile, 4); if (col) bmap[4] = colbase + col;
		col = EXTRACT_PIXEL(tile, 5); if (col) bmap[5] = colbase + col;
		col = EXTRACT_PIXEL(tile, 6); if (col) bmap[6] = colbase + col;
		col = EXTRACT_PIXEL(tile, 7); if (col) bmap[7] = colbase + col;
	}

	/* transparent */
	else
	{
		col = EXTRACT_PIXEL(tile, 0);
		if (col)
		{
			if (col < 0x0e) bmap[0] = colbase + col;
			else bmap[0] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[0] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 1);
		if (col)
		{
			if (col < 0x0e) bmap[1] = colbase + col;
			else bmap[1] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[1] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 2);
		if (col)
		{
			if (col < 0x0e) bmap[2] = colbase + col;
			else bmap[2] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[2] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 3);
		if (col)
		{
			if (col < 0x0e) bmap[3] = colbase + col;
			else bmap[3] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[3] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 4);
		if (col)
		{
			if (col < 0x0e) bmap[4] = colbase + col;
			else bmap[4] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[4] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 5);
		if (col)
		{
			if (col < 0x0e) bmap[5] = colbase + col;
			else bmap[5] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[5] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 6);
		if (col)
		{
			if (col < 0x0e) bmap[6] = colbase + col;
			else bmap[6] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[6] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 7);
		if (col)
		{
			if (col < 0x0e) bmap[7] = colbase + col;
			else bmap[7] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[7] & 0x7ff)];
		}
	}
}


/* draw a horizontally-flipped section of a sprite */
static INLINE void draw8pixs_hflip(UINT16 *bmap, int patno, int priority, int colbase, int patline)
{
	UINT32 tile = *(UINT32 *)&VDP_VRAM_BYTE(patno * 32 + 4 * patline);
	int col;

	/* skip if all-transparent */
	if (!tile)
		return;

	/* non-transparent */
	if (raw_mode || (colbase & 0x30) != 0x30 || !(vdp.regs[12] & 0x08))
	{
		col = EXTRACT_PIXEL(tile, 7); if (col) bmap[0] = colbase + col;
		col = EXTRACT_PIXEL(tile, 6); if (col) bmap[1] = colbase + col;
		col = EXTRACT_PIXEL(tile, 5); if (col) bmap[2] = colbase + col;
		col = EXTRACT_PIXEL(tile, 4); if (col) bmap[3] = colbase + col;
		col = EXTRACT_PIXEL(tile, 3); if (col) bmap[4] = colbase + col;
		col = EXTRACT_PIXEL(tile, 2); if (col) bmap[5] = colbase + col;
		col = EXTRACT_PIXEL(tile, 1); if (col) bmap[6] = colbase + col;
		col = EXTRACT_PIXEL(tile, 0); if (col) bmap[7] = colbase + col;
	}

	/* transparent */
	else
	{
		col = EXTRACT_PIXEL(tile, 7);
		if (col)
		{
			if (col < 0x0e) bmap[0] = colbase + col;
			else bmap[0] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[0] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 6);
		if (col)
		{
			if (col < 0x0e) bmap[1] = colbase + col;
			else bmap[1] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[1] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 5);
		if (col)
		{
			if (col < 0x0e) bmap[2] = colbase + col;
			else bmap[2] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[2] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 4);
		if (col)
		{
			if (col < 0x0e) bmap[3] = colbase + col;
			else bmap[3] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[3] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 3);
		if (col)
		{
			if (col < 0x0e) bmap[4] = colbase + col;
			else bmap[4] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[4] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 2);
		if (col)
		{
			if (col < 0x0e) bmap[5] = colbase + col;
			else bmap[5] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[5] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 1);
		if (col)
		{
			if (col < 0x0e) bmap[6] = colbase + col;
			else bmap[6] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[6] & 0x7ff)];
		}
		col = EXTRACT_PIXEL(tile, 0);
		if (col)
		{
			if (col < 0x0e) bmap[7] = colbase + col;
			else bmap[7] = vdp.transparent_lookup[((col & 1) << 11) | (bmap[7] & 0x7ff)];
		}
	}
}


static void drawline_sprite(int line, UINT16 *bmap, int priority, UINT8 *spritebase)
{
	int spriteypos   = (((spritebase[0] & 0x01) << 8) | spritebase[1]) - 0x80;
	int spritexpos   = (((spritebase[6] & 0x01) << 8) | spritebase[7]) - 0x80;
	int spriteheight = ((spritebase[2] & 0x03) + 1) * 8;
	int spritewidth  = (((spritebase[2] & 0x0c) >> 2) + 1) * 8;
	int spriteattr, patno, patflip, patline, colbase, x;

	UINT16 keep[32], *line_base = bmap;
	int keep_from = 0, keep_to = 0;

	/* skip if out of range */
	if (line < spriteypos || line >= spriteypos + spriteheight)
		return;
	if (spritexpos + spritewidth < 0 || spritexpos >= BITMAP_WIDTH)
		return;

	/* only the first sprite_draw_limit pixels (sprite_line_limits) */
	if (sprite_draw_limit >= 0 && sprite_draw_limit < spritewidth)
	{
		keep_from = spritexpos + sprite_draw_limit;
		keep_to = spritexpos + spritewidth;
		if (keep_from < 0) keep_from = 0;
		if (keep_to > BITMAP_WIDTH) keep_to = BITMAP_WIDTH;
		for (x = keep_from; x < keep_to; x++)
			keep[x - keep_from] = bmap[x];
	}

	/* extract the remaining data */
	spriteattr = (spritebase[4] << 8) | spritebase[5];
	patno      = spriteattr & 0x07FF;
	patflip    = (spriteattr & 0x1800) >> 11;
	patline    = line - spriteypos;

	/* determine the color base */
	if (raw_mode)
		colbase = (((spriteattr >> 13) & 3) << 4) | ((spriteattr & 0x8000) ? 0x80 : 0x40);
	else
		colbase = vdp.sp_pal_lookup[(spriteattr >> 13) & 3];

	/* adjust for the X position */
	spritewidth >>= 3;
	spriteheight >>= 3;

	/* switch off the flip mode */
	bmap += spritexpos;
	switch (patflip)
	{
		case 0x00: /* No Flip */
			for (x = 0; x < spritewidth; x++, bmap += 8)
			{
				if (spritexpos >= -7 && spritexpos < BITMAP_WIDTH)
					draw8pixs(bmap, patno, priority, colbase, patline);
				spritexpos += 8;
				patno += spriteheight;
			}
			break;

		case 0x01: /* Horizontal Flip */
			patno += spriteheight * (spritewidth - 1);
			for (x = 0; x < spritewidth; x++, bmap += 8)
			{
				if (spritexpos >= -7 && spritexpos < BITMAP_WIDTH)
					draw8pixs_hflip(bmap, patno, priority, colbase, patline);
				spritexpos += 8;
				patno -= spriteheight;
			}
			break;

		case 0x02: /* Vertical Flip */
			patline = 8 * spriteheight - patline - 1;
			for (x = 0; x < spritewidth; x++, bmap += 8)
			{
				if (spritexpos >= -7 && spritexpos < BITMAP_WIDTH)
					draw8pixs(bmap, patno, priority, colbase, patline);
				spritexpos += 8;
				patno += spriteheight;
			}
			break;

		case 0x03: /* Both Flip */
			patno += spriteheight * (spritewidth - 1);
			patline = 8 * spriteheight - patline - 1;
			for (x = 0; x < spritewidth; x++, bmap += 8)
			{
				if (spritexpos >= -7 && spritexpos < BITMAP_WIDTH)
					draw8pixs_hflip(bmap, patno, priority, colbase, patline);
				spritexpos += 8;
				patno -= spriteheight;
			}
			break;
	}

	/* put back the pixels past the limit */
	for (x = keep_from; x < keep_to; x++)
		line_base[x] = keep[x - keep_from];
}

/******************************************************************************/



/******************************************************************************/
/* General Information (Mainly Genesis Related                                */
/******************************************************************************/
/* Genesis VDP Registers (from sega2.doc)

Reg# : |  Bit7  |  Bit6  |  Bit5  |  Bit4  |  Bit3  |  Bit2  |  Bit1  |  Bit0  |    General Function
--------------------------------------------------------------------------------
0x00 : |  0     |  0     |  0     |  IE1   |  0     |  1     |  M3    |  0     |    Mode Set Register #1
IE = Enabled H Interrupt (Lev 4), M3 = Enable HV Counter Read / Stopped
0x01 : |  0     |  DISP  |  IE0   |  M1    |  M2    |  1     |  0     |  0     |    Mode Set Register #2
DISP = Display Enable, IE0 = Enabled V Interrupt (Lev 6), M1 = DMA Enabled, M2 = 30 Cell Mode
0x02 : |  0     |  0     |  SA15  |  SA14  |  SA13  |  0     |  0     |  0     |    Scroll A Base Address
SA13-15 = Bits 13-15 of the Scroll A Name Table Base Address in VRAM
0x03 : |  0     |  0     |  WD15  |  WD14  |  WD13  |  WD12  |  WD11  |  0     |    Window Base Address
WD11-15 = Bits 11-15 of the Window Name Table Base Address in VRAM
0x04 : |  0     |  0     |  0     |   0    |  0     |  SB15  |  SB14  |  SB13  |    Scroll B Base Address
SB13-15 = Bits 13-15 of the Scroll B Name Table Base Address in VRAM
0x05 : |  0     |  AT15  |  AT14  |  AT13  |  AT12  |  AT11  |  AT10  |  AT9   |    Sprite Table Base Address
AT9=15 = Bits 9-15 of the Sprite Name Table Base Address in VRAM
0x06 : |  0     |  0     |  0     |  0     |  0     |  0     |  0     |  0     |    Unused
0x07 : |  0     |  0     |  CPT1  |  CPT0  |  COL3  |  COL2  |  COL1  |  COL0  |    Background Colour Select
CPT0-1 = Palette Number, COL = Colour in Palette
0x08 : |  0     |  0     |  0     |  0     |  0     |  0     |  0     |  0     |    Unused
0x09 : |  0     |  0     |  0     |  0     |  0     |  0     |  0     |  0     |    Unused
0x0A : |  BIT7  |  BIT6  |  BIT5  |  BIT4  |  BIT3  |  BIT2  |  BIT1  |  BIT0  |    H-Interrupt Register
BIT0-7 = Controls Level 4 Interrupt Timing
0x0B : |  0     |  0     |  0     |  0     |  IE2   |  VSCR  |  HSCR  |  LSCR  |    Mode Set Register #3
IE2 = Enable E Interrupt (Lev 2), VSCR = Vertical Scroll Mode, HSCR / LSCR = Horizontal Scroll Mode
0x0C : |  RS0   |  0     |  0     |  0     |  S/TE  |  LSM1  |  LSM0  |  RS1   |    Mode Set Register #4
RS0 / RS1 = Cell Mode, S/TE = Shadow/Hilight Enable, LSM0 / LSM1 = Interlace Mode Setting
0x0D : |  0     |  0     |  HS15  |  HS14  |  HS13  |  HS12  |  HS11  |  HS10  |    HScroll Base Address
HS10-15 = Bits 10-15 of the HScroll Name Table Base Address in VRAM
0x0E : |  0     |  0     |  0     |  0     |  0     |  0     |  0     |  0     |    Unused
0x0F : |  INC7  |  INC6  |  INC5  |  INC4  |  INC3  |  INC2  |  INC1  |  INC0  |    Auto-Increment
INC0-7 = Auto Increment Value (after VRam Access)
0x10 : |  0     |  0     |  VSZ1  |  VSZ0  |  0     |  0     |  HSZ1  |  HSZ0  |    Scroll Size
VSZ0-1 = Vertical Plane Size, HSZ0-1 = Horizontal Plane Size
0x11 : |  RIGT  |  0     |  0     |  WHP5  |  WHP4  |  WHP3  |  WHP2  |  WHP1  |    Window H Position
RIGT = Window Right Side of Base Point, WHP1-5 = Bits1-5 of Window H Point
0x12 : |  DOWN  |  0     |  0     |  WVP4  |  WVP3  |  WVP2  |  WVP1  |  WVP0  |    Window V Position
DOWN = Window Below Base Point, WVP0-4 = Bits0-4 of Window V Point
0x13 : |  LG7   |  LG6   |  LG5   |  LG4   |  LG3   |  LG2   |  LG1   |  LG0   |    DMA Length Counter LOW
0x14 : |  LG15  |  LG14  |  LG13  |  LG12  |  LG11  |  LG10  |  LG9   |  LG8   |    DMA Length Counter HIGH
LG0-15 = Bits 0-15 of DMA Length Counter
0x15 : |  SA8   |  SA7   |  SA6   |  SA5   |  SA4   |  SA3   |  SA2   |  SA1   |    DMA Source Address LOW
0x16 : |  SA16  |  SA15  |  SA14  |  SA13  |  SA12  |  SA11  |  SA10  |  SA9   |    DMA Source Address MID
0x17 : |  DMD1  |  DMD0  |  SA22  |  SA21  |  SA20  |  SA19  |  SA18  |  S17   |    DMA Source Address HIGH
LG0-15 = Bits 1-22 of DMA Source Address
DMD0-1 = DMA Mode

Memory Layouts ...

Scroll Name Table
16-bits are used to Define a Tile in the Scroll Plane
|  PRI   |  CP1   |  CP0   |  VF    |  HF    |  PT10  |  PT9   |  PT8   |
|  PT7   |  PT6   |  PT5   |  PT4   |  PT3   |  PT2   |  PT1   |  PT0   |
PRI = Priority, CP = Colour Palette, VF = VFlip, HF = HFlip, PT0-9 = Tile # in VRAM

HScroll Data  (in VRAM)
0x00 |
0x01 / H Scroll of Plane A (Used in Overall, Cell & Line Modes)
0x02 |
0x03 / H Scroll of Plane B (Used in Overall, Cell & Line Modes)
0x04 |
0x05 / H Scroll of Plane A (Used in Line Mode)
0x06 |
0x07 / H Scroll of Plane B (Used in Line Mode)
...
0x20 |
0x21 / H Scroll of Plane A (Used in Cell & Line Mode)
0x22 |
0x23 / H Scroll of Plane B (Used in Cell & Line Mode)
etc.. That kinda thing :)
Data is in Format ..
|  x     |  x     |  x     |  x     |  x     |  x     |  HS9   |  HS8   |
|  HS7   |  HS6   |  HS5   |  HS4   |  HS3   |  HS2   |  HS1   |  HS0   |
HS = HScroll Amount for Overall / Cell / Line depending on Mode.

VScroll Data (in VSRAM)
0x00 |
0x01 / V Scroll of Plane A (Used in Overall & Cell Modes)
0x02 |
0x03 / V Scroll of Plane B (Used in Overall & Cell Modes)
0x04 |
0x05 / V Scroll of Plane A (Used in Cell Mode)
0x06 |
0x07 / V Scroll of Plane B (Used in Cell Modes)
etc..
Data is in Format ..
|  x     |  x     |  x     |  x     |  x     |  VS10  |  VS9   |  VS8   |
|  VS7   |  VS6   |  VS5   |  VS4   |  VS3   |  VS2   |  VS1   |  VS0   |
VS = HScroll Amount for Overall / Cell / Line depending on Mode.

Sprite Attributes Table (in VRAM)
Each Sprite uses 64-bits of Information (8 bytes)
|  x     |  x     |  x     |  x     |  x     |  x     |  YP9   |  YP8   |
|  YP7   |  YP6   |  YP5   |  YP4   |  YP3   |  YP2   |  YP1   |  YP0   |
|  x     |  x     |  x     |  x     |  HS1   |  HS0   |  VS1   |  VS0   |
|  x     |  LN6   |  LN5   |  LN4   |  LN3   |  LN2   |  LN1   |  LN0   |
|  PRI   |  CP1   |  CP0   |  VF    |  HF    |  PT10  |  PT9   |  PT8   |
|  PT7   |  PT6   |  PT5   |  PT4   |  PT3   |  PT2   |  PT1   |  PT0   |
|  x     |  x     |  x     |  x     |  x     |  x     |  XP9   |  XP8   |
|  XP7   |  XP6   |  XP5   |  XP4   |  XP3   |  XP2   |  XP1   |  XP0   |
YP = Y Position of Sprite
HS = Horizontal Size (Blocks)
VS = Vertical Size (Blocks)
LN = Link Field
PRI = Priority
CP = Colour Palette
VF = VFlip
HF = HFlip
PT = Pattern Number
XP = X Position of Sprite



*/
