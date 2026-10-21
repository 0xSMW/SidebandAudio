#include "include/SBEngine.h"
#include "sb_dsp.h"
#include "sb_stretcher.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define BLOCK 4096u
#define POST_CAP (BLOCK * 2u)
struct SBEngine {
    sb_dsp *dsp;
    sb_stretcher *stretch;
    SBEngineConfig config;
    unsigned channels;
    double base;
    uint64_t processed, smart_output;
    size_t post_count, cut_count, cut_offset;
    float post[2][POST_CAP], work[2][BLOCK];
    uint16_t quiet[BLOCK];
    double source[BLOCK], saved[BLOCK];
    int finished, dsp_done, drained;
};
SBEngineConfig sb_engine_default_config(void) {
    SBEngineConfig c;
    memset(&c, 0, sizeof c);
    c.rate = 1; c.smart_speed = 1; c.voice_boost = 1; c.target_lufs = -14;
    return c;
}
void sb_engine_config_set_band(SBEngineConfig *c, size_t i, int type, float f, float q, float g) {
    if (!c || i >= SB_ENGINE_MAX_EQ_BANDS) return;
    c->eq[i] = (SBEngineEQBand){type, f, q, g};
    if (c->eq_count <= i) c->eq_count = i + 1;
}
double sb_engine_custom_eq_gain_db(const SBEngineConfig *config,double sample_rate,double frequency) {
    if(!config||config->eq_count>SB_ENGINE_MAX_EQ_BANDS)return NAN;
    sb_dsp_config dsp={0};
    dsp.eq_count=config->eq_count;
    for(size_t i=0;i<dsp.eq_count;i++) {
        SBEngineEQBand band=config->eq[i];
        dsp.eq[i]=(sb_dsp_eq_band){band.type,band.frequency,band.q,band.gain_db};
    }
    return sb_dsp_custom_eq_gain_db(&dsp,sample_rate,frequency);
}
int sb_engine_configure(SBEngine *e, const SBEngineConfig *c) {
    if (!e || !c || !isfinite(c->rate) || c->rate < .5f || c->rate > 5 || c->eq_count > 12) return 0;
    if (!sb_stretcher_set_rate(e->stretch, c->rate)) return 0;
    sb_dsp_config d;
    memset(&d, 0, sizeof d);
    d.voice_boost = c->voice_boost; d.mono = c->mono; d.noise_reduction = c->noise_reduction;
    d.normalization_mode = c->normalization_mode; d.target_lufs = c->target_lufs; d.deesser = c->deesser;
    d.eq_count = c->eq_count;
    for (size_t i=0;i<d.eq_count;i++) d.eq[i]=(sb_dsp_eq_band){c->eq[i].type,c->eq[i].frequency,c->eq[i].q,c->eq[i].gain_db};
    sb_dsp_configure(e->dsp, &d);
    e->config = *c;
    return 1;
}
SBEngine *sb_engine_create(double fs, unsigned channels, size_t max_input) {
    if (!isfinite(fs) || fs < 8000 || fs > 192000 || channels < 1 || channels > 2 || !max_input || max_input > 1048576) return NULL;
    SBEngine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->channels = channels;
    e->dsp = sb_dsp_create(fs, channels, BLOCK);
    size_t capacity = max_input * 4 + (size_t)ceil(fs * .5) + BLOCK * 4;
    e->stretch = sb_stretcher_create(fs, channels, capacity);
    if (!e->dsp || !e->stretch) { sb_engine_destroy(e); return NULL; }
    SBEngineConfig c=sb_engine_default_config();
    if (!sb_engine_configure(e,&c)) { sb_engine_destroy(e); return NULL; }
    return e;
}
void sb_engine_destroy(SBEngine *e) {
    if (!e) return;
    sb_dsp_destroy(e->dsp); sb_stretcher_destroy(e->stretch); free(e);
}
void sb_engine_reset(SBEngine *e, int64_t frame) {
    if (!e) return;
    sb_dsp_reset(e->dsp); sb_stretcher_reset(e->stretch);
    e->base = frame < 0 ? 0 : (double)frame;
    e->processed=e->smart_output=0;
    e->post_count=e->cut_count=e->cut_offset=0;
    e->finished=e->dsp_done=e->drained=0;
}
void sb_engine_seed_loudness(SBEngine *e, float level) {
    if (e) sb_dsp_seed_loudness(e->dsp, level);
}
/* Overcast-like fixed-sample micro-splices, evaluated in original block coordinates.
   Each accepted 148-frame peak-quiet window produces a complementary 46-frame blend. */
static size_t shorten(SBEngine *e, size_t n) {
    if (!e->config.smart_speed || n < 149) return n;
    float threshold=sb_dsp_smart_threshold(e->dsp);
    if (!isfinite(threshold) || threshold < 0) threshold=.003981071f;
    /* Count consecutive quiet source frames once. The predicate is identical
       to max(peak[cursor...cursor+148]) <= threshold, without rescanning the
       same 148 samples at every voiced frame. Build before any splice moves. */
    unsigned quiet=0;
    for (size_t i=n;i-- > 0;) {
        float p=fabsf(e->work[0][i]);
        if (e->channels==2) p=fmaxf(p,fabsf(e->work[1][i]));
        quiet=p>threshold?0:quiet<148?quiet+1:148;
        e->quiet[i]=(uint16_t)quiet;
    }
    size_t cursor=0, removed=0;
    while (cursor+148<n) {
        if (e->quiet[cursor]<148) { cursor++; continue; }
        size_t dst=cursor-removed;
        for (unsigned c=0;c<e->channels;c++) {
            for (size_t j=0;j<46;j++) {
                float a=(float)j/45.0f;
                e->work[c][dst+j]=e->work[c][dst+j]*(1-a)+e->work[c][dst+102+j]*a;
            }
            /* Unlike an internal padded native buffer, never read beyond logical source data. */
            memmove(e->work[c]+dst+46,e->work[c]+dst+148,(n-cursor-148)*sizeof(float));
        }
        memmove(e->source+dst,e->source+dst+102,(n-cursor-102)*sizeof(double));
        removed+=102; cursor+=148;
    }
    return n-removed;
}
/* Retain a partly accepted cut block until the stretcher has capacity. */
static int dispatch(SBEngine *e, int tail) {
    for (;;) {
        if (e->cut_offset<e->cut_count) {
            const float *p[2]={e->work[0]+e->cut_offset,e->work[1]+e->cut_offset};
            size_t n=sb_stretcher_push(e->stretch,p,e->source+e->cut_offset,e->saved+e->cut_offset,e->cut_count-e->cut_offset);
            e->cut_offset+=n;
            if (e->cut_offset<e->cut_count) return 0;
        }
        e->cut_offset=e->cut_count=0;
        if (e->post_count<BLOCK && !(tail && e->post_count)) return 1;
        size_t n=e->post_count<BLOCK?e->post_count:BLOCK;
        for (unsigned c=0;c<e->channels;c++) memcpy(e->work[c],e->post[c],n*sizeof(float));
        for (size_t i=0;i<n;i++) e->source[i]=e->base+(double)e->processed+(double)i;
        e->processed+=n;
        size_t out=shorten(e,n);
        for (size_t i=0;i<out;i++) e->saved[i]=fmax(0,e->source[i]-e->base-(double)e->smart_output-(double)i);
        e->smart_output+=out;
        e->cut_count=out;
        e->post_count-=n;
        for (unsigned c=0;c<e->channels;c++) memmove(e->post[c],e->post[c]+n,e->post_count*sizeof(float));
    }
}
size_t sb_engine_push(SBEngine *e,const float *l,const float *r,size_t frames) {
    if (!e || e->finished || !l || (e->channels==2&&!r)) return 0;
    size_t accepted=0;
    while (accepted<frames) {
        if (!dispatch(e,0)) break;
        size_t n=frames-accepted; if (n>BLOCK) n=BLOCK;
        if (e->post_count+n>POST_CAP) break;
        const float *in[2]={l+accepted,r?r+accepted:l+accepted};
        float *out[2]={e->post[0]+e->post_count,e->post[1]+e->post_count};
        size_t produced=sb_dsp_process(e->dsp,in,n,out,POST_CAP-e->post_count);
        e->post_count+=produced; accepted+=n;
    }
    (void)dispatch(e,0);
    return accepted;
}
void sb_engine_finish(SBEngine *e) { if(e) e->finished=1; }
static int flush_dsp(SBEngine *e) {
    if (!dispatch(e,0)) return 0;
    if (!e->dsp_done) {
        float *out[2]={e->post[0]+e->post_count,e->post[1]+e->post_count};
        size_t n=sb_dsp_flush(e->dsp,out,POST_CAP-e->post_count);
        e->post_count+=n;
        if (!n) e->dsp_done=1;
        if (!dispatch(e,0)) return 0;
    }
    return e->dsp_done && dispatch(e,1);
}
size_t sb_engine_read(SBEngine *e,float *l,float *r,double *src,double *saved,size_t cap) {
    if(!e || !l || !src || !saved || !cap || (e->channels==2&&!r)) return 0;
    int done=0;
    if(e->finished) {
        /* DSP may need several bounded tail passes before the stretcher can finalize. */
        for(size_t i=0;i<64;i++) {
            done=flush_dsp(e);
            if(done || e->cut_offset<e->cut_count) break;
        }
    } else (void)dispatch(e,0);
    float *out[2]={l,r?r:l};
    size_t n=sb_stretcher_pull(e->stretch,out,src,saved,cap,done);
    if(!n && done && sb_stretcher_is_drained(e->stretch)) e->drained=1;
    return n;
}
int sb_engine_is_drained(const SBEngine *e) { return e?e->drained:1; }
float sb_engine_loudness(const SBEngine *e) { return e?sb_dsp_loudness_db(e->dsp):-160; }
size_t sb_engine_latency(const SBEngine *e) { return e?sb_dsp_latency(e->dsp):0; }
