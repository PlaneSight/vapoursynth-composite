/*
 * 3D Transform PAL chroma separation.
 *
 * Port of ld-chroma-decoder's TransformPal3D: the 2D tile filter gains
 * a temporal axis. Tiles are 16 samples by 32 frame lines by 8 fields,
 * half-overlapped in every axis; rows belonging to the other field are
 * filled with black, so the FFT sees the interlaced lattice directly.
 * Bins are kept only if symmetric with their reflection about the
 * chroma carrier at (fsc, 72 c/aph, 18.75 Hz). The reference doubts
 * its own temporal reflection offset (ZTILE/4, with a comment saying
 * 3*ZTILE/4), but ZTILE/4 is correct: the black-filled lattice holds
 * the carrier at two alias pairs, and this reflection maps every
 * sideband to its mirror or the mirror's alias, equal in magnitude
 * either way. The commented offset pairs sidebands with empty bins.
 */

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "osdep.h"
#include "subcarrier.h"
#include "transform3d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define XTILE COMP_T3D_XTILE
#define YTILE COMP_T3D_YTILE
#define ZTILE COMP_T3D_ZTILE
#define XC COMP_T3D_XCOMPLEX
#define TILE_REAL (ZTILE * YTILE * XTILE)
#define TILE_CPLX (ZTILE * YTILE * XC)

#define LEVEL_BLACK 16384.0f

/* FFTW's planner is not thread-safe; execute-with-new-arrays is */
static pthread_mutex_t planner_lock = PTHREAD_MUTEX_INITIALIZER;

static double compute_window(int element, int limit)
{
    return 0.5 - 0.5 * cos((2.0 * M_PI * (element + 0.5)) / limit);
}

int comp_transform3d_init(comp_transform3d_t *t, double threshold, int standard,
                          int level, double evidence)
{
    if (!(threshold > 0.0 && threshold <= 1.0) || evidence < 0.0)
        return -1;
    t->standard = standard;
    t->level = !!level;
    t->use_lut = 0;
    t->evidence = (float)evidence;
    for (int i = 0; i < COMP_T3D_NTHRESH; i++)
        t->threshold_sq[i] = (float)(threshold * threshold);

    for (int z = 0; z < ZTILE; z++)
        for (int y = 0; y < YTILE; y++)
            for (int x = 0; x < XTILE; x++)
                t->window[z][y][x] = (float)(compute_window(z, ZTILE)
                                             * compute_window(y, YTILE)
                                             * compute_window(x, XTILE));

    ALIGNED_32( float real[TILE_REAL] );
    ALIGNED_32( fftwf_complex cplx[TILE_CPLX] );
    pthread_mutex_lock(&planner_lock);
    t->forward = fftwf_plan_dft_r2c_3d(ZTILE, YTILE, XTILE, real, cplx,
                                       FFTW_MEASURE);
    t->inverse = fftwf_plan_dft_c2r_3d(ZTILE, YTILE, XTILE, cplx, real,
                                       FFTW_MEASURE);
    pthread_mutex_unlock(&planner_lock);
    if (!t->forward || !t->inverse) {
        comp_transform3d_free(t);
        return -1;
    }
    return 0;
}

void comp_transform3d_set_lut(comp_transform3d_t *t, const double *v)
{
    for (int b = 0; b < COMP_T3D_NTHRESH; b++)
        for (int k = 0; k < COMP_LUT_K; k++)
            t->lut[b][k] = (float)v[b * COMP_LUT_K + k];
    t->use_lut = 1;
}

void comp_transform3d_free(comp_transform3d_t *t)
{
    pthread_mutex_lock(&planner_lock);
    if (t->forward)
        fftwf_destroy_plan(t->forward);
    if (t->inverse)
        fftwf_destroy_plan(t->inverse);
    pthread_mutex_unlock(&planner_lock);
    t->forward = NULL;
    t->inverse = NULL;
}

/* gain from a per-bin LUT row: linear interpolation over the
 * pair-symmetry ratio, knots uniform on [0, 1] */
static inline float lut_gain(const float *row, float lo, float hi)
{
    const float r = hi > 0.0f ? lo / hi : 1.0f;
    const float pos = r * (COMP_LUT_K - 1);
    int k = (int)pos;
    if (k > COMP_LUT_K - 2)
        k = COMP_LUT_K - 2;
    return row[k] + (row[k + 1] - row[k]) * (pos - k);
}

/* Returns the tile's chroma confidence: the kept output energy's mean
 * pair-symmetry ratio, 0 when nothing was kept. */
static float apply_filter(const comp_transform3d_t *t,
                          const fftwf_complex *in, fftwf_complex *out)
{
    const float *tsq = t->threshold_sq;
    float conf_num = 0.0f, conf_den = 0.0f;

    memset(out, 0, sizeof(fftwf_complex) * TILE_CPLX);

    for (int z = 0; z < ZTILE; z++) {
        const int z_ref = ((ZTILE / 4) + ZTILE - z) % ZTILE;
        for (int y = 0; y < YTILE; y++) {
            const int y_ref = ((YTILE / 4) + YTILE - y) % YTILE;
            const fftwf_complex *bi = in + (z * YTILE + y) * XC;
            const fftwf_complex *bi_ref = in + (z_ref * YTILE + y_ref) * XC;
            fftwf_complex *bo = out + (z * YTILE + y) * XC;
            fftwf_complex *bo_ref = out + (z_ref * YTILE + y_ref) * XC;

            for (int x = XTILE / 8; x <= XTILE / 4; x++) {
                const int x_ref = (XTILE / 2) - x;
                const float threshold_sq = *tsq++;

                if (x == x_ref && y == y_ref && z == z_ref) {
                    bo[x][0] = bi[x][0];
                    bo[x][1] = bi[x][1];
                    conf_num += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                    conf_den += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                    continue;
                }

                const float m_in_sq = bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                const float m_ref_sq = bi_ref[x_ref][0] * bi_ref[x_ref][0]
                                     + bi_ref[x_ref][1] * bi_ref[x_ref][1];
                const float lo = m_in_sq < m_ref_sq ? m_in_sq : m_ref_sq;
                const float hi = m_in_sq < m_ref_sq ? m_ref_sq : m_in_sq;
                const float r = hi > 0.0f ? lo / hi : 1.0f;

                /* LF-luma evidence at the pair's baseband difference
                 * frequency k - c. In this black-filled interlace
                 * lattice the U and V carriers each appear twice —
                 * (z,y) = (7,12)/(3,28) and (5,4)/(1,20), measured on
                 * an encoded flat field — and luma replicates onto the
                 * same alias offset, so take the strongest of the four
                 * difference positions. */
                float g_e = 1.0f;
                if (t->evidence > 0.0f) {
                    static const int cz[4] = { 7, 3, 5, 1 };
                    static const int cy[4] = { 12, 28, 4, 20 };
                    float e = 0.0f;
                    for (int c = 0; c < 4; c++) {
                        const fftwf_complex *lf =
                            in + (((cz[c] - z + ZTILE) % ZTILE) * YTILE
                                  + (cy[c] - y + YTILE) % YTILE) * XC
                               + (XTILE / 4) - x;
                        const float ec = (*lf)[0] * (*lf)[0] + (*lf)[1] * (*lf)[1];
                        e = ec > e ? ec : e;
                    }
                    const float den = e + t->evidence * hi;
                    if (den > 0.0f)
                        g_e = e / den;
                }

                if (t->use_lut) {
                    const int bin = (int)(tsq - t->threshold_sq) - 1;
                    const float g = lut_gain(t->lut[bin], lo, hi) * g_e;
                    bo[x][0] = bi[x][0] * g;
                    bo[x][1] = bi[x][1] * g;
                    bo_ref[x_ref][0] = bi_ref[x_ref][0] * g;
                    bo_ref[x_ref][1] = bi_ref[x_ref][1] * g;
                    const float e = g * g * (m_in_sq + m_ref_sq);
                    conf_num += e * r;
                    conf_den += e;
                    continue;
                }

                if (t->level) {
                    /* set the larger of the pair to the smaller, phase
                     * preserved (GB 2365247 A) */
                    float f_in = g_e, f_ref = g_e;
                    if (m_in_sq > m_ref_sq)
                        f_in *= sqrtf(m_ref_sq / m_in_sq);
                    else if (m_ref_sq > m_in_sq)
                        f_ref *= sqrtf(m_in_sq / m_ref_sq);
                    bo[x][0] = bi[x][0] * f_in;
                    bo[x][1] = bi[x][1] * f_in;
                    bo_ref[x_ref][0] = bi_ref[x_ref][0] * f_ref;
                    bo_ref[x_ref][1] = bi_ref[x_ref][1] * f_ref;
                    conf_num += 2.0f * lo * g_e * g_e * r;
                    conf_den += 2.0f * lo * g_e * g_e;
                    continue;
                }

                if (m_in_sq < m_ref_sq * threshold_sq ||
                    m_ref_sq < m_in_sq * threshold_sq)
                    continue;

                bo[x][0] = bi[x][0] * g_e;
                bo[x][1] = bi[x][1] * g_e;
                bo_ref[x_ref][0] = bi_ref[x_ref][0] * g_e;
                bo_ref[x_ref][1] = bi_ref[x_ref][1] * g_e;
                conf_num += (m_in_sq + m_ref_sq) * g_e * g_e * r;
                conf_den += (m_in_sq + m_ref_sq) * g_e * g_e;
            }
        }
    }

    return conf_den > 0.0f ? conf_num / conf_den : 0.0f;
}


static inline float dist_sq3(float a, float b, float c)
{
    return a * a + b * b + c * c;
}

/* NTSC frequency-domain filter, ported from the ld-decode transform-ntsc
 * branch (TransformNtsc3D). NTSC's U and V share one carrier, so chroma
 * is only approximately symmetric about it; the test compensates with a
 * frequency-shaped threshold (lenient near the chroma carrier at
 * (fsc, 120 c/aph, 15 Hz), strict near luma) and cross-checks each
 * candidate against the energy at its demodulated-luma positions k±c
 * (chroma with no corresponding luma is suspect). A trained LUT
 * replaces both devices — the per-bin gains learn the frequency
 * dependence directly — so the luma-evidence test is not applied in
 * LUT mode. Returns the tile's chroma confidence as apply_filter does. */
static float apply_filter_ntsc(const comp_transform3d_t *t,
                               const fftwf_complex *in, fftwf_complex *out)
{
    const float *tsq = t->threshold_sq;
    float conf_num = 0.0f, conf_den = 0.0f;

    memset(out, 0, sizeof(fftwf_complex) * TILE_CPLX);

    for (int z = 0; z < ZTILE; z++) {
        const int z_ref = ((ZTILE / 2) + ZTILE - z) % ZTILE;
        const int z_lr = (z - ZTILE / 4 + ZTILE) % ZTILE;
        const int z_lrn = (ZTILE - z_lr) % ZTILE;
        const float kz0 = (float)z / ZTILE;

        for (int y = 0; y < YTILE; y++) {
            const int y_ref = ((YTILE / 2) + YTILE - y) % YTILE;
            const int y_lr = (y - YTILE / 4 + YTILE) % YTILE;
            const int y_lrn = (YTILE - y_lr) % YTILE;
            const float ky0 = (float)y / YTILE;

            /* map (ky, kz) into the interlace-equivalence diamond */
            float ky = ky0, kz = kz0;
            if (kz0 + ky0 < 0.5f) {
                kz = kz0 + 0.5f;
                ky = ky0 + 0.5f;
            } else if (kz0 + ky0 > 1.5f) {
                kz = kz0 - 0.5f;
                ky = ky0 - 0.5f;
            } else if (kz0 - ky0 > 0.5f) {
                kz = kz0 - 0.5f;
                ky = ky0 + 0.5f;
            } else if (ky0 - kz0 > 0.5f) {
                kz = kz0 + 0.5f;
                ky = ky0 - 0.5f;
            }
            if (kz + ky > 1.0f) {
                kz = 1.0f - kz;
                ky = 1.0f - ky;
            }

            const fftwf_complex *bi = in + (z * YTILE + y) * XC;
            const fftwf_complex *bi_ref = in + (z_ref * YTILE + y_ref) * XC;
            const fftwf_complex *bi_lr = in + (z_lr * YTILE + y_lr) * XC;
            const fftwf_complex *bi_lrn = in + (z_lrn * YTILE + y_lrn) * XC;
            fftwf_complex *bo = out + (z * YTILE + y) * XC;
            fftwf_complex *bo_ref = out + (z_ref * YTILE + y_ref) * XC;

            for (int x = XTILE / 8; x <= XTILE / 4; x++) {
                const int x_ref = (XTILE / 2) - x;
                const int x_lr = x - XTILE / 4;
                const float kx = (float)x / XTILE;
                const float t0_sq = *tsq++;

                const fftwf_complex *lr1, *lr2;
                if (x_lr >= 0) {
                    lr1 = &bi_lr[x_lr];
                    lr2 = &bi_lrn[XTILE / 2 - x_lr];
                } else {
                    lr1 = &bi_lrn[-x_lr];
                    lr2 = &bi_lr[XTILE / 2 + x_lr];
                }

                if (x == x_ref) {
                    if ((y == YTILE / 4 && z == ZTILE / 4)
                        || (y == 3 * YTILE / 4 && z == 3 * ZTILE / 4)) {
                        /* its own reflection and a carrier: keep */
                        bo[x][0] = bi[x][0];
                        bo[x][1] = bi[x][1];
                        conf_num += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                        conf_den += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                        continue;
                    }
                    if (((y == 0 || y == YTILE / 2) && (z == 0 || z == ZTILE / 2))
                        || (y == YTILE / 4 && z == 3 * ZTILE / 4)
                        || (y == 3 * YTILE / 4 && z == ZTILE / 4)) {
                        /* its own reflection but not a carrier: discard */
                        continue;
                    }
                }

                const float m_in = bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                const float m_ref = bi_ref[x_ref][0] * bi_ref[x_ref][0]
                                  + bi_ref[x_ref][1] * bi_ref[x_ref][1];
                const float lo = m_in < m_ref ? m_in : m_ref;
                const float m_max = m_in > m_ref ? m_in : m_ref;
                const float r = m_max > 0.0f ? lo / m_max : 1.0f;

                if (t->use_lut) {
                    const int bin = (int)(tsq - t->threshold_sq) - 1;
                    const float g = lut_gain(t->lut[bin], lo, m_max);
                    bo[x][0] = bi[x][0] * g;
                    bo[x][1] = bi[x][1] * g;
                    bo_ref[x_ref][0] = bi_ref[x_ref][0] * g;
                    bo_ref[x_ref][1] = bi_ref[x_ref][1] * g;
                    const float e = g * g * (m_in + m_ref);
                    conf_num += e * r;
                    conf_den += e;
                    continue;
                }

                const float l1 = (*lr1)[0] * (*lr1)[0] + (*lr1)[1] * (*lr1)[1];
                const float l2 = (*lr2)[0] * (*lr2)[0] + (*lr2)[1] * (*lr2)[1];
                const float m_luma = l1 > l2 ? l1 : l2;

                if (t->level) {
                    /* the reference's levelMode: discard the pair when
                     * it lacks luma evidence, pass it untouched inside
                     * a 10x squared-magnitude dead zone, and beyond
                     * that scale the larger down to the smaller */
                    if (m_max > 10.0f * m_luma)
                        continue;
                    float f_in = 1.0f, f_ref = 1.0f;
                    if (m_in > 10.0f * m_ref)
                        f_in = sqrtf(m_ref / m_in);
                    else if (m_ref > 10.0f * m_in)
                        f_ref = sqrtf(m_in / m_ref);
                    bo[x][0] = bi[x][0] * f_in;
                    bo[x][1] = bi[x][1] * f_in;
                    bo_ref[x_ref][0] = bi_ref[x_ref][0] * f_ref;
                    bo_ref[x_ref][1] = bi_ref[x_ref][1] * f_ref;
                    const float e = f_in * f_in * m_in + f_ref * f_ref * m_ref;
                    conf_num += e * r;
                    conf_den += e;
                    continue;
                }

                /* threshold shaped by proximity to chroma vs luma */
                const float k_luma = dist_sq3(kz - 0.5f, ky - 0.5f, kx);
                const float k_chroma = dist_sq3(kz - 0.25f, ky - 0.25f, kx - 0.25f);
                float th_sq = powf(k_chroma / (k_luma + k_chroma), 10.0f * t0_sq);

                if (m_luma < m_max * th_sq)
                    th_sq = 0.5f * (1.0f + th_sq);

                if (m_in < m_ref * th_sq || m_ref < m_in * th_sq)
                    continue;

                bo[x][0] = bi[x][0];
                bo[x][1] = bi[x][1];
                bo_ref[x_ref][0] = bi_ref[x_ref][0];
                bo_ref[x_ref][1] = bi_ref[x_ref][1];
                conf_num += (m_in + m_ref) * r;
                conf_den += m_in + m_ref;
            }
        }
    }

    return conf_den > 0.0f ? conf_num / conf_den : 0.0f;
}

/* the slab origin never takes this value: the grid is anchored at
 * absolute field 0 and the first frame's low slab sits at -ZTILE/2 */
#define SLAB_EMPTY INT_MIN

int comp_t3d_cache_init(comp_t3d_cache_t *c, int nslabs, int width,
                        int field_rows, int with_conf)
{
    if (nslabs < 2 || width < 1 || field_rows < 1)
        return -1;
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->slabs = calloc(nslabs, sizeof(*c->slabs));
    if (!c->slabs) {
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->cond);
        return -1;
    }
    c->nslabs = nslabs;
    c->width = width;
    c->field_rows = field_rows;

    const size_t planes = sizeof(float) * ZTILE * field_rows * width;
    for (int i = 0; i < nslabs; i++) {
        c->slabs[i].tz = SLAB_EMPTY;
        c->slabs[i].chroma = malloc(planes);
        if (with_conf)
            c->slabs[i].conf = malloc(planes);
        if (!c->slabs[i].chroma || (with_conf && !c->slabs[i].conf)) {
            comp_t3d_cache_free(c);
            return -1;
        }
    }
    return 0;
}

void comp_t3d_cache_free(comp_t3d_cache_t *c)
{
    if (!c->slabs)
        return;
    for (int i = 0; i < c->nslabs; i++) {
        free(c->slabs[i].chroma);
        free(c->slabs[i].conf);
    }
    free(c->slabs);
    c->slabs = NULL;
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
}

static comp_t3d_slab_t *cache_lookup(comp_t3d_cache_t *c, int tz, int parity)
{
    for (int i = 0; i < c->nslabs; i++)
        if (c->slabs[i].tz == tz && c->slabs[i].parity == parity)
            return &c->slabs[i];
    return NULL;
}

static comp_t3d_slab_t *cache_victim(comp_t3d_cache_t *c,
                                     const comp_t3d_slab_t *ex1,
                                     const comp_t3d_slab_t *ex2)
{
    comp_t3d_slab_t *v = NULL;
    for (int i = 0; i < c->nslabs; i++) {
        comp_t3d_slab_t *s = &c->slabs[i];
        if (s == ex1 || s == ex2 || s->refs > 0)
            continue;
        if (s->tz == SLAB_EMPTY)
            return s;
        if (!v || s->stamp < v->stamp)
            v = s;
    }
    return v;
}

/* Take references on the slabs for tz1 and tz2, claiming missing ones
 * for the caller to build (*build set, ready cleared). Both slots are
 * secured under one lock acquisition and a thread that cannot get both
 * holds nothing while it waits, so waiters cannot deadlock: builders
 * never block, and every wait is for a builder to finish or a
 * reference to drop. */
static void cache_acquire(comp_t3d_cache_t *c, int parity, int tz1, int tz2,
                          comp_t3d_slab_t **s1, comp_t3d_slab_t **s2,
                          int *build1, int *build2)
{
    pthread_mutex_lock(&c->lock);
    for (;;) {
        comp_t3d_slab_t *a = cache_lookup(c, tz1, parity);
        comp_t3d_slab_t *b = cache_lookup(c, tz2, parity);
        comp_t3d_slab_t *va = a ? NULL : cache_victim(c, b, NULL);
        comp_t3d_slab_t *vb = b ? NULL : cache_victim(c, a, va);

        if ((a || va) && (b || vb)) {
            if (!a) {
                a = va;
                a->tz = tz1;
                a->parity = parity;
                a->ready = 0;
                *build1 = 1;
            }
            if (!b) {
                b = vb;
                b->tz = tz2;
                b->parity = parity;
                b->ready = 0;
                *build2 = 1;
            }
            a->refs++;
            b->refs++;
            a->stamp = ++c->counter;
            b->stamp = ++c->counter;
            *s1 = a;
            *s2 = b;
            pthread_mutex_unlock(&c->lock);
            return;
        }
        pthread_cond_wait(&c->cond, &c->lock);
    }
}

/* Overlap-add every tile at temporal grid position slab->tz into the
 * slab's ZTILE contribution planes. width/field_rows give the frame
 * geometry (at most the cache's capacity) and define the plane layout,
 * which the assembly in comp_transform3d_frame mirrors. */
static void build_slab(const comp_transform3d_t *t, comp_t3d_slab_t *s,
                       const comp_field_view_t *fields, int z0, int nfields,
                       int width, int field_rows)
{
    ALIGNED_32( float real[TILE_REAL] );
    ALIGNED_32( fftwf_complex cplx_in[TILE_CPLX] );
    ALIGNED_32( fftwf_complex cplx_out[TILE_CPLX] );
    const int frame_lines = field_rows * 2;
    const int tz = s->tz;
    const int parity = s->parity;
    const size_t plane = (size_t)field_rows * width;

    memset(s->chroma, 0, sizeof(float) * ZTILE * plane);
    if (s->conf)
        memset(s->conf, 0, sizeof(float) * ZTILE * plane);

    for (int tile_y = -YTILE / 2; tile_y < frame_lines; tile_y += YTILE / 2) {
        const int start_y = tile_y < 0 ? -tile_y : 0;
        const int end_y = frame_lines - tile_y < YTILE ? frame_lines - tile_y : YTILE;

        for (int tile_x = -XTILE / 2; tile_x < width; tile_x += XTILE / 2) {
            const int start_x = tile_x < 0 ? -tile_x : 0;
            const int end_x = width - tile_x < XTILE ? width - tile_x : XTILE;

            /* windowed forward FFT: a row is real data only when it
             * is inside the picture and belongs to field tz+z */
            for (int z = 0; z < ZTILE; z++) {
                const int g = tz + z;
                const comp_field_view_t *fv =
                    (g >= z0 && g < z0 + nfields) ? &fields[g - z0] : NULL;
                if (fv && !fv->data)
                    fv = NULL;
                for (int y = 0; y < YTILE; y++) {
                    const int fl = tile_y + y;
                    const int usable = fv && y >= start_y && y < end_y
                                       && ((fl + parity) & 1) == (g & 1);
                    const uint16_t *b = usable
                        ? fv->data + (fl >> 1) * fv->stride : NULL;
                    float *dst = &real[(z * YTILE + y) * XTILE];
                    for (int x = 0; x < XTILE; x++) {
                        const float v = (!usable || x < start_x || x >= end_x)
                                        ? LEVEL_BLACK : (float)b[tile_x + x];
                        dst[x] = v * t->window[z][y][x];
                    }
                }
            }
            fftwf_execute_dft_r2c(t->forward, real, cplx_in);

            const float w = t->standard == COMP_STD_PAL
                ? apply_filter(t, cplx_in, cplx_out)
                : apply_filter_ntsc(t, cplx_in, cplx_out);

            fftwf_execute_dft_c2r(t->inverse, cplx_out, real);

            /* the confidence uses the same unity-sum window weights */
            for (int z = 0; z < ZTILE; z++) {
                const int g = tz + z;
                float *cb = s->chroma + z * plane;
                float *wb = s->conf ? s->conf + z * plane : NULL;
                for (int y = start_y; y < end_y; y++) {
                    const int fl = tile_y + y;
                    if (((fl + parity) & 1) != (g & 1))
                        continue;
                    float *b = cb + (fl >> 1) * width;
                    for (int x = start_x; x < end_x; x++)
                        b[tile_x + x] += real[(z * YTILE + y) * XTILE + x]
                                         / (ZTILE * YTILE * XTILE);
                    if (wb) {
                        float *bw = wb + (fl >> 1) * width;
                        for (int x = start_x; x < end_x; x++)
                            bw[tile_x + x] += w * t->window[z][y][x];
                    }
                }
            }
        }
    }
}

void comp_transform3d_frame(const comp_transform3d_t *t, comp_t3d_cache_t *c,
                            const comp_field_view_t *fields, int z0, int nfields,
                            int frame, int parity, int width, int field_rows,
                            float *chroma0, float *chroma1, ptrdiff_t chroma_stride,
                            float *conf0, float *conf1)
{
    float *chroma[2] = { chroma0, chroma1 };
    float *conf[2] = { conf0, conf1 };
    const int fout = frame * 2;

    /* the two half-overlapped z positions whose tiles cover this frame's
     * fields; the grid is anchored at absolute field 0 */
    const int tz_hi = ((fout + 1) / (ZTILE / 2)) * (ZTILE / 2);
    comp_t3d_slab_t *s1, *s2;
    int build1 = 0, build2 = 0;

    cache_acquire(c, parity, tz_hi - ZTILE / 2, tz_hi, &s1, &s2,
                  &build1, &build2);
    if (build1)
        build_slab(t, s1, fields, z0, nfields, width, field_rows);
    if (build2)
        build_slab(t, s2, fields, z0, nfields, width, field_rows);

    pthread_mutex_lock(&c->lock);
    if (build1)
        s1->ready = 1;
    if (build2)
        s2->ready = 1;
    if (build1 || build2)
        pthread_cond_broadcast(&c->cond);
    while (!s1->ready || !s2->ready)
        pthread_cond_wait(&c->cond, &c->lock);
    pthread_mutex_unlock(&c->lock);

    /* each output field is the sum of the two covering slabs' planes */
    const size_t plane = (size_t)field_rows * width;
    for (int f = 0; f < 2; f++) {
        const int g = fout + f;
        const float *p1 = s1->chroma + (g - s1->tz) * plane;
        const float *p2 = s2->chroma + (g - s2->tz) * plane;
        float *dc = chroma[f ^ parity];
        float *dw = conf[f ^ parity];
        for (int r = 0; r < field_rows; r++)
            for (int x = 0; x < width; x++)
                dc[r * chroma_stride + x] = p1[r * width + x] + p2[r * width + x];
        if (dw) {
            const float *q1 = s1->conf + (g - s1->tz) * plane;
            const float *q2 = s2->conf + (g - s2->tz) * plane;
            for (int r = 0; r < field_rows; r++)
                for (int x = 0; x < width; x++)
                    dw[r * chroma_stride + x] = q1[r * width + x] + q2[r * width + x];
        }
    }

    pthread_mutex_lock(&c->lock);
    s1->refs--;
    s2->refs--;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}
