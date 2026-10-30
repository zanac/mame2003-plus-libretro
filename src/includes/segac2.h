/*************************************************************************

	Sega System C/C2 Driver

**************************************************************************/

#include "megadriv.h"

/*----------- defined in vidhrdw/segac2_vidhrdw.c -----------*/

VIDEO_START( segac2 );
VIDEO_UPDATE( segac2_rgb );
#define SEGAC2_BLACK_PEN	(2048 * 3)	/* the black palette entry of System C/C2 */
