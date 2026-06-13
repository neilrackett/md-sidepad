/*
 * launchpad.c — LAUNCHPAD, a Missile-Command-style game for the Atari ST
 * (low resolution).
 *
 * Defend your six cities from the central launch pad. Move the crosshair
 * and fire to launch an interceptor; its explosion destroys any incoming
 * missiles it touches (chain reactions included). A wave ends when every
 * incoming missile is gone; you get a bonus for surviving cities and
 * spare ammo. Lose every city and it's over.
 *
 * Works with mouse OR joystick, chosen on the splash screen (fire =
 * joystick, left click = mouse). The joystick is read by hooking joyvec
 * (offset 24 in the KBDVECS table from Kbdvbase) — the physical stick or
 * the one Sidepad injects — and the mouse by hooking mousevec (offset
 * 16). No IKBD mode commands are sent: the desktop default reports both
 * mouse packets and joystick 1 events, and selecting joystick event
 * reporting ($14) would switch the mouse off.
 *
 * Controls: stick/mouse = crosshair, fire/left click = launch,
 * ESC = quit.
 *
 * Build (inside atarist-toolkit-docker): stcmd make
 */

#include <osbind.h>
#include <string.h>

#define LINE_BYTES  160
#define SCREEN_SIZE 32000L

/* IKBD joystick state bits */
#define JOY_UP    0x01
#define JOY_DOWN  0x02
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x80

/* mouse packet header bit: left button */
#define MOUSE_LEFT 0x02

#define MODE_JOY   0
#define MODE_MOUSE 1

/* positions/velocities are fixed point, 1/16 pixel */
#define FP 4

#define NCITY     6
#define MAXIN     12 /* incoming missiles in flight */
#define MAXSHOT   4  /* interceptors in flight */
#define MAXEXP    8

#define SKY_TOP   16  /* incoming missiles start here */
#define GROUND_Y  184 /* ground fills from here down */
#define IMPACT_Y  180 /* incoming missiles detonate here */
#define BASE_X    160
#define AMMO_WAVE 30
#define JOY_SPEED 4   /* crosshair px/frame on the stick */
#define SHOT_PX   6   /* interceptor px/frame */
#define EXP_R     12  /* full explosion radius */

/* KBDVECS layout (Kbdvbase); mousevec at offset 16, joyvec at 24 */
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
 * Input handlers. Explicit asm symbol names so this works whether or not
 * the toolchain prefixes C symbols with an underscore.
 *
 * joyvec: a0 -> [header, joy0, joy1]; latch the joystick 1 byte.
 *
 * mousevec: a0 -> [$F8|buttons, dx, dy]; latch the buttons and add the
 * deltas into free-running accumulators. The main loop keeps the last
 * values it saw and takes differences, so there is no read/reset race.
 */
volatile unsigned char joy1 __asm__("g_joy1") = 0;
volatile unsigned char mbut __asm__("g_mbut") = 0;
volatile short mdx_acc __asm__("g_mdx") = 0;
volatile short mdy_acc __asm__("g_mdy") = 0;
void joy_handler(void) __asm__("g_joyhandler");
void mouse_handler(void) __asm__("g_mousehandler");

__asm__("\t.text\n"
        "\t.even\n"
        "\t.globl g_joyhandler\n"
        "g_joyhandler:\n"
        "\tmove.b 2(%a0),g_joy1\n"
        "\trts\n"
        "\t.even\n"
        "\t.globl g_mousehandler\n"
        "g_mousehandler:\n"
        "\tmove.w %d0,-(%sp)\n"
        "\tmove.b (%a0),g_mbut\n"
        "\tmove.b 1(%a0),%d0\n"
        "\text.w %d0\n"
        "\tadd.w %d0,g_mdx\n"
        "\tmove.b 2(%a0),%d0\n"
        "\text.w %d0\n"
        "\tadd.w %d0,g_mdy\n"
        "\tmove.w (%sp)+,%d0\n"
        "\trts\n");

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

/* 16x8 city skyline and its rubble */
static const unsigned short spr_city[8] = {
    0x1020, 0x3870, 0x3B76, 0x7B76, 0x7FFE, 0x7FFE, 0xFFFF, 0xFFFF
};
static const unsigned short spr_rubble[8] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0420, 0x1188, 0x5AB4, 0xFFFF
};

/* 16x10 launch pad pyramid */
static const unsigned short spr_base[10] = {
    0x0180, 0x03C0, 0x03C0, 0x07E0, 0x07E0,
    0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE, 0xFFFF
};

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static const int cityx[NCITY] = { 40, 80, 120, 200, 240, 280 };
static int city_alive[NCITY];

static struct {
    int active;
    int ox;           /* trail origin, px */
    int x, y, vx, vy; /* head, 1/16 px */
    int target;       /* city index */
} in[MAXIN];

static struct {
    int active;
    int x, y, vx, vy; /* 1/16 px */
    int tx, ty;       /* detonation point, px */
    int steps;
} shot[MAXSHOT];

static struct {
    int active;
    int x, y, t;
} expl[MAXEXP];

static int mode;            /* MODE_JOY or MODE_MOUSE */
static int crx, cry;        /* crosshair, px */
static short last_mdx, last_mdy;
static int prev_fire;
static int wave, ammo, pending;
static long frame;
static long score;
static int quit;

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

/* fast horizontal span (inclusive), clipped */
static void hspan(int x0, int x1, int y, int color)
{
    unsigned char *row;
    int g0, g1, g, pl;

    if ((unsigned)y >= 200)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > 319)
        x1 = 319;
    if (x0 > x1)
        return;
    row = back + (long)y * LINE_BYTES;
    g0 = x0 >> 4;
    g1 = x1 >> 4;
    for (g = g0; g <= g1; g++) {
        unsigned short m = 0xFFFF;
        unsigned char *p = row + (g << 3);
        if (g == g0)
            m &= 0xFFFF >> (x0 & 15);
        if (g == g1)
            m &= (unsigned short)(0xFFFFU << (15 - (x1 & 15)));
        for (pl = 0; pl < 4; pl++)
            if (color & (1 << pl))
                *(unsigned short *)(p + (pl << 1)) |= m;
    }
}

static void fill_circle(int cx, int cy, int r, int color)
{
    int dy, w;

    for (dy = -r; dy <= r; dy++) {
        w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r)
            w++;
        hspan(cx - w, cx + w, cy + dy, color);
    }
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

/* --- scenery --- */

static void draw_scenery(void)
{
    int y, i;

    for (y = GROUND_Y; y < 200; y++)
        hspan(0, 319, y, 3);
    for (i = 0; i < NCITY; i++)
        draw_sprite(cityx[i] - 8, 176, city_alive[i] ? spr_city : spr_rubble,
                    8, city_alive[i] ? 6 : 8);
    draw_sprite(BASE_X - 8, 174, spr_base, 10, 8);
}

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
    digits[2] = '\0';
    digits[0] = (char)('0' + ammo / 10);
    digits[1] = (char)('0' + ammo % 10);
    draw_text(140, 4, "AMMO", 1);
    draw_text(178, 4, digits, ammo ? 1 : 2);
    digits[0] = (char)('0' + wave / 10);
    digits[1] = (char)('0' + wave % 10);
    draw_text(248, 4, "WAVE", 1);
    draw_text(286, 4, digits, 1);
}

static void draw_crosshair(void)
{
    line(crx - 3, cry, crx + 3, cry, 1);
    line(crx, cry - 3, crx, cry + 3, 1);
}

/* explosion radius for age t, or -1 when finished */
static int exp_radius(int t)
{
    if (t <= 16)
        return t * EXP_R / 16;
    if (t <= 40)
        return EXP_R;
    if (t <= 64)
        return EXP_R - (t - 40) * EXP_R / 24;
    return -1;
}

static void draw_frame(void)
{
    int i;

    clear_back();
    draw_status();
    draw_scenery();
    for (i = 0; i < MAXIN; i++)
        if (in[i].active) {
            line(in[i].ox, SKY_TOP, in[i].x >> FP, in[i].y >> FP, 2);
            plot(in[i].x >> FP, in[i].y >> FP, 1);
        }
    for (i = 0; i < MAXSHOT; i++)
        if (shot[i].active) {
            line(BASE_X, 174, shot[i].x >> FP, shot[i].y >> FP, 6);
            plot(shot[i].tx, shot[i].ty, (frame & 2) ? 4 : 6);
        }
    for (i = 0; i < MAXEXP; i++)
        if (expl[i].active)
            fill_circle(expl[i].x, expl[i].y, exp_radius(expl[i].t),
                        (frame & 2) ? 4 : 3);
    draw_crosshair();
    flip();
}

/* --- game logic --- */

static void spawn_explosion(int x, int y)
{
    int i;

    for (i = 0; i < MAXEXP; i++)
        if (!expl[i].active) {
            expl[i].active = 1;
            expl[i].x = x;
            expl[i].y = y;
            expl[i].t = 0;
            return;
        }
}

static int cities_alive(void)
{
    int i, n = 0;

    for (i = 0; i < NCITY; i++)
        n += city_alive[i];
    return n;
}

static void spawn_incoming(void)
{
    int i, t, tries;

    for (i = 0; i < MAXIN; i++)
        if (!in[i].active)
            break;
    if (i == MAXIN)
        return;
    t = rnd(NCITY);
    for (tries = 0; tries < NCITY && !city_alive[t]; tries++)
        t = (t + 1) % NCITY;
    if (!city_alive[t])
        return;
    in[i].active = 1;
    in[i].ox = 20 + rnd(280);
    in[i].x = in[i].ox << FP;
    in[i].y = SKY_TOP << FP;
    in[i].target = t;
    in[i].vy = 3 + wave;
    if (in[i].vy > 14)
        in[i].vy = 14;
    in[i].vx = (cityx[t] - in[i].ox) * in[i].vy / (IMPACT_Y - SKY_TOP);
    pending--;
}

static void fire_shot(void)
{
    int i, dx, dy, d, steps;

    if (ammo <= 0)
        return;
    for (i = 0; i < MAXSHOT; i++)
        if (!shot[i].active)
            break;
    if (i == MAXSHOT)
        return;
    ammo--;
    dx = crx - BASE_X;
    dy = cry - 174;
    d = dx < 0 ? -dx : dx;
    if (dy < -d)
        d = -dy;
    steps = d / SHOT_PX + 1;
    shot[i].active = 1;
    shot[i].x = BASE_X << FP;
    shot[i].y = 174 << FP;
    shot[i].tx = crx;
    shot[i].ty = cry;
    shot[i].vx = (dx << FP) / steps;
    shot[i].vy = (dy << FP) / steps;
    shot[i].steps = steps;
}

static void reset_wave(void)
{
    int i;

    ammo = AMMO_WAVE;
    pending = 6 + wave * 3;
    for (i = 0; i < MAXIN; i++)
        in[i].active = 0;
    for (i = 0; i < MAXSHOT; i++)
        shot[i].active = 0;
    for (i = 0; i < MAXEXP; i++)
        expl[i].active = 0;
}

static void reset_game(void)
{
    int i;

    score = 0;
    wave = 1;
    for (i = 0; i < NCITY; i++)
        city_alive[i] = 1;
    crx = 160;
    cry = 100;
    last_mdx = mdx_acc;
    last_mdy = mdy_acc;
    prev_fire = 1; /* require a fresh press */
    reset_wave();
}

/* hold a message over the live scene for n frames */
static void msg_pause(const char *m, int color, int n)
{
    int t;

    for (t = 0; t < n; t++) {
        clear_back();
        draw_status();
        draw_scenery();
        draw_text((int)(320 - 8 * strlen(m)) / 2, 64, m, color);
        flip();
    }
}

static void play_frame(void)
{
    unsigned char joy = joy1;
    int i, j, fire_now;

    frame++;

    /* crosshair */
    if (mode == MODE_MOUSE) {
        short cx = mdx_acc, cy = mdy_acc;
        crx += (short)(cx - last_mdx);
        cry += (short)(cy - last_mdy);
        last_mdx = cx;
        last_mdy = cy;
        fire_now = (mbut & MOUSE_LEFT) != 0;
    } else {
        if (joy & JOY_LEFT)
            crx -= JOY_SPEED;
        if (joy & JOY_RIGHT)
            crx += JOY_SPEED;
        if (joy & JOY_UP)
            cry -= JOY_SPEED;
        if (joy & JOY_DOWN)
            cry += JOY_SPEED;
        fire_now = (joy & JOY_FIRE) != 0;
    }
    if (crx < 8)
        crx = 8;
    if (crx > 311)
        crx = 311;
    if (cry < 24)
        cry = 24;
    if (cry > 168)
        cry = 168;
    if (fire_now && !prev_fire)
        fire_shot();
    prev_fire = fire_now;

    /* stagger the incoming missiles */
    if (pending > 0 && rnd(40) == 0)
        spawn_incoming();

    /* interceptors */
    for (i = 0; i < MAXSHOT; i++)
        if (shot[i].active) {
            shot[i].x += shot[i].vx;
            shot[i].y += shot[i].vy;
            if (--shot[i].steps <= 0) {
                shot[i].active = 0;
                spawn_explosion(shot[i].tx, shot[i].ty);
            }
        }

    /* explosions age out */
    for (i = 0; i < MAXEXP; i++)
        if (expl[i].active && exp_radius(++expl[i].t) < 0)
            expl[i].active = 0;

    /* incoming missiles: explosion contact, then ground impact */
    for (i = 0; i < MAXIN; i++) {
        int hx, hy, hit = 0;

        if (!in[i].active)
            continue;
        in[i].x += in[i].vx;
        in[i].y += in[i].vy;
        hx = in[i].x >> FP;
        hy = in[i].y >> FP;
        for (j = 0; j < MAXEXP && !hit; j++)
            if (expl[j].active) {
                int r = exp_radius(expl[j].t);
                int dx = hx - expl[j].x, dy = hy - expl[j].y;
                if (dx * dx + dy * dy <= r * r)
                    hit = 1;
            }
        if (hit) {
            in[i].active = 0;
            score += 25;
            spawn_explosion(hx, hy); /* chain reaction */
        } else if (hy >= IMPACT_Y) {
            in[i].active = 0;
            city_alive[in[i].target] = 0;
            spawn_explosion(cityx[in[i].target], 176);
        }
    }
}

/* anything still in the air? */
static int wave_busy(void)
{
    int i;

    if (pending > 0)
        return 1;
    for (i = 0; i < MAXIN; i++)
        if (in[i].active)
            return 1;
    for (i = 0; i < MAXSHOT; i++)
        if (shot[i].active)
            return 1;
    for (i = 0; i < MAXEXP; i++)
        if (expl[i].active)
            return 1;
    return 0;
}

/* fire/click released, then pressed; returns 1 if ESC was hit */
static int wait_start(const char *m)
{
    int released = 0;
    long t = 0;

    for (;;) {
        clear_back();
        draw_status();
        draw_scenery();
        draw_text2x((int)(320 - 16 * strlen(m)) / 2, 72, m, 2);
        if ((t & 63) < 44) /* blink */
            draw_text(108, 112, "FIRE OR CLICK", 1);
        flip();
        t++;
        if (esc_pressed())
            return 1;
        if (!released) {
            if (!(joy1 & JOY_FIRE) && !(mbut & MOUSE_LEFT))
                released = 1;
        } else if ((joy1 & JOY_FIRE) || (mbut & MOUSE_LEFT)) {
            return 0;
        }
    }
}

/* title screen: missiles rain down behind the title.
 * Returns the chosen mode, or -1 if ESC was hit. */
static int splash(void)
{
    int released = 0, i;
    long t = 0;
    int dmy[3], dmox[3], dmtx[3];

    for (i = 0; i < NCITY; i++)
        city_alive[i] = 1;
    for (i = 0; i < 3; i++) {
        dmy[i] = 90 + i * 30;
        dmox[i] = 20 + rnd(280);
        dmtx[i] = cityx[rnd(NCITY)];
    }

    for (;;) {
        clear_back();
        for (i = 0; i < 3; i++) {
            int hx = dmox[i] +
                     (dmtx[i] - dmox[i]) * (dmy[i] - SKY_TOP) /
                         (IMPACT_Y - SKY_TOP);
            line(dmox[i], SKY_TOP, hx, dmy[i], 2);
            plot(hx, dmy[i], 1);
            if (++dmy[i] >= IMPACT_Y) {
                dmy[i] = 90;
                dmox[i] = 20 + rnd(280);
                dmtx[i] = cityx[rnd(NCITY)];
            }
        }
        draw_scenery();
        draw_text2x_rainbow(88, 32, "LAUNCHPAD");
        if ((t & 63) < 44) { /* blink */
            draw_text(76, 88, "PRESS FIRE = JOYSTICK", 1);
            draw_text(76, 104, "LEFT CLICK = MOUSE", 1);
        }
        flip();
        t++;
        if (esc_pressed())
            return -1;
        if (!released) {
            if (!(joy1 & JOY_FIRE) && !(mbut & MOUSE_LEFT))
                released = 1;
        } else if (joy1 & JOY_FIRE) {
            return MODE_JOY;
        } else if (mbut & MOUSE_LEFT) {
            return MODE_MOUSE;
        }
    }
}

int main(void)
{
    Kbdvecs *kv;
    void *old_joyvec, *old_mousevec, *old_phys, *old_log, *blk;
    short old_palette[16];
    int old_rez, i;
    char wm[16];

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
    (void)Setcolor(1, 0x777);     /* white: crosshair, text */
    (void)Setcolor(2, 0x700);     /* red: missile trails */
    (void)Setcolor(3, 0x750);     /* orange: ground, explosions */
    (void)Setcolor(4, 0x770);     /* yellow: explosions */
    (void)Setcolor(5, 0x070);     /* green */
    (void)Setcolor(6, 0x077);     /* cyan: cities, interceptors */
    (void)Setcolor(7, 0x707);     /* magenta */
    (void)Setcolor(8, 0x444);     /* grey: rubble, launch pad */
    for (i = 9; i < 16; i++)
        (void)Setcolor(i, 0x000);

    /*
     * Hook the mouse and joystick vectors. Don't send any IKBD mode
     * command: the default state already reports both.
     */
    kv = (Kbdvecs *)Kbdvbase();
    old_mousevec = kv->mousevec;
    old_joyvec = kv->joyvec;
    kv->mousevec = (void *)mouse_handler;
    kv->joyvec = (void *)joy_handler;

    quit = 0;
    mode = splash();
    if (mode >= 0) {
        while (!quit) {
            reset_game();
            for (;;) {
                if (esc_pressed()) {
                    quit = 1;
                    break;
                }
                play_frame();
                draw_frame();
                if (cities_alive() == 0)
                    break;
                if (!wave_busy()) {
                    score += cities_alive() * 50 + ammo * 5;
                    wm[0] = 'W';
                    wm[1] = 'A';
                    wm[2] = 'V';
                    wm[3] = 'E';
                    wm[4] = ' ';
                    wm[5] = (char)('0' + wave / 10);
                    wm[6] = (char)('0' + wave % 10);
                    wm[7] = '\0';
                    strcat(wm, " CLEARED");
                    msg_pause(wm, 4, 100);
                    wave++;
                    reset_wave();
                }
            }
            if (!quit && wait_start("GAME OVER"))
                break;
        }
    }

    /* restore everything */
    kv->mousevec = old_mousevec;
    kv->joyvec = old_joyvec;
    Setscreen(old_log, old_phys, old_rez);
    for (i = 0; i < 16; i++)
        (void)Setcolor(i, old_palette[i]);
    (void)Mfree(blk);

    return 0;
}
