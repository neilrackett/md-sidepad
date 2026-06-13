/*
 * invaders.c — simple Space Invaders for the Atari ST (low resolution).
 *
 * Joystick input is read by hooking the IKBD joystick vector (joyvec,
 * offset 24 in the KBDVECS table returned by Kbdvbase). TOS calls the
 * vector with a0 pointing at a 3-byte packet [header, joy0, joy1]; the
 * game reads joystick 1 (byte 2), which is both the physical joystick
 * port and the stick Sidepad injects into. IKBD encoding: bit0=Up,
 * bit1=Down, bit2=Left, bit3=Right, bit7=Fire.
 *
 * Controls: left/right = move, fire = shoot, ESC = quit.
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

#define PLAYER_W     13
#define PLAYER_H     8
#define PLAYER_Y     184
#define PLAYER_SPEED 3

#define ALIEN_W    11
#define ALIEN_H    8
#define ALIEN_ROWS 4
#define ALIEN_COLS 8
#define ALIEN_DX   24 /* column pitch */
#define ALIEN_DY   14 /* row pitch */
#define ALIEN_STEP 4  /* horizontal step */
#define ALIEN_DROP 6  /* descent on edge hit */

#define BULLET_W     1
#define BULLET_H     5
#define BULLET_SPEED 6
#define MAX_BOMBS    3
#define BOMB_W       2
#define BOMB_H       6
#define BOMB_SPEED   2

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

/* --- sprites (16-bit rows, MSB = leftmost pixel) --- */

/* two walk frames, toggled on every group step */
static const unsigned short spr_alien[2][ALIEN_H] = {
    { 0x2080, 0x1100, 0x3F80, 0x6EC0, 0xFFE0, 0xBFA0, 0xA0A0, 0x1B00 },
    { 0x2080, 0x9120, 0xBFA0, 0xEEE0, 0xFFE0, 0x7FC0, 0x2080, 0x4040 }
};

static const unsigned short spr_player[PLAYER_H] = {
    0x0200, 0x0700, 0x0700, 0x7FF0, 0xFFF8, 0xFFF8, 0xFFF8, 0xFFF8
};

static const unsigned short spr_bullet[BULLET_H] = {
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000
};

static const unsigned short spr_bomb[BOMB_H] = {
    0x8000, 0x4000, 0x8000, 0x4000, 0x8000, 0x4000
};

/* two debris frames for the exploding ship */
static const unsigned short spr_boom[2][PLAYER_H] = {
    { 0x1084, 0x4420, 0x1290, 0x844A, 0x2921, 0x4288, 0x1424, 0x8942 },
    { 0x4212, 0x0920, 0x2409, 0x48A4, 0x1248, 0x9122, 0x2484, 0x4851 }
};

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

static unsigned char alien_alive[ALIEN_ROWS][ALIEN_COLS];
static int alien_count;
static int gx, gy, gdir, step_timer; /* alien group */
static int anim;                     /* alien walk frame */
static int px;                       /* player x */
static int bullet_x, bullet_y, bullet_on;
static struct {
    int x, y, on;
} bombs[MAX_BOMBS];
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
        draw_sprite(316 - 18 * (i + 1), 4, spr_player, PLAYER_H, 1);
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

/* --- game logic --- */

static void reset_wave(void)
{
    int i;

    memset(alien_alive, 1, sizeof(alien_alive));
    alien_count = ALIEN_ROWS * ALIEN_COLS;
    gx = 20;
    gy = 32;
    gdir = 1;
    anim = 0;
    step_timer = 1;
    bullet_on = 0;
    for (i = 0; i < MAX_BOMBS; i++)
        bombs[i].on = 0;
    px = (320 - PLAYER_W) / 2;
}

static void reset_game(void)
{
    score = 0;
    lives = 3;
    wave = 0;
    reset_wave();
}

static void alien_bounds(int *lo, int *hi)
{
    int r, c;

    *lo = ALIEN_COLS - 1;
    *hi = 0;
    for (c = 0; c < ALIEN_COLS; c++)
        for (r = 0; r < ALIEN_ROWS; r++)
            if (alien_alive[r][c]) {
                if (c < *lo)
                    *lo = c;
                if (c > *hi)
                    *hi = c;
            }
}

static void spawn_bomb(void)
{
    int i, r, c;

    for (i = 0; i < MAX_BOMBS; i++)
        if (!bombs[i].on)
            break;
    if (i == MAX_BOMBS)
        return;
    c = (int)(Random() % ALIEN_COLS);
    for (r = ALIEN_ROWS - 1; r >= 0; r--)
        if (alien_alive[r][c]) {
            bombs[i].on = 1;
            bombs[i].x = gx + c * ALIEN_DX + ALIEN_W / 2;
            bombs[i].y = gy + r * ALIEN_DY + ALIEN_H;
            return;
        }
}

static void player_hit(void)
{
    int i, r, c;

    lives--;
    for (i = 0; i < MAX_BOMBS; i++)
        bombs[i].on = 0;
    bullet_on = 0;
    /* the ship explodes in a flicker of debris; the aliens pause */
    for (i = 0; i < 40; i++) {
        clear_back();
        draw_status();
        for (r = 0; r < ALIEN_ROWS; r++)
            for (c = 0; c < ALIEN_COLS; c++)
                if (alien_alive[r][c])
                    draw_sprite(gx + c * ALIEN_DX, gy + r * ALIEN_DY,
                                spr_alien[anim], ALIEN_H, 0);
        draw_sprite(px - 2, PLAYER_Y, spr_boom[(i >> 2) & 1], PLAYER_H,
                    (i >> 3) & 1);
        flip();
    }
    px = (320 - PLAYER_W) / 2;
}

static void play_frame(void)
{
    unsigned char joy = joy1;
    int r, c, i;

    /* player */
    if (joy & JOY_LEFT)
        px -= PLAYER_SPEED;
    if (joy & JOY_RIGHT)
        px += PLAYER_SPEED;
    if (px < 4)
        px = 4;
    if (px > 320 - PLAYER_W - 4)
        px = 320 - PLAYER_W - 4;
    if ((joy & JOY_FIRE) && !bullet_on) {
        bullet_on = 1;
        bullet_x = px + PLAYER_W / 2;
        bullet_y = PLAYER_Y - BULLET_H;
    }

    /* player bullet */
    if (bullet_on) {
        bullet_y -= BULLET_SPEED;
        if (bullet_y <= 14)
            bullet_on = 0;
    }
    if (bullet_on) {
        for (r = 0; r < ALIEN_ROWS && bullet_on; r++)
            for (c = 0; c < ALIEN_COLS; c++) {
                int ax, ay;
                if (!alien_alive[r][c])
                    continue;
                ax = gx + c * ALIEN_DX;
                ay = gy + r * ALIEN_DY;
                if (bullet_x + BULLET_W > ax && bullet_x < ax + ALIEN_W &&
                    bullet_y + BULLET_H > ay && bullet_y < ay + ALIEN_H) {
                    alien_alive[r][c] = 0;
                    alien_count--;
                    score += (ALIEN_ROWS - r) * 10L;
                    bullet_on = 0;
                    break;
                }
            }
    }

    /* alien group step */
    if (alien_count > 0 && --step_timer <= 0) {
        int lo, hi, nx;

        step_timer = alien_count / 2 + 3 - wave * 2;
        if (step_timer < 1)
            step_timer = 1;
        alien_bounds(&lo, &hi);
        nx = gx + gdir * ALIEN_STEP;
        if (nx + lo * ALIEN_DX < 4 || nx + hi * ALIEN_DX + ALIEN_W > 316) {
            gdir = -gdir;
            gy += ALIEN_DROP;
        } else {
            gx = nx;
        }
        anim ^= 1;
        if ((Random() & 3) == 0)
            spawn_bomb();
        /* invasion complete? */
        for (r = ALIEN_ROWS - 1; r >= 0; r--)
            for (c = 0; c < ALIEN_COLS; c++)
                if (alien_alive[r][c] &&
                    gy + r * ALIEN_DY + ALIEN_H >= PLAYER_Y) {
                    lives = 0;
                    return;
                }
    }

    /* bombs */
    for (i = 0; i < MAX_BOMBS; i++) {
        if (!bombs[i].on)
            continue;
        bombs[i].y += BOMB_SPEED;
        if (bombs[i].y > 200 - BOMB_H) {
            bombs[i].on = 0;
        } else if (bombs[i].x + BOMB_W > px && bombs[i].x < px + PLAYER_W &&
                   bombs[i].y + BOMB_H > PLAYER_Y &&
                   bombs[i].y < PLAYER_Y + PLAYER_H) {
            bombs[i].on = 0;
            player_hit();
            return;
        }
    }
}

static void draw_frame(void)
{
    int r, c, i;

    clear_back();
    draw_status();
    for (r = 0; r < ALIEN_ROWS; r++)
        for (c = 0; c < ALIEN_COLS; c++)
            if (alien_alive[r][c])
                draw_sprite(gx + c * ALIEN_DX, gy + r * ALIEN_DY,
                            spr_alien[anim], ALIEN_H, 0);
    draw_sprite(px, PLAYER_Y, spr_player, PLAYER_H, 1);
    if (bullet_on)
        draw_sprite(bullet_x, bullet_y, spr_bullet, BULLET_H, 0);
    for (i = 0; i < MAX_BOMBS; i++)
        if (bombs[i].on)
            draw_sprite(bombs[i].x, bombs[i].y, spr_bomb, BOMB_H, 0);
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

/* title screen: big title, marching aliens, blinking prompt.
 * Returns 1 if ESC was hit, 0 when fire starts the game. */
static int splash(void)
{
    int released = 0, t = 0, mx = 60, mdir = 1, i;

    for (;;) {
        clear_back();
        draw_text2x(32, 40, "SIDEPAD INVADERS", 1);
        for (i = 0; i < 5; i++)
            draw_sprite(mx + i * 28, 92, spr_alien[(t >> 4) & 1], ALIEN_H, 0);
        if ((t & 63) < 44) /* blink */
            draw_text(120, 144, "PRESS FIRE", 0);
        flip();
        t++;
        if ((t & 3) == 0) { /* slow drift, bounce at the edges */
            mx += mdir;
            if (mx <= 24 || mx >= 320 - 24 - (4 * 28 + ALIEN_W))
                mdir = -mdir;
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
    (void)Setcolor(0, 0x000);     /* background */
    (void)Setcolor(1, 0x777);     /* plane 0: aliens, shots, text */
    (void)Setcolor(2, 0x070);     /* plane 1: player, lives */
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
                if (alien_count == 0) {
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
