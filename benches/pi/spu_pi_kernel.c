#include <stdint.h>

static inline double dfloor_pos(double x)
{
    return (double)(int32_t)x;
}

static inline double dfloor_any(double x)
{
    double f = (double)(int32_t)x;
    return (f > x) ? (f - 1.0) : f;
}

static inline double dmod1_pos(double s)
{
    return s - dfloor_pos(s);
}

static double mod_pow16(uint32_t p, uint32_t m)
{
    double dm, inv_dm, r, b;

    if (m <= 1u) return 0.0;

    dm     = (double)m;
    inv_dm = 1.0 / dm;
    r      = 1.0;
    b      = 16.0;

    if (b >= dm) {
        b = b - dm * dfloor_pos(b * inv_dm);
    }

    while (p) {
        if (p & 1u) {
            double rb = r * b;
            r = rb - dm * dfloor_pos(rb * inv_dm);
        }
        {
            double bb = b * b;
            b = bb - dm * dfloor_pos(bb * inv_dm);
        }
        p >>= 1;
    }
    return r;
}

static double series_j(uint32_t n, uint32_t j)
{
    double   s = 0.0;
    uint32_t k;

    for (k = 0u; k <= n; k++) {
        uint32_t denom = 8u * k + j;
        double   r     = mod_pow16(n - k, denom);
        s += r / (double)denom;
        s  = dmod1_pos(s);
    }

    {
        double pw = 1.0 / 16.0;
        for (k = n + 1u; k < n + 26u; k++) {
            uint32_t denom = 8u * k + j;
            s += pw / (double)denom;
            pw *= (1.0 / 16.0);
        }
    }
    return dmod1_pos(s);
}

uint8_t pi_hex_digit(uint32_t n)
{
    double s1 = series_j(n, 1u);
    double s4 = series_j(n, 4u);
    double s5 = series_j(n, 5u);
    double s6 = series_j(n, 6u);
    double x  = 4.0 * s1 - 2.0 * s4 - s5 - s6;
    x = x - dfloor_any(x);
    if (x < 0.0) x += 1.0;
    return (uint8_t)(16.0 * x);
}