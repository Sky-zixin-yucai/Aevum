#include "lib/geom/line.h"
#include "lib/fixed.h"

i64 line2_len_sq(const struct line2 *l)
{
    i64 dx, dz;

    if (!l) return 0;
    dx = (i64)l->x1 - (i64)l->x0;
    dz = (i64)l->z1 - (i64)l->z0;
    return dx * dx + dz * dz;
}

i32 line2_length(const struct line2 *l)
{
    i64 len_sq;

    if (!l) return 0;
    len_sq = line2_len_sq(l);
    if (len_sq <= 0) return 0;
    return (i32)isqrt64((u64)len_sq);
}

int line2_point(const struct line2 *l, i32 t, i32 *x, i32 *z)
{
    i32 dx, dz;

    if (!l || !x || !z) return 0;
    if (t < 0 || t > FXONE) return 0;
    if (line2_len_sq(l) == 0) return 0;

    dx = l->x1 - l->x0;
    dz = l->z1 - l->z0;
    *x = l->x0 + fx_mul(t, dx);
    *z = l->z0 + fx_mul(t, dz);
    return 1;
}

int line2_project(const struct line2 *l, i32 px, i32 pz, i32 *t)
{
    i64 dx, dz, dot, den;
    i64 tq;

    if (!l || !t) return -1;
    dx = (i64)l->x1 - (i64)l->x0;
    dz = (i64)l->z1 - (i64)l->z0;
    den = dx * dx + dz * dz;
    if (den == 0) return -1;

    dot = dx * ((i64)px - (i64)l->x0) +
          dz * ((i64)pz - (i64)l->z0);
    tq = (dot * FXONE) / den;
    /* 整数除法向零截断：端点外侧的小量会被截成 0/FXONE，边界要补正。 */
    if (dot < 0 && tq >= 0) tq = -1;
    if (dot > den && tq <= FXONE) tq = FXONE + 1;
    *t = (i32)tq;
    return (tq >= 0 && tq <= FXONE) ? 1 : 0;
}
