/*
 * landingpad.c — LANDING PAD, a simple Lunar-Lander-style game for the
 * Atari ST (low resolution).
 *
 * Joystick input is read by hooking the IKBD joystick vector (joyvec,
 * offset 24 in the KBDVECS table returned by Kbdvbase). TOS calls the
 * vector with a0 pointing at a 3-byte packet [header, joy0, joy1]; the
 * game reads joystick 1 (byte 2), which is both the physical joystick
 * port and the stick Sidepad injects into.
 *
 * Controls: left/right = rotate, up or fire = thrust, ESC = quit.
 * Touch down gently (upright, low speed) on a green pad. Thrust burns
 * fuel; a good landing refuels you and makes new terrain.
 *
 * Build (inside atarist-toolkit-docker): stcmd make
 */

#include <osbind.h>
#include <string.h>

#define LINE_BYTES  160
#define SCREEN_SIZE 32000L

/* IKBD joystick state bits */
#define JOY_UP    0x01
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x80

/* positions/velocities are fixed point, 1/16 pixel */
#define FP 4

#define GRAVITY    1  /* 1/16 px/frame^2 */
#define THRUST     3  /* along the lander axis */
#define LANDER_R   6  /* body radius, px */
#define FOOT_X     5  /* feet at centre +/- this, px */
#define FOOT_Y     8  /* feet this far below centre, px */
#define SAFE_VY    20 /* max landing speeds, 1/16 px/frame */
#define SAFE_VX    12

#define START_FUEL 600
#define LAND_SCORE 100
#define LAND_FUEL  250

#define TPOINTS  21 /* terrain heights every 16 px */
#define NPADS    3  /* flat pads, 32 px wide */

/* KBDVECS layout (Kbdvbase); joyvec is at byte offset 24 */
typedef struct {
    void *midivec;
    void *vkbderr;
    void *vmiderr;
    void *statvec;
    void *mousevec;
    void *clrvec;
    void *joyvec;
    void *midisys;
    void *ikbdsys;
} Kbdvecs;

/*
 * joyvec handler. Called by TOS (or Sidepad's resident VBL handler) via
 * jsr with a0 -> [header, joy0, joy1]; just latch the joystick 1 byte.
 * Explicit asm symbol names so this works whether or not the toolchain
 * prefixes C symbols with an underscore.
 */
volatile unsigned char joy1 __asm__("g_joy1") = 0;
void joy_handler(void) __asm__("g_joyhandler");

__asm__("\t.text\n"
        "\t.even\n"
        "\t.globl g_joyhandler\n"
        "g_joyhandler:\n"
        "\tmove.b 2(%a0),g_joy1\n"
        "\trts\n");

/* sin(2*pi*k/32) * 256; cos(a) = sin32[(a + 8) & 31] */
static const int sin32[32] = {
    0,    50,   98,   142,  181,  213,  236,  251,
    256,  251,  236,  213,  181,  142,  98,   50,
    0,    -50,  -98,  -142, -181, -213, -236, -251,
    -256, -251, -236, -213, -181, -142, -98,  -50
};

/* 8x8 font, only the glyphs the game needs */
static const char font_chars[] = "0123456789ABCDEFGHIJKLMNOPRSTUVWY=";
static const unsigned char font[][8] = {
    { 0x7C, 0xC6, 0xCE, 0xD6, 0xE6, 0xC6, 0x7C, 0x00 }, /* 0 */
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* 1 */
    { 0x7C, 0xC6, 0x06, 0x1C, 0x30, 0x60, 0xFE, 0x00 }, /* 2 */
    { 0x7C, 0xC6, 0x06, 0x1C, 0x06, 0xC6, 0x7C, 0x00 }, /* 3 */
    { 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00 }, /* 4 */
    { 0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00 }, /* 5 */
    { 0x3C, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00 }, /* 6 */
    { 0xFE, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 }, /* 7 */
    { 0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00 }, /* 8 */
    { 0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00 }, /* 9 */
    { 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0x00 }, /* A */
    { 0xFC, 0xC6, 0xC6, 0xFC, 0xC6, 0xC6, 0xFC, 0x00 }, /* B */
    { 0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00 }, /* C */
    { 0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00 }, /* D */
    { 0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xFE, 0x00 }, /* E */
    { 0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xC0, 0x00 }, /* F */
    { 0x7C, 0xC6, 0xC0, 0xDE, 0xC6, 0xC6, 0x7C, 0x00 }, /* G */
    { 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00 }, /* H */
    { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* I */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0x78, 0x00 }, /* J */
    { 0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0xC6, 0x00 }, /* K */
    { 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFE, 0x00 }, /* L */
    { 0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00 }, /* M */
    { 0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00 }, /* N */
    { 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 }, /* O */
    { 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00 }, /* P */
    { 0xFC, 0xC6, 0xC6, 0xFC, 0xD8, 0xCC, 0xC6, 0x00 }, /* R */
    { 0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00 }, /* S */
    { 0xFE, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x00 }, /* T */
    { 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 }, /* U */
    { 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00 }, /* V */
    { 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00 }, /* W */
    { 0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x38, 0x38, 0x00 }, /* Y */
    { 0x00, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0x00, 0x00 }, /* = */
};

/* rainbow colours for the title letters */
static const int rainbow[6] = { 2, 3, 4, 5, 6, 7 };

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static int ty[TPOINTS];     /* terrain height (pixel y) every 16 px */
static int pad_seg[NPADS];  /* pads start at these segments, 2 wide */
static int lx, ly, lvx, lvy; /* lander, 1/16 px */
static int langle;           /* 0..31, 0 = up, clockwise */
static int fuel, thrusting;
static const char *msg;
static int msg_timer;
static long frame;
static long score;
static int lives, quit;

static void clear_back(void)
{
    memset(back, 0, SCREEN_SIZE);
}

/* OR a sprite into every bitplane set in color (palette index 1-15) */
static void draw_sprite(int x, int y, const unsigned short *rows, int h,
                        int color)
{
    unsigned char *p = back + (long)y * LINE_BYTES + ((x >> 4) << 3);
    int shift = x & 15;
    int last_group = (x >> 4) >= 19;
    int i, pl;

    for (i = 0; i < h; i++) {
        unsigned long d = ((unsigned long)rows[i] << 16) >> shift;
        unsigned short hi = (unsigned short)(d >> 16);
        unsigned short lo = (unsigned short)d;
        for (pl = 0; pl < 4; pl++) {
            if (color & (1 << pl)) {
                *(unsigned short *)(p + (pl << 1)) |= hi;
                if (lo && !last_group)
                    *(unsigned short *)(p + (pl << 1) + 8) |= lo;
            }
        }
        p += LINE_BYTES;
    }
}

/* set one pixel in every plane set in color, clipped to the screen */
static void plot(int x, int y, int color)
{
    unsigned char *p;
    int pl;

    if ((unsigned)x >= 320 || (unsigned)y >= 200)
        return;
    p = back + (long)y * LINE_BYTES + ((x >> 4) << 3) + ((x & 8) >> 3);
    for (pl = 0; pl < 4; pl++)
        if (color & (1 << pl))
            p[pl << 1] |= (unsigned char)(0x80 >> (x & 7));
}

/* Bresenham line, clipped per pixel */
static void line(int x0, int y0, int x1, int y1, int color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sxs = 1, sys = 1, err;

    if (dx < 0) {
        dx = -dx;
        sxs = -1;
    }
    if (dy < 0) {
        dy = -dy;
        sys = -1;
    }
    err = (dx > dy ? dx : -dy) / 2;
    for (;;) {
        int e2;
        plot(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = err;
        if (e2 > -dx) {
            err -= dy;
            x0 += sxs;
        }
        if (e2 < dy) {
            err += dx;
            y0 += sys;
        }
    }
}

/* x/y components of a vector of length r at angle a (0 = up) */
static int dirx(int a, int r)
{
    return sin32[a & 31] * r >> 8;
}

static int diry(int a, int r)
{
    return -(sin32[(a + 8) & 31] * r) >> 8;
}

/* triangular body plus two legs */
static void draw_lander(int cx, int cy, int a, int r, int color)
{
    int x0 = cx + dirx(a, r), y0 = cy + diry(a, r);
    int x1 = cx + dirx(a + 12, r), y1 = cy + diry(a + 12, r);
    int x2 = cx + dirx(a + 20, r), y2 = cy + diry(a + 20, r);

    line(x0, y0, x1, y1, color);
    line(x1, y1, x2, y2, color);
    line(x2, y2, x0, y0, color);
    line(x1, y1, cx + dirx(a + 13, r + 4), cy + diry(a + 13, r + 4), color);
    line(x2, y2, cx + dirx(a + 19, r + 4), cy + diry(a + 19, r + 4), color);
}

static void draw_flame(int cx, int cy, int a, int color)
{
    int ra = a + 16;

    line(cx + dirx(ra, 5), cy + diry(ra, 5), cx + dirx(ra, 9 + (frame & 3)),
         cy + diry(ra, 9 + (frame & 3)), color);
}

/* spread the 8 bits of b over 16 bits (each pixel doubled) */
static unsigned short double_bits(unsigned char b)
{
    unsigned short r = 0;
    int i;

    for (i = 0; i < 8; i++)
        if (b & (0x80 >> i))
            r |= 0xC000 >> (i * 2);
    return r;
}

static void draw_text(int x, int y, const char *s, int color)
{
    while (*s) {
        if (*s != ' ') {
            int g = 0;
            while (font_chars[g] && font_chars[g] != *s)
                g++;
            if (font_chars[g]) {
                unsigned short rows[8];
                int i;
                for (i = 0; i < 8; i++)
                    rows[i] = (unsigned short)font[g][i] << 8;
                draw_sprite(x, y, rows, 8, color);
            }
        }
        x += 8;
        s++;
    }
}

/* draw_text at double size (16x16 glyphs) */
static void draw_text2x(int x, int y, const char *s, int color)
{
    while (*s) {
        if (*s != ' ') {
            int g = 0;
            while (font_chars[g] && font_chars[g] != *s)
                g++;
            if (font_chars[g]) {
                unsigned short rows[16];
                int i;
                for (i = 0; i < 8; i++) {
                    rows[i * 2] = double_bits(font[g][i]);
                    rows[i * 2 + 1] = rows[i * 2];
                }
                draw_sprite(x, y, rows, 16, color);
            }
        }
        x += 16;
        s++;
    }
}

/* big text with each letter in a different colour */
static void draw_text2x_rainbow(int x, int y, const char *s)
{
    char one[2];
    int i = 0;

    one[1] = '\0';
    while (*s) {
        one[0] = *s;
        draw_text2x(x, y, one, rainbow[i % 6]);
        x += 16;
        s++;
        i++;
    }
}

static int esc_pressed(void)
{
    while (Cconis()) {
        if ((Crawcin() & 0xFF) == 27)
            return 1;
    }
    return 0;
}

static int rnd(int n)
{
    return (int)(Random() % n);
}

/* show what was just drawn, start drawing into the other buffer */
static void flip(void)
{
    Setscreen(buf[draw_buf ^ 1], buf[draw_buf], -1);
    draw_buf ^= 1;
    back = buf[draw_buf];
    Vsync();
}

/* --- terrain --- */

static void gen_terrain(void)
{
    int i, p;

    ty[0] = 140 + rnd(40);
    for (i = 1; i < TPOINTS; i++) {
        ty[i] = ty[i - 1] + rnd(37) - 18;
        if (ty[i] < 110)
            ty[i] = 110;
        if (ty[i] > 188)
            ty[i] = 188;
    }
    pad_seg[0] = 1 + rnd(4);
    pad_seg[1] = 8 + rnd(3);
    pad_seg[2] = 14 + rnd(4);
    for (i = 0; i < NPADS; i++) {
        p = pad_seg[i];
        ty[p + 1] = ty[p];
        ty[p + 2] = ty[p];
    }
}

/* terrain height (pixel y) under pixel x */
static int terrain_y(int x)
{
    int seg, frac;

    if (x < 0)
        x = 0;
    if (x > 319)
        x = 319;
    seg = x >> 4;
    frac = x & 15;
    return ty[seg] + ((ty[seg + 1] - ty[seg]) * frac >> 4);
}

/* is pixel x over a flat pad? */
static int on_pad(int x)
{
    int i, px;

    for (i = 0; i < NPADS; i++) {
        px = pad_seg[i] << 4;
        if (x >= px && x < px + 32)
            return 1;
    }
    return 0;
}

static void draw_terrain(void)
{
    int s, pad, i;

    for (s = 0; s < TPOINTS - 1; s++) {
        pad = 0;
        for (i = 0; i < NPADS; i++)
            if (s == pad_seg[i] || s == pad_seg[i] + 1)
                pad = 1;
        line(s << 4, ty[s], (s + 1) << 4, ty[s + 1], pad ? 5 : 1);
    }
}

/* --- game logic --- */

static void draw_status(void)
{
    char digits[7];
    long s = score;
    int i;

    for (i = 5; i >= 0; i--) {
        digits[i] = (char)('0' + (s % 10));
        s /= 10;
    }
    digits[6] = '\0';
    draw_text(8, 4, "SCORE", 1);
    draw_text(56, 4, digits, 1);
    draw_text(150, 4, "FUEL", 1);
    digits[4] = '\0';
    for (i = 3, s = fuel; i >= 0; i--) {
        digits[i] = (char)('0' + (s % 10));
        s /= 10;
    }
    draw_text(190, 4, digits, fuel < 100 ? 2 : 1);
    for (i = 0; i < lives; i++)
        draw_lander(306 - i * 14, 9, 0, 4, 6);
}

static void reset_lander(void)
{
    lx = (40 + rnd(240)) << FP;
    ly = 24 << FP;
    lvx = rnd(2) ? 8 : -8;
    lvy = 0;
    langle = 0;
    thrusting = 0;
}

static void reset_game(void)
{
    score = 0;
    lives = 3;
    fuel = START_FUEL;
    msg_timer = 0;
    gen_terrain();
    reset_lander();
}

static void draw_frame(void)
{
    int cx = lx >> FP, cy = ly >> FP;

    clear_back();
    draw_status();
    draw_terrain();
    draw_lander(cx, cy, langle, LANDER_R, 6);
    if (thrusting && fuel > 0)
        draw_flame(cx, cy, langle, (frame & 2) ? 3 : 4);
    if (msg_timer > 0) {
        msg_timer--;
        draw_text((int)(320 - 8 * strlen(msg)) / 2, 56, msg, 4);
    }
    flip();
}

static void explosion(void)
{
    int cx = lx >> FP, cy = ly >> FP, t, k;

    for (t = 3; t < 19; t += 2) {
        clear_back();
        draw_status();
        draw_terrain();
        for (k = 0; k < 8; k++)
            line(cx + dirx(k * 4 + 2, t / 2), cy + diry(k * 4 + 2, t / 2),
                 cx + dirx(k * 4 + 2, t), cy + diry(k * 4 + 2, t),
                 (k & 1) ? 3 : 4);
        flip();
    }
}

/* feet touched down: a good landing or a crash? */
static void touchdown(void)
{
    int cx = lx >> FP;
    int upright = (langle <= 1 || langle == 31);

    if (upright && on_pad(cx - FOOT_X) && on_pad(cx + FOOT_X) &&
        lvy <= SAFE_VY && lvx >= -SAFE_VX && lvx <= SAFE_VX) {
        score += LAND_SCORE;
        fuel += LAND_FUEL;
        if (fuel > 9999)
            fuel = 9999;
        msg = "LANDED";
        msg_timer = 75;
        gen_terrain();
        reset_lander();
    } else {
        explosion();
        lives--;
        if (lives > 0) {
            msg = "CRASHED";
            msg_timer = 75;
            reset_lander();
        }
    }
}

static void play_frame(void)
{
    unsigned char joy = joy1;
    int cx, cy;

    frame++;

    /* rotate every other frame */
    if (frame & 1) {
        if (joy & JOY_LEFT)
            langle = (langle + 31) & 31;
        if (joy & JOY_RIGHT)
            langle = (langle + 1) & 31;
    }

    thrusting = (joy & (JOY_UP | JOY_FIRE)) != 0;
    if (thrusting && fuel > 0) {
        lvx += sin32[langle] * THRUST >> 8;
        lvy -= sin32[(langle + 8) & 31] * THRUST >> 8;
        fuel--;
    }
    lvy += GRAVITY;

    lx += lvx;
    ly += lvy;

    /* keep it on screen */
    if (lx < 8 << FP) {
        lx = 8 << FP;
        lvx = 0;
    }
    if (lx > 312 << FP) {
        lx = 312 << FP;
        lvx = 0;
    }
    if (ly < 16 << FP) {
        ly = 16 << FP;
        if (lvy < 0)
            lvy = 0;
    }

    /* ground contact at either foot */
    cx = lx >> FP;
    cy = (ly >> FP) + FOOT_Y;
    if (cy >= terrain_y(cx - FOOT_X) || cy >= terrain_y(cx + FOOT_X))
        touchdown();
}

/* wait for fire (release, then press); returns 1 if ESC was hit */
static int wait_fire(const char *m)
{
    int released = 0;

    for (;;) {
        clear_back();
        draw_status();
        draw_terrain();
        draw_text((int)(320 - 8 * strlen(m)) / 2, 80, m, 1);
        flip();
        if (esc_pressed())
            return 1;
        if (!released) {
            if (!(joy1 & JOY_FIRE))
                released = 1;
        } else if (joy1 & JOY_FIRE) {
            return 0;
        }
    }
}

/* title screen: a lander descends onto the middle pad on repeat.
 * Returns 1 if ESC was hit, 0 when fire starts the game. */
/* 3x5 mini-font for the splash credit (low 3 bits per row, MSB = left) */
static const char font3_chars[] = "X.COM/NEILRAKT";
static const unsigned char font3[][5] = {
    { 5, 5, 2, 5, 5 }, /* X */
    { 0, 0, 0, 0, 2 }, /* . */
    { 7, 4, 4, 4, 7 }, /* C */
    { 7, 5, 5, 5, 7 }, /* O */
    { 5, 7, 7, 5, 5 }, /* M */
    { 1, 1, 2, 4, 4 }, /* / */
    { 6, 5, 5, 5, 5 }, /* N */
    { 7, 4, 6, 4, 7 }, /* E */
    { 7, 2, 2, 2, 7 }, /* I */
    { 4, 4, 4, 4, 7 }, /* L */
    { 6, 5, 6, 5, 5 }, /* R */
    { 2, 5, 7, 5, 5 }, /* A */
    { 5, 6, 4, 6, 5 }, /* K */
    { 7, 2, 2, 2, 2 }, /* T */
};

/* 3x5 mini text; advance 4px per char (3 wide + 1 gap) */
static void draw_text3x5(int x, int y, const char *s, int color)
{
    while (*s) {
        if (*s != ' ') {
            int g = 0;
            while (font3_chars[g] && font3_chars[g] != *s)
                g++;
            if (font3_chars[g]) {
                unsigned short rows[5];
                int i;
                for (i = 0; i < 5; i++)
                    rows[i] = (unsigned short)font3[g][i] << 13;
                draw_sprite(x, y, rows, 5, color);
            }
        }
        x += 4;
        s++;
    }
}

static int splash(void)
{
    int released = 0, t = 0;
    int dx_, dy_, land_y, hold = 0;

    gen_terrain();
    dx_ = (pad_seg[1] << 4) + 16;
    land_y = ty[pad_seg[1]] - FOOT_Y;
    dy_ = 50;

    for (;;) {
        clear_back();
        draw_terrain();
        draw_text2x_rainbow(72, 32, "LANDING PAD");
        if ((t & 63) < 44) /* blink */
            draw_text(120, 72, "PRESS FIRE", 1);
        draw_lander(dx_, dy_, 0, LANDER_R, 6);
        if (dy_ < land_y)
            draw_flame(dx_, dy_, 0, (t & 2) ? 3 : 4);
        draw_text3x5(126, 100, "X.COM/NEILRACKETT", 1);
        flip();
        t++;
        if (dy_ < land_y) {
            if (t & 1)
                dy_++;
        } else if (++hold > 75) {
            dy_ = 50;
            hold = 0;
        }
        if (esc_pressed())
            return 1;
        if (!released) {
            if (!(joy1 & JOY_FIRE))
                released = 1;
        } else if (joy1 & JOY_FIRE) {
            return 0;
        }
    }
}

int main(void)
{
    Kbdvecs *kv;
    void *old_joyvec, *old_phys, *old_log, *blk;
    short old_palette[16];
    int old_rez, i;

    old_rez = (int)Getrez();
    if (old_rez == 2) {
        (void)Cconws("This game needs a colour monitor (ST low res).\r\n");
        return 1;
    }

    blk = (void *)Malloc(SCREEN_SIZE + 256);
    if (!blk) {
        (void)Cconws("Out of memory.\r\n");
        return 1;
    }

    old_phys = Physbase();
    old_log = Logbase();
    for (i = 0; i < 16; i++)
        old_palette[i] = Setcolor(i, -1);

    buf[0] = (unsigned char *)old_phys;
    buf[1] = (unsigned char *)(((unsigned long)blk + 255) & ~255UL);
    memset(buf[0], 0, SCREEN_SIZE);
    memset(buf[1], 0, SCREEN_SIZE);
    draw_buf = 1;
    back = buf[1];
    Setscreen(buf[0], buf[0], 0); /* low res */
    (void)Setcolor(0, 0x000);     /* black */
    (void)Setcolor(1, 0x777);     /* white: terrain, text */
    (void)Setcolor(2, 0x700);     /* red */
    (void)Setcolor(3, 0x750);     /* orange: flame */
    (void)Setcolor(4, 0x770);     /* yellow: flame, messages */
    (void)Setcolor(5, 0x070);     /* green: pads */
    (void)Setcolor(6, 0x077);     /* cyan: lander, lives */
    (void)Setcolor(7, 0x707);     /* magenta */
    for (i = 8; i < 16; i++)
        (void)Setcolor(i, 0x000);

    /* hook joystick 1 and make sure the IKBD reports joystick events */
    kv = (Kbdvecs *)Kbdvbase();
    old_joyvec = kv->joyvec;
    kv->joyvec = (void *)joy_handler;
    Ikbdws(0, "\024"); /* $14: joystick event reporting */

    quit = 0;
    if (!splash()) {
        while (!quit) {
            reset_game();
            while (lives > 0) {
                if (esc_pressed()) {
                    quit = 1;
                    break;
                }
                play_frame();
                draw_frame();
            }
            if (!quit && wait_fire("GAME OVER"))
                break;
        }
    }

    /* restore everything */
    kv->joyvec = old_joyvec;
    Ikbdws(0, "\010"); /* $08: mouse relative reporting (desktop default) */
    Setscreen(old_log, old_phys, old_rez);
    for (i = 0; i < 16; i++)
        (void)Setcolor(i, old_palette[i]);
    (void)Mfree(blk);

    return 0;
}
