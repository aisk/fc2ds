#include "cpu6502.h"

#define RD(a) c->read(c->ctx, (a))
#define WR(a, v) c->write(c->ctx, (a), (v))

static uint16_t rd16(Cpu *c, uint16_t a)
{
    return RD(a) | RD((uint16_t)(a + 1)) << 8;
}

// JMP (ind) and zero page pointers wrap within their page.
static uint16_t rd16_wrap(Cpu *c, uint16_t a)
{
    uint16_t hi = (a & 0xFF00) | ((a + 1) & 0xFF);
    return RD(a) | RD(hi) << 8;
}

static void push(Cpu *c, uint8_t v)
{
    WR(0x100 | c->s, v);
    c->s--;
}

static uint8_t pull(Cpu *c)
{
    c->s++;
    return RD(0x100 | c->s);
}

static void setzn(Cpu *c, uint8_t v)
{
    c->p = (c->p & ~(FLAG_Z | FLAG_N)) | (v ? 0 : FLAG_Z) | (v & FLAG_N);
}

void cpuReset(Cpu *c)
{
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = FLAG_I | FLAG_U;
    c->pc = rd16(c, 0xFFFC);
    c->cycles = 7;
    c->nmi = c->irq = 0;
}

static void interrupt(Cpu *c, uint16_t vector, int brk)
{
    push(c, c->pc >> 8);
    push(c, c->pc & 0xFF);
    push(c, (c->p | FLAG_U | (brk ? FLAG_B : 0)) & ~(brk ? 0 : FLAG_B));
    c->p |= FLAG_I;
    c->pc = rd16(c, vector);
}

enum { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };

// Addressing mode and base cycles per opcode; 0 cycles = not implemented.
static const uint8_t mode_of[256] = {
    IMP, IZX, 0, 0, 0, ZP, ZP, 0, IMP, IMM, ACC, 0, 0, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
    ABS, IZX, 0, 0, ZP, ZP, ZP, 0, IMP, IMM, ACC, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
    IMP, IZX, 0, 0, 0, ZP, ZP, 0, IMP, IMM, ACC, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
    IMP, IZX, 0, 0, 0, ZP, ZP, 0, IMP, IMM, ACC, 0, IND, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
    0, IZX, 0, 0, ZP, ZP, ZP, 0, IMP, 0, IMP, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, ZPX, ZPX, ZPY, 0, IMP, ABY, IMP, 0, 0, ABX, 0, 0,
    IMM, IZX, IMM, 0, ZP, ZP, ZP, 0, IMP, IMM, IMP, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, ZPX, ZPX, ZPY, 0, IMP, ABY, IMP, 0, ABX, ABX, ABY, 0,
    IMM, IZX, 0, 0, ZP, ZP, ZP, 0, IMP, IMM, IMP, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
    IMM, IZX, 0, 0, ZP, ZP, ZP, 0, IMP, IMM, IMP, 0, ABS, ABS, ABS, 0,
    REL, IZY, 0, 0, 0, ZPX, ZPX, 0, IMP, ABY, 0, 0, 0, ABX, ABX, 0,
};

static const uint8_t cycles_of[256] = {
    7, 6, 0, 0, 0, 3, 5, 0, 3, 2, 2, 0, 0, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
    6, 6, 0, 0, 3, 3, 5, 0, 4, 2, 2, 0, 4, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
    6, 6, 0, 0, 0, 3, 5, 0, 3, 2, 2, 0, 3, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
    6, 6, 0, 0, 0, 3, 5, 0, 4, 2, 2, 0, 5, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
    0, 6, 0, 0, 3, 3, 3, 0, 2, 0, 2, 0, 4, 4, 4, 0,
    2, 6, 0, 0, 4, 4, 4, 0, 2, 5, 2, 0, 0, 5, 0, 0,
    2, 6, 2, 0, 3, 3, 3, 0, 2, 2, 2, 0, 4, 4, 4, 0,
    2, 5, 0, 0, 4, 4, 4, 0, 2, 4, 2, 0, 4, 4, 4, 0,
    2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
    2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
    2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
};

// Reads (not stores or read-modify-write) take one more cycle when indexing
// crosses a page.
static int page_penalty(uint8_t op)
{
    switch (op & 0x1F) {
    case 0x11: // (zp),y
    case 0x19: // abs,y
    case 0x1D: // abs,x
    case 0x1C:
    case 0x1E:
        break;
    default:
        return 0;
    }
    if (op == 0x91 || op == 0x99 || op == 0x9D) // stores
        return 0;
    if ((op & 0x0F) == 0x0E && op != 0xBE) // rmw abs,x
        return 0;
    return 1;
}

static void adc(Cpu *c, uint8_t v)
{
    unsigned sum = c->a + v + (c->p & FLAG_C);
    c->p &= ~(FLAG_C | FLAG_V);
    if (sum > 0xFF)
        c->p |= FLAG_C;
    if (~(c->a ^ v) & (c->a ^ sum) & 0x80)
        c->p |= FLAG_V;
    c->a = sum;
    setzn(c, c->a);
}

static void cmp(Cpu *c, uint8_t r, uint8_t v)
{
    c->p = (c->p & ~FLAG_C) | (r >= v ? FLAG_C : 0);
    setzn(c, r - v);
}

int cpuStep(Cpu *c)
{
    if (c->nmi) {
        c->nmi = 0;
        interrupt(c, 0xFFFA, 0);
        c->cycles += 7;
        return 7;
    }
    if (c->irq && !(c->p & FLAG_I)) {
        interrupt(c, 0xFFFE, 0);
        c->cycles += 7;
        return 7;
    }

    uint16_t at = c->pc;
    uint8_t op = RD(c->pc++);
    int cyc = cycles_of[op];
    if (!cyc) {
        c->pc = at;
        return -1;
    }

    uint16_t addr = 0, base;
    int crossed = 0;
    switch (mode_of[op]) {
    case IMP:
    case ACC:
        break;
    case IMM:
        addr = c->pc++;
        break;
    case ZP:
        addr = RD(c->pc++);
        break;
    case ZPX:
        addr = (RD(c->pc++) + c->x) & 0xFF;
        break;
    case ZPY:
        addr = (RD(c->pc++) + c->y) & 0xFF;
        break;
    case ABS:
        addr = rd16(c, c->pc);
        c->pc += 2;
        break;
    case ABX:
        base = rd16(c, c->pc);
        c->pc += 2;
        addr = base + c->x;
        crossed = (base ^ addr) & 0x100;
        break;
    case ABY:
        base = rd16(c, c->pc);
        c->pc += 2;
        addr = base + c->y;
        crossed = (base ^ addr) & 0x100;
        break;
    case IND:
        addr = rd16_wrap(c, rd16(c, c->pc));
        c->pc += 2;
        break;
    case IZX:
        addr = rd16_wrap(c, (RD(c->pc++) + c->x) & 0xFF);
        break;
    case IZY:
        base = rd16_wrap(c, RD(c->pc++));
        addr = base + c->y;
        crossed = (base ^ addr) & 0x100;
        break;
    case REL: {
        int8_t off = RD(c->pc++);
        addr = c->pc + off;
        break;
    }
    }
    if (crossed && page_penalty(op))
        cyc++;

    uint8_t v;
    switch (op) {
    // Loads and stores
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1:
        c->a = RD(addr); setzn(c, c->a); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        c->x = RD(addr); setzn(c, c->x); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        c->y = RD(addr); setzn(c, c->y); break;
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91:
        WR(addr, c->a); break;
    case 0x86: case 0x96: case 0x8E:
        WR(addr, c->x); break;
    case 0x84: case 0x94: case 0x8C:
        WR(addr, c->y); break;

    // Transfers
    case 0xAA: c->x = c->a; setzn(c, c->x); break;
    case 0xA8: c->y = c->a; setzn(c, c->y); break;
    case 0x8A: c->a = c->x; setzn(c, c->a); break;
    case 0x98: c->a = c->y; setzn(c, c->a); break;
    case 0xBA: c->x = c->s; setzn(c, c->x); break;
    case 0x9A: c->s = c->x; break;

    // Stack
    case 0x48: push(c, c->a); break;
    case 0x08: push(c, c->p | FLAG_B | FLAG_U); break;
    case 0x68: c->a = pull(c); setzn(c, c->a); break;
    case 0x28: c->p = (pull(c) & ~FLAG_B) | FLAG_U; break;

    // Logic and arithmetic
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31:
        c->a &= RD(addr); setzn(c, c->a); break;
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11:
        c->a |= RD(addr); setzn(c, c->a); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51:
        c->a ^= RD(addr); setzn(c, c->a); break;
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71:
        adc(c, RD(addr)); break;
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1:
        adc(c, RD(addr) ^ 0xFF); break;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1:
        cmp(c, c->a, RD(addr)); break;
    case 0xE0: case 0xE4: case 0xEC: cmp(c, c->x, RD(addr)); break;
    case 0xC0: case 0xC4: case 0xCC: cmp(c, c->y, RD(addr)); break;
    case 0x24: case 0x2C:
        v = RD(addr);
        c->p = (c->p & ~(FLAG_Z | FLAG_V | FLAG_N)) | (v & (FLAG_V | FLAG_N)) | ((c->a & v) ? 0 : FLAG_Z);
        break;

    // Increments
    case 0xE6: case 0xF6: case 0xEE: case 0xFE:
        v = RD(addr) + 1; WR(addr, v); setzn(c, v); break;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE:
        v = RD(addr) - 1; WR(addr, v); setzn(c, v); break;
    case 0xE8: c->x++; setzn(c, c->x); break;
    case 0xC8: c->y++; setzn(c, c->y); break;
    case 0xCA: c->x--; setzn(c, c->x); break;
    case 0x88: c->y--; setzn(c, c->y); break;

    // Shifts
    case 0x0A: c->p = (c->p & ~FLAG_C) | (c->a >> 7); c->a <<= 1; setzn(c, c->a); break;
    case 0x4A: c->p = (c->p & ~FLAG_C) | (c->a & 1); c->a >>= 1; setzn(c, c->a); break;
    case 0x2A: { uint8_t ci = c->p & FLAG_C; c->p = (c->p & ~FLAG_C) | (c->a >> 7); c->a = c->a << 1 | ci; setzn(c, c->a); break; }
    case 0x6A: { uint8_t ci = c->p & FLAG_C; c->p = (c->p & ~FLAG_C) | (c->a & 1); c->a = c->a >> 1 | ci << 7; setzn(c, c->a); break; }
    case 0x06: case 0x16: case 0x0E: case 0x1E:
        v = RD(addr); c->p = (c->p & ~FLAG_C) | (v >> 7); v <<= 1; WR(addr, v); setzn(c, v); break;
    case 0x46: case 0x56: case 0x4E: case 0x5E:
        v = RD(addr); c->p = (c->p & ~FLAG_C) | (v & 1); v >>= 1; WR(addr, v); setzn(c, v); break;
    case 0x26: case 0x36: case 0x2E: case 0x3E: {
        uint8_t ci = c->p & FLAG_C;
        v = RD(addr); c->p = (c->p & ~FLAG_C) | (v >> 7); v = v << 1 | ci; WR(addr, v); setzn(c, v); break;
    }
    case 0x66: case 0x76: case 0x6E: case 0x7E: {
        uint8_t ci = c->p & FLAG_C;
        v = RD(addr); c->p = (c->p & ~FLAG_C) | (v & 1); v = v >> 1 | ci << 7; WR(addr, v); setzn(c, v); break;
    }

    // Jumps
    case 0x4C: case 0x6C: c->pc = addr; break;
    case 0x20:
        c->pc--;
        push(c, c->pc >> 8);
        push(c, c->pc & 0xFF);
        c->pc = addr;
        break;
    case 0x60: c->pc = pull(c); c->pc |= pull(c) << 8; c->pc++; break;
    case 0x40:
        c->p = (pull(c) & ~FLAG_B) | FLAG_U;
        c->pc = pull(c);
        c->pc |= pull(c) << 8;
        break;
    case 0x00: c->pc++; interrupt(c, 0xFFFE, 1); break;

    // Branches
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: {
        static const uint8_t flag[4] = { FLAG_N, FLAG_V, FLAG_C, FLAG_Z };
        int taken = !!(c->p & flag[op >> 6]) == !!(op & 0x20);
        if (taken) {
            cyc += 1 + (((c->pc ^ addr) & 0x100) ? 1 : 0);
            c->pc = addr;
        }
        break;
    }

    // Flags
    case 0x18: c->p &= ~FLAG_C; break;
    case 0x38: c->p |= FLAG_C; break;
    case 0x58: c->p &= ~FLAG_I; break;
    case 0x78: c->p |= FLAG_I; break;
    case 0xB8: c->p &= ~FLAG_V; break;
    case 0xD8: c->p &= ~FLAG_D; break;
    case 0xF8: c->p |= FLAG_D; break;
    case 0xEA: break;
    default:
        c->pc = at;
        return -1;
    }
    c->cycles += cyc;
    return cyc;
}
