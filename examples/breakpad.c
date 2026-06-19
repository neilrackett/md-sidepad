/*
 * breakpad.c — BREAKPAD, a simple Breakout-style game for the
 * Atari ST (low resolution).
 *
 * Input is read by hooking the IKBD vectors in the KBDVECS table
 * returned by Kbdvbase: joyvec (offset 24) for the joystick and
 * mousevec (offset 16) for the mouse. On the splash screen, pressing
 * joystick fire selects joystick control and the left mouse button
 * selects mouse control. Joystick packets are [header, joy0, joy1]
 * (the game reads joystick 1, which is also the stick Sidepad injects
 * into); mouse packets are [$F8|buttons, dx, dy] with left button =
 * bit 1 of the header.
 *
 * Controls: left/right (or mouse) = move paddle, fire/click = launch,
 * ESC = quit.
 *
 * Build (inside atarist-toolkit-docker): stcmd make
 */

#include <osbind.h>
#include <string.h>

#define LINE_BYTES  160
#define SCREEN_SIZE 32000L

/* IKBD joystick state bits */
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x80

#define MOUSE_LEFT 0x02 /* in the mouse packet header */

#define MODE_JOY   0
#define MODE_MOUSE 1

/* ball position/velocity is fixed point, 1/16 pixel */
#define FP 4

#define PADDLE_W     32
#define PADDLE_H     5
#define PADDLE_Y     188
#define PADDLE_SPEED 6 /* joystick px/frame */

#define BALL_W      4
#define BALL_VY     50 /* base vertical speed, 1/16 px/frame */
#define BALL_VYMAX  60

#define BRICK_ROWS 6
#define BRICK_COLS 20
#define BRICK_TOP  28 /* grid pitch is 16x8 */

#define TOP_Y 16 /* ball bounces here, below the status bar */

/* KBDVECS layout (Kbdvbase) */
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
 * IKBD vector handlers, called via jsr with a0 -> packet. The joystick
 * handler latches the joystick 1 byte; the mouse handler latches the
 * button bits and accumulates dx into a free-running counter (the game
 * keeps its own last-read value, so no reset is needed and reads stay
 * race-free). Explicit asm symbol names so this works whether or not
 * the toolchain prefixes C symbols with an underscore.
 */
volatile unsigned char joy1 __asm__("g_joy1") = 0;
volatile unsigned char mbut __asm__("g_mbut") = 0;
volatile short mdx_acc __asm__("g_mdx") = 0;
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
        "\tmove.w (%sp)+,%d0\n"
        "\trts\n");

/* --- sprites (16-bit rows, MSB = leftmost pixel) --- */

static const unsigned short spr_brick[7] = {
    0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE
};

static const unsigned short spr_paddle[PADDLE_H] = {
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
};

static const unsigned short spr_ball[BALL_W] = {
    0x6000, 0xF000, 0xF000, 0x6000
};

static const unsigned short spr_life[3] = { 0xFFC0, 0xFFC0, 0xFFC0 };

/* colour of each brick row, top to bottom (palette indices) */
static const int row_color[BRICK_ROWS] = { 2, 3, 4, 5, 6, 7 };

/* 8x8 font, only the glyphs the game needs */
static const char font_chars[] = "0123456789ABCDEFGIJKLMNOPRSTUVY=";
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
    { 0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x38, 0x38, 0x00 }, /* Y */
    { 0x00, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0x00, 0x00 }, /* = */
};

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

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static int input_mode;
static short last_mdx;
static unsigned char brick[BRICK_ROWS][BRICK_COLS];
static int brick_count;
static int ppx;               /* paddle x, pixels */
static int bx, by, bvx, bvy;  /* ball, 1/16 px */
static int ball_stuck, stuck_timer;
static long frame;
static long score;
static int lives, level, quit;

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
        draw_text2x(x, y, one, row_color[i % BRICK_ROWS]);
        x += 16;
        s++;
        i++;
    }
}

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

static void draw_paddle(int x, int y)
{
    draw_sprite(x, y, spr_paddle, PADDLE_H, 6);
    draw_sprite(x + 16, y, spr_paddle, PADDLE_H, 6);
}

static void draw_bricks(void)
{
    int r, c;

    for (r = 0; r < BRICK_ROWS; r++)
        for (c = 0; c < BRICK_COLS; c++)
            if (brick[r][c])
                draw_sprite(c << 4, BRICK_TOP + (r << 3), spr_brick, 7,
                            row_color[r]);
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
    for (i = 0; i < lives; i++)
        draw_sprite(304 - i * 14, 6, spr_life, 3, 6);
    /* grey line under the status bar; the ball bounces off it */
    for (i = 0; i < 20; i++) {
        *(unsigned short *)(back + 14L * LINE_BYTES + (i << 3) + 6) = 0xFFFF;
        *(unsigned short *)(back + 15L * LINE_BYTES + (i << 3) + 6) = 0xFFFF;
    }
}

/* show what was just drawn, start drawing into the other buffer */
static void flip(void)
{
    Setscreen(buf[draw_buf ^ 1], buf[draw_buf], -1);
    draw_buf ^= 1;
    back = buf[draw_buf];
    Vsync();
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

/* fire / left click, depending on the selected mode */
static int btn_down(void)
{
    if (input_mode == MODE_MOUSE)
        return (mbut & MOUSE_LEFT) != 0;
    return (joy1 & JOY_FIRE) != 0;
}

/* --- game logic --- */

static void reset_ball(void)
{
    ball_stuck = 1;
    stuck_timer = 50;
    bvx = 0;
    bvy = 0;
}

static void reset_level(void)
{
    memset(brick, 1, sizeof(brick));
    brick_count = BRICK_ROWS * BRICK_COLS;
    ppx = (320 - PADDLE_W) / 2;
    reset_ball();
}

static void reset_game(void)
{
    score = 0;
    lives = 3;
    level = 0;
    reset_level();
}

static void launch_ball(void)
{
    int spd = BALL_VY + level * 6;

    if (spd > BALL_VYMAX)
        spd = BALL_VYMAX;
    ball_stuck = 0;
    bvy = -spd;
    bvx = rnd(2) ? 24 : -24;
}

static void ball_lost(void)
{
    int i;

    lives--;
    reset_ball();
    for (i = 0; i < 25; i++)
        Vsync();
}

static void play_frame(void)
{
    static int prev_btn;
    int pressed, cx, cy;

    frame++;

    /* paddle */
    if (input_mode == MODE_MOUSE) {
        short cur = mdx_acc;
        ppx += (short)(cur - last_mdx);
        last_mdx = cur;
    } else {
        unsigned char joy = joy1;
        if (joy & JOY_LEFT)
            ppx -= PADDLE_SPEED;
        if (joy & JOY_RIGHT)
            ppx += PADDLE_SPEED;
    }
    if (ppx < 0)
        ppx = 0;
    if (ppx > 320 - PADDLE_W)
        ppx = 320 - PADDLE_W;

    pressed = btn_down();

    if (ball_stuck) {
        bx = (ppx + PADDLE_W / 2 - BALL_W / 2) << FP;
        by = (PADDLE_Y - BALL_W) << FP;
        if (--stuck_timer <= 0 || (pressed && !prev_btn))
            launch_ball();
        prev_btn = pressed;
        return;
    }
    prev_btn = pressed;

    bx += bvx;
    by += bvy;

    /* walls */
    if (bx < 0) {
        bx = 0;
        bvx = -bvx;
    }
    if (bx > (320 - BALL_W) << FP) {
        bx = (320 - BALL_W) << FP;
        bvx = -bvx;
    }
    if (by < TOP_Y << FP) {
        by = TOP_Y << FP;
        bvy = -bvy;
    }

    /* paddle */
    if (bvy > 0 && (by >> FP) + BALL_W >= PADDLE_Y &&
        (by >> FP) + BALL_W <= PADDLE_Y + PADDLE_H + 2 &&
        (bx >> FP) + BALL_W > ppx && (bx >> FP) < ppx + PADDLE_W) {
        int off = (bx >> FP) + BALL_W / 2 - (ppx + PADDLE_W / 2);
        bvx = off * 3;
        if (bvx > -8 && bvx < 8)
            bvx = bvx < 0 ? -8 : 8;
        bvy = -bvy;
        by = (PADDLE_Y - BALL_W) << FP;
    }

    /* bricks (check at the ball centre; one brick per frame) */
    cx = (bx >> FP) + BALL_W / 2;
    cy = (by >> FP) + BALL_W / 2;
    if (cy >= BRICK_TOP && cy < BRICK_TOP + BRICK_ROWS * 8) {
        int r = (cy - BRICK_TOP) >> 3;
        int c = cx >> 4;
        if (c >= 0 && c < BRICK_COLS && brick[r][c]) {
            brick[r][c] = 0;
            brick_count--;
            score += (BRICK_ROWS - r) * 10L;
            bvy = -bvy;
        }
    }

    /* floor */
    if ((by >> FP) > 200)
        ball_lost();
}

static void draw_frame(void)
{
    clear_back();
    draw_status();
    draw_bricks();
    draw_paddle(ppx, PADDLE_Y);
    draw_sprite(bx >> FP, by >> FP, spr_ball, BALL_W, 1);
    flip();
}

/* wait for the selected button (release, then press); 1 if ESC */
static int wait_btn(const char *msg)
{
    int released = 0;

    for (;;) {
        clear_back();
        draw_status();
        draw_text((int)(320 - 8 * strlen(msg)) / 2, 96, msg, 1);
        flip();
        if (esc_pressed())
            return 1;
        if (!released) {
            if (!btn_down())
                released = 1;
        } else if (btn_down()) {
            return 0;
        }
    }
}

/* title screen; picks the input mode.
 * Returns MODE_JOY, MODE_MOUSE, or -1 if ESC was hit. */
static int splash(void)
{
    int joy_released = 0, mouse_released = 0;
    int t = 0, demo_x = 60, demo_dir = 3, demo_p, r, c;

    for (;;) {
        clear_back();
        /* decorative brick rows */
        for (r = 0; r < 3; r++)
            for (c = 0; c < BRICK_COLS; c++)
                draw_sprite(c << 4, 16 + (r << 3), spr_brick, 7,
                            row_color[r * 2]);
        draw_text2x_rainbow(96, 64, "BREAKPAD");
        if ((t & 63) < 44) { /* blink */
            draw_text(100, 112, "FIRE  = JOYSTICK", 1);
            draw_text(100, 128, "CLICK = MOUSE", 1);
        }
        /* bouncing demo ball with a paddle chasing it */
        draw_sprite(demo_x, 162, spr_ball, BALL_W, 1);
        demo_p = demo_x - PADDLE_W / 2 + BALL_W / 2;
        if (demo_p < 0)
            demo_p = 0;
        if (demo_p > 320 - PADDLE_W)
            demo_p = 320 - PADDLE_W;
        draw_paddle(demo_p, 172);
        draw_text3x5(126, 192, "X.COM/NEILRACKETT", 1);
        flip();
        t++;
        demo_x += demo_dir;
        if (demo_x < 8 || demo_x > 308)
            demo_dir = -demo_dir;
        if (esc_pressed())
            return -1;
        if (!joy_released) {
            if (!(joy1 & JOY_FIRE))
                joy_released = 1;
        } else if (joy1 & JOY_FIRE) {
            return MODE_JOY;
        }
        if (!mouse_released) {
            if (!(mbut & MOUSE_LEFT))
                mouse_released = 1;
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
    int old_rez, i, mode;

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
    (void)Setcolor(5, 0x070);     /* green */
    (void)Setcolor(6, 0x077);     /* cyan: paddle, lives */
    (void)Setcolor(7, 0x707);     /* magenta */
    (void)Setcolor(8, 0x444);     /* grey: status bar line */
    for (i = 9; i < 16; i++)
        (void)Setcolor(i, 0x000);

    /* Hook joystick 1 + mouse. Do NOT send IKBD $14 (joystick event
     * reporting) here: in the real 6301 firmware the mouse and joystick
     * auto-report modes are mutually exclusive, so $14 switches mouse
     * packet reporting OFF. The power-up/desktop default already
     * reports mouse packets and joystick 1 events simultaneously,
     * which is exactly what this game needs. */
    kv = (Kbdvecs *)Kbdvbase();
    old_joyvec = kv->joyvec;
    old_mousevec = kv->mousevec;
    kv->joyvec = (void *)joy_handler;
    kv->mousevec = (void *)mouse_handler;
    last_mdx = mdx_acc;

    quit = 0;
    mode = splash();
    if (mode >= 0) {
        input_mode = mode;
        while (!quit) {
            reset_game();
            while (lives > 0) {
                if (esc_pressed()) {
                    quit = 1;
                    break;
                }
                play_frame();
                if (brick_count == 0) {
                    level++;
                    reset_level();
                }
                draw_frame();
            }
            if (!quit && wait_btn("GAME OVER"))
                break;
        }
    }

    /* restore everything (IKBD modes were never changed) */
    kv->joyvec = old_joyvec;
    kv->mousevec = old_mousevec;
    Setscreen(old_log, old_phys, old_rez);
    for (i = 0; i < 16; i++)
        (void)Setcolor(i, old_palette[i]);
    (void)Mfree(blk);

    return 0;
}
