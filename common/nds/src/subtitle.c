#include "subtitle.h"

#include <nds.h>

#include "subscreen.h"

enum { COL_BOX = 1, COL_TEXT, COL_SHADOW };
#define NOTICE_FRAMES 90 // how long the language name stays after a switch

static const SubtitleLang *langs;
static int lang_count, lang = -1;
static int band_y, band_h;
static TextCanvas *canvas;
static int msg = -1, notice;
static int dirty;

void subtitleInit(const SubtitleLang *l, int count, int start, int y, int h)
{
    langs = l;
    lang_count = count;
    lang = start;
    band_y = y;
    band_h = h;
    canvas = subscreenInit();
    subscreenColour(COL_BOX, RGB15(2, 2, 6));
    subscreenColour(COL_TEXT, RGB15(31, 31, 31));
    subscreenColour(COL_SHADOW, RGB15(6, 6, 10));
    dirty = 1;
}

void subtitleShow(int m)
{
    if (m != msg) {
        msg = m;
        dirty = 1;
    }
}

// Lines centred in the band, on a box as wide as the screen.
static void draw_lines(const uint8_t *font, const char *const *lines, int n)
{
    int lh = textLineHeight(font);
    int total = n * lh;
    int y = band_y + (band_h - total) / 2;
    textFill(canvas, 0, y - 4, 256, total + 8, COL_BOX);
    for (int i = 0; i < n; i++, y += lh) {
        int x = (256 - textWidth(font, lines[i])) / 2;
        if (x < 2)
            x = 2;
        textDraw(canvas, font, x + 1, y + 1, lines[i], COL_SHADOW);
        textDraw(canvas, font, x, y, lines[i], COL_TEXT);
    }
}

static void draw(void)
{
    textFill(canvas, 0, band_y - 8, 256, band_h + 16, 0);
    if (notice) {
        const char *name = lang < 0 ? "OFF" : langs[lang].name;
        draw_lines(langs[lang < 0 ? 0 : lang].font, &name, 1);
        return;
    }
    if (lang < 0 || msg < 0 || msg >= textMessageCount(langs[lang].text))
        return;
    int n;
    const uint8_t *l = textMessage(langs[lang].text, msg, &n);
    const char *lines[8];
    int count = 0;
    for (int i = 0; i < n && count < 8; i++, l = textNextLine(l))
        lines[count++] = (const char *)l + 2;
    // Trailing empty lines only add height.
    while (count && !*lines[count - 1])
        count--;
    if (count)
        draw_lines(langs[lang].font, lines, count);
}

void subtitleFrame(void)
{
    int tx, ty;
    if (!canvas)
        return;
    if (subscreenTouched(&tx, &ty) && ty >= band_y - 8) {
        lang = lang + 1 < lang_count ? lang + 1 : -1;
        notice = NOTICE_FRAMES;
        dirty = 1;
    } else if (notice && !--notice) {
        dirty = 1;
    }
    if (dirty) {
        dirty = 0;
        draw();
    }
}
