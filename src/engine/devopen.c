/*
 * Copyright (c) 1999, 2000, 2001, 2003 Greg Haerr <greg@censoft.com>
 * Portions Copyright (c) 2002 by Koninklijke Philips Electronics N.V.
 * Portions Copyright (c) 1991 David I. Bell
 *
 * Device-independent mid level screen device init routines
 *
 * These routines implement the smallest Microwindows engine level
 * interface to the screen driver.  By setting the NOFONTSORCLIPPING
 * config option, only these routines will be included, which can
 * be used to generate a low-level interface to the screen drivers
 * without dragging in any other GdXXX routines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "device.h"
#include "swap.h"

#if MSDOS | ELKS
#define NOSTDPAL8
#endif

/*
 * The following define can change depending on the window manager
 * usage of colors and layout of the 8bpp palette devpal8.c.
 * Color entries below this value won't be overwritten by user
 * programs or bitmap display conversion tables.
 */
#define FIRSTUSERPALENTRY	24  /* first writable pal entry over 16 color*/

MWPIXELVAL gr_foreground;	/* current foreground color */
MWPIXELVAL gr_background;	/* current background color */
MWBOOL 	gr_usebg;    	    /* TRUE if background drawn in pixmaps */
int 	gr_mode = MWROP_COPY; 	    /* drawing mode */
/*static*/ MWPALENTRY	gr_palette[256];    /* current palette*/
/*static*/ int	gr_firstuserpalentry;/* first user-changable palette entry*/
/*static*/ int 	gr_nextpalentry;    /* next available palette entry*/
MWCOLORVAL gr_foreground_rgb;	/* current fg color in 0xAARRGGBB format for mono convblits*/
MWCOLORVAL gr_background_rgb;	/* current background color */

uint32_t gr_dashmask;     /* An actual bitmask of the dash values */
uint32_t gr_dashcount;    /* The number of bits defined in the dashmask */

int        gr_fillmode;
MWSTIPPLE  gr_stipple;
MWTILE     gr_tile;

MWPOINT    gr_ts_offset;

/**
 * Open low level graphics driver.
 *
 * @return The screen drawing surface.
 */
PSD
GdOpenScreen(void)
{
	PSD			psd;
	MWPALENTRY *		stdpal;

	psd = scrdev.Open(&scrdev);
	if (!psd)
		return NULL;

	/* assume no user changable palette entries*/
	gr_firstuserpalentry = (int)psd->ncolors;

	/* set palette according to system colors and devpalX.c*/
	switch((int)psd->ncolors) {

#if !defined(NOSTDPAL1) /* don't require stdpal1 if not needed */
	case 2:		/* 1bpp*/
	{
		extern MWPALENTRY	mwstdpal1[2];
		stdpal = mwstdpal1;
	}
	break;
#endif

#if !defined(NOSTDPAL2) /* don't require stdpal2 if not needed */
	case 4:		/* 2bpp*/
	{
		extern MWPALENTRY	mwstdpal2[4];
		stdpal = mwstdpal2;
	}
	break;
#endif

#if !defined(NOSTDPAL4)
	/* don't require stdpal4 if not needed */
	case 8:		/* 3bpp - not fully supported*/
	case 16:	/* 4bpp*/
	{
		extern MWPALENTRY	mwstdpal4[16];
		stdpal = mwstdpal4;
	}
	break;
#endif

#if !defined(NOSTDPAL8) /* don't require large stdpal8 if not needed */
	case 256:	/* 8bpp*/
	{
		extern MWPALENTRY	mwstdpal8[256];
#if UNIFORMPALETTE
		/* don't change uniform palette if alpha blending*/
		gr_firstuserpalentry = 256;
#else
		/* start after last system-reserved color*/
		gr_firstuserpalentry = FIRSTUSERPALENTRY;
#endif
		stdpal = mwstdpal8;
	} 
	break;
#endif	/* !defined(NOSTDPAL8)*/

	default:	/* truecolor*/
		/* no palette*/
		gr_firstuserpalentry = 0;
		stdpal = NULL;
	}

	/* reset next user palette entry, write hardware palette*/
	GdResetPalette();
	GdSetPalette(psd, 0, (int)psd->ncolors, stdpal);

	/* init local vars*/
	GdSetMode(MWROP_COPY);
	GdSetFillMode(MWFILL_SOLID);  /* Set the fill mode to solid */

	GdSetForegroundColor(psd, MWRGB(255, 255, 255));	/* WHITE*/
	GdSetBackgroundColor(psd, MWRGB(0, 0, 0));		/* BLACK*/
	GdSetUseBackground(TRUE);
	/* select first builtin font (usually MWFONT_SYSTEM_VAR)*/
	//GdSetFont(GdCreateFont(psd, NULL, 0, 0, NULL));

	GdSetDash(0, 0);  /* No dashing to start */
	GdSetStippleBitmap(0,0,0);  /* No stipple to start */

#if !NOCLIPPING
#if DYNAMICREGIONS
	GdSetClipRegion(psd, GdAllocRectRegion(0, 0, psd->xvirtres, psd->yvirtres));
#else
	GdSetClipRects(psd, 0, NULL);
#endif /* DYNAMICREGIONS*/
#endif /* NOCLIPPING*/

	/* fill black (actually fill to first palette entry or truecolor 0*/
	psd->FillRect(psd, 0, 0, psd->xvirtres-1, psd->yvirtres-1, 0);
	return psd;
}

/**
 * Close low level graphics driver
 *
 * @param psd Screen drawing surface.
 */
void 
GdCloseScreen(PSD psd)
{
	psd->Close(psd);
}

/**
 * Set dynamic screen portrait mode, return new mode
 *
 * @param psd Screen drawing surface.
 * @param portraitmode New portrait mode requested.
 * @return New portrait mode actually set.
 */
int
GdSetPortraitMode(PSD psd, int portraitmode)
{
	/* set portrait mode if supported*/
	if (psd->SetPortrait)
		psd->SetPortrait(psd, portraitmode);
	return psd->portrait;
}

/**
 * Get information about the screen (resolution etc).
 *
 * @param psd Screen drawing surface.
 * @param psi Destination for screen information.
 */
void
GdGetScreenInfo(PSD psd, PMWSCREENINFO psi)
{
	psd->GetScreenInfo(psd, psi);
	GdGetButtonInfo(&psi->buttons);
	GdGetModifierInfo(&psi->modifiers, NULL);
	GdGetCursorPos(&psi->xpos, &psi->ypos);
}

/**
 *
 * reset palette to empty except for system colors
 */
void
GdResetPalette(void)
{
	/* note: when palette entries are changed, all 
	 * windows may need to be redrawn
	 */
	gr_nextpalentry = gr_firstuserpalentry;
}

/**
 * Set the system palette section to the passed palette entries
 *
 * @param psd Screen device.
 * @param first First palette entry to set.
 * @param count Number of palette entries to set.
 * @param palette New palette entries (array of size @param count).
 */
void
GdSetPalette(PSD psd, int first, int count, MWPALENTRY *palette)
{
	int	i;

	/* no palette management needed if running truecolor*/
	if(psd->pixtype != MWPF_PALETTE)
		return;

	/* bounds check against # of device color entries*/
	if(first + count > (int)psd->ncolors)
		count = (int)psd->ncolors - first;
	if(count >= 0 && first < (int)psd->ncolors) {
		psd->SetPalette(psd, first, count, palette);

		/* copy palette for GdFind*Color*/
		for(i=0; i<count; ++i)
			gr_palette[i+first] = palette[i];
	}
}

/**
 * Get system palette entries
 *
 * @param psd Screen device.
 * @param first First palette index to get.
 * @param count Number of palette entries to retrieve.
 * @param palette Recieves palette entries (array of size @param count).
 */
int
GdGetPalette(PSD psd, int first, int count, MWPALENTRY *palette)
{
	int	i;

	/* no palette if running truecolor*/
	if(psd->pixtype != MWPF_PALETTE)
		return 0;

	/* bounds check against # of device color entries*/
	if(first + count > (int)psd->ncolors)
		if( (count = (int)psd->ncolors - first) <= 0)
			return 0;

	for(i=0; i<count; ++i)
		*palette++ = gr_palette[i+first];

	return count;
}

/**
 * Convert a palette-independent value to a hardware color
 *
 * @param psd Screen device
 * @param c 24-bit RGB color.
 * @return Hardware-specific color.
 */
MWPIXELVAL
GdFindColor(PSD psd, MWCOLORVAL c)
{
	/*
	 * Handle truecolor displays.
	 */
	switch(psd->pixtype) {
	case MWPF_TRUECOLOR8888:
		/* create 32 bit ARGB pixel (0xAARRGGBB) from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL8888(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL8888(c);

	case MWPF_TRUECOLORABGR:
		/* create 32 bit ABGR pixel (0xAABBGGRR) from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXELABGR(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXELABGR(c);

	case MWPF_TRUECOLOR888:
		/* create 24 bit 0RGB pixel (0x00RRGGBB) from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL888(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL888(c);

	case MWPF_TRUECOLOR565:
		/* create 16 bit RGB5/6/5 format pixel from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL565(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL565(c);

	case MWPF_TRUECOLOR555:
		/* create 16 bit RGB5/5/5 format pixel from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL555(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL555(c);

	case MWPF_TRUECOLOR332:
		/* create 8 bit RGB3/3/2 format pixel from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL332(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL332(c);
	case MWPF_TRUECOLOR233:
		/* create 8 bit BGR2/3/3 format pixel from ABGR colorval (0xAABBGGRR)*/
		/*RGB2PIXEL332(REDVALUE(c), GREENVALUE(c), BLUEVALUE(c))*/
		return COLOR2PIXEL233(c);
        }

	/* case MWPF_PALETTE: must be running 1, 2, 4 or 8 bit palette*/

	/* search palette for closest match*/
	return GdFindNearestColor(gr_palette, (int)psd->ncolors, c);
}

/**
 * Search a palette to find the nearest color requested.
 * Uses a weighted squares comparison.
 *
 * @param pal Palette to search.
 * @param size Size of palette (number of entries).
 * @param cr Color to look for.
 */
MWPIXELVAL
GdFindNearestColor(MWPALENTRY *pal, int size, MWCOLORVAL cr)
{
	MWPALENTRY *	rgb;
	int		r, g, b;
	int		R, G, B;
	int32_t		diff = 0x7fffffffL;
	int32_t		sq;
	int		best = 0;

	r = REDVALUE(cr);
	g = GREENVALUE(cr);
	b = BLUEVALUE(cr);
	for(rgb=pal; diff && rgb < &pal[size]; ++rgb) {
		R = rgb->r - r;
		G = rgb->g - g;
		B = rgb->b - b;
#if 1
		/* speedy linear distance method*/
		sq = abs(R) + abs(G) + abs(B);
#else
		/* slower distance-cubed with luminance adjustment*/
		/* gray is .30R + .59G + .11B*/
		/* = (R*77 + G*151 + B*28)/256*/
		sq = (int32_t)R*R*30*30 + (int32_t)G*G*59*59 + (int32_t)B*B*11*11;
#endif

		if(sq < diff) {
			best = rgb - pal;
			if((diff = sq) == 0)
				return best;
		}
	}
	return best;
}

/**
 * Convert a color from a driver-dependent PIXELVAL to a COLORVAL.
 *
 * @param psd Screen device.
 * @param pixel Hardware-specific color.
 * @return 24-bit RGB color.
 */
MWCOLORVAL
GdGetColorRGB(PSD psd, MWPIXELVAL pixel)
{
	switch (psd->pixtype) {
	case MWPF_TRUECOLOR8888:
		return PIXEL8888TOCOLORVAL(pixel);

	case MWPF_TRUECOLORABGR:
		return PIXELABGRTOCOLORVAL(pixel);

	case MWPF_TRUECOLOR888:
		return PIXEL888TOCOLORVAL(pixel);

	case MWPF_TRUECOLOR565:
		return PIXEL565TOCOLORVAL(pixel);

	case MWPF_TRUECOLOR555:
		return PIXEL555TOCOLORVAL(pixel);

	case MWPF_TRUECOLOR332:
		return PIXEL332TOCOLORVAL(pixel);

	case MWPF_TRUECOLOR233:
		return PIXEL233TOCOLORVAL(pixel);

	case MWPF_PALETTE:
		return GETPALENTRY(gr_palette, pixel);

	default:
		assert(FALSE);
		return 0;
	}
}

#if DEBUG
#include <unistd.h>
#include <fcntl.h>

#define BI_RGB		0L
#define BI_RLE8		1L
#define BI_RLE4		2L
#define BI_BITFIELDS	3L

typedef unsigned char	BYTE;
typedef unsigned short	WORD;
typedef uint32_t	DWORD;
typedef int32_t		LONG;

/* windows style bmp - must be byte packed*/
typedef struct {
	/* BITMAPFILEHEADER*/
	BYTE	bfType[2];
	DWORD	bfSize;
	WORD	bfReserved1;
	WORD	bfReserved2;
	DWORD	bfOffBits;
	/* BITMAPINFOHEADER*/
	DWORD	BiSize;
	LONG	BiWidth;
	LONG	BiHeight;
	WORD	BiPlanes;
	WORD	BiBitCount;
	DWORD	BiCompression;
	DWORD	BiSizeImage;
	LONG	BiXpelsPerMeter;
	LONG	BiYpelsPerMeter;
	DWORD	BiClrUsed;
	DWORD	BiClrImportant;
} MWPACKED BMPHEAD;

/* r/g/b masks for non-palette bitmaps*/

static void
putsw(uint32_t dw, FILE *ofp)
{
	/* little-endian storage of shortword*/
	fputc((unsigned char)dw, ofp);
	dw >>= 8;
	fputc((unsigned char)dw, ofp);
}

static void
putdw(uint32_t dw, FILE *ofp)
{
	/* little-endian storage of longword*/
	fputc((unsigned char)dw, ofp);
	dw >>= 8;
	fputc((unsigned char)dw, ofp);
	dw >>= 8;
	fputc((unsigned char)dw, ofp);
	dw >>= 8;
	fputc((unsigned char)dw, ofp);
}

/**
 * Create .bmp file from framebuffer data
 * 1, 4, 8, 16, 24 and 32 bpp supported
 *
 * @param path Output file.
 * @return 0 on success, nonzero on error.
 */
int
GdCaptureScreen(PSD psd, char *pathname)
{
	FILE *ofp;
	int	w, h, i, cx, cy, extra, bpp, bytespp, ncolors, sizecolortable;
	BMPHEAD	bmp;
	MWSCREENINFO sinfo;

	ofp = fopen(pathname, "wb");
	if (!ofp)
		return 1;

	if (!psd)
		psd = &scrdev;
	GdGetScreenInfo(psd, &sinfo);

	cx = psd->xres;
	cy = psd->yres;
	bpp = psd->bpp;
	bytespp = (bpp+7)/8;

	/* dword right padded*/
	extra = (cx*bytespp) & 3;
	if (extra)
		extra = 4 - extra;

	ncolors = (bpp <= 8)? (1<<bpp): 0;

	/* color table is either palette or 3 longword r/g/b masks*/
	sizecolortable = ncolors? ncolors*4: 3*4;
	if (bpp == 24)
		sizecolortable = 0;	/* special case 24bpp has no table*/

	/* fill out bmp header*/
	memset(&bmp, 0, sizeof(bmp));
	bmp.bfType[0] = 'B';
	bmp.bfType[1] = 'M';
	bmp.bfSize = dwswap(sizeof(bmp) + sizecolortable + (int32_t)(cx+extra)*cy*bytespp);
	bmp.bfOffBits = dwswap(sizeof(bmp) + sizecolortable);
	bmp.BiSize = dwswap(40);
	bmp.BiWidth = dwswap(cx);
	bmp.BiHeight = dwswap(cy);
	bmp.BiPlanes = wswap(1);
	bmp.BiBitCount = wswap(bpp);
	bmp.BiCompression = dwswap((bpp==16 || bpp==32)? BI_BITFIELDS: BI_RGB);
	bmp.BiSizeImage = dwswap((int32_t)(cx+extra)*cy*bytespp);
	bmp.BiClrUsed = dwswap((bpp <= 8)? ncolors: 0);
	/*bmp.BiClrImportant = 0;*/

	/* write header*/
	fwrite(&bmp, sizeof(bmp), 1, ofp);

	/* write colortable*/
	if (sizecolortable) {
		if(bpp <= 8) {
			/* write palette*/
			for(i=0; i<ncolors; i++) {
				fputc(gr_palette[i].b, ofp);
				fputc(gr_palette[i].g, ofp);
				fputc(gr_palette[i].r, ofp);
				fputc(0, ofp);
			}
		} else {
			/* write 3 r/g/b masks*/
			putdw(sinfo.rmask, ofp);
			putdw(sinfo.gmask, ofp);
			putdw(sinfo.bmask, ofp);
		}
	}

	/* write image data, upside down ;)*/
	for(h=cy-1; h>=0; --h) {
		unsigned char *buf = ((unsigned char *)psd->addr) + h * cx * bytespp;
		unsigned char *cptr;
		unsigned short *sptr;
		uint32_t *lptr;
		switch (bpp) {
		case 32:
			lptr = (uint32_t *)buf;
			for(w=0; w<cx; ++w)
				putdw(*lptr++, ofp);
			break;
		case 24:
		case 18:
			cptr = (unsigned char *)buf;
			for(w=0; w<cx; ++w) {
				fputc(*cptr++, ofp);
				fputc(*cptr++, ofp);
				fputc(*cptr++, ofp);
			}
			break;
		case 16:
			sptr = (unsigned short *)buf;
			for(w=0; w<cx; ++w)
				putsw(*sptr++, ofp);
			break;
		default:
			cptr = (unsigned char *)buf;
			for(w=0; w<cx; ++w)
				fputc(*cptr++, ofp);
			break;
		}
		for(w=0; w<extra; ++w)
			fputc(0, ofp);		/* DWORD pad each line*/
	}

	fclose(ofp);
	return 0;
}

void GdPrintBitmap(PMWBLITPARMS gc, int SSZ)
{
	unsigned char *src;
	int height;
	unsigned int v;

	src = ((unsigned char *)gc->data)     + gc->srcy * gc->src_pitch + gc->srcx * SSZ;

	DPRINTF("Image %d,%d SSZ %d\n", gc->width, gc->height, SSZ);
	height = gc->height;
	while (--height >= 0)
	{
		register unsigned char *s = src;
		int w = gc->width;

		while (--w >= 0)
		{
			switch (SSZ) {
			case 2:
				v = s[0] | (s[1] << 8);
				v = PIXEL565RED(v) + PIXEL565GREEN(v) + PIXEL565BLUE(v);
				DPRINTF("%c", "_.:;oVM@X"[v]);
				break;
			case 3:
				v = (s[0] + s[1] + s[2]) / 3;
				DPRINTF("%c", "_.:;oVM@X"[v >> 5]);
				break;
			case 4:
				//if (s[4])
					v = (s[0] + s[1] + s[2]) / 3;
				//else v = 256;
				DPRINTF("%c", "_.:;oVM@X"[v >> 5]);
				break;
			}
			s += SSZ;				/* src: next pixel right*/
		}
		DPRINTF("\n");
		src += gc->src_pitch;		/* src: next line down*/
	}
}
#endif /* DEBUG*/
