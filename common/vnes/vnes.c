#include "vnes.h"

#include <string.h>

// Hot loops run every frame: on the NDS keep them in ITCM as ARM code.
#if defined(ARM9)
#include <nds/ndstypes.h>
#define VNES_HOT ITCM_CODE ARM_CODE
#else
#define VNES_HOT
#endif

Vnes *vnes;

// Log a raster register access during the visible lines.
static void raster_event(uint16_t addr, uint8_t value)
{
    if (vnes->line >= 240 || vnes->raster_count >= VNES_RASTER_EVENTS)
        return;
    VnesRasterEvent *e = &vnes->raster[vnes->raster_count++];
    e->line = vnes->line;
    e->addr = addr;
    e->value = value;
}


void vnesInit(Vnes *n, const uint8_t *prg, int prg_size, const uint8_t *chr, int chr_size)
{
    memset(n, 0, sizeof *n);
    n->prg = prg;
    n->prg_size = prg_size;
    n->chr = chr;
    n->chr_size = chr_size;
    n->dots_per_line = 341;
    n->sprite0_line = -1;
}

// MMC3 PRG mode 0 or 1 (bit 6 of bank select); NROM maps 16 or 32 KiB.
int vnesPrgBank(int slot)
{
    if (vnes->mapper == VNES_NROM)
        return slot % (vnes->prg_size / 0x2000);
    int last = vnes->prg_size / 0x2000 - 1;
    int swap = vnes->bank_select & 0x40;
    switch (slot) {
    case 0: return swap ? last - 1 : vnes->regs[6];
    case 1: return vnes->regs[7];
    case 2: return swap ? vnes->regs[6] : last - 1;
    default: return last;
    }
}

int vnesChrOffset(uint16_t a)
{
    if (vnes->mapper == VNES_NROM)
        return a % vnes->chr_size;
    int inv = (vnes->bank_select & 0x80) ? 0x1000 : 0;
    a ^= inv;
    int bank;
    if (a < 0x1000)
        bank = (vnes->regs[a >> 11] & 0xFE) + ((a >> 10) & 1);
    else
        bank = vnes->regs[2 + ((a - 0x1000) >> 10)];
    return (bank * 0x400 + (a & 0x3FF)) % vnes->chr_size;
}

uint16_t vnesNametableIndex(uint16_t a)
{
    a &= 0xFFF;
    if (vnes->mirroring) // horizontal
        return ((a >> 1) & 0x400) | (a & 0x3FF);
    return a & 0x7FF;
}

uint8_t vnesPpuRead(uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return vnes->chr[vnesChrOffset(a)];
    if (a < 0x3F00)
        return vnes->vram[vnesNametableIndex(a)];
    a &= 0x1F;
    if ((a & 0x13) == 0x10)
        a &= 0x0F;
    return vnes->palette[a];
}

static void ppu_mem_write(uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return; // CHR ROM
    if (a < 0x3F00) {
        vnes->vram[vnesNametableIndex(a)] = v;
        return;
    }
    a &= 0x1F;
    if ((a & 0x13) == 0x10)
        a &= 0x0F;
    vnes->palette[a] = v & 0x3F;
}

uint8_t vnesRead(uint16_t a)
{
    if (a < 0x2000)
        return vnes->ram[a & 0x7FF];
    if (a < 0x4000) {
        switch (a & 7) {
        case 2: {
            uint8_t s = vnes->status;
            vnes->status &= 0x7F;
            vnes->w = 0;
            raster_event(0x2002, 0);
            return s;
        }
        case 4:
            return vnes->oam[vnes->oam_addr];
        case 7: {
            uint8_t r;
            if ((vnes->v & 0x3FFF) >= 0x3F00) {
                r = vnesPpuRead(vnes->v);
                vnes->read_buffer = vnesPpuRead(vnes->v - 0x1000);
            } else {
                r = vnes->read_buffer;
                vnes->read_buffer = vnesPpuRead(vnes->v);
            }
            vnes->v += (vnes->ctrl & 4) ? 32 : 1;
            return r;
        }
        }
        return 0;
    }
    if (a == 0x4016) {
        uint8_t bit = vnes->strobe ? (vnes->pad & 1) : (vnes->pad_shift & 1);
        if (!vnes->strobe)
            vnes->pad_shift = (vnes->pad_shift >> 1) | 0x80;
        return 0x40 | bit;
    }
    if (a < 0x6000)
        return 0x40;
    if (a < 0x8000)
        return vnes->prg_ram[a & 0x1FFF];
    int slot = (a - 0x8000) >> 13;
    return vnes->prg[(vnesPrgBank(slot) * 0x2000 + (a & 0x1FFF)) % vnes->prg_size];
}

void vnesRasterSave(VnesRaster *r)
{
    r->t = vnes->t;
    r->v = vnes->v;
    r->x = vnes->x;
    r->w = vnes->w;
    r->ctrl = vnes->ctrl;
    r->mask = vnes->mask;
    r->bank_select = vnes->bank_select;
    for (int i = 0; i < 8; i++)
        r->regs[i] = vnes->regs[i];
    r->mirroring = vnes->mirroring;
}

void vnesWrite(uint16_t a, uint8_t v)
{
    if (a < 0x2000) {
        vnes->ram[a & 0x7FF] = v;
        return;
    }
    if (a < 0x4000) {
        if ((a & 7) <= 1 || (a & 7) == 5 || (a & 7) == 6)
            raster_event(0x2000 | (a & 7), v);
        switch (a & 7) {
        case 0:
            // NMI enabled during vblank fires at once.
            if (!(vnes->ctrl & 0x80) && (v & 0x80) && (vnes->status & 0x80))
                vnes->nmi = 1;
            vnes->ctrl = v;
            vnes->t = (vnes->t & 0xF3FF) | (v & 3) << 10;
            break;
        case 1: vnes->mask = v; break;
        case 3: vnes->oam_addr = v; break;
        case 4: vnes->oam[vnes->oam_addr++] = v; break;
        case 5:
            if (!vnes->w) {
                vnes->t = (vnes->t & 0xFFE0) | v >> 3;
                vnes->x = v & 7;
            } else
                vnes->t = (vnes->t & 0x8C1F) | (v & 7) << 12 | (v & 0xF8) << 2;
            vnes->w ^= 1;
            break;
        case 6:
            if (!vnes->w)
                vnes->t = (vnes->t & 0x00FF) | (v & 0x3F) << 8;
            else {
                vnes->t = (vnes->t & 0xFF00) | v;
                vnes->v = vnes->t;
            }
            vnes->w ^= 1;
            break;
        case 7:
            ppu_mem_write(vnes->v, v);
            vnes->v += (vnes->ctrl & 4) ? 32 : 1;
            break;
        }
        return;
    }
    if (a == 0x4014) {
        for (int i = 0; i < 256; i++)
            vnes->oam[(vnes->oam_addr + i) & 0xFF] = vnesRead(v << 8 | i);
        vnes->dma_cycles += 513;
        return;
    }
    if (a == 0x4016) {
        vnes->strobe = v & 1;
        if (vnes->strobe)
            vnes->pad_shift = vnes->pad;
        return;
    }
    if (a < 0x4018) {
        vnes->apu[a - 0x4000] = v;
        vnes->apu_hash = vnes->apu_hash * 31 + ((uint32_t)a << 8 | v);
        if (vnes->apu_write)
            vnes->apu_write(a, v);
        if (a != 0x4017)
            return;
    }
    if (a < 0x6000)
        return;
    if (a < 0x8000) {
        vnes->prg_ram[a & 0x1FFF] = v;
        return;
    }
    if (vnes->mapper != VNES_MMC3)
        return; // NROM: ROM, no registers
    if ((a & 0xE001) <= 0xA000)
        raster_event(a & 0xE001, v);
    switch (a & 0xE001) {
    case 0x8000: vnes->bank_select = v; break;
    case 0x8001: vnes->regs[vnes->bank_select & 7] = v; break;
    case 0xA000: vnes->mirroring = v & 1; break;
    case 0xA001: break;
    case 0xC000: vnes->irq_latch = v; break;
    case 0xC001: vnes->irq_counter = 0; vnes->irq_reload = 1; break;
    case 0xE000: vnes->irq_enabled = 0; vnes->irq_pending = 0; break;
    case 0xE001: vnes->irq_enabled = 1; break;
    }
}

static void mmc3_clock(void)
{
    if (vnes->irq_counter == 0 || vnes->irq_reload) {
        vnes->irq_counter = vnes->irq_latch;
        vnes->irq_reload = 0;
    } else {
        vnes->irq_counter--;
    }
    if (vnes->irq_counter == 0 && vnes->irq_enabled)
        vnes->irq_pending = 1;
}

// Colour index (0-3) of a pattern pixel.
static int pattern_pixel(uint16_t base, int tile, int row, int col)
{
    uint16_t a = base + tile * 16 + row;
    return (vnesPpuRead(a) >> (7 - col) & 1) | (vnesPpuRead(a + 8) >> (7 - col) & 1) << 1;
}

// First line where an opaque pixel of sprite 0 meets an opaque background
// pixel, from the OAM and the scroll (t, fine X) at the start of the frame.
// Mid-frame changes are not followed: games put sprite 0 at the top of the
// screen, in the part that is drawn with the frame's starting scroll.
static int sprite0_hit_line(void)
{
    uint8_t mask = vnes->mask, ctrl = vnes->ctrl;
    if ((mask & 0x18) != 0x18)
        return -1;
    int sy = vnes->oam[0] + 1, tile = vnes->oam[1], attr = vnes->oam[2], sx = vnes->oam[3];
    int tall = ctrl & 0x20, h = tall ? 16 : 8;
    uint16_t t = vnes->t;
    int scroll_x = (t & 0x1F) * 8 + vnes->x + (t & 0x400 ? 256 : 0);
    int scroll_y = ((t >> 5) & 0x1F) * 8 + ((t >> 12) & 7) + (t & 0x800 ? 240 : 0);
    for (int r = 0; r < h; r++) {
        int y = sy + r;
        if (y >= 240)
            break;
        int pr = attr & 0x80 ? h - 1 - r : r;
        uint16_t sbase;
        int stile;
        if (tall) {
            sbase = tile & 1 ? 0x1000 : 0;
            stile = (tile & 0xFE) + (pr >= 8);
        } else {
            sbase = ctrl & 0x08 ? 0x1000 : 0;
            stile = tile;
        }
        for (int c = 0; c < 8; c++) {
            int x = sx + c;
            if (x >= 255 || (x < 8 && (mask & 0x06) != 0x06))
                continue;
            if (!pattern_pixel(sbase, stile, pr & 7, attr & 0x40 ? 7 - c : c))
                continue;
            int wx = (scroll_x + x) % 512, wy = (scroll_y + y) % 480;
            uint16_t nt = 0x2000 + (wy >= 240 ? 0x800 : 0) + (wx >= 256 ? 0x400 : 0);
            int bx = wx & 255, by = wy % 240;
            int btile = vnesPpuRead(nt + (by >> 3) * 32 + (bx >> 3));
            if (pattern_pixel(ctrl & 0x10 ? 0x1000 : 0, btile, by & 7, bx & 7))
                return y;
        }
    }
    return -1;
}

static inline void add_cycles(int cycles)
{
    vnes->dots += (long)(cycles + vnes->dma_cycles) * 3;
    vnes->dma_cycles = 0;
    while (vnes->dots >= vnes->dots_per_line) {
        vnes->dots -= vnes->dots_per_line;
        if ((vnes->mask & 0x18) && (vnes->line < 240 || vnes->line == 261) && vnes->mapper == VNES_MMC3)
            mmc3_clock();
        vnes->line++;
        if (vnes->line == vnes->sprite0_line) {
            vnes->status |= 0x40;
        } else if (vnes->line == 241) {
            vnes->status |= 0x80;
            if (vnes->ctrl & 0x80)
                vnes->nmi = 1;
        } else if (vnes->line == 261) {
            // Pre-render line: what the next frame starts from. The
            // sprite 0 hit and overflow flags are cleared here.
            vnesRasterSave(&vnes->raster_start);
            vnes->raster_count = 0;
            vnes->status &= 0x9F;
            vnes->sprite0_line = sprite0_hit_line();
        } else if (vnes->line == 262) {
            vnes->line = 0;
            vnes->status &= 0x1F;
            vnes->frame++;
        }
    }
}

void vnesAddCycles(int cycles)
{
    add_cycles(cycles);
}

VNES_HOT int vnesRunUntil(int stop_line)
{
    for (;;) {
        if (vnes->nmi || vnes->irq_pending)
            return 0;
        add_cycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
        if (vnes->line == stop_line)
            return 1;
    }
}

int vnesWaitStatus(uint8_t mask, uint8_t value)
{
    for (int i = 0; i < 262; i++) {
        if ((vnes->status & mask) == value)
            return 1;
        add_cycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
    }
    return (vnes->status & mask) == value;
}
