/*
 * Copyright (c) 2014 The KnightOS Group
 * Modified to support the eZ80 processor by CEmu developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
 * TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
*/

#include <string.h>

#include "cpu.h"
#include "emu.h"
#include "registers.h"
#include "interrupt.h"
#include "debug/debug.h"

// Global CPU state
eZ80cpu_t cpu;

static void cpu_get_cntrl_data_blocks_format(void) {
    cpu.PREFIX = cpu.SUFFIX = 0;
    cpu.L = cpu.ADL;
    cpu.IL = cpu.ADL;
}

static uint32_t cpu_mask_mode(uint32_t value, bool mode) {
    return value & (mode ? 0xFFFFFF : 0xFFFF);
}

static uint32_t cpu_address_mode(uint32_t address, bool mode) {
    if (mode) {
        return address & 0xFFFFFF;
    }
    return (cpu.registers.MBASE << 16) | (address & 0xFFFF);
}

static void cpu_prefetch(uint32_t address, bool mode) {
    cpu.ADL = mode;
    cpu.registers.PC = cpu_address_mode(address, mode);
    cpu.prefetch = memory_read_byte(cpu.registers.PC);
}
static uint8_t cpu_fetch_byte(void) {
    uint8_t value;
    if (!in_debugger && mem.debug.block[cpu.registers.PC] & (DBG_EXEC_BREAKPOINT | DBG_STEP_OVER_BREAKPOINT)) {
        debugger(mem.debug.block[cpu.registers.PC] & DBG_EXEC_BREAKPOINT ? HIT_EXEC_BREAKPOINT : DBG_STEP, cpu.registers.PC);
    }
    value = cpu.prefetch;
    cpu_prefetch(cpu.registers.PC + 1, cpu.ADL);
    return value;
}
static int8_t cpu_fetch_offset(void) {
    return (int8_t)cpu_fetch_byte();
}
static uint32_t cpu_fetch_word(void) {
    uint32_t value = cpu_fetch_byte();
    value |= cpu_fetch_byte() << 8;
    if (cpu.IL) {
        value |= cpu_fetch_byte() << 16;
    }
    return value;
}
static uint32_t cpu_fetch_word_no_prefetch(void) {
    uint32_t value = cpu_fetch_byte();
    value |= cpu.prefetch << 8;
    if (cpu.IL) {
        cpu_fetch_byte();
        value |= cpu.prefetch << 16;
    }
    cpu.registers.PC++;
    return value;
}

static uint8_t cpu_read_byte(uint32_t address) {
    return memory_read_byte(cpu_address_mode(address, cpu.L));
}
static void cpu_write_byte(uint32_t address, uint8_t value) {
    memory_write_byte(cpu_address_mode(address, cpu.L), value);
}

static uint32_t cpu_read_word(uint32_t address) {
    uint32_t value = cpu_read_byte(address);
    value |= cpu_read_byte(address + 1) << 8;
    if (cpu.L) {
        value |= cpu_read_byte(address + 2) << 16;
    }
    return value;
}
static void cpu_write_word(uint32_t address, uint32_t value) {
    cpu_write_byte(address, value);
    cpu_write_byte(address + 1, value >> 8);
    if (cpu.L) {
        cpu_write_byte(address + 2, value >> 16);
    }
}

static uint8_t cpu_pop_byte(void) {
    return cpu_read_byte(cpu.registers.stack[cpu.L].hl++);
}
static void cpu_push_byte(uint8_t value) {
    cpu_write_byte(--cpu.registers.stack[cpu.L].hl, value);
}

static void cpu_push_word(uint32_t value) {
    if (cpu.L) {
        cpu_push_byte(value >> 16);
    }
    cpu_push_byte(value >> 8);
    cpu_push_byte(value);
}

static uint32_t cpu_pop_word(void) {
    uint32_t value = cpu_pop_byte();
    value |= cpu_pop_byte() << 8;
    if (cpu.L) {
        value |= cpu_pop_byte() << 16;
    }
    return value;
}

static uint8_t cpu_read_in(uint16_t pio) {
    return port_read_byte(pio);
}

static void cpu_write_out(uint16_t pio, uint8_t value) {
    port_write_byte(pio, value);
}

static uint32_t cpu_read_sp(void) {
    return cpu.registers.stack[cpu.L].hl;
}
static void cpu_write_sp(uint32_t value) {
    cpu.registers.stack[cpu.L].hl = value;
}

static uint8_t cpu_read_index_low(void) {
    return cpu.registers.index[cpu.PREFIX].l;
}
static void cpu_write_index_low(uint8_t value) {
    cpu.registers.index[cpu.PREFIX].l = value;
}

static uint8_t cpu_read_index_high(void) {
    return cpu.registers.index[cpu.PREFIX].h;
}
static void cpu_write_index_high(uint8_t value) {
    cpu.registers.index[cpu.PREFIX].h = value;
}

static uint32_t cpu_read_index(void) {
    return cpu.registers.index[cpu.PREFIX].hl;
}
static void cpu_write_index(uint32_t value) {
    cpu.registers.index[cpu.PREFIX].hl = value;
}

static uint32_t cpu_read_other_index(void) {
    return cpu.registers.index[cpu.PREFIX ^ 1].hl;
}
static void cpu_write_other_index(uint32_t value) {
    cpu.registers.index[cpu.PREFIX ^ 1].hl = value;
}

static uint32_t cpu_index_address(void) {
    uint32_t value = cpu_read_index();
    if (cpu.PREFIX) {
        value += cpu_fetch_offset();
    }
    return cpu_mask_mode(value, cpu.L);
}

static uint8_t cpu_read_reg(int i) {
    uint8_t value;
    switch (i) {
        case 0: value = cpu.registers.B; break;
        case 1: value = cpu.registers.C; break;
        case 2: value = cpu.registers.D; break;
        case 3: value = cpu.registers.E; break;
        case 4: value = cpu_read_index_high(); break;
        case 5: value = cpu_read_index_low(); break;
        case 6: value = cpu_read_byte(cpu_index_address()); break;
        case 7: value = cpu.registers.A; break;
        default: abort();
    }
    return value;
}
static void cpu_write_reg(int i, uint8_t value) {
    switch (i) {
        case 0: cpu.registers.B = value; break;
        case 1: cpu.registers.C = value; break;
        case 2: cpu.registers.D = value; break;
        case 3: cpu.registers.E = value; break;
        case 4: cpu_write_index_high(value); break;
        case 5: cpu_write_index_low(value); break;
        case 6: cpu_write_byte(cpu_index_address(), value); break;
        case 7: cpu.registers.A = value; break;
        default: abort();
    }
}
static void cpu_read_write_reg(int read, int write) {
    uint8_t value;
    int old_prefix = cpu.PREFIX;
    cpu.PREFIX = (write != 6) ? old_prefix : 0;
    value = cpu_read_reg(read);
    cpu.PREFIX = (read != 6) ? old_prefix : 0;
    cpu_write_reg(write, value);
}

static uint8_t cpu_read_reg_prefetched(int i, uint32_t address) {
    uint8_t value;
    switch (i) {
        case 0: value = cpu.registers.B; break;
        case 1: value = cpu.registers.C; break;
        case 2: value = cpu.registers.D; break;
        case 3: value = cpu.registers.E; break;
        case 4: value = cpu_read_index_high(); break;
        case 5: value = cpu_read_index_low(); break;
        case 6: value = cpu_read_byte(address); break;
        case 7: value = cpu.registers.A; break;
        default: abort();
    }
    return value;
}
static void cpu_write_reg_prefetched(int i, uint32_t address, uint8_t value) {
    switch (i) {
        case 0: cpu.registers.B = value; break;
        case 1: cpu.registers.C = value; break;
        case 2: cpu.registers.D = value; break;
        case 3: cpu.registers.E = value; break;
        case 4: cpu_write_index_high(value); break;
        case 5: cpu_write_index_low(value); break;
        case 6: cpu_write_byte(address, value); break;
        case 7: cpu.registers.A = value; break;
        default: abort();
    }
}

static uint32_t cpu_read_rp(int i) {
    uint32_t value;
    switch (i) {
        case 0: value = cpu.registers.BC; break;
        case 1: value = cpu.registers.DE; break;
        case 2: value = cpu_read_index(); break;
        case 3: value = cpu_read_sp(); break;
        default: abort();
    }
    return cpu_mask_mode(value, cpu.L);
}
static void cpu_write_rp(int i, uint32_t value) {
    value = cpu_mask_mode(value, cpu.L);
    switch (i) {
        case 0: cpu.registers.BC = value; break;
        case 1: cpu.registers.DE = value; break;
        case 2: cpu_write_index(value); break;
        case 3: cpu_write_sp(value); break;
        default: abort();
    }
}

static uint32_t cpu_read_rp2(int i) {
    if (i == 3) {
        return cpu.registers.AF;
    } else {
        return cpu_read_rp(i);
    }
}
static void cpu_write_rp2(int i, uint32_t value) {
    if (i == 3) {
        cpu.registers.AF = value;
    } else {
        cpu_write_rp(i, value);
    }
}

static uint32_t cpu_read_rp3(int i) {
    uint32_t value;
    switch (i) {
        case 0: value = cpu.registers.BC; break;
        case 1: value = cpu.registers.DE; break;
        case 2: value = cpu.registers.HL; break;
        case 3: value = cpu_read_index(); break;
        default: abort();
    }
    return cpu_mask_mode(value, cpu.L);
}
static void cpu_write_rp3(int i, uint32_t value) {
    value = cpu_mask_mode(value, cpu.L);
    switch (i) {
        case 0: cpu.registers.BC = value; break;
        case 1: cpu.registers.DE = value; break;
        case 2: cpu.registers.HL = value; break;
        case 3: cpu_write_index(value); break;
        default: abort();
    }
}

static bool cpu_read_cc(const int i) {
    eZ80registers_t *r = &cpu.registers;
    switch (i) {
        case 0: return !r->flags.Z;
        case 1: return  r->flags.Z;
        case 2: return !r->flags.C;
        case 3: return  r->flags.C;
        case 4: return !r->flags.PV;
        case 5: return  r->flags.PV;
        case 6: return !r->flags.S;
        case 7: return  r->flags.S;
        default: abort();
    }
}

static void cpu_execute_daa(void) {
    eZ80registers_t *r = &cpu.registers;
    uint8_t old = r->A;
    uint8_t v = 0;
    if ((r->A & 0xF) > 9 || r->flags.H) {
        v += 6;
    }
    if (((r->A + v) >> 4) > 9 || _flag_carry_b(r->A + v) || r->flags.C) {
        v += 0x60;
    }
    if (r->flags.N) {
        r->A -= v;
        r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
            | _flag_undef(r->F) | _flag_parity(r->A)
            | _flag_subtract(r->flags.N) | __flag_c(v >= 0x60)
            | _flag_halfcarry_b_sub(old, v, 0);
    } else {
        r->A += v;
        r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
            | _flag_undef(r->F) | _flag_parity(r->A)
            | _flag_subtract(r->flags.N) | __flag_c(v >= 0x60)
            | _flag_halfcarry_b_add(old, v, 0);
    }
}

static uint32_t cpu_dec_bc_partial_mode() {
    uint32_t value = cpu_mask_mode(cpu.registers.BC - 1, cpu.L);
    if (cpu.L) {
        cpu.registers.BC = value;
    } else {
        cpu.registers.BCS = value;
    }
    return value;
}

static void cpu_call(uint32_t address, uint8_t mixed) {
    eZ80registers_t *r = &cpu.registers;
    if (mixed) {
        if (cpu.ADL) {
            cpu_write_byte(--r->SPL, r->PCU);
        }
        if (cpu.IL || (cpu.L && !cpu.ADL)) {
            cpu_write_byte(--r->SPL, r->PCH);
            cpu_write_byte(--r->SPL, r->PCL);
        } else {
            cpu_write_byte(--r->SPS, r->PCH);
            cpu_write_byte(--r->SPS, r->PCL);
        }
        cpu_write_byte(--r->SPL, (cpu.MADL << 1) | cpu.ADL);
    } else {
        cpu_push_word(r->PC);
    }
    cpu_prefetch(address, cpu.IL);
}

static void cpu_return(void) {
    eZ80registers_t *r = &cpu.registers;
    uint32_t address;
    bool mode = cpu.ADL;
    cpu.cycles += 1;
    if (cpu.SUFFIX) {
        mode = cpu_read_byte(r->SPL++) & 1;
        if (cpu.ADL) {
            address  = cpu_read_byte(r->SPL++);
            address |= cpu_read_byte(r->SPL++) << 8;
        } else {
            address  = cpu_read_byte(r->SPS++);
            address |= cpu_read_byte(r->SPS++) << 8;
        }
        if (mode) {
            address |= cpu_mask_mode(cpu_read_byte(r->SPL++) << 16, cpu.ADL || cpu.L);
        }
    } else {
        address = cpu_pop_word();
    }
    cpu_prefetch(address, mode);
}

static void cpu_execute_alu(int i, uint8_t v) {
    uint8_t old;
    eZ80registers_t *r = &cpu.registers;
    switch (i) {
        case 0: // ADD A, v
            cpu.cycles += 1;
            old = r->A;
            r->A += v;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_overflow_b_add(old, v, r->A)
                | _flag_subtract(0) | _flag_carry_b(old + v)
                | _flag_halfcarry_b_add(old, v, 0);
            break;
        case 1: // ADC A, v
            cpu.cycles += 1;
            old = r->A;
            r->A += v + r->flags.C;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_overflow_b_add(old, v, r->A)
                | _flag_subtract(0) | _flag_carry_b(old + v + r->flags.C)
                | _flag_halfcarry_b_add(old, v, r->flags.C);
            break;
        case 2: // SUB v
            cpu.cycles += 1;
            old = r->A;
            r->A -= v;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_overflow_b_sub(old, v, r->A)
                | _flag_subtract(1) | _flag_carry_b(old - v)
                | _flag_halfcarry_b_sub(old, v, 0);
            break;
        case 3: // SBC v
            cpu.cycles += 1;
            old = r->A;
            r->A -= v + r->flags.C;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_overflow_b_sub(old, v, r->A)
                | _flag_subtract(1) | _flag_carry_b(old - v - r->flags.C)
                | _flag_halfcarry_b_sub(old, v, r->flags.C);
            break;
        case 4: // AND v
            cpu.cycles += 1;
            r->A &= v;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_parity(r->A)
                | FLAG_H;
            break;
        case 5: // XOR v
            cpu.cycles += 1;
            r->A ^= v;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_parity(r->A);
            break;
        case 6: // OR v
            cpu.cycles += 1;
            r->A |= v;
            r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                | _flag_undef(r->F) | _flag_parity(r->A);
            break;
        case 7: // CP v
            cpu.cycles += 1;
            old = r->A - v;
            r->F = _flag_sign_b(old) | _flag_zero(old)
                | _flag_undef(r->F) | _flag_subtract(1)
                | _flag_carry_b(r->A - v)
                | _flag_overflow_b_sub(r->A, v, old)
                | _flag_halfcarry_b_sub(r->A, v, 0);
            break;
    }
}

static void cpu_execute_rot(int y, int z, uint32_t address, uint8_t value) {
    eZ80registers_t *r = &cpu.registers;
    uint8_t old_7 = (value & 0x80) != 0;
    uint8_t old_0 = (value & 0x01) != 0;
    uint8_t old_c = r->flags.C;
    uint8_t new_c;
    switch (y) {
        case 0: // RLC value[z]
            cpu.cycles += 2;
            value <<= 1;
            value |= old_7;
            new_c = old_7;
            break;
        case 1: // RRC value[z]
            cpu.cycles += 2;
            value >>= 1;
            value |= old_0 << 7;
            new_c = old_0;
            break;
        case 2: // RL value[z]
            cpu.cycles += 2;
            value <<= 1;
            value |= old_c;
            new_c = old_7;
            break;
        case 3: // RR value[z]
            cpu.cycles += 2;
            value >>= 1;
            value |= old_c << 7;
            new_c = old_0;
            break;
        case 4: // SLA value[z]
            cpu.cycles += 2;
            value <<= 1;
            new_c = old_7;
            break;
        case 5: // SRA value[z]
            cpu.cycles += 2;
            value >>= 1;
            value |= old_7 << 7;
            new_c = old_0;
            break;
        case 6: // OPCODETRAP
            cpu.IEF_wait = 1;
            return;
        case 7: // SRL value[z]
            cpu.cycles += 2;
            value >>= 1;
            new_c = old_0;
            break;
        default:
            abort();
    }
    cpu_write_reg_prefetched(z, address, value);
    r->F = __flag_c(new_c) | _flag_sign_b(value) | _flag_parity(value)
        | _flag_undef(r->F) | _flag_zero(value);
}

static void cpu_execute_rot_acc(int y)
{
    eZ80registers_t *r = &cpu.registers;
    uint8_t old;
    switch (y) {
        case 0: // RLCA
            cpu.cycles += 1;
            old = (r->A & 0x80) > 0;
            r->flags.C = old;
            r->A <<= 1;
            r->A |= old;
            r->flags.N = r->flags.H = 0;
            break;
        case 1: // RRCA
            cpu.cycles += 1;
            old = (r->A & 1) > 0;
            r->flags.C = old;
            r->A >>= 1;
            r->A |= old << 7;
            r->flags.N = r->flags.H = 0;
            break;
        case 2: // RLA
            cpu.cycles += 1;
            old = r->flags.C;
            r->flags.C = (r->A & 0x80) > 0;
            r->A <<= 1;
            r->A |= old;
            r->flags.N = r->flags.H = 0;
            break;
        case 3: // RRA
            cpu.cycles += 1;
            old = r->flags.C;
            r->flags.C = (r->A & 1) > 0;
            r->A >>= 1;
            r->A |= old << 7;
            r->flags.N = r->flags.H = 0;
            break;
        case 4: // DAA
            cpu.cycles += 1;
            old = r->A;
            cpu_execute_daa();
            break;
        case 5: // CPL
            cpu.cycles += 1;
            r->A = ~r->A;
            r->flags.N = r->flags.H = 1;
            break;
        case 6: // SCF
            cpu.cycles += 1;
            r->flags.C = 1;
            r->flags.N = r->flags.H = 0;
            break;
        case 7: // CCF
            cpu.cycles += 1;
            r->flags.H = r->flags.C;
            r->flags.C = !r->flags.C;
            r->flags.N = 0;
            break;
    }
}

static void cpu_execute_bli(int y, int z) {
    eZ80registers_t *r = &cpu.registers;
    uint8_t old = 0, new = 0;
    switch (y) {
        case 0:
            switch (z) {
                case 2: // INIM
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->C));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    break;
                case 3: // OTIM
                    cpu.cycles += 1;
                    cpu_write_out(r->C, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    break;
                case 4: // INI2
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
            }
            break;
        case 1:
            switch (z) {
                case 2: // INDM
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->C));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    break;
                case 3: // OTDM
                    cpu.cycles += 1;
                    cpu_write_out(r->C, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    break;
                case 4: // IND2
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
            }
            break;
        case 2:
            switch (z) {
                case 2: // INIMR
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->C));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 3: // OTIMR
                    cpu.cycles += 1;
                    cpu_write_out(r->C, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 4: // INI2R
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->DE));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->DE++; mask_mode(r->DE, cpu.L);
                    old = cpu_dec_bc_partial_mode(); // Do not mask BC
                    r->flags.Z = _flag_zero(old) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (old) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
            }
            break;
        case 3:
            switch (z) {
                case 2: // INDMR
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->C));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 3: // OTDMR
                    cpu.cycles += 1;
                    cpu_write_out(r->C, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    old = r->B;
                    r->B--;
                    r->F = _flag_sign_b(r->B) | _flag_zero(r->B)
                        | _flag_halfcarry_b_sub(old, 0, 1)
                        | _flag_subtract(_flag_sign_b(new)) | _flag_undef(r->F);
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 4: // IND2R
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->DE));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->DE--; mask_mode(r->DE, cpu.L);
                    old = cpu_dec_bc_partial_mode(); // Do not mask BC
                    r->flags.Z = _flag_zero(old) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (old) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
            }
            break;
        case 4:
            switch (z) {
                case 0: // LDI
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    r->HL++; mask_mode(r->HL, cpu.L);
                    cpu_write_byte(r->DE, old);
                    r->DE++; mask_mode(r->DE, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A + old;
                    r->flags.PV = r->BC != 0;
                    r->flags.N = 0;
                    break;
                case 1: // CPI
                    old = cpu_read_byte(r->HL);
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A - old;
                    r->F = _flag_sign_b(new) | _flag_zero(new)
                        | _flag_halfcarry_b_sub(r->A, old, 0) | __flag_pv(r->BC)
                        | _flag_subtract(1) | __flag_c(r->flags.C)
                        | _flag_undef(r->F);
                    break;
                case 2: // INI
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
                case 3: // OUTI
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
                case 4: // OUTI2
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->C++;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
            }
            break;
        case 5:
            switch (z) {
                case 0: // LDD
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    r->HL--; mask_mode(r->HL, cpu.L);
                    cpu_write_byte(r->DE, old);
                    r->DE--; mask_mode(r->DE, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A + old;
                    r->flags.PV = r->BC != 0;
                    r->flags.N = 0;
                    break;
                case 1: // CPD
                    old = cpu_read_byte(r->HL);
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A - old;
                    r->F = _flag_sign_b(new) | _flag_zero(new)
                        | _flag_halfcarry_b_sub(r->A, old, 0)
                        | __flag_pv(r->BC)
                        | _flag_subtract(1) | __flag_c(r->flags.C)
                        | _flag_undef(r->F);
                    break;
                case 2: // IND
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
                case 3: // OUTD
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
                case 4: // OUTD2
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->C--;
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    break;
            }
            break;
        case 6:
            switch (z) {
                case 0: // LDIR
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    cpu_write_byte(r->DE, old);
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->DE++; mask_mode(r->DE, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A + old;
                    r->flags.PV = r->BC != 0;
                    r->flags.N = 0;
                    if (r->BC) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 1: // CPIR
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A - old;
                    r->F = _flag_sign_b(new) | _flag_zero(new)
                        | _flag_halfcarry_b_sub(r->A, old, 0) | __flag_pv(r->BC)
                        | _flag_subtract(1) | __flag_c(r->flags.C)
                        | _flag_undef(r->F);
                    if (r->BC && !r->flags.Z) {
                        cpu.cycles += 1;
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 2: // INIR
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 3: // OTIR
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 4: // OTI2R
                    cpu.cycles += 1;
                    cpu_write_out(r->DE, new = cpu_read_byte(r->HL));
                    r->HL++; mask_mode(r->HL, cpu.L);
                    r->DE++; mask_mode(r->DE, cpu.L);
                    old = cpu_dec_bc_partial_mode(); // Do not mask BC
                    r->flags.Z = _flag_zero(old) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (old) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
            }
            break;
        case 7:
            switch (z) {
                case 0: // LDDR
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    r->HL--; mask_mode(r->HL, cpu.L);
                    cpu_write_byte(r->DE, old);
                    r->DE--; mask_mode(r->DE, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A + old;
                    r->flags.PV = r->BC != 0;
                    r->flags.N = 0;
                    if (r->BC) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 1: // CPDR
                    cpu.cycles += 1;
                    old = cpu_read_byte(r->HL);
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->BC--; mask_mode(r->BC, cpu.L);
                    new = r->A - old;
                    r->F = _flag_sign_b(new) | _flag_zero(new)
                        | _flag_halfcarry_b_sub(r->A, old, 0) | __flag_pv(r->BC)
                        | _flag_subtract(1) | __flag_c(r->flags.C)
                        | _flag_undef(r->F);
                    if (r->BC && !r->flags.Z) {
                        cpu.cycles += 1;
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 2: // INDR
                    cpu.cycles += 1;
                    cpu_write_byte(r->HL, new = cpu_read_in(r->BC));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 3: // OTDR
                    cpu.cycles += 1;
                    cpu_write_out(r->BC, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->B--;
                    r->flags.Z = _flag_zero(r->B) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (r->B) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
                case 4: // OTD2R
                    cpu.cycles += 1;
                    cpu_write_out(r->DE, new = cpu_read_byte(r->HL));
                    r->HL--; mask_mode(r->HL, cpu.L);
                    r->DE--; mask_mode(r->DE, cpu.L);
                    old = cpu_dec_bc_partial_mode(); // Do not mask BC
                    r->flags.Z = _flag_zero(old) != 0;
                    r->flags.N = _flag_sign_b(new) != 0;
                    if (old) {
                        cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                    }
                    break;
            }
            break;
    }
}

void cpu_init(void) {
    memset(&cpu, 0, sizeof cpu);
    gui_console_printf("Initialized CPU...\n");
}

void cpu_reset(void) {
    memset(&cpu.registers, 0, sizeof cpu.registers);
    cpu.IEF1 = cpu.IEF2 = cpu.ADL = cpu.MADL = cpu.IM = cpu.IEF_wait = cpu.halted = 0;
    cpu_flush(0, 0);
}

void cpu_flush(uint32_t address, bool mode) {
    cpu_prefetch(address, mode);
    cpu_get_cntrl_data_blocks_format();
}

void cpu_execute(void) {
    // variable declaration
    int8_t s;
    uint32_t w;

    uint8_t old = 0;
    uint32_t old_word;

    uint8_t new = 0;
    uint32_t new_word;

    uint32_t op_word;

    eZ80registers_t *r = &cpu.registers;
    union {
        uint8_t opcode;
        struct {
            uint8_t z : 3;
            uint8_t y : 3;
            uint8_t x : 2;
        };
        struct {
            uint8_t r : 1;
            uint8_t   : 2;
            uint8_t q : 1;
            uint8_t p : 2;
        };
    } context;

    int cycle_offset;

    while (!exiting && cycle_count_delta < 0) {
        cycle_offset = 0;
        if (cpu.IEF_wait) {
            cpu.IEF_wait = 0;
            cpu.IEF1 = cpu.IEF2 = 1;
        }
        if (cpu.IEF1 && (intrpt.request->status & intrpt.request->enabled)) {
            cpu.IEF1 = cpu.IEF2 = cpu.halted = 0;
            cycle_count_delta++;
            if (cpu.IM != 3) {
                cpu_call(0x38, cpu.MADL);
            } else {
                cycle_count_delta++;
                cpu_call(cpu_read_word(r->I << 8 | ~r->R), cpu.MADL);
                cycle_count_delta += cpu.cycles;
            }
        } else if (cpu.halted) {
            cycle_count_delta = 0; // consume all of the cycles
        }

        while (!exiting && (cpu.PREFIX || cpu.SUFFIX || cycle_count_delta < 0)) {
            cpu.cycles = 0;

            // fetch opcode
            context.opcode = cpu_fetch_byte();

            r->R = ((r->R + 1) & 0x7F) | (r->R & 0x80);

            switch (context.x) {
                case 0:
                    switch (context.z) {
                        case 0:
                            switch (context.y) {
                                case 0:  // NOP
                                    cpu.cycles += 1;
                                    break;
                                case 1:  // EX af,af'
                                    cpu.cycles += 1;
                                    rswap(r->AF, r->_AF);
                                    break;
                                case 2: // DJNZ d
                                    cpu.cycles += 1;
                                    s = cpu_fetch_offset();
                                    if (--r->B) {
                                        cpu.cycles += 1;
                                        cpu_prefetch(cpu_mask_mode(r->PC + s, cpu.L), cpu.ADL);
                                    }
                                    break;
                                case 3: // JR d
                                    cpu.cycles += 2;
                                    s = cpu_fetch_offset();
                                    cpu_prefetch(cpu_mask_mode(r->PC + s, cpu.L), cpu.ADL);
                                    break;
                                case 4:
                                case 5:
                                case 6:
                                case 7: // JR cc[y-4], d
                                    cpu.cycles += 1;
                                    s = cpu_fetch_offset();
                                    if (cpu_read_cc(context.y - 4)) {
                                        cpu.cycles += 1;
                                        cpu_prefetch(cpu_mask_mode(r->PC + s, cpu.L), cpu.ADL);
                                    }
                                    break;
                            }
                            break;
                        case 1:
                            switch (context.q) {
                                case 0: // LD rr, Mmn
                                    if (context.p == 3 && cpu.PREFIX) { // LD IY/IX, (IX/IY + d)
                                        cpu.cycles += 6;
                                        cpu_write_other_index(cpu_read_word(cpu_index_address()));
                                        break;
                                    }
                                    cpu.cycles += 4;
                                    cpu_write_rp(context.p, cpu_fetch_word());
                                    break;
                                case 1: // ADD HL,rr
                                    cpu.cycles += 1;
                                    old_word = cpu_mask_mode(cpu_read_index(), cpu.L);
                                    op_word = cpu_mask_mode(cpu_read_rp(context.p), cpu.L);
                                    new_word = old_word + op_word;
                                    cpu_write_index(cpu_mask_mode(new_word, cpu.L));
                                    r->F = __flag_s(r->flags.S) | _flag_zero(!r->flags.Z)
                                        | _flag_undef(r->F) | __flag_pv(r->flags.PV)
                                        | _flag_subtract(0) | _flag_carry_w(new_word, cpu.L)
                                        | _flag_halfcarry_w_add(old_word, op_word, 0);
                                    break;
                            }
                            break;
                        case 2:
                            switch (context.q) {
                                case 0:
                                    switch (context.p) {
                                        case 0: // LD (BC), A
                                            cpu.cycles += 2;
                                            cpu_write_byte(r->BC, r->A);
                                            break;
                                        case 1: // LD (DE), A
                                            cpu.cycles += 2;
                                            cpu_write_byte(r->DE, r->A);
                                            break;
                                        case 2: // LD (Mmn), HL
                                            cpu.cycles += 7;
                                            cpu_write_word(cpu_fetch_word(), cpu_read_index());
                                            break;
                                        case 3: // LD (Mmn), A
                                            cpu.cycles += 5;
                                            cpu_write_byte(cpu_fetch_word(), r->A);
                                            break;
                                    }
                                    break;
                                case 1:
                                    switch (context.p) {
                                        case 0: // LD A, (BC)
                                            cpu.cycles += 2;
                                            r->A = cpu_read_byte(r->BC);
                                            break;
                                        case 1: // LD A, (DE)
                                            cpu.cycles += 2;
                                            r->A = cpu_read_byte(r->DE);
                                            break;
                                        case 2: // LD HL, (Mmn)
                                            cpu.cycles += 7;
                                            cpu_write_index(cpu_read_word(cpu_fetch_word()));
                                            break;
                                        case 3: // LD A, (Mmn)
                                            cpu.cycles += 5;
                                            r->A = cpu_read_byte(cpu_fetch_word());
                                            break;
                                    }
                                    break;
                            }
                            break;
                        case 3:
                            switch (context.q) {
                                case 0: // INC rp[p]
                                    cpu.cycles += 1;
                                    cpu_write_rp(context.p, cpu_read_rp(context.p) + 1);
                                    break;
                                case 1: // DEC rp[p]
                                    cpu.cycles += 1;
                                    cpu_write_rp(context.p, cpu_read_rp(context.p) - 1);
                                    break;
                            }
                            break;
                        case 4: // INC r[y]
                            cpu.cycles += 1;
                            w = (context.y == 6) ? cpu_index_address() : 0;
                            old = cpu_read_reg_prefetched(context.y, w);
                            new = old + 1;
                            cpu_write_reg_prefetched(context.y, w, new);
                            r->F = __flag_c(r->flags.C) | _flag_sign_b(new) | _flag_zero(new)
                                | _flag_halfcarry_b_add(old, 0, 1) | __flag_pv(new == 0x80)
                                | _flag_subtract(0) | _flag_undef(r->F);
                            break;
                        case 5: // DEC r[y]
                            cpu.cycles += 1;
                            w = (context.y == 6) ? cpu_index_address() : 0;
                            old = cpu_read_reg_prefetched(context.y, w);
                            new = old - 1;
                            cpu_write_reg_prefetched(context.y, w, new);
                            r->F = __flag_c(r->flags.C) | _flag_sign_b(new) | _flag_zero(new)
                                | _flag_halfcarry_b_sub(old, 0, 1) | __flag_pv(old == 0x80)
                                | _flag_subtract(1) | _flag_undef(r->F);
                            break;
                        case 6: // LD r[y], n
                            cpu.cycles += 2;
                            if (context.y == 7 && cpu.PREFIX) { // LD (IX/IY + d), IY/IX
                                cpu_write_word(cpu_index_address(), cpu_read_other_index());
                                break;
                            }
                            w = (context.y == 6) ? cpu_index_address() : 0;
                            cpu_write_reg_prefetched(context.y, w, cpu_fetch_byte());
                            break;
                        case 7:
                            if (cpu.PREFIX) {
                                cpu.cycles += 6;
                                if (context.q) { // LD (IX/IY + d), rp3[p]
                                    cpu_write_word(cpu_index_address(), cpu_read_rp3(context.p));
                                } else { // LD rp3[p], (IX/IY + d)
                                    cpu_write_rp3(context.p, cpu_read_word(cpu_index_address()));
                                }
                            } else {
                                cpu_execute_rot_acc(context.y);
                            }
                            break;
                    }
                    break;
                case 1: // ignore prefixed prefixes
                    if (context.z == context.y) {
                        switch (context.z) {
                            case 0: // .SIS
                                cpu.cycles += 1;
                                cpu.SUFFIX = 1;
                                cpu.L = 0; cpu.IL = 0;
                                goto exit_loop;
                            case 1: // .LIS
                                cpu.cycles += 1;
                                cpu.SUFFIX = 1;
                                cpu.L = 1; cpu.IL = 0;
                                goto exit_loop;
                            case 2: // .SIL
                                cpu.cycles += 1;
                                cpu.SUFFIX = 1;
                                cpu.L = 0; cpu.IL = 1;
                                goto exit_loop;
                            case 3: // .LIL
                                cpu.cycles += 1;
                                cpu.SUFFIX = 1;
                                cpu.L = 1; cpu.IL = 1;
                                goto exit_loop;
                            case 6: // HALT
                                cpu.halted = 1;
                                if (cycle_count_delta + cpu.cycles < 0) {
                                    cpu.cycles = -cycle_count_delta;
                                }
                                break;
                            case 4: // LD H, H
                            case 5: // LD L, L
                            case 7: // LD A, A
                                break;
                            default:
                                abort();
                        }
                    } else {
                        cpu_read_write_reg(context.z, context.y);
                    }
                    break;
                case 2: // ALU[y] r[z]
                    cpu_execute_alu(context.y, cpu_read_reg(context.z));
                    break;
                case 3:
                    switch (context.z) {
                        case 0: // RET cc[y]
                            cpu.cycles += 2;
                            if (cpu_read_cc(context.y)) {
                                cpu.cycles += 5;
                                cpu_return();
                            }
                            break;
                        case 1:
                            switch (context.q) {
                                case 0: // POP rp2[p]
                                    cpu.cycles += 4;
                                    cpu_write_rp2(context.p, cpu_pop_word());
                                    break;
                                case 1:
                                    switch (context.p) {
                                        case 0: // RET
                                            cpu.cycles += 7;
                                            cpu_return();
                                            break;
                                        case 1: // EXX
                                            cpu.cycles += 1;
                                            exx(&cpu.registers);
                                            break;
                                        case 2: // JP (rr)
                                            cpu.cycles += 3;
                                            cpu_prefetch(cpu_read_index(), cpu.L);
                                            break;
                                        case 3: // LD SP, HL
                                            cpu.cycles += 1;
                                            cpu_write_sp(cpu_read_index());
                                            break;
                                    }
                                    break;
                            }
                            break;
                        case 2: // JP cc[y], nn
                            if (cpu_read_cc(context.y)) {
                                cpu.cycles += 5;
                                cpu_prefetch(cpu_fetch_word_no_prefetch(), cpu.L);
                            } else {
                                cpu.cycles += 4;
                                cpu_fetch_word();
                            }
                            break;
                        case 3:
                            switch (context.y) {
                                case 0: // JP nn
                                    cpu.cycles += 5;
                                    cpu_prefetch(cpu_fetch_word_no_prefetch(), cpu.L);
                                    break;
                                case 1: // 0xCB prefixed opcodes
                                    w = cpu_index_address();
                                    context.opcode = cpu_fetch_byte();
                                    old = cpu_read_reg_prefetched(context.z, w);
                                    switch (context.x) {
                                        case 0: // rot[y] r[z]
                                            cpu_execute_rot(context.y, context.z, w, old);
                                            break;
                                        case 1: // BIT y, r[z]
                                            cpu.cycles += 2;
                                            old &= (1 << context.y);
                                            r->F = _flag_sign_b(old) | _flag_zero(old) | _flag_undef(r->F)
                                               | _flag_parity(old) | __flag_c(r->flags.C)
                                               | FLAG_H;
                                            break;
                                        case 2: // RES y, r[z]
                                            cpu.cycles += 2;
                                            old &= ~(1 << context.y);
                                            cpu_write_reg_prefetched(context.z, w, old);
                                            break;
                                        case 3: // SET y, r[z]
                                            cpu.cycles += 2;
                                            old |= 1 << context.y;
                                            cpu_write_reg_prefetched(context.z, w, old);
                                            break;
                                    }
                                    break;
                                case 2: // OUT (n), A
                                    cpu.cycles += 3;
                                    cpu_write_out((r->A << 8) | cpu_fetch_byte(), r->A);
                                    break;
                                case 3: // IN A, (n)
                                    cpu.cycles += 3;
                                    r->A = cpu_read_in((r->A << 8) | cpu_fetch_byte());
                                    break;
                                case 4: // EX (SP), HL/I
                                    cpu.cycles += 7;
                                    w = cpu_read_sp();
                                    old_word = cpu_read_word(w);
                                    new_word = cpu_read_index();
                                    cpu_write_index(old_word);
                                    cpu_write_word(w, new_word);
                                    break;
                                case 5: // EX DE, HL
                                    cpu.cycles += 1;
                                    rswap(r->HL, r->DE);
                                    break;
                                case 6: // DI
                                    cpu.cycles += 1;
                                    cpu.IEF1 = cpu.IEF2 = 0;
                                    break;
                                case 7: // EI
                                    cpu.IEF_wait = 1;
                                    cycle_count_delta += cpu.cycles;
                                    cycle_offset = cycle_count_delta + 1;
                                    cycle_count_delta = -1; // execute one more instruction
                                    continue;
                            }
                            break;
                        case 4: // CALL cc[y], nn
                            if (cpu_read_cc(context.y)) {
                                cpu.cycles += 7;
                                cpu_call(cpu_fetch_word_no_prefetch(), cpu.SUFFIX);
                            } else {
                                cpu.cycles += 4;
                                cpu_fetch_word();
                            }
                            break;
                        case 5:
                            switch (context.q) {
                                case 0: // PUSH r2p[p]
                                    cpu.cycles += 4;
                                    cpu_push_word(cpu_read_rp2(context.p));
                                    break;
                                case 1:
                                    switch (context.p) {
                                        case 0: // CALL nn
                                            cpu.cycles += 7;
                                            cpu_call(cpu_fetch_word_no_prefetch(), cpu.SUFFIX);
                                            break;
                                        case 1: // 0xDD prefixed opcodes
                                            cpu.cycles += 1;
                                            cpu.PREFIX = 2;
                                            goto exit_loop;
                                        case 2: // 0xED prefixed opcodes
                                            cpu.cycles += 1;
                                            cpu.PREFIX = 0; // ED cancels effect of DD/FD prefix
                                            context.opcode = cpu_fetch_byte();
                                            switch (context.x) {
                                                case 0:
                                                    switch (context.z) {
                                                        case 0:
                                                            if (context.y == 6) { // OPCODETRAP
                                                                cpu.IEF_wait = 1;
                                                            } else { // IN0 r[y], (n)
                                                                cpu.cycles += 2;
                                                                cpu_write_reg(context.y, new = cpu_read_in(cpu_fetch_byte()));
                                                                r->F = _flag_sign_b(new) | _flag_zero(new)
                                                                    | _flag_undef(r->F) | _flag_parity(new)
                                                                    | __flag_c(r->flags.C);
                                                            }
                                                            break;
                                                         case 1:
                                                            if (context.y == 6) { // LD IY, (HL)
                                                                cpu.cycles += 5;
                                                                r->IY = cpu_read_word(r->HL);
                                                            } else { // OUT0 (n), r[y]
                                                                cpu.cycles += 2;
                                                                cpu_write_out(cpu_fetch_byte(), cpu_read_reg(context.y));
                                                            }
                                                            break;
                                                        case 2: // LEA rp3[p], IX
                                                        case 3: // LEA rp3[p], IY
                                                            if (context.q) { // OPCODETRAP
                                                                cpu.IEF_wait = 1;
                                                            } else {
                                                                cpu.cycles += 3;
                                                                cpu.PREFIX = context.z;
                                                                cpu_write_rp3(context.p, cpu_index_address());
                                                            }
                                                            break;
                                                        case 4: // TST A, r[y]
                                                            cpu.cycles += 2;
                                                            new = r->A & cpu_read_reg(context.y);
                                                            r->F = _flag_sign_b(new) | _flag_zero(new)
                                                                | _flag_undef(r->F) | _flag_parity(new)
                                                                | FLAG_H;
                                                            break;
                                                        case 6:
                                                            if (context.y == 7) { // LD (HL), IY
                                                                cpu.cycles += 5;
                                                                cpu_write_word(r->HL, r->IY);
                                                                break;
                                                            }
                                                        case 5: // OPCODETRAP
                                                            cpu.IEF_wait = 1;
                                                            break;
                                                        case 7:
                                                            cpu.PREFIX = 2;
                                                            if (context.q) { // LD (HL), rp3[p]
                                                                cpu.cycles += 5;
                                                                cpu_write_word(r->HL, cpu_read_rp3(context.p));
                                                            } else { // LD rp3[p], (HL)
                                                                cpu.cycles += 5;
                                                                cpu_write_rp3(context.p, cpu_read_word(r->HL));
                                                            }
                                                            break;
                                                    }
                                                    break;
                                                case 1:
                                                    switch (context.z) {
                                                        case 0:
                                                            if (context.y == 6) { // OPCODETRAP (ADL)
                                                                cpu.IEF_wait = 1;
                                                            } else { // IN r[y], (BC)
                                                                cpu.cycles += 3;
                                                                cpu_write_reg(context.y, new = cpu_read_in(r->BC));
                                                                r->F = _flag_sign_b(new) | _flag_zero(new)
                                                                    | _flag_undef(r->F) | _flag_parity(new)
                                                                    | __flag_c(r->flags.C);
                                                            }
                                                            break;
                                                        case 1:
                                                            if (context.y == 6) { // OPCODETRAP (ADL)
                                                                cpu.IEF_wait = 1;
                                                            } else { // OUT (BC), r[y]
                                                                cpu.cycles += 3;
                                                                cpu_write_out(r->BC, cpu_read_reg(context.y));
                                                            }
                                                            break;
                                                        case 2:
                                                            old_word = cpu_mask_mode(r->HL, cpu.L);
                                                            op_word = cpu_mask_mode(cpu_read_rp(context.p), cpu.L);
                                                            if (context.q == 0) { // SBC HL, rp[p]
                                                                cpu.cycles += 2;
                                                                r->HL = cpu_mask_mode(old_word - op_word - r->flags.C, cpu.L);
                                                                r->F = _flag_sign_w(r->HL, cpu.L) | _flag_zero(r->HL)
                                                                    | _flag_undef(r->F) | _flag_overflow_w_sub(old_word, op_word, r->HL, cpu.L)
                                                                    | _flag_subtract(1) | _flag_carry_w(old_word - op_word - r->flags.C, cpu.L)
                                                                    | _flag_halfcarry_w_sub(old_word, op_word, r->flags.C);
                                                            } else { // ADC HL, rp[p]
                                                                cpu.cycles += 2;
                                                                r->HL = cpu_mask_mode(old_word + op_word + r->flags.C, cpu.L);
                                                                r->F = _flag_sign_w(r->HL, cpu.L) | _flag_zero(r->HL)
                                                                    | _flag_undef(r->F) | _flag_overflow_w_add(old_word, op_word, r->HL, cpu.L)
                                                                    | _flag_subtract(0) | _flag_carry_w(old_word + op_word + r->flags.C, cpu.L)
                                                                    | _flag_halfcarry_w_add(old_word, op_word, r->flags.C);
                                                            }
                                                            break;
                                                        case 3:
                                                            if (context.q == 0) { // LD (nn), rp[p]
                                                                cpu.cycles += 8;
                                                                cpu_write_word(cpu_fetch_word(), cpu_read_rp(context.p));
                                                            } else { // LD rp[p], (nn)
                                                                cpu.cycles += 8;
                                                                cpu_write_rp(context.p, cpu_read_word(cpu_fetch_word()));
                                                            }
                                                            break;
                                                        case 4:
                                                            if (context.q == 0) {
                                                                switch (context.p) {
                                                                    case 0:  // NEG
                                                                        cpu.cycles += 2;
                                                                        old = r->A;
                                                                        r->A = -r->A;
                                                                        r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                                                                            | _flag_undef(r->F) | __flag_pv(old == 0x80)
                                                                            | _flag_subtract(1) | __flag_c(old != 0)
                                                                            | _flag_halfcarry_b_sub(0, old, 0);
                                                                        break;
                                                                    case 1:  // LEA IX, IY + d
                                                                        cpu.cycles += 3;
                                                                        cpu.PREFIX = 3;
                                                                        r->IX = cpu_index_address();
                                                                        break;
                                                                    case 2:  // TST A, n
                                                                        cpu.cycles += 2;
                                                                        new = r->A & cpu_fetch_byte();
                                                                        r->F = _flag_sign_b(new) | _flag_zero(new)
                                                                            | _flag_undef(r->F) | _flag_parity(new)
                                                                            | FLAG_H;
                                                                        break;
                                                                    case 3:  // TSTIO n
                                                                        cpu.cycles += 2;
                                                                        new = cpu_read_in(r->C) & cpu_fetch_byte();
                                                                        r->F = _flag_sign_b(new) | _flag_zero(new)
                                                                            | _flag_undef(r->F) | _flag_parity(new)
                                                                            | FLAG_H;
                                                                        break;
                                                                }
                                                            }
                                                            else { // MLT rp[p]
                                                                cpu.cycles += 4;
                                                                old_word = cpu_read_rp(context.p);
                                                                new_word = (old_word&0xFF) * ((old_word>>8)&0xFF);
                                                                cpu_write_rp(context.p, new_word);
                                                                break;
                                                            }
                                                            break;
                                                        case 5:
                                                            switch (context.y) {
                                                                case 0: // RETN
                                                                    // This is actually identical to reti on the z80
                                                                case 1: // RETI
                                                                    cpu.cycles += 7;
                                                                    cpu.IEF1 = cpu.IEF2;
                                                                    cpu_return();
                                                                    break;
                                                                case 2: // LEA IY, IX + d
                                                                    cpu.cycles += 3;
                                                                    cpu.PREFIX = 2;
                                                                    r->IY = cpu_index_address();
                                                                    break;
                                                                case 3:
                                                                case 6: // OPCODETRAP
                                                                    cpu.IEF_wait = 1;
                                                                    break;
                                                                case 4: // PEA IX + d
                                                                    cpu.cycles += 6;
                                                                    cpu_push_word(r->IX + cpu_fetch_offset());
                                                                    break;
                                                                case 5: // LD MB, A
                                                                    cpu.cycles += 2;
                                                                    if (cpu.ADL) {
                                                                        r->MBASE = r->A;
                                                                    }
                                                                    break;
                                                                case 7: // STMIX
                                                                    cpu.cycles += 2;
                                                                    cpu.MADL = 1;
                                                                    break;
                                                            }
                                                            break;
                                                        case 6: // IM im[y]
                                                            switch (context.y) {
                                                                case 0:
                                                                case 2:
                                                                case 3: // IM im[y]
                                                                    cpu.cycles += 2;
                                                                    cpu.IM = context.y;
                                                                    break;
                                                                case 1: // OPCODETRAP
                                                                    cpu.IEF_wait = 1;
                                                                    break;
                                                                case 4: // PEA IY + d
                                                                    cpu.cycles += 6;
                                                                    cpu_push_word(r->IY + cpu_fetch_offset());
                                                                    break;
                                                                case 5: // LD A, MB
                                                                    cpu.cycles += 2;
                                                                    r->A = r->MBASE;
                                                                    break;
                                                                case 6: // SLP -- NOT IMPLEMENTED
                                                                    cpu.cycles += 1;
                                                                    break;
                                                                case 7: // RSMIX
                                                                    cpu.cycles += 2;
                                                                    cpu.MADL = 0;
                                                                    break;
                                                            }
                                                            break;
                                                        case 7:
                                                            switch (context.y) {
                                                                case 0: // LD I, A
                                                                    cpu.cycles += 2;
                                                                    r->I = r->A | (r->I & 0xF0);
                                                                    break;
                                                                case 1: // LD R, A
                                                                    cpu.cycles += 2;
                                                                    r->R = r->A;
                                                                    break;
                                                                case 2: // LD A, I
                                                                    cpu.cycles += 2;
                                                                    r->A = r->I & 0x0F;
                                                                    r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                                                                        | _flag_undef(r->F) | __flag_pv(cpu.IEF1)
                                                                        | _flag_subtract(0) | __flag_c(r->flags.C);
                                                                    break;
                                                                case 3: // LD A, R
                                                                    cpu.cycles += 2;
                                                                    r->A = r->R;
                                                                    r->F = _flag_sign_b(r->A) | _flag_zero(r->A)
                                                                        | _flag_undef(r->F) | __flag_pv(cpu.IEF1)
                                                                        | _flag_subtract(0) | __flag_c(r->flags.C);
                                                                    break;
                                                                case 4: // RRD
                                                                    cpu.cycles += 5;
                                                                    old = r->A;
                                                                    new = cpu_read_byte(r->HL);
                                                                    r->A &= 0xF0;
                                                                    r->A |= new & 0x0F;
                                                                    new >>= 4;
                                                                    new |= old << 4;
                                                                    cpu_write_byte(r->HL, new);
                                                                    r->F = __flag_c(r->flags.C) | _flag_sign_b(r->A) | _flag_zero(r->A)
                                                                        | _flag_parity(r->A) | _flag_undef(r->F);
                                                                    break;
                                                                case 5: // RLD
                                                                    cpu.cycles += 5;
                                                                    old = r->A;
                                                                    new = cpu_read_byte(r->HL);
                                                                    r->A &= 0xF0;
                                                                    r->A |= new >> 4;
                                                                    new <<= 4;
                                                                    new |= old & 0x0F;
                                                                    cpu_write_byte(r->HL, new);
                                                                    r->F = __flag_c(r->flags.C) | _flag_sign_b(r->A) | _flag_zero(r->A)
                                                                        | _flag_parity(r->A) | _flag_undef(r->F);
                                                                    break;
                                                                default: // OPCODETRAP
                                                                    cpu.IEF_wait = 1;
                                                                    break;
                                                            }
                                                            break;
                                                    }
                                                    break;
                                                case 2:
                                                    if (context.y >= 0 && context.z <= 4) { // bli[y,z]
                                                        cpu_execute_bli(context.y, context.z);
                                                    } else { // OPCODETRAP
                                                        cpu.IEF_wait = 1;
                                                    }
                                                    break;
                                                case 3:  // There are only a few of these, so a simple switch for these shouldn't matter too much
                                                    switch(context.opcode) {
                                                        case 0xC2: // INIRX
                                                            cpu.cycles += 1;
                                                            cpu_write_byte(r->HL, new = cpu_read_in(r->DE));
                                                            r->HL++; mask_mode(r->HL, cpu.L);
                                                            old = cpu_dec_bc_partial_mode(); // Do not mask BC
                                                            r->flags.Z = _flag_zero(old) != 0;
                                                            r->flags.N = _flag_sign_b(new) != 0;
                                                            if (old) {
                                                                cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                                                            }
                                                            break;
                                                        case 0xC3: // OTIRX
                                                            cpu.cycles += 1;
                                                            cpu_write_out(r->DE, new = cpu_read_byte(r->HL));
                                                            r->HL++; mask_mode(r->HL, cpu.L);
                                                            old = cpu_dec_bc_partial_mode(); // Do not mask BC
                                                            r->flags.Z = _flag_zero(old) != 0;
                                                            r->flags.N = _flag_sign_b(new) != 0;
                                                            if (old) {
                                                                cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                                                            }
                                                            break;
                                                        case 0xC7: // LD I, HL
                                                            cpu.cycles += 2;
                                                            r->I = r->HL & 0xFFFF;
                                                            break;
                                                        case 0xD7: // LD HL, I
                                                            cpu.cycles += 2;
                                                            r->HL = r->I | (r->MBASE << 16);
                                                            break;
                                                        case 0xCA: // INDRX
                                                            cpu.cycles += 1;
                                                            cpu_write_byte(r->HL, new = cpu_read_in(r->DE));
                                                            r->HL--; mask_mode(r->HL, cpu.L);
                                                            old = cpu_dec_bc_partial_mode(); // Do not mask BC
                                                            r->flags.Z = _flag_zero(old) != 0;
                                                            r->flags.N = _flag_sign_b(new) != 0;
                                                            if (old) {
                                                                cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                                                            }
                                                            break;
                                                        case 0xCB: // OTDRX
                                                            cpu.cycles += 1;
                                                            cpu_write_out(r->DE, new = cpu_read_byte(r->HL));
                                                            r->HL--; mask_mode(r->HL, cpu.L);
                                                            old = cpu_dec_bc_partial_mode(); // Do not mask BC
                                                            r->flags.Z = _flag_zero(old) != 0;
                                                            r->flags.N = _flag_sign_b(new) != 0;
                                                            if (old) {
                                                                cpu_prefetch(r->PC - 2 - cpu.SUFFIX, cpu.ADL);
                                                            }
                                                            break;
                                                        case 0xEE: // flash erase
                                                            memset(mem.flash.block + (r->HL & ~0x3FFF), 0xFF, 0x4000);
                                                            break;
                                                        default:   // OPCODETRAP
                                                            cpu.IEF_wait = 1;
                                                            break;
                                                    }
                                                    break;
                                                default: // OPCODETRAP
                                                    cpu.IEF_wait = 1;
                                                    break;
                                            }
                                            break;
                                        case 3: // 0xFD prefixed opcodes
                                            cpu.cycles += 1;
                                            cpu.PREFIX = 3;
                                            goto exit_loop;
                                    }
                                    break;
                            }
                            break;
                        case 6: // alu[y] n
                            cpu_execute_alu(context.y, cpu_fetch_byte());
                            break;
                        case 7: // RST y*8
                            cpu.cycles += 1;
                            cpu_call(context.y << 3, cpu.SUFFIX);
                            break;
                    }
                    break;
            }

            cpu_get_cntrl_data_blocks_format();

            if (cpu_events & EVENT_DEBUG_STEP) {
                // Flush the cycles
                cycle_count_delta = 0;
                break;
            }
exit_loop:
            cycle_count_delta += cpu.cycles;
            if (cpu.cycles == 0) {
                //logprintf(LOG_CPU, "Error: Unrecognized instruction 0x%02X.", context.opcode);
                cycle_count_delta++;
            }
        }
        cycle_count_delta += cycle_offset;
    }
}
