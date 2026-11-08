#include "text.h"

#include <string.h>

// FNT1 header: magic, u16 count, u8 ascent, u8 descent, 4 reserved.
#define FONT_HEADER 12

typedef struct {
    uint32_t cp;
    uint32_t offset;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t advance;
    uint8_t pad[3];
} Glyph;

static int font_count(const uint8_t *font)
{
    return font[4] | font[5] << 8;
}

int textAscent(const uint8_t *font)
{
    return font[6];
}

int textLineHeight(const uint8_t *font)
{
    return font[6] + font[7];
}

static const Glyph *find_glyph(const uint8_t *font, uint32_t cp)
{
    const Glyph *g = (const Glyph *)(font + FONT_HEADER);
    int lo = 0, hi = font_count(font) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g[mid].cp == cp)
            return &g[mid];
        if (g[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

// Glyph for a code point, or '?' if the font does not have it.
static const Glyph *glyph(const uint8_t *font, uint32_t cp)
{
    const Glyph *g = find_glyph(font, cp);
    return g ? g : find_glyph(font, '?');
}

static uint32_t next_cp(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t cp = *s++;
    int more = 0;
    if (cp >= 0xF0) {
        cp &= 0x07;
        more = 3;
    } else if (cp >= 0xE0) {
        cp &= 0x0F;
        more = 2;
    } else if (cp >= 0xC0) {
        cp &= 0x1F;
        more = 1;
    }
    while (more-- && (*s & 0xC0) == 0x80)
        cp = cp << 6 | (*s++ & 0x3F);
    *ps = (const char *)s;
    return cp;
}

void textCanvasInit(TextCanvas *c, uint16_t *map, uint32_t *gfx, int first_tile, int pal,
                    int w, int h)
{
    c->gfx = gfx;
    c->w = w;
    c->h = h;
    for (int ty = 0; ty < h; ty++)
        for (int tx = 0; tx < w; tx++)
            map[ty * 32 + tx] = (first_tile + ty * w + tx) | pal << 12;
    // VRAM takes 16/32-bit writes only, so no memset.
    for (int i = 0; i < w * h * 8; i++)
        gfx[i] = 0;
}

static inline void plot(TextCanvas *c, int x, int y, int colour)
{
    if ((unsigned)x >= (unsigned)(c->w * 8) || (unsigned)y >= (unsigned)(c->h * 8))
        return;
    uint32_t *row = &c->gfx[((y >> 3) * c->w + (x >> 3)) * 8 + (y & 7)];
    int shift = (x & 7) * 4;
    *row = (*row & ~(0xFu << shift)) | (uint32_t)colour << shift;
}

void textFill(TextCanvas *c, int x, int y, int w, int h, int colour)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            plot(c, i, j, colour);
}

int textWidth(const uint8_t *font, const char *s)
{
    int w = 0;
    while (*s) {
        const Glyph *g = glyph(font, next_cp(&s));
        if (g)
            w += g->advance;
    }
    return w;
}

int textDraw(TextCanvas *c, const uint8_t *font, int x, int y, const char *s, int colour)
{
    int baseline = y + textAscent(font);
    while (*s) {
        const Glyph *g = glyph(font, next_cp(&s));
        if (!g)
            continue;
        const uint8_t *bits = font + g->offset;
        int stride = (g->w + 7) / 8;
        int gx = x + g->xoff, gy = baseline - g->yoff - g->h;
        for (int r = 0; r < g->h; r++)
            for (int col = 0; col < g->w; col++)
                if (bits[r * stride + col / 8] & (0x80 >> (col & 7)))
                    plot(c, gx + col, gy + r, colour);
        x += g->advance;
    }
    return x;
}

int textMessageCount(const uint8_t *txt)
{
    return txt[4] | txt[5] << 8;
}

const uint8_t *textMessage(const uint8_t *txt, int index, int *lines)
{
    const uint8_t *o = txt + 8 + index * 4;
    const uint8_t *m = txt + (o[0] | o[1] << 8 | o[2] << 16 | o[3] << 24);
    *lines = m[0];
    return m + 1;
}

const uint8_t *textNextLine(const uint8_t *line)
{
    return line + 2 + strlen((const char *)line + 2) + 1;
}
