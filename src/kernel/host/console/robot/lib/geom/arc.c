#include "lib/geom/arc.h"
#include "lib/fixed.h"
#include "lib/trig.h"

static int arc2_valid(const struct arc2 *a)
{
    return a != 0 && a->r > 0 && a->sweep != 0;
}

i32 arc2_length(const struct arc2 *a)
{
    i64 sweep;

    if (!arc2_valid(a)) return 0;

    sweep = a->sweep < 0 ? -(i64)a->sweep : (i64)a->sweep;
    return (i32)((sweep * (i64)a->r) >> FXBITS);
}

int arc2_point(const struct arc2 *a, i32 t, i32 *x, i32 *z)
{
    i32 ang;

    if (!arc2_valid(a) || t < 0 || t > FXONE) return 0;

    ang = a->a0 + fx_mul(a->sweep, t);
    if (x) *x = a->cx + fx_mul(a->r, fx_cos(ang));
    if (z) *z = a->cz + fx_mul(a->r, fx_sin(ang));
    return 1;
}

int arc2_tangent(const struct arc2 *a, i32 t, i32 *dx, i32 *dz)
{
    i32 ang, s, c;

    if (!arc2_valid(a) || t < 0 || t > FXONE) return 0;

    ang = a->a0 + fx_mul(a->sweep, t);
    s = fx_sin(ang);
    c = fx_cos(ang);
    if (a->sweep > 0) {
        if (dx) *dx = -s;
        if (dz) *dz = c;
    } else {
        if (dx) *dx = s;
        if (dz) *dz = -c;
    }
    return 1;
}

i32 arc2_curvature(const struct arc2 *a)
{
    if (!arc2_valid(a)) return 0;
    return a->sweep > 0 ? -fx_div(FXONE, a->r) : fx_div(FXONE, a->r);
}
