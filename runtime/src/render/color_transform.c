#include "swfrt/movie.h"

#include <math.h>

const swf_matrix SWF_MATRIX_IDENTITY = {1, 0, 0, 1, 0, 0};
const swf_cxform SWF_CXFORM_IDENTITY = {{1, 1, 1, 1}, {0, 0, 0, 0}};

swf_matrix swf_matrix_mul(const swf_matrix *p, const swf_matrix *c)
{
    swf_matrix r;
    r.a = p->a * c->a + p->c * c->b;
    r.b = p->b * c->a + p->d * c->b;
    r.c = p->a * c->c + p->c * c->d;
    r.d = p->b * c->c + p->d * c->d;
    r.tx = p->a * c->tx + p->c * c->ty + p->tx;
    r.ty = p->b * c->tx + p->d * c->ty + p->ty;
    return r;
}

swf_cxform swf_cxform_mul(const swf_cxform *p, const swf_cxform *c)
{
    swf_cxform r;
    for (int i = 0; i < 4; i++) {
        r.mul[i] = c->mul[i] * p->mul[i];
        r.add[i] = c->add[i] * p->mul[i] + p->add[i];
    }
    return r;
}

bool swf_cxform_is_identity_mul(const swf_cxform *c)
{
    return c->mul[0] == 1.0f && c->mul[1] == 1.0f && c->mul[2] == 1.0f && c->mul[3] == 1.0f;
}

bool swf_cxform_has_add(const swf_cxform *c)
{
    return c->add[0] != 0.0f || c->add[1] != 0.0f || c->add[2] != 0.0f || c->add[3] != 0.0f;
}

void swf_cxform_apply(const swf_cxform *c, const uint8_t in[4], uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        float v = in[i] * c->mul[i] + c->add[i];
        out[i] = (uint8_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : lrintf(v));
    }
}
