#include "include/SBEngine.h"
#include "sb_dsp.h"
#include "sb_stretcher.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define BLOCK 4096u
#define PAUSE_HISTORY 100u
#define PAUSE_QUEUE 32u
#define PAUSE_LOOKAHEAD 24u
#define PAUSE_CONFIRM 20u
#define PAUSE_GUARD 6u
#define VOICING_SAMPLES 320u

typedef struct {
    double previous_input, high_pass, speech_low, voicing_low;
    double energy[3], noise[3], speech_reference;
    float history[3][PAUSE_HISTORY], voiced[VOICING_SAMPLES];
    size_t history_count, history_position, voiced_count, voiced_position;
    double decimation_phase;
} PauseChannel;
typedef struct { size_t frames; uint64_t index; int quiet, cut; } PauseFrame;
struct SBEngine {
    sb_dsp *dsp;
    sb_stretcher *stretch;
    SBEngineConfig config;
    unsigned channels;
    double sample_rate, high_pass_alpha, speech_low_alpha, voicing_low_alpha;
    size_t pause_hop, post_capacity, raw_capacity, raw_count, pause_analyzed, pause_head, pause_count;
    uint64_t pause_index, quiet_start;
    unsigned quiet_frames;
    PauseChannel pause_channel[2];
    PauseFrame pause_frames[PAUSE_QUEUE];
    double base;
    uint64_t processed, smart_output;
    size_t post_count, cut_count, cut_offset;
    float *post[2], *raw[2], work[2][BLOCK];
    uint16_t quiet[BLOCK];
    double source[BLOCK], saved[BLOCK];
    int finished, dsp_done, drained;
};
static void reset_pause_detector(SBEngine *e) {
    memset(e->pause_channel,0,sizeof(e->pause_channel));
    for (unsigned c=0;c<e->channels;c++) {
        e->pause_channel[c].speech_reference=-160.;
        for (unsigned band=0;band<3;band++) e->pause_channel[c].noise[band]=-160.;
    }
    e->pause_analyzed=e->pause_head=e->pause_count=0;
    e->pause_index=e->quiet_start=0;
    e->quiet_frames=0;
}
static double energy_db(double energy) { return 10.*log10(fmax(energy,1.e-16)); }
static double noise_quantile(const PauseChannel *p,unsigned band) {
    float sorted[PAUSE_HISTORY];
    size_t count=p->history_count;
    for (size_t i=0;i<count;i++) {
        float value=p->history[band][i];
        size_t j=i;
        while (j && sorted[j-1]>value) { sorted[j]=sorted[j-1];j--; }
        sorted[j]=value;
    }
    return count?sorted[(count-1)/5]:-160.;
}
static int has_voicing(const PauseChannel *p) {
    /* 40 ms correlation at 4 kHz, with 40...400 Hz candidate fundamentals.
       Per-channel analysis preserves quiet voiced and antiphase stereo speech. */
    if (p->voiced_count<280) return 0;
    for (size_t lag=10;lag<=100;lag++) {
        double xy=0.,xx=0.,yy=0.;
        for (size_t i=0;i<160;i++) {
            size_t a=(p->voiced_position+VOICING_SAMPLES-1-i)%VOICING_SAMPLES;
            size_t b=(a+VOICING_SAMPLES-lag)%VOICING_SAMPLES;
            double x=p->voiced[a],y=p->voiced[b];
            xy+=x*y;xx+=x*x;yy+=y*y;
        }
        if (xx>1.e-12 && yy>1.e-12 && xy>.78*sqrt(xx*yy)) return 1;
    }
    return 0;
}
static int pause_channel_quiet(SBEngine *e,unsigned channel,const float *audio,size_t n) {
    PauseChannel *p=&e->pause_channel[channel];
    double sums[3]={0,0,0};
    for (size_t i=0;i<n;i++) {
        double x=audio[i];
        p->high_pass=e->high_pass_alpha*(p->high_pass+x-p->previous_input);
        p->previous_input=x;
        p->speech_low=e->speech_low_alpha*p->speech_low+(1.-e->speech_low_alpha)*p->high_pass;
        p->voicing_low=e->voicing_low_alpha*p->voicing_low+(1.-e->voicing_low_alpha)*p->high_pass;
        double high=p->high_pass-p->speech_low;
        sums[0]+=x*x;sums[1]+=p->speech_low*p->speech_low;sums[2]+=high*high;
        p->decimation_phase+=4000.;
        if (p->decimation_phase>=e->sample_rate) {
            p->decimation_phase-=e->sample_rate;
            p->voiced[p->voiced_position]=(float)p->voicing_low;
            p->voiced_position=(p->voiced_position+1)%VOICING_SAMPLES;
            if (p->voiced_count<VOICING_SAMPLES) p->voiced_count++;
        }
    }
    double instant=energy_db(sums[0]/n),levels[3],instant_levels[3];
    for (unsigned band=0;band<3;band++) {
        /* 20 ms smoothing reduces noise jitter, while instant energy and
           periodicity below veto speech without waiting for this average. */
        double mean=sums[band]/n;
        instant_levels[band]=energy_db(mean);
        p->energy[band]=p->history_count? .6*p->energy[band]+.4*mean:mean;
        levels[band]=energy_db(p->energy[band]);
    }
    p->speech_reference=fmax(instant,p->speech_reference-.02);
    /* Digital zero must not bias the learned floor toward minus infinity. */
    if (instant>-80.) {
        for (unsigned band=0;band<3;band++) p->history[band][p->history_position]=(float)instant_levels[band];
        p->history_position=(p->history_position+1)%PAUSE_HISTORY;
        if (p->history_count<PAUSE_HISTORY) p->history_count++;
        for (unsigned band=0;band<3;band++) {
            double candidate=noise_quantile(p,band);
            if (p->noise[band]<-150.) p->noise[band]=candidate;
            else if (candidate<p->noise[band]) p->noise[band]+=.35*(candidate-p->noise[band]);
            else p->noise[band]+=.15*(candidate-p->noise[band]);
        }
    }
    if (instant<-90.) return 1;
    if (p->history_count<PAUSE_CONFIRM || p->speech_reference-p->noise[0]<9.) return 0;
    /* Breathy consonants can be aperiodic. Large short-term band changes veto
       quiet even when their average lies near a newly learned noise floor. */
    for (unsigned band=0;band<3;band++) {
        double low=0.,high=-160.;
        for (size_t i=0;i<10;i++) {
            size_t index=(p->history_position+PAUSE_HISTORY-1-i)%PAUSE_HISTORY;
            double level=p->history[band][index];
            low=fmin(low,level);high=fmax(high,level);
        }
        if (high-low>12.) return 0;
    }
    double sensitivity=e->config.silence_sensitivity;
    double allowance=2.+4.*sensitivity,margin=14.-6.*sensitivity;
    if (instant>p->speech_reference-margin || levels[0]>p->speech_reference-margin) return 0;
    for (unsigned band=0;band<3;band++) if (levels[band]>p->noise[band]+allowance) return 0;
    return !has_voicing(p);
}
static void analyze_pause_frame(SBEngine *e,size_t n) {
    PauseFrame *frame=&e->pause_frames[(e->pause_head+e->pause_count)%PAUSE_QUEUE];
    int quiet=1;
    for (unsigned c=0;c<e->channels;c++) {
        int channel_quiet=pause_channel_quiet(e,c,e->raw[c]+e->pause_analyzed,n);
        if (!channel_quiet) quiet=0;
    }
    /* An incomplete EOF window is retained without shortening. */
    if (n!=e->pause_hop) quiet=0;
    *frame=(PauseFrame){n,e->pause_index++,quiet,0};
    e->pause_count++;
    e->pause_analyzed+=n;
    if (!quiet) { e->quiet_frames=0;return; }
    if (!e->quiet_frames) e->quiet_start=frame->index;
    if (e->quiet_frames<PAUSE_CONFIRM) e->quiet_frames++;
    if (e->quiet_frames<PAUSE_CONFIRM) return;
    for (size_t i=0;i<e->pause_count;i++) {
        PauseFrame *candidate=&e->pause_frames[(e->pause_head+i)%PAUSE_QUEUE];
        if (candidate->quiet && candidate->index>=e->quiet_start+PAUSE_GUARD &&
            candidate->index+PAUSE_GUARD<=frame->index) candidate->cut=1;
    }
}

SBEngineConfig sb_engine_default_config(void) {
    SBEngineConfig c;
    memset(&c, 0, sizeof c);
    c.rate = 1; c.smart_speed = 1; c.voice_boost = 1; c.target_lufs = -14; c.silence_sensitivity = .5f;
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
    if (!e || !c || !isfinite(c->rate) || c->rate < .5f || c->rate > 5 || c->eq_count > 12 ||
        !isfinite(c->silence_sensitivity) || c->silence_sensitivity < 0 || c->silence_sensitivity > 1) return 0;
    if (!sb_stretcher_set_rate(e->stretch, c->rate)) return 0;
    sb_dsp_config d;
    memset(&d, 0, sizeof d);
    d.voice_boost = c->voice_boost; d.mono = c->mono; d.noise_reduction = c->noise_reduction;
    d.normalization_mode = c->normalization_mode; d.target_lufs = c->target_lufs; d.deesser = c->deesser;
    d.eq_count = c->eq_count;
    for (size_t i=0;i<d.eq_count;i++) d.eq[i]=(sb_dsp_eq_band){c->eq[i].type,c->eq[i].frequency,c->eq[i].q,c->eq[i].gain_db};
    sb_dsp_configure(e->dsp, &d);
    if (e->config.smart_speed!=c->smart_speed || e->config.silence_sensitivity!=c->silence_sensitivity)
        reset_pause_detector(e);
    e->config = *c;
    return 1;
}
SBEngine *sb_engine_create(double fs, unsigned channels, size_t max_input) {
    if (!isfinite(fs) || fs < 8000 || fs > 192000 || channels < 1 || channels > 2 || !max_input || max_input > 1048576) return NULL;
    SBEngine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->channels = channels;e->sample_rate=fs;
    e->pause_hop=(size_t)llround(fs*.01);
    e->post_capacity=(PAUSE_LOOKAHEAD+2)*e->pause_hop+BLOCK*2;
    e->high_pass_alpha=exp(-2.*3.141592653589793*120./fs);
    e->speech_low_alpha=exp(-2.*3.141592653589793*fmin(3500.,fs*.4)/fs);
    e->voicing_low_alpha=exp(-2.*3.141592653589793*1000./fs);
    for (unsigned c=0;c<channels;c++) e->post[c]=calloc(e->post_capacity,sizeof(float));
    e->dsp = sb_dsp_create(fs, channels, BLOCK);
    e->raw_capacity=e->post_capacity+sb_dsp_latency(e->dsp)+BLOCK;
    for (unsigned c=0;c<channels;c++) e->raw[c]=calloc(e->raw_capacity,sizeof(float));
    size_t capacity = max_input * 4 + (size_t)ceil(fs * .5) + BLOCK * 4;
    e->stretch = sb_stretcher_create(fs, channels, capacity);
    if (!e->dsp || !e->stretch || !e->post[0] || !e->raw[0] || (channels==2&&(!e->post[1]||!e->raw[1]))) { sb_engine_destroy(e); return NULL; }
    SBEngineConfig c=sb_engine_default_config();
    if (!sb_engine_configure(e,&c)) { sb_engine_destroy(e); return NULL; }
    return e;
}
void sb_engine_destroy(SBEngine *e) {
    if (!e) return;
    sb_dsp_destroy(e->dsp); sb_stretcher_destroy(e->stretch); free(e->post[0]);free(e->post[1]);free(e->raw[0]);free(e->raw[1]);free(e);
}
void sb_engine_reset(SBEngine *e, int64_t frame) {
    if (!e) return;
    sb_dsp_reset(e->dsp); sb_stretcher_reset(e->stretch);
    e->base = frame < 0 ? 0 : (double)frame;
    e->processed=e->smart_output=0;
    e->post_count=e->raw_count=e->cut_count=e->cut_offset=0;
    e->finished=e->dsp_done=e->drained=0;
    reset_pause_detector(e);
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
/* A confirmed pause keeps its leading/trailing guards. Only its interior is
   shortened, using a sample-rate-scaled complementary splice. */
static size_t shorten_confirmed_pause(SBEngine *e,size_t n) {
    size_t window=e->pause_hop/3;
    size_t blend=(size_t)llround(46.*e->sample_rate/44100.);
    if (blend<2) blend=2;
    if (window<=blend) return n;
    size_t cut=window-blend,cursor=0,removed=0;
    while (cursor+window<=n) {
        size_t dst=cursor-removed;
        for (unsigned c=0;c<e->channels;c++) {
            for (size_t j=0;j<blend;j++) {
                float a=(float)j/(float)(blend-1);
                e->work[c][dst+j]=e->work[c][dst+j]*(1-a)+e->work[c][dst+cut+j]*a;
            }
            memmove(e->work[c]+dst+blend,e->work[c]+dst+window,(n-cursor-window)*sizeof(float));
        }
        memmove(e->source+dst,e->source+dst+cut,(n-cursor-cut)*sizeof(double));
        removed+=cut;cursor+=window;
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
        int adaptive=e->config.smart_speed && e->config.silence_sensitivity>0;
        size_t n;int cut=0;
        if (adaptive) {
            while (e->pause_count<=PAUSE_LOOKAHEAD && e->pause_analyzed+e->pause_hop<=e->post_count)
                analyze_pause_frame(e,e->pause_hop);
            if (tail && e->pause_count<=PAUSE_LOOKAHEAD && e->pause_analyzed<e->post_count)
                analyze_pause_frame(e,e->post_count-e->pause_analyzed);
            if (!(e->pause_count>PAUSE_LOOKAHEAD || (tail&&e->pause_count))) return 1;
            PauseFrame frame=e->pause_frames[e->pause_head];
            n=frame.frames;cut=frame.cut;
            e->pause_head=(e->pause_head+1)%PAUSE_QUEUE;e->pause_count--;
            e->pause_analyzed-=n;
        } else {
            if (e->post_count<BLOCK && !(tail && e->post_count)) return 1;
            n=e->post_count<BLOCK?e->post_count:BLOCK;
        }
        for (unsigned c=0;c<e->channels;c++) memcpy(e->work[c],e->post[c],n*sizeof(float));
        for (size_t i=0;i<n;i++) e->source[i]=e->base+(double)e->processed+(double)i;
        e->processed+=n;
        size_t out=adaptive?(cut?shorten_confirmed_pause(e,n):n):shorten(e,n);
        for (size_t i=0;i<out;i++) e->saved[i]=fmax(0,e->source[i]-e->base-(double)e->smart_output-(double)i);
        e->smart_output+=out;
        e->cut_count=out;
        e->post_count-=n;
        e->raw_count-=n;
        for (unsigned c=0;c<e->channels;c++) {
            memmove(e->post[c],e->post[c]+n,e->post_count*sizeof(float));
            memmove(e->raw[c],e->raw[c]+n,e->raw_count*sizeof(float));
        }
    }
}
size_t sb_engine_push(SBEngine *e,const float *l,const float *r,size_t frames) {
    if (!e || e->finished || !l || (e->channels==2&&!r)) return 0;
    size_t accepted=0;
    while (accepted<frames) {
        if (!dispatch(e,0)) break;
        size_t n=frames-accepted; if (n>BLOCK) n=BLOCK;
        if (e->post_count+n>e->post_capacity || e->raw_count+n>e->raw_capacity) break;
        const float *in[2]={l+accepted,r?r+accepted:l+accepted};
        float *out[2]={e->post[0]+e->post_count,e->post[e->channels==2?1:0]+e->post_count};
        /* Keep original-input analysis aligned with DSP output's source frame zero.
           Processing controls cannot change which pauses the detector recognizes. */
        for (unsigned c=0;c<e->channels;c++) memcpy(e->raw[c]+e->raw_count,in[c],n*sizeof(float));
        e->raw_count+=n;
        size_t produced=sb_dsp_process(e->dsp,in,n,out,e->post_capacity-e->post_count);
        e->post_count+=produced; accepted+=n;
    }
    (void)dispatch(e,0);
    return accepted;
}
void sb_engine_finish(SBEngine *e) { if(e) e->finished=1; }
static int flush_dsp(SBEngine *e) {
    if (!dispatch(e,0)) return 0;
    if (!e->dsp_done) {
        float *out[2]={e->post[0]+e->post_count,e->post[e->channels==2?1:0]+e->post_count};
        size_t n=sb_dsp_flush(e->dsp,out,e->post_capacity-e->post_count);
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
size_t sb_engine_latency(const SBEngine *e) {
    if (!e) return 0;
    size_t detection=e->config.smart_speed&&e->config.silence_sensitivity>0?PAUSE_LOOKAHEAD*e->pause_hop:0;
    return sb_dsp_latency(e->dsp)+detection;
}
