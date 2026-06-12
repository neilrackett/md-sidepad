/*
 * sideroids.c — SIDEROIDS, a simple Asteroids-style game for the
 * Atari ST (low resolution).
 *
 * Joystick input is read by hooking the IKBD joystick vector (joyvec,
 * offset 24 in the KBDVECS table returned by Kbdvbase). TOS calls the
 * vector with a0 pointing at a 3-byte packet [header, joy0, joy1]; the
 * game reads joystick 1 (byte 2), which is both the physical joystick
 * port and the stick Sidepad injects into. IKBD encoding: bit0=Up,
 * bit1=Down, bit2=Left, bit3=Right, bit7=Fire.
 *
 * Controls: left/right = rotate, up = thrust, fire = shoot, ESC = quit.
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
#define FP        4
#define WORLD_W   (320 << FP)
#define WORLD_H   (200 << FP)

#define SHIP_R       7  /* drawn radius, pixels */
#define SHIP_ACCEL   8  /* 1/16 px/frame^2 at full thrust */
#define SHIP_VMAX    60 /* 1/16 px/frame */
#define INVUL_FRAMES 120

#define MAX_BULLETS  4
#define BULLET_SPEED 80 /* 1/16 px/frame, added to ship velocity */
#define BULLET_LIFE  40 /* frames */

#define MAX_AST 28 /* 6 large -> 12 medium -> 24 small */

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

/* two jagged asteroid outlines, vertex offsets at large size (r=16);
 * medium and small shift the offsets right by 1 and 2 */
static const signed char ast_shape[2][8][2] = {
    { { 16, 0 },  { 10, -10 }, { 2, -15 }, { -8, -13 },
      { -16, -2 }, { -12, 9 },  { -3, 16 }, { 9, 12 } },
    { { 14, 3 },  { 12, -9 },  { 3, -16 }, { -9, -12 },
      { -15, -4 }, { -14, 8 },  { -2, 14 }, { 8, 15 } }
};

static const int ast_crad[3] = { 14, 7, 4 };   /* collision radius, px */
static const int ast_pts[3] = { 20, 50, 100 }; /* score per size */

/* 8x8 font, only the glyphs the game needs */
static const char font_chars[] = "0123456789ACDEFGIMNOPRSV";
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
    { 0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00 }, /* C */
    { 0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00 }, /* D */
    { 0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xFE, 0x00 }, /* E */
    { 0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xC0, 0x00 }, /* F */
    { 0x7C, 0xC6, 0xC0, 0xDE, 0xC6, 0xC6, 0x7C, 0x00 }, /* G */
    { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, /* I */
    { 0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00 }, /* M */
    { 0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00 }, /* N */
    { 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 }, /* O */
    { 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00 }, /* P */
    { 0xFC, 0xC6, 0xC6, 0xFC, 0xD8, 0xCC, 0xC6, 0x00 }, /* R */
    { 0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00 }, /* S */
    { 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00 }, /* V */
};

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

typedef struct {
    int on, size, shape;
    int x, y, vx, vy; /* 1/16 px */
} Ast;

typedef struct {
    int on, life;
    int x, y, vx, vy; /* 1/16 px */
} Bullet;

static Ast ast[MAX_AST];
static int ast_count;
static Bullet bullets[MAX_BULLETS];
static int sx, sy, svx, svy; /* ship, 1/16 px */
static int sangle;           /* 0..31, 0 = up, clockwise */
static int invul, thrusting;
static long frame;
static long score;
static int lives, wave, quit;

/* clear planes 0 and 1 (planes 2/3 stay black after init) */
static void clear_back(void)
{
    unsigned long *p = (unsigned long *)back;
    int n = 200 * 20;
    do {
        *p = 0; /* plane 0 + plane 1 words */
        p += 2; /* skip planes 2/3 */
    } while (--n);
}

/* OR a sprite into one bitplane; plane 0 = white, plane 1 = green */
static void draw_sprite(int x, int y, const unsigned short *rows, int h,
                        int plane)
{
    unsigned char *p =
        back + (long)y * LINE_BYTES + ((x >> 4) << 3) + (plane << 1);
    int shift = x & 15;
    int last_group = (x >> 4) >= 19;
    int i;

    for (i = 0; i < h; i++) {
        unsigned long d = ((unsigned long)rows[i] << 16) >> shift;
        *(unsigned short *)p |= (unsigned short)(d >> 16);
        if ((d & 0xFFFF) && !last_group)
            *(unsigned short *)(p + 8) |= (unsigned short)d;
        p += LINE_BYTES;
    }
}

/* set one pixel, clipped to the screen */
static void plot(int x, int y, int plane)
{
    if ((unsigned)x >= 320 || (unsigned)y >= 200)
        return;
    back[(long)y * LINE_BYTES + ((x >> 4) << 3) + (plane << 1) +
         ((x & 8) >> 3)] |= (unsigned char)(0x80 >> (x & 7));
}

/* Bresenham line, clipped per pixel */
static void line(int x0, int y0, int x1, int y1, int plane)
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
        plot(x0, y0, plane);
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

/* x/y components of a vector of length r at angle a (0 = up, clockwise) */
static int dirx(int a, int r)
{
    return sin32[a & 31] * r >> 8;
}

static int diry(int a, int r)
{
    return -(sin32[(a + 8) & 31] * r) >> 8;
}

/* classic arrowhead: nose plus two rear corners at ~146/214 degrees */
static void draw_ship_shape(int cx, int cy, int a, int r, int plane)
{
    int x0 = cx + dirx(a, r), y0 = cy + diry(a, r);
    int x1 = cx + dirx(a + 13, r), y1 = cy + diry(a + 13, r);
    int x2 = cx + dirx(a + 19, r), y2 = cy + diry(a + 19, r);

    line(x0, y0, x1, y1, plane);
    line(x1, y1, x2, y2, plane);
    line(x2, y2, x0, y0, plane);
}

static void draw_ast_at(int px, int py, int shape, int size, int plane)
{
    const signed char(*s)[2] = ast_shape[shape];
    int i;

    for (i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        line(px + (s[i][0] >> size), py + (s[i][1] >> size),
             px + (s[j][0] >> size), py + (s[j][1] >> size), plane);
    }
}

/* draw an asteroid, plus wrapped copies near the screen edges */
static void draw_ast(const Ast *a)
{
    int px = a->x >> FP, py = a->y >> FP;
    int wx = px, wy = py;

    if (px < 20)
        wx = px + 320;
    else if (px > 300)
        wx = px - 320;
    if (py < 20)
        wy = py + 200;
    else if (py > 180)
        wy = py - 200;
    draw_ast_at(px, py, a->shape, a->size, 0);
    if (wx != px)
        draw_ast_at(wx, py, a->shape, a->size, 0);
    if (wy != py)
        draw_ast_at(px, wy, a->shape, a->size, 0);
    if (wx != px && wy != py)
        draw_ast_at(wx, wy, a->shape, a->size, 0);
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

static void draw_text(int x, int y, const char *s, int plane)
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
                draw_sprite(x, y, rows, 8, plane);
            }
        }
        x += 8;
        s++;
    }
}

/* draw_text at double size (16x16 glyphs) */
static void draw_text2x(int x, int y, const char *s, int plane)
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
                draw_sprite(x, y, rows, 16, plane);
            }
        }
        x += 16;
        s++;
    }
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
    draw_text(8, 4, "SCORE", 0);
    draw_text(56, 4, digits, 0);
    for (i = 0; i < lives; i++)
        draw_ship_shape(304 - i * 12, 9, 0, 5, 1);
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

/* --- game logic --- */

static void spawn_ast(int x, int y, int size)
{
    static const int base_speed[3] = { 8, 14, 20 }; /* 1/16 px/frame */
    int i, an, spd;

    for (i = 0; i < MAX_AST; i++)
        if (!ast[i].on)
            break;
    if (i == MAX_AST)
        return;
    an = rnd(32);
    spd = base_speed[size] + rnd(base_speed[size]);
    ast[i].on = 1;
    ast[i].size = size;
    ast[i].shape = rnd(2);
    ast[i].x = x;
    ast[i].y = y;
    ast[i].vx = sin32[an] * spd >> 8;
    ast[i].vy = sin32[(an + 8) & 31] * spd >> 8;
    ast_count++;
}

static void split_ast(Ast *a)
{
    int size = a->size;

    score += ast_pts[size];
    a->on = 0;
    ast_count--;
    if (size < 2) {
        spawn_ast(a->x, a->y, size + 1);
        spawn_ast(a->x, a->y, size + 1);
    }
}

static void reset_wave(void)
{
    int i, n, x, y;

    memset(ast, 0, sizeof(ast));
    ast_count = 0;
    memset(bullets, 0, sizeof(bullets));
    n = 3 + wave;
    if (n > 6)
        n = 6;
    for (i = 0; i < n; i++) {
        /* spawn away from the ship */
        x = rnd(320);
        y = rnd(200);
        if (x > (sx >> FP) - 80 && x < (sx >> FP) + 80 &&
            y > (sy >> FP) - 60 && y < (sy >> FP) + 60) {
            x = (x + 160) % 320;
            y = (y + 100) % 200;
        }
        spawn_ast(x << FP, y << FP, 0);
    }
}

static void reset_ship(void)
{
    sx = WORLD_W / 2;
    sy = WORLD_H / 2;
    svx = svy = 0;
    sangle = 0;
    invul = INVUL_FRAMES;
}

static void reset_game(void)
{
    score = 0;
    lives = 3;
    wave = 0;
    reset_ship();
    reset_wave();
}

static void ship_hit(void)
{
    int i;

    lives--;
    reset_ship();
    for (i = 0; i < 25; i++)
        Vsync();
}

static void fire_bullet(void)
{
    int i;

    for (i = 0; i < MAX_BULLETS; i++)
        if (!bullets[i].on)
            break;
    if (i == MAX_BULLETS)
        return;
    bullets[i].on = 1;
    bullets[i].life = BULLET_LIFE;
    bullets[i].x = sx + (dirx(sangle, SHIP_R) << FP);
    bullets[i].y = sy + (diry(sangle, SHIP_R) << FP);
    bullets[i].vx = svx + (sin32[sangle] * BULLET_SPEED >> 8);
    bullets[i].vy = svy - (sin32[(sangle + 8) & 31] * BULLET_SPEED >> 8);
}

static void wrap_pos(int *x, int *y)
{
    if (*x < 0)
        *x += WORLD_W;
    else if (*x >= WORLD_W)
        *x -= WORLD_W;
    if (*y < 0)
        *y += WORLD_H;
    else if (*y >= WORLD_H)
        *y -= WORLD_H;
}

static void play_frame(void)
{
    static int prev_fire;
    unsigned char joy = joy1;
    int i, j;

    frame++;

    /* rotate every other frame: a full turn in ~1.3 s */
    if (frame & 1) {
        if (joy & JOY_LEFT)
            sangle = (sangle + 31) & 31;
        if (joy & JOY_RIGHT)
            sangle = (sangle + 1) & 31;
    }

    thrusting = (joy & JOY_UP) != 0;
    if (thrusting) {
        svx += sin32[sangle] * SHIP_ACCEL >> 8;
        svy -= sin32[(sangle + 8) & 31] * SHIP_ACCEL >> 8;
        if (svx > SHIP_VMAX)
            svx = SHIP_VMAX;
        if (svx < -SHIP_VMAX)
            svx = -SHIP_VMAX;
        if (svy > SHIP_VMAX)
            svy = SHIP_VMAX;
        if (svy < -SHIP_VMAX)
            svy = -SHIP_VMAX;
    }
    sx += svx;
    sy += svy;
    wrap_pos(&sx, &sy);
    if (invul > 0)
        invul--;

    if ((joy & JOY_FIRE) && !prev_fire)
        fire_bullet();
    prev_fire = joy & JOY_FIRE;

    /* bullets */
    for (i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->on)
            continue;
        if (--b->life <= 0) {
            b->on = 0;
            continue;
        }
        b->x += b->vx;
        b->y += b->vy;
        wrap_pos(&b->x, &b->y);
        for (j = 0; j < MAX_AST; j++) {
            Ast *a = &ast[j];
            long dx, dy, r;
            if (!a->on)
                continue;
            dx = (b->x - a->x) >> FP;
            dy = (b->y - a->y) >> FP;
            r = ast_crad[a->size];
            if (dx * dx + dy * dy <= r * r) {
                b->on = 0;
                split_ast(a);
                break;
            }
        }
    }

    /* asteroids; collide with the ship */
    for (i = 0; i < MAX_AST; i++) {
        Ast *a = &ast[i];
        long dx, dy, r;
        if (!a->on)
            continue;
        a->x += a->vx;
        a->y += a->vy;
        wrap_pos(&a->x, &a->y);
        if (invul > 0)
            continue;
        dx = (sx - a->x) >> FP;
        dy = (sy - a->y) >> FP;
        r = ast_crad[a->size] + 3;
        if (dx * dx + dy * dy <= r * r) {
            ship_hit();
            return;
        }
    }
}

static void draw_frame(void)
{
    int i, cx, cy;

    clear_back();
    draw_status();
    for (i = 0; i < MAX_AST; i++)
        if (ast[i].on)
            draw_ast(&ast[i]);
    for (i = 0; i < MAX_BULLETS; i++)
        if (bullets[i].on) {
            cx = bullets[i].x >> FP;
            cy = bullets[i].y >> FP;
            plot(cx, cy, 0);
            plot(cx + 1, cy, 0);
            plot(cx, cy + 1, 0);
            plot(cx + 1, cy + 1, 0);
        }
    cx = sx >> FP;
    cy = sy >> FP;
    if (invul == 0 || (frame & 3) < 2) { /* blink while invulnerable */
        draw_ship_shape(cx, cy, sangle, SHIP_R, 1);
        if (thrusting && (frame & 1)) {
            int ra = sangle + 16;
            line(cx + dirx(ra, 4), cy + diry(ra, 4), cx + dirx(ra, 8),
                 cy + diry(ra, 8), 1);
        }
    }
    flip();
}

/* wait for fire (release, then press); returns 1 if ESC was hit */
static int wait_fire(const char *msg)
{
    int released = 0;

    for (;;) {
        clear_back();
        draw_status();
        draw_text((int)(320 - 8 * strlen(msg)) / 2, 92, msg, 0);
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

/* title screen: big title, drifting asteroids, slowly rotating ship,
 * blinking prompt. Returns 1 if ESC was hit, 0 when fire starts. */
static int splash(void)
{
    static const int sizes[5] = { 0, 0, 1, 1, 2 };
    struct {
        int x, y, vx, vy, shape;
    } bg[5];
    int released = 0, t = 0, i;

    for (i = 0; i < 5; i++) {
        int an = rnd(32);
        bg[i].x = rnd(320) << FP;
        bg[i].y = rnd(200) << FP;
        bg[i].vx = sin32[an] * 10 >> 8;
        bg[i].vy = sin32[(an + 8) & 31] * 10 >> 8;
        bg[i].shape = rnd(2);
    }

    for (;;) {
        clear_back();
        for (i = 0; i < 5; i++) {
            bg[i].x += bg[i].vx;
            bg[i].y += bg[i].vy;
            wrap_pos(&bg[i].x, &bg[i].y);
            draw_ast_at(bg[i].x >> FP, bg[i].y >> FP, bg[i].shape, sizes[i],
                        0);
        }
        draw_text2x(88, 40, "SIDEROIDS", 1);
        draw_ship_shape(160, 108, (t >> 3) & 31, 10, 1);
        if ((t & 63) < 44) /* blink */
            draw_text(120, 150, "PRESS FIRE", 0);
        flip();
        t++;
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
    (void)Setcolor(0, 0x000);     /* background */
    (void)Setcolor(1, 0x777);     /* plane 0: asteroids, shots, text */
    (void)Setcolor(2, 0x070);     /* plane 1: ship, lives */
    (void)Setcolor(3, 0x773);     /* plane 0+1 overlap */

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
                if (ast_count == 0) {
                    wave++;
                    reset_wave();
                }
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
