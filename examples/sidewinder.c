/*
 * sidewinder.c — SIDEWINDER, a Snake game for the Atari ST (low
 * resolution).
 *
 * Joystick input is read by hooking the IKBD joystick vector (joyvec,
 * offset 24 in the KBDVECS table returned by Kbdvbase). TOS calls the
 * vector with a0 pointing at a 3-byte packet [header, joy0, joy1]; the
 * game reads joystick 1 (byte 2), which is both the physical joystick
 * port and the stick Sidepad injects into.
 *
 * Steer the sidewinder around the pit, eat the apples, don't bite
 * yourself or the walls. Every apple makes you longer; every few apples
 * make you faster. ESC = quit.
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

/* 8x8 cells: 40 columns; rows 3..24 are the walled pit */
#define COLS     40
#define ROW_TOP  3
#define ROW_BOT  24
#define MAXLEN   760

#define START_LEN   4
#define GROW_PER    3  /* segments per apple */
#define FOOD_SCORE  10
#define START_SPEED 7  /* frames per step */
#define MIN_SPEED   3
#define SPEEDUP_AT  4  /* apples per speed-up */

/* directions: 0 up, 1 right, 2 down, 3 left */
static const int dirx[4] = { 0, 1, 0, -1 };
static const int diry[4] = { -1, 0, 1, 0 };

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

/* 8x8 apple */
static const unsigned char spr_apple[8] = {
    0x18, 0x10, 0x7C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38
};

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static unsigned char sx[MAXLEN], sy[MAXLEN]; /* segment ring buffer */
static int head, len;
static unsigned char occ[COLS][ROW_BOT + 1]; /* cell occupied by snake? */
static int dir, want_dir, growing;
static int fx, fy; /* apple cell */
static int speed, apples;
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

/* solid 8x8 block at cell (cx, cy) */
static void draw_cell(int cx, int cy, int color)
{
    static const unsigned short solid[8] = {
        0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFF00
    };

    draw_sprite(cx << 3, cy << 3, solid, 8, color);
}

static void draw_apple(int cx, int cy)
{
    unsigned short rows[8];
    int i;

    for (i = 0; i < 8; i++)
        rows[i] = (unsigned short)spr_apple[i] << 8;
    draw_sprite(cx << 3, cy << 3, rows, 8, 2);
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

/* --- game drawing --- */

static void draw_walls(void)
{
    int c, r;

    for (c = 0; c < COLS; c++) {
        draw_cell(c, ROW_TOP, 8);
        draw_cell(c, ROW_BOT, 8);
    }
    for (r = ROW_TOP + 1; r < ROW_BOT; r++) {
        draw_cell(0, r, 8);
        draw_cell(COLS - 1, r, 8);
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
    draw_text(8, 8, "SCORE", 1);
    draw_text(56, 8, digits, 1);
    digits[2] = '\0';
    digits[0] = (char)('0' + len / 100);
    digits[1] = (char)('0' + (len / 10) % 10);
    digits[2] = (char)('0' + len % 10);
    digits[3] = '\0';
    draw_text(232, 8, "LENGTH", 1);
    draw_text(284, 8, digits, 1);
}

/* head bright yellow, body green */
static void draw_snake(void)
{
    int i, idx;

    for (i = 0; i < len; i++) {
        idx = (head - i + MAXLEN) % MAXLEN;
        draw_cell(sx[idx], sy[idx], i == 0 ? 4 : 5);
    }
}

static void draw_frame(void)
{
    clear_back();
    draw_status();
    draw_walls();
    draw_snake();
    draw_apple(fx, fy);
    flip();
}

/* --- game logic --- */

static void place_apple(void)
{
    do {
        fx = 1 + rnd(COLS - 2);
        fy = ROW_TOP + 1 + rnd(ROW_BOT - ROW_TOP - 1);
    } while (occ[fx][fy]);
}

static void reset_game(void)
{
    int i;

    memset(occ, 0, sizeof(occ));
    head = 0;
    len = START_LEN;
    for (i = 0; i < len; i++) {
        sx[i] = (unsigned char)(8 + i);
        sy[i] = 14;
        occ[8 + i][14] = 1;
    }
    head = len - 1;
    dir = 1; /* right */
    want_dir = 1;
    growing = 0;
    speed = START_SPEED;
    apples = 0;
    score = 0;
    place_apple();
}

/* latch the latest direction change; reversing is ignored */
static void read_dir(void)
{
    unsigned char joy = joy1;
    int d = -1;

    if (joy & JOY_UP)
        d = 0;
    else if (joy & JOY_DOWN)
        d = 2;
    else if (joy & JOY_LEFT)
        d = 3;
    else if (joy & JOY_RIGHT)
        d = 1;
    if (d >= 0 && d != ((dir + 2) & 3))
        want_dir = d;
}

/* advance one cell; returns 0 on death */
static int step(void)
{
    int nx, ny, tail;

    dir = want_dir;
    nx = sx[head] + dirx[dir];
    ny = sy[head] + diry[dir];

    if (nx <= 0 || nx >= COLS - 1 || ny <= ROW_TOP || ny >= ROW_BOT)
        return 0;

    /* the tail cell vacates this step unless we're growing */
    tail = (head - len + 1 + MAXLEN) % MAXLEN;
    if (!growing) {
        occ[sx[tail]][sy[tail]] = 0;
    }
    if (occ[nx][ny])
        return 0;

    head = (head + 1) % MAXLEN;
    sx[head] = (unsigned char)nx;
    sy[head] = (unsigned char)ny;
    occ[nx][ny] = 1;
    if (growing) {
        growing--;
        len++;
    }

    if (nx == fx && ny == fy) {
        score += FOOD_SCORE;
        apples++;
        growing += GROW_PER;
        if (apples % SPEEDUP_AT == 0 && speed > MIN_SPEED)
            speed--;
        place_apple();
    }
    return 1;
}

/* flash the dead snake */
static void death_anim(void)
{
    int t, i, idx;

    for (t = 0; t < 24; t++) {
        clear_back();
        draw_status();
        draw_walls();
        if (t & 2) {
            for (i = 0; i < len; i++) {
                idx = (head - i + MAXLEN) % MAXLEN;
                draw_cell(sx[idx], sy[idx], 2);
            }
        }
        draw_apple(fx, fy);
        flip();
    }
}

/* wait for fire (release, then press); returns 1 if ESC was hit */
static int wait_fire(const char *m)
{
    int released = 0;
    long t = 0;

    for (;;) {
        clear_back();
        draw_status();
        draw_walls();
        draw_text2x((int)(320 - 16 * strlen(m)) / 2, 88, m, 2);
        if ((t & 63) < 44) /* blink */
            draw_text(120, 128, "PRESS FIRE", 1);
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

/* title screen: a snake slithers across behind the title.
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
    /* sin(2*pi*k/32) * 256, for the demo snake's wave */
    static const int sin32[32] = {
        0,    50,   98,   142,  181,  213,  236,  251,
        256,  251,  236,  213,  181,  142,  98,   50,
        0,    -50,  -98,  -142, -181, -213, -236, -251,
        -256, -251, -236, -213, -181, -142, -98,  -50
    };
    int released = 0, i, hx = 0, tick = 0;
    long t = 0;

    for (;;) {
        clear_back();
        draw_text2x_rainbow(80, 32, "SIDEWINDER");
        for (i = 0; i < 14; i++) {
            int cx = (hx - i + COLS) % COLS;
            int cy = 14 + (sin32[(cx * 3) & 31] * 3 >> 8);
            draw_cell(cx, cy, i == 0 ? 4 : 5);
        }
        draw_apple((hx + 6) % COLS, 14);
        if ((t & 63) < 44) /* blink */
            draw_text(120, 160, "PRESS FIRE", 1);
        draw_text3x5(126, 190, "X.COM/NEILRACKETT", 1);
        flip();
        t++;
        if (++tick >= 5) {
            tick = 0;
            hx = (hx + 1) % COLS;
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
    int old_rez, i, tick;

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
    (void)Setcolor(1, 0x777);     /* white: text */
    (void)Setcolor(2, 0x700);     /* red: apple, game over */
    (void)Setcolor(3, 0x750);     /* orange */
    (void)Setcolor(4, 0x770);     /* yellow: snake head */
    (void)Setcolor(5, 0x070);     /* green: snake body */
    (void)Setcolor(6, 0x077);     /* cyan */
    (void)Setcolor(7, 0x707);     /* magenta */
    (void)Setcolor(8, 0x444);     /* grey: walls */
    for (i = 9; i < 16; i++)
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
            tick = 0;
            for (;;) {
                if (esc_pressed()) {
                    quit = 1;
                    break;
                }
                read_dir();
                if (++tick >= speed) {
                    tick = 0;
                    if (!step()) {
                        death_anim();
                        break;
                    }
                    draw_frame();
                } else {
                    Vsync();
                }
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
