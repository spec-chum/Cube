
#include "support/gcc8_c_support.h"

#include <proto/exec.h>
#include <proto/graphics.h>
#include <hardware/cia.h>
#include <hardware/custom.h>
#include <hardware/blit.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <graphics/gfxbase.h>

#define CMOVE(addr, data) offsetof(struct Custom, addr), data
#define CMOVE32(addr, data) offsetof(struct Custom, addr), data >> 16, offsetof(struct Custom, addr) + 2, data & 0xffff
#define CWAIT(vhpos, flags) vhpos, flags
#define CEND 0xffff, 0xfffe

#define WIDTH 320
#define HEIGHT 256
#define CENTRE_X WIDTH / 2	// Centre of the screen (horizontal)
#define CENTRE_Y HEIGHT / 2 // Centre of the screen (vertical)
#define CUBE_SIZE 64
#define HALF_CUBE_SIZE (CUBE_SIZE / 2)

// #define DEBUG

struct Vector3
{
	WORD x, y, z;
} points[8];

struct ExecBase *SysBase;
struct Custom *custom = (struct Custom *)0xdff000;
struct CIA *ciaa = (struct CIA *)0xbfe001;

// clang-format off

__attribute__((section(".MEMF_FAST"))) struct Vector3 cubePoints[] = {
	{ -HALF_CUBE_SIZE, -HALF_CUBE_SIZE, -HALF_CUBE_SIZE }, // Vertex 0
	{  HALF_CUBE_SIZE, -HALF_CUBE_SIZE, -HALF_CUBE_SIZE }, // Vertex 1
	{  HALF_CUBE_SIZE,  HALF_CUBE_SIZE, -HALF_CUBE_SIZE }, // Vertex 2
	{ -HALF_CUBE_SIZE,  HALF_CUBE_SIZE, -HALF_CUBE_SIZE }, // Vertex 3
	{ -HALF_CUBE_SIZE, -HALF_CUBE_SIZE,  HALF_CUBE_SIZE }, // Vertex 4
	{  HALF_CUBE_SIZE, -HALF_CUBE_SIZE,  HALF_CUBE_SIZE }, // Vertex 5
	{  HALF_CUBE_SIZE,  HALF_CUBE_SIZE,  HALF_CUBE_SIZE }, // Vertex 6
	{ -HALF_CUBE_SIZE,  HALF_CUBE_SIZE,  HALF_CUBE_SIZE }  // Vertex 7
};

// indices of points for each line
__attribute__((section(".MEMF_FAST"))) UWORD lineIndices[12][2] = {
	{0, 1}, {1, 2}, {2, 3}, {3, 0},	// Front face edges
	{4, 5},	{5, 6},	{6, 7},	{7, 4},	// Back face edges
	{0, 4},	{1, 5},	{2, 6},	{3, 7}	// Side edges
};

// sine from 0 to 90 degrees << 14 in 1 degree steps
__attribute__((section(".MEMF_FAST"))) UWORD sinTable[] = {
	0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126,
	3406, 3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138,
	6402, 6664, 6924, 7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923,
	9162, 9397, 9630, 9860, 10087, 10311, 10531, 10749, 10963, 11174,
	11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733, 12911,
	13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330,
	14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296, 15396,
	15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083,
	16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374,
	16382, 16384};

// clang-format on

__attribute__((section(".MEMF_FAST"))) static ULONG yOffset[HEIGHT];

__attribute__((section(".MEMF_CHIP"))) static UBYTE screen1[(WIDTH * HEIGHT * 1) / 8];
__attribute__((section(".MEMF_CHIP"))) static UBYTE screen2[(WIDTH * HEIGHT * 1) / 8];

__attribute__((section(".MEMF_CHIP"))) static UWORD copperlist[] = {
	CMOVE32(bplpt[0], 0), // APTR bitplane 1

	CMOVE(color[0], 0),
	CMOVE(color[1], 0xf00),

	CEND};

static inline void WaitRaster(const UWORD raster)
{
	while (raster != ((custom->vpos32 >> 8) & 0x01ff))
	{
	}
}

static inline void WaitNotRaster(const UWORD raster)
{
	while (raster == ((custom->vpos32 >> 8) & 0x01ff))
	{
	}
}

static void SetBPLPtr(APTR address)
{
	copperlist[1] = (ULONG)address >> 16;
	copperlist[3] = (ULONG)address & 0xffff;
}

static void GenerateYTable()
{
	for (WORD i = 0; i < HEIGHT; i++)
	{
		yOffset[i] = i * (WIDTH / 8);
	}
}

static inline void GetSinCos(const WORD angle, WORD *sinValue, WORD *cosValue)
{
	// Calculate sinValue and cosValue for the angle: cos(angle) = sin(90 - angle)
	switch (angle)
	{
	case 0 ... 89:
		*sinValue = sinTable[angle];
		*cosValue = sinTable[90 - angle];
		break;
	case 90 ... 179:
		*sinValue = sinTable[180 - angle];
		*cosValue = -sinTable[angle - 90];
		break;
	case 180 ... 269:
		*sinValue = -sinTable[angle - 180];
		*cosValue = -sinTable[270 - angle];
		break;
	default:
		*sinValue = -sinTable[360 - angle];
		*cosValue = sinTable[angle - 270];
		break;
	}
}

__attribute__((always_inline)) static inline WORD ScaleFP(LONG value)
{
	return ((value << 2) >> 16) & 0xffff;
}

static void RotateX(struct Vector3 *point, const WORD sinValue, const WORD cosValue)
{
	// Perform the x rotation
	WORD newY = ScaleFP(point->y * cosValue - point->z * sinValue);
	WORD newZ = ScaleFP(point->y * sinValue + point->z * cosValue);

	// Update point coordinates
	point->y = newY;
	point->z = newZ;
}

static void RotateY(struct Vector3 *point, const WORD sinValue, const WORD cosValue)
{
	// Perform the y rotation
	WORD newX = ScaleFP(point->x * cosValue + point->z * sinValue);
	WORD newZ = ScaleFP(point->z * cosValue - point->x * sinValue);

	// Update point coordinates
	point->x = newX;
	point->z = newZ;
}

static void RotateZ(struct Vector3 *point, const WORD sinValue, const WORD cosValue)
{
	// Perform the z rotation
	WORD newX = ScaleFP(point->x * cosValue - point->y * sinValue);
	WORD newY = ScaleFP(point->x * sinValue + point->y * cosValue);

	// Update point coordinates
	point->x = newX;
	point->y = newY;
}

static inline void WaitBlitInline()
{
	UWORD dummyRead = custom->dmaconr;
	while (custom->dmaconr & DMAF_BLTDONE)
	{
	}
}

static inline void BlitClear(const APTR dest, const WORD height, const WORD width)
{
	WaitBlitInline();

	__asm volatile(
		"move.l	%1, 0x40(%0)\n" // bltcon0 = DEST; bltcon1 = 0
		"move.l	%2, 0x54(%0)\n" // bltdpt  = dest
		"move.w %3, 0x66(%0)\n" // bltdmod = WIDTH / 8 - width * 2
		"move.w	%4, 0x58(%0)"	// bltsize = (height << 6) + width
		:
		: "a"(custom), "g"(DEST << 16), "g"(dest), "g"(WIDTH / 8 - width * 2), "g"((height << 6) + width)
		: "cc");
}

__attribute__((always_inline)) static inline void SetPixel(const UBYTE *screen, const WORD x, const WORD y)
{
	__asm volatile("bset %[pixel], %[ptr]\n"
				   :
				   : [pixel] "d"((BYTE)~x), [ptr] "m"(*(screen + yOffset[y] + (x / 8)))
				   : "cc");
}

__attribute__((always_inline)) static inline void Swap(WORD *a, WORD *b)
{
	__asm volatile(
		"exg %0, %1"
		: "+r"(*a), "+r"(*b));
}

static inline WORD Abs(const WORD x)
{
	return x < 0 ? -x : x;
}

static void DrawLine(const APTR screen, struct Vector3 *p0, struct Vector3 *p1)
{
	WORD x0 = p0->x;
	WORD y0 = p0->y;
	WORD x1 = p1->x;
	WORD y1 = p1->y;

	// optimise for vertical lines
	if (x0 == x1)
	{
		if (y0 > y1)
		{
			Swap(&y0, &y1);
		}

		while (y0 <= y1)
		{
			SetPixel(screen, x0, y0);
			y0++;
		}
		return;
	}

	// optimise for horizontal lines
	if (y0 == y1)
	{
		if (x0 > x1)
		{
			Swap(&x0, &x1);
		}

		while (x0 <= x1)
		{
			SetPixel(screen, x0, y0);
			x0++;
		}
		return;
	}

	const BOOL steep = Abs(y1 - y0) > Abs(x1 - x0);
	if (steep)
	{
		Swap(&x0, &y0);
		Swap(&x1, &y1);
	}

	if (x0 > x1)
	{
		Swap(&x0, &x1);
		Swap(&y0, &y1);
	}

	const WORD dx = 2 * (x1 - x0);
	const WORD dy = 2 * Abs(y1 - y0);
	const WORD sy = y0 < y1 ? 1 : -1;
	WORD error = dx;

	while (x0 <= x1)
	{
		if (steep)
		{
			SetPixel(screen, y0, x0);
		}
		else
		{
			SetPixel(screen, x0, y0);
		}

		error -= dy;
		if (error < 0)
		{
			y0 += sy;
			error += dx;
		}
		x0++;
	}
}

static void RotatePoints(const WORD angle)
{
	const WORD scaleFactor = 8;
	const WORD perspectiveFactor = 1 << scaleFactor;

	WORD sin, cos;
	GetSinCos(angle, &sin, &cos);

	for (UWORD i = 0; i < sizeof(cubePoints) / sizeof(cubePoints[0]); i++)
	{
		struct Vector3 point = cubePoints[i];
		RotateY(&point, sin, cos);
		RotateX(&point, sin, cos);
		RotateZ(&point, sin, cos);

		const WORD zFactor = (1 << scaleFactor) - ((point.z * perspectiveFactor) >> scaleFactor);
		point.x = ((point.x * zFactor) >> scaleFactor) + CENTRE_X;
		point.y = ((point.y * zFactor) >> scaleFactor) + CENTRE_Y;
		points[i] = point;
	}
}

void DrawCube(UBYTE *currentScreen)
{
	// Draw the lines using the lineIndices array
	for (UWORD i = 0; i < 12; i++)
	{
		DrawLine(currentScreen, &points[lineIndices[i][0]], &points[lineIndices[i][1]]);
	}
}

LONG main()
{
	SysBase = *(struct ExecBase **)4L;

	// open gfx lib and save original copperlist
	struct GfxBase *GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 0);
	struct copinit *oldCopinit = GfxBase->copinit;

	// save original view and set a default one
	struct View *actiView = GfxBase->ActiView;
	LoadView(NULL);
	WaitTOF();
	WaitTOF();

	// Save interrupts and DMA
	UWORD oldInt = custom->intenar;
	UWORD oldDMA = custom->dmaconr;

	// disable all interrupts and DMA
	custom->intena = 0x3fff;
	custom->dmacon = 0x01ff;

	// Set BPL addresses in copperlist and generate y address table
	SetBPLPtr(screen2);
	GenerateYTable();

	// initiate our copper
	custom->cop1lc = (ULONG)copperlist;

	// prepare playfield
	custom->fmode = 0;
	custom->bplcon0 = 0x1200;
	custom->bplcon1 = 0;
	custom->bplcon2 = 0;
	custom->diwstrt = 0x2c81;
	custom->diwstop = 0x2cc1;
	custom->ddfstrt = 0x38;
	custom->ddfstop = 0xd0;

	// enable DMA
	custom->dmacon = DMAF_SETCLR | DMAF_COPPER | DMAF_RASTER | DMAF_BLITTER;

	WORD angle = 0;
	UBYTE *currentScreen = screen1;

	// loop until mouse clicked
	while (ciaa->ciapra & CIAF_GAMEPORT0)
	{
		RotatePoints(angle);
		WaitBlitInline();
		DrawCube(currentScreen);

		WORD offset = (CENTRE_Y - 58) * (WIDTH / 8) + (CENTRE_X - 58) / 8;
		WORD blitHeight = 116;
		WORD blitWidth = 8;
		if (currentScreen == &screen1[0])
		{
			BlitClear(screen2 + offset, blitHeight, blitWidth);
			currentScreen = screen2;
		}
		else
		{
			BlitClear(screen1 + offset, blitHeight, blitWidth);
			currentScreen = screen1;
		}

		WaitRaster(0x2c);
		SetBPLPtr(currentScreen);

		angle++;
		if (angle >= 360)
		{
			angle = 0;
		}
	}

	// restore DMA and interrupts
	custom->dmacon = oldDMA | DMAF_SETCLR;
	custom->intena = oldInt | INTF_SETCLR;

	// restore original copper
	custom->cop1lc = (ULONG)oldCopinit;

	// restore original view and close library
	LoadView(actiView);
	WaitTOF();
	WaitTOF();
	CloseLibrary((struct Library *)GfxBase);

	return 0;
}
