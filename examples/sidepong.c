/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * sidepong.c — SIDEPONG, a two-player Pong for the Atari ST (low res):
 * mouse vs joystick.
 *
 * The left paddle is driven by joystick 1 (hooking joyvec, offset 24 in
 * the KBDVECS table returned by Kbdvbase) — the physical stick or the
 * one Sidepad injects — and the right paddle by the mouse (hooking
 * mousevec, offset 16), matching Sidepad's left-stick-joystick /
 * right-stick-mouse layout. No IKBD mode commands are sent: the default
 * reports both mouse packets and joystick 1 events, and selecting
 * joystick event reporting ($14) would switch the mouse off.
 *
 * Controls: joystick up/down = left paddle, mouse up/down = right
 * paddle, fire or left click = start, ESC = quit. First to 11 wins.
 *
 * Build (inside atarist-toolkit-docker): stcmd make
 */

#include <osbind.h>
#include <string.h>

#define LINE_BYTES  160
#define SCREEN_SIZE 32000L

/* IKBD joystick state bits */
#define JOY_UP   0x01
#define JOY_DOWN 0x02
#define JOY_FIRE 0x80

/* mouse packet header bit: left button */
#define MOUSE_LEFT 0x02

/* positions/velocities are fixed point, 1/16 pixel */
#define FP 4

#define WALL_TOP 16  /* grey walls at y 16-17 and 196-197 */
#define WALL_BOT 196
#define PLAY_TOP (WALL_TOP + 2)
#define PLAY_BOT WALL_BOT /* exclusive */

#define PAD_H       32
#define LEFT_X      8   /* paddles are 4 px wide */
#define RIGHT_X     308
#define JOY_SPEED   5   /* joystick paddle px/frame */

#define BALL_VX     40  /* serve speed, 1/16 px/frame */
#define VX_STEP     3   /* speed up per paddle hit */
#define VX_MAX      80
#define SERVE_DELAY 50  /* frames the ball waits at centre */
#define WIN_SCORE   11

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
 * mousevec: a0 -> [$F8|buttons, dx, dy]; latch the buttons and add dy
 * into a free-running accumulator. The main loop keeps the last value it
 * saw and takes differences, so there is no read/reset race.
 */
volatile unsigned char joy1 __asm__("g_joy1") = 0;
volatile unsigned char mbut __asm__("g_mbut") = 0;
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

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static int mpy, jpy;          /* paddle top y, pixels */
static short last_mdy;        /* last mdy_acc value seen */
static int bx, by, bvx, bvy;  /* ball top-left, 1/16 px */
static int serve_timer;       /* ball held at centre while > 0 */
static int mscore, jscore;
static long frame;
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

/* 4-pixel-wide vertical bar (paddles and the ball) */
static void draw_bar(int x, int y, int h, int color)
{
    unsigned char *p = back + (long)y * LINE_BYTES + ((x >> 4) << 3);
    int shift = x & 15;
    int last_group = (x >> 4) >= 19;
    unsigned long d = 0xF0000000UL >> shift;
    unsigned short hi = (unsigned short)(d >> 16);
    unsigned short lo = (unsigned short)d;
    int i, pl;

    for (i = 0; i < h; i++) {
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

/* full-width horizontal bar, 2 px tall (the court walls) */
static void draw_wall(int y, int color)
{
    unsigned char *p = back + (long)y * LINE_BYTES;
    int row, g, pl;

    for (row = 0; row < 2; row++) {
        unsigned char *q = p;
        for (g = 0; g < 20; g++) {
            for (pl = 0; pl < 4; pl++)
                if (color & (1 << pl)) {
                    q[pl << 1] = 0xFF;
                    q[(pl << 1) + 1] = 0xFF;
                }
            q += 8;
        }
        p += LINE_BYTES;
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

/* fire button or left mouse button currently down? */
static int start_pressed(void)
{
    return (joy1 & JOY_FIRE) || (mbut & MOUSE_LEFT);
}

/* --- court drawing --- */

static void draw_court(void)
{
    int y;

    draw_wall(WALL_TOP, 8);
    draw_wall(WALL_BOT, 8);
    for (y = PLAY_TOP + 2; y < PLAY_BOT - 4; y += 8)
        draw_bar(159, y, 4, 8);
}

static void draw_scores(void)
{
    char s[3];
    int n;

    n = jscore;
    if (n >= 10) {
        s[0] = (char)('0' + n / 10);
        s[1] = (char)('0' + n % 10);
        s[2] = '\0';
        draw_text2x(112, 24, s, 5);
    } else {
        s[0] = (char)('0' + n);
        s[1] = '\0';
        draw_text2x(128, 24, s, 5);
    }
    n = mscore;
    s[0] = (char)('0' + n / 10);
    s[1] = (char)('0' + n % 10);
    s[2] = '\0';
    if (n >= 10)
        draw_text2x(176, 24, s, 6);
    else
        draw_text2x(176, 24, s + 1, 6);
}

static void draw_frame(void)
{
    clear_back();
    draw_court();
    draw_scores();
    draw_bar(LEFT_X, jpy, PAD_H, 5);
    draw_bar(RIGHT_X, mpy, PAD_H, 6);
    if (!serve_timer || (frame & 4))
        draw_bar(bx >> FP, by >> FP, 4, 1);
    flip();
}

/* --- game logic --- */

/* dir: +1 = serve toward the mouse, -1 = toward the joystick */
static void serve(int dir)
{
    bx = 158 << FP;
    by = (PLAY_TOP + rnd(PLAY_BOT - PLAY_TOP - 4)) << FP;
    bvx = dir * BALL_VX;
    bvy = rnd(2) ? 16 : -16;
    serve_timer = SERVE_DELAY;
}

static void reset_game(void)
{
    mscore = 0;
    jscore = 0;
    mpy = (PLAY_TOP + PLAY_BOT - PAD_H) / 2;
    jpy = mpy;
    last_mdy = mdy_acc;
    serve(rnd(2) ? 1 : -1);
}

/* ball hit a paddle: reflect, speed up, set spin from the hit offset */
static void paddle_hit(int pad_y)
{
    int off = ((by >> FP) + 2) - (pad_y + PAD_H / 2);

    bvx = -bvx;
    if (bvx > 0)
        bvx += VX_STEP;
    else
        bvx -= VX_STEP;
    if (bvx > VX_MAX)
        bvx = VX_MAX;
    if (bvx < -VX_MAX)
        bvx = -VX_MAX;
    bvy = off * 3;
}

static void play_frame(void)
{
    unsigned char joy = joy1;
    short cur = mdy_acc;

    frame++;

    /* mouse paddle: apply the accumulated delta since last frame */
    mpy += (short)(cur - last_mdy);
    last_mdy = cur;
    if (mpy < PLAY_TOP)
        mpy = PLAY_TOP;
    if (mpy > PLAY_BOT - PAD_H)
        mpy = PLAY_BOT - PAD_H;

    /* joystick paddle */
    if (joy & JOY_UP)
        jpy -= JOY_SPEED;
    if (joy & JOY_DOWN)
        jpy += JOY_SPEED;
    if (jpy < PLAY_TOP)
        jpy = PLAY_TOP;
    if (jpy > PLAY_BOT - PAD_H)
        jpy = PLAY_BOT - PAD_H;

    if (serve_timer) {
        serve_timer--;
        return;
    }

    bx += bvx;
    by += bvy;

    /* walls */
    if (by < PLAY_TOP << FP) {
        by = PLAY_TOP << FP;
        bvy = -bvy;
    }
    if (by > (PLAY_BOT - 4) << FP) {
        by = (PLAY_BOT - 4) << FP;
        bvy = -bvy;
    }

    /* paddles (only when the ball is moving toward them) */
    if (bvx < 0 && bx <= (LEFT_X + 4) << FP && bx >= (LEFT_X - 2) << FP &&
        (by >> FP) + 4 > jpy && (by >> FP) < jpy + PAD_H) {
        bx = (LEFT_X + 4) << FP;
        paddle_hit(jpy);
    }
    if (bvx > 0 && bx + (4 << FP) >= RIGHT_X << FP &&
        bx <= (RIGHT_X + 2) << FP && (by >> FP) + 4 > mpy &&
        (by >> FP) < mpy + PAD_H) {
        bx = (RIGHT_X - 4) << FP;
        paddle_hit(mpy);
    }

    /* out: point to the other side, serve toward the loser */
    if (bx < -(16 << FP)) {
        mscore++;
        if (mscore < WIN_SCORE)
            serve(-1);
    } else if (bx > 336 << FP) {
        jscore++;
        if (jscore < WIN_SCORE)
            serve(1);
    }
}

/* wait for fire/click (release, then press); returns 1 if ESC was hit.
 * Keeps drawing the court behind the message. */
static int wait_start(const char *m, int color)
{
    int released = 0;
    long t = 0;

    for (;;) {
        clear_back();
        draw_court();
        draw_scores();
        draw_bar(LEFT_X, jpy, PAD_H, 5);
        draw_bar(RIGHT_X, mpy, PAD_H, 6);
        draw_text2x((int)(320 - 16 * strlen(m)) / 2, 88, m, color);
        if ((t & 63) < 44) /* blink */
            draw_text(108, 120, "FIRE OR CLICK", 1);
        flip();
        t++;
        if (esc_pressed())
            return 1;
        if (!released) {
            if (!start_pressed())
                released = 1;
        } else if (start_pressed()) {
            return 0;
        }
    }
}

/* title screen with a self-playing rally.
 * Returns 1 if ESC was hit, 0 when fire/click starts the game. */
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
    int released = 0;
    long t = 0;
    int dbx = 150, dby = 110, dvx = 3, dvy = 2;
    int dlp = 100, drp = 100, c;

    for (;;) {
        clear_back();
        draw_text2x_rainbow(96, 24, "SIDEPONG");
        draw_text(92, 56, "JOYSTICK", 5);
        draw_text(164, 56, "VS", 1);
        draw_text(188, 56, "MOUSE", 6);
        draw_bar(48, dlp, 24, 5);
        draw_bar(268, drp, 24, 6);
        draw_bar(dbx, dby, 4, 1);
        if ((t & 63) < 44) /* blink */
            draw_text(72, 168, "FIRE OR CLICK TO START", 1);
        draw_text3x5(126, 190, "X.COM/NEILRACKETT", 1);
        flip();
        t++;

        /* demo rally: ball bounces, paddles chase it */
        dbx += dvx;
        dby += dvy;
        if (dbx <= 52) {
            dbx = 52;
            dvx = 3;
            dvy = rnd(5) - 2;
        }
        if (dbx >= 264) {
            dbx = 264;
            dvx = -3;
            dvy = rnd(5) - 2;
        }
        if (dby <= 84) {
            dby = 84;
            dvy = -dvy;
        }
        if (dby >= 148) {
            dby = 148;
            dvy = -dvy;
        }
        c = dby - 10;
        if (dlp < c)
            dlp += 2;
        if (dlp > c)
            dlp -= 2;
        if (dvx > 0) {
            if (drp < c)
                drp += 2;
            if (drp > c)
                drp -= 2;
        }
        if (dlp < 84)
            dlp = 84;
        if (dlp > 128)
            dlp = 128;
        if (drp < 84)
            drp = 84;
        if (drp > 128)
            drp = 128;

        if (esc_pressed())
            return 1;
        if (!released) {
            if (!start_pressed())
                released = 1;
        } else if (start_pressed()) {
            return 0;
        }
    }
}

int main(void)
{
    Kbdvecs *kv;
    void *old_joyvec, *old_mousevec, *old_phys, *old_log, *blk;
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
    (void)Setcolor(1, 0x777);     /* white: ball, text */
    (void)Setcolor(2, 0x700);     /* red */
    (void)Setcolor(3, 0x750);     /* orange */
    (void)Setcolor(4, 0x770);     /* yellow */
    (void)Setcolor(5, 0x070);     /* green: joystick paddle */
    (void)Setcolor(6, 0x077);     /* cyan: mouse paddle */
    (void)Setcolor(7, 0x707);     /* magenta */
    (void)Setcolor(8, 0x444);     /* grey: walls, centre line */
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
    if (!splash()) {
        while (!quit) {
            reset_game();
            while (mscore < WIN_SCORE && jscore < WIN_SCORE) {
                if (esc_pressed()) {
                    quit = 1;
                    break;
                }
                play_frame();
                draw_frame();
            }
            if (!quit) {
                if (mscore >= WIN_SCORE) {
                    if (wait_start("MOUSE WINS", 6))
                        break;
                } else if (wait_start("JOYSTICK WINS", 5)) {
                    break;
                }
            }
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
