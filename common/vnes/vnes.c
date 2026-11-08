#include "vnes.h"

#include <string.h>

Vnes *vnes;

void vnesInit(Vnes *n, const uint8_t *prg, int prg_size, const uint8_t *chr, int chr_size)
{
    memset(n, 0, sizeof *n);
    n->prg = prg;
    n->prg_size = prg_size;
    n->chr = chr;
    n->chr_size = chr_size;
    n->dots_per_line = 341;
}

// MMC3 PRG mode 0 or 1 (bit 6 of bank select).
int vnesPrgBank(int slot)
{
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

void vnesWrite(uint16_t a, uint8_t v)
{
    if (a < 0x2000) {
        vnes->ram[a & 0x7FF] = v;
        return;
    }
    if (a < 0x4000) {
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
            if (!vnes->w)
                vnes->t = (vnes->t & 0xFFE0) | v >> 3;
            else
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
        if (a != 0x4017)
            return;
    }
    if (a < 0x6000)
        return;
    if (a < 0x8000) {
        vnes->prg_ram[a & 0x1FFF] = v;
        return;
    }
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

void vnesAddCycles(int cycles)
{
    vnes->dots += (long)(cycles + vnes->dma_cycles) * 3;
    vnes->dma_cycles = 0;
    while (vnes->dots >= vnes->dots_per_line) {
        vnes->dots -= vnes->dots_per_line;
        if ((vnes->mask & 0x18) && (vnes->line < 240 || vnes->line == 261))
            mmc3_clock();
        vnes->line++;
        if (vnes->line == 241) {
            vnes->status |= 0x80;
            if (vnes->ctrl & 0x80)
                vnes->nmi = 1;
        } else if (vnes->line == 262) {
            vnes->line = 0;
            vnes->status &= 0x1F;
            vnes->frame++;
        }
    }
}
