/* IEEE half <-> single/double conversions exported for llvmpipe's ORC JIT on riscv64. */
#include <stdint.h>
#include <string.h>

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    if (exp == 0x1Fu) return bits_to_float(sign | 0x7F800000u | (man << 13));
    if (exp == 0) {
        if (man == 0) return bits_to_float(sign);
        int e = -1;
        do { man <<= 1; ++e; } while (!(man & 0x400u));
        return bits_to_float(sign | ((uint32_t)(112 - e) << 23) | ((man & 0x3FFu) << 13));
    }
    return bits_to_float(sign | ((exp + 112u) << 23) | (man << 13));
}

static uint16_t float_to_half(float f) {
    uint32_t u = float_to_bits(f), sign = (u >> 16) & 0x8000u, man = u & 0x7FFFFFu;
    int32_t exp = (int32_t)((u >> 23) & 0xFFu);
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00u | (man ? 0x200u | (man >> 13) : 0u));
    exp -= 112;
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp), half = man >> shift, rem = man & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (half & 1u))) ++half;
        return (uint16_t)(sign | half);
    }
    uint32_t half = sign | ((uint32_t)exp << 10) | (man >> 13), rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;
    return (uint16_t)half;
}

static uint16_t h_bits(_Float16 h) { uint16_t b; memcpy(&b, &h, 2); return b; }
static _Float16 h_from(uint16_t b) { _Float16 h; memcpy(&h, &b, 2); return h; }

float __extendhfsf2(_Float16 h) { return half_to_float(h_bits(h)); }
double __extendhfdf2(_Float16 h) { return (double)half_to_float(h_bits(h)); }
_Float16 __truncsfhf2(float f) { return h_from(float_to_half(f)); }
_Float16 __truncdfhf2(double d) { return h_from(float_to_half((float)d)); }
