#include "vraster.h"

#if defined(ARM9)
#include <nds/ndstypes.h>
#define VRASTER_HOT ITCM_CODE ARM_CODE
#else
#define VRASTER_HOT
#endif

static void apply(VnesRaster *r, const VnesRasterEvent *e)
{
    uint8_t v = e->value;
    switch (e->addr) {
    case 0x2000:
        r->ctrl = v;
        r->t = (r->t & 0xF3FF) | (v & 3) << 10;
        break;
    case 0x2001: r->mask = v; break;
    case 0x2002: r->w = 0; break;
    case 0x2005:
        if (!r->w) {
            r->t = (r->t & 0xFFE0) | v >> 3;
            r->x = v & 7;
        } else {
            r->t = (r->t & 0x8C1F) | (v & 7) << 12 | (v & 0xF8) << 2;
        }
        r->w ^= 1;
        break;
    case 0x2006:
        if (!r->w) {
            r->t = (r->t & 0x00FF) | (v & 0x3F) << 8;
        } else {
            r->t = (r->t & 0xFF00) | v;
            r->v = r->t;
        }
        r->w ^= 1;
        break;
    case 0x8000: r->bank_select = v; break;
    case 0x8001: r->regs[r->bank_select & 7] = v; break;
    case 0xA000: r->mirroring = v & 1; break;
    }
}

// 1 KiB CHR banks for PPU $0000-$1FFF (MMC3, bit 7 of bank select swaps
// the 2 KiB and 1 KiB halves).
static void chr_banks(const VnesRaster *r, uint8_t chr[8])
{
    if (vnes->mapper != VNES_MMC3) {
        for (int i = 0; i < 8; i++)
            chr[i] = i;
        return;
    }
    uint8_t lo[4] = { r->regs[0] & 0xFE, r->regs[0] | 1, r->regs[1] & 0xFE, r->regs[1] | 1 };
    uint8_t hi[4] = { r->regs[2], r->regs[3], r->regs[4], r->regs[5] };
    int inv = r->bank_select & 0x80;
    for (int i = 0; i < 4; i++) {
        chr[i] = inv ? hi[i] : lo[i];
        chr[4 + i] = inv ? lo[i] : hi[i];
    }
}

// End of a visible line: increment Y in v (fine, then coarse with the
// nametable wrap at row 29), then copy the horizontal bits from t.
static void next_line(VnesRaster *r)
{
    if (!(r->mask & 0x18))
        return;
    uint16_t v = r->v;
    if ((v & 0x7000) != 0x7000) {
        v += 0x1000;
    } else {
        v &= ~0x7000;
        int y = (v >> 5) & 0x1F;
        if (y == 29) {
            y = 0;
            v ^= 0x0800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        v = (v & ~0x03E0) | y << 5;
    }
    r->v = (v & ~0x041F) | (r->t & 0x041F);
}

VRASTER_HOT void vrasterBuild(VrasterFrame *f)
{
    VnesRaster r = vnes->raster_start;
    // Pre-render line: v gets all of t when rendering is on.
    if (r.mask & 0x18)
        r.v = r.t;
    int e = 0;
    for (int line = 0; line < 240; line++) {
        while (e < vnes->raster_count && vnes->raster[e].line < line)
            apply(&r, &vnes->raster[e++]);
        VrasterLine *l = &f->line[line];
        uint16_t v = r.v;
        l->x = (v & 0x400 ? 256 : 0) + ((v & 0x1F) << 3) + r.x;
        l->y = (v & 0x800 ? 256 : 0) + (((v >> 5) & 0x1F) << 3) + ((v >> 12) & 7);
        l->ctrl = r.ctrl;
        l->mask = r.mask;
        l->mirroring = r.mirroring;
        chr_banks(&r, l->chr);
        next_line(&r);
    }
}

uint8_t vrasterNametable(const VrasterLine *l, int col, int row)
{
    int nt = (row >= 32 ? 2 : 0) + (col >= 32 ? 1 : 0);
    uint16_t a = nt * 0x400 + (row & 31) * 32 + (col & 31);
    if (l->mirroring) // horizontal
        return vnes->vram[((a >> 1) & 0x400) | (a & 0x3FF)];
    return vnes->vram[a & 0x7FF];
}
