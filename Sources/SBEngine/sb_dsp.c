#include "sb_dsp.h"
#include "sb_fft.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define N 1024u
#define H 256u
#define OLA 4096u
#define BANDS (SB_DSP_MAX_EQ_BANDS + 4)
#define PI 3.14159265358979323846

typedef struct { double b0,b1,b2,a1,a2,z1[2],z2[2]; } biquad;
typedef struct { biquad band[BANDS]; size_t count; } eq_chain;
struct sb_dsp {
    double fs; unsigned channels; size_t max_frames, lookahead, latency;
    sb_dsp_config config;
    eq_chain eq, old_eq; size_t eq_fade, eq_fade_length;
    biquad weight_shelf, weight_hp, bright_hp, reference_hp, deess_shelf;
    double window_power[N];
    double input[2][N], ola[2][OLA], re[2][N], im[2][N], window[N];
    double noise[N/2+1], noise_gain[N/2+1]; int noise_initialized;
    double noise_mix, mono_mix, vb_mix;
    sb_fft_plan fft;
    double smooth20, smooth50, smooth10, noise_rate, limiter_ceiling;
    double *meter, meter_sum; size_t meter_size, meter_pos, meter_fill, meter_step;
    double history[25], history_sum, highest, loudness; size_t hist_count,hist_pos;
    double norm_gain, norm_target, norm_step; size_t norm_remaining;
    double comp_db, bright_energy, ref_energy, deess_db, shelf_frequency, shelf_gain;
    double adaptive_energy, adaptive_diff, prev_bright[2]; size_t adaptive_count;
    double *delay[2], *peak_value; uint64_t *peak_index;
    size_t peak_head,peak_tail,peak_capacity,delay_position;
    double limiter_gain;
    uint64_t ticks, accepted, emitted, dynamics_frames;
    int flushing, mono_fast;
};
static size_t next_index(size_t i,size_t capacity) {return i+1==capacity?0:i+1;}
static double clamp(double x,double a,double b) { return x<a?a:x>b?b:x; }
static double amplitude(double db) { return pow(10., db/20.); }
static double db(double x) { return 20.*log10(fmax(x,1.e-12)); }
static double clean(float x) { return isfinite(x)?clamp(x,-64.,64.):0.; }
static void identity(biquad *b) { memset(b,0,sizeof(*b)); b->b0=1.; }
static int sanitize_band(sb_dsp_eq_band *b,double fs) {
    if(b->type<0||b->type>6||!isfinite(b->frequency)||!isfinite(b->q)||!isfinite(b->gain_db))return 0;
    b->frequency=(float)clamp(b->frequency,10.,fs*.475);
    b->q=(float)clamp(b->q,.1,12.);
    b->gain_db=(float)clamp(b->gain_db,-24.,24.);
    return 1;
}
/* RBJ cookbook transfer functions; q is Q, including shelves' shape control. */
static void coefficients(biquad *b,int type,double f,double q,double gain,double fs) {
    f=clamp(f,10.,fs*.475); q=clamp(q,.1,12.); gain=clamp(gain,-24.,24.);
    double w=2.*PI*f/fs,c=cos(w),s=sin(w),a=s/(2.*q),A=pow(10.,gain/40.);
    double b0=1,b1=0,b2=0,a0=1,a1=0,a2=0;
    switch(type) {
    case 0: b0=1+a*A;b1=-2*c;b2=1-a*A;a0=1+a/A;a1=-2*c;a2=1-a/A;break;
    case 1: b0=(1-c)/2;b1=1-c;b2=b0;a0=1+a;a1=-2*c;a2=1-a;break;
    case 2: b0=(1+c)/2;b1=-(1+c);b2=b0;a0=1+a;a1=-2*c;a2=1-a;break;
    case 3: b0=a;b1=0;b2=-a;a0=1+a;a1=-2*c;a2=1-a;break;
    case 4: b0=1;b1=-2*c;b2=1;a0=1+a;a1=-2*c;a2=1-a;break;
    case 5: { double t=2*sqrt(A)*a; b0=A*((A+1)-(A-1)*c+t); b1=2*A*((A-1)-(A+1)*c); b2=A*((A+1)-(A-1)*c-t); a0=(A+1)+(A-1)*c+t;a1=-2*((A-1)+(A+1)*c);a2=(A+1)+(A-1)*c-t;break; }
    case 6: { double t=2*sqrt(A)*a; b0=A*((A+1)+(A-1)*c+t); b1=-2*A*((A-1)+(A+1)*c);b2=A*((A+1)+(A-1)*c-t);a0=(A+1)-(A-1)*c+t;a1=2*((A-1)-(A+1)*c);a2=(A+1)-(A-1)*c-t;break; }
    default: break;
    }
    b->b0=b0/a0;b->b1=b1/a0;b->b2=b2/a0;b->a1=a1/a0;b->a2=a2/a0;
}
double sb_dsp_custom_eq_gain_db(const sb_dsp_config *config,double fs,double frequency) {
    if(!config||config->eq_count>SB_DSP_MAX_EQ_BANDS||!isfinite(fs)||fs<8000.||fs>192000.||
       !isfinite(frequency)||frequency<=0.||frequency>=fs*.5)return NAN;
    double w=2.*PI*frequency/fs,c1=cos(w),s1=sin(w),c2=cos(2.*w),s2=sin(2.*w),gain=0.;
    for(size_t i=0;i<config->eq_count;i++) {
        sb_dsp_eq_band band=config->eq[i];
        if(!sanitize_band(&band,fs))continue;
        biquad b={0};
        coefficients(&b,band.type,band.frequency,band.q,band.gain_db,fs);
        double numerator=hypot(b.b0+b.b1*c1+b.b2*c2,b.b1*s1+b.b2*s2);
        double denominator=hypot(1.+b.a1*c1+b.a2*c2,b.a1*s1+b.a2*s2);
        gain+=20.*log10(numerator/denominator);
    }
    return gain;
}
static double filter(biquad *b,unsigned c,double x) {
    double y=b->b0*x+b->z1[c];
    b->z1[c]=b->b1*x-b->a1*y+b->z2[c];b->z2[c]=b->b2*x-b->a2*y;
    if(fabs(b->z1[c])<1.e-24)b->z1[c]=0;
    if(fabs(b->z2[c])<1.e-24)b->z2[c]=0;
    return y;
}
static double equalize(eq_chain *eq,unsigned c,double x) { for(size_t i=0;i<eq->count;i++)x=filter(&eq->band[i],c,x);return x; }
static void make_eq(sb_dsp *d,eq_chain *eq) {
    memset(eq,0,sizeof(*eq));
    for(size_t i=0;i<d->config.eq_count;i++) {
        sb_dsp_eq_band *b=&d->config.eq[i];
        coefficients(&eq->band[eq->count++],b->type,b->frequency,b->q,b->gain_db,d->fs);
    }
    if(d->config.voice_boost) {
        coefficients(&eq->band[eq->count++],2,80,.5,0,d->fs);
        coefficients(&eq->band[eq->count++],0,1635,.8,-3,d->fs);
        coefficients(&eq->band[eq->count++],0,9000,1.4,-4,d->fs);
        coefficients(&eq->band[eq->count++],1,16000,.5,0,d->fs);
    }
}
/* 1024-point sqrt-Hann analysis/synthesis, 256-frame hop. Summed squared
   windows equal two. One real nonnegative gain per bin is shared by channels,
   preserving their phase relationships. Negative-start frames include zeros. */
/* Keep the per-hop FFT scratch frame out of the per-sample tick stack. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void spectral_frame(sb_dsp *d,uint64_t end) {
    int packed=0;
#ifdef SB_FFT_ACCELERATE
    packed=d->channels==2&&d->fft.accelerated;
#endif
    if(packed) {
        /* A complex FFT carries both real channels. Paired bin energies
         * recover their joint power without changing the stereo gain. */
        for(unsigned i=0;i<N;i++) {
            d->re[0][i]=d->input[0][(end+i)%N]*d->window[i];
            d->im[0][i]=d->input[1][(end+i)%N]*d->window[i];
        }
        sb_fft_transform(&d->fft,d->re[0],d->im[0],0);
    } else for(unsigned c=0;c<d->channels;c++) {
        for(unsigned i=0;i<N;i++) {d->re[c][i]=d->input[c][(end+i)%N]*d->window[i];d->im[c][i]=0;}
        sb_fft_transform(&d->fft,d->re[c],d->im[c],0);
    }
    double power[N/2+1];
    for(unsigned k=0;k<=N/2;k++) {
        if(packed) {
            unsigned mirror=(N-k)%N;
            power[k]=(d->re[0][k]*d->re[0][k]+d->im[0][k]*d->im[0][k]
                     +d->re[0][mirror]*d->re[0][mirror]+d->im[0][mirror]*d->im[0][mirror])*.25;
        } else {power[k]=0;for(unsigned c=0;c<d->channels;c++)power[k]+=d->re[c][k]*d->re[c][k]+d->im[c][k]*d->im[c][k];power[k]/=d->channels;}
    }
    int dry=d->noise_mix==0.;
    for(unsigned k=0;k<=N/2;k++) {
        unsigned lo=k>2?k-2:0,hi=k+2<N/2?k+2:N/2;double neighbors=0;unsigned count=0;
        for(unsigned j=lo;j<=hi;j++)if(j!=k){neighbors+=power[j];count++;}
        double tonal=power[k]/(neighbors/fmax(1.,count)+1.e-18);
        if(!d->noise_initialized)d->noise[k]=power[k]*(tonal>3.? .04:1.);
        else {double rate=power[k]<d->noise[k]?.15:.008;d->noise[k]+=rate*(power[k]-d->noise[k]);}
        double raw=sqrt(clamp(1.-1.5*d->noise[k]/(power[k]+1.e-18),.025,1.));
        if(tonal>3.)raw=fmax(raw,.90);
        double rate=raw>d->noise_gain[k]?.3:.65;
        d->noise_gain[k]+=rate*(raw-d->noise_gain[k]);
        double g=1.+d->noise_mix*(d->noise_gain[k]-1.);
        if(!dry) for(unsigned c=0;c<(packed?1:d->channels);c++) {d->re[c][k]*=g;d->im[c][k]*=g;if(k>0&&k<N/2){d->re[c][N-k]*=g;d->im[c][N-k]*=g;}}
    }
    d->noise_initialized=1;
    if(packed&&!dry)sb_fft_transform(&d->fft,d->re[0],d->im[0],1);
    for(unsigned c=0;c<d->channels;c++) {
        if(dry) {
            /* Keep pending unity OLA history for an immediate NR-on transition. */
            for(unsigned i=0;i<N;i++)if(end+i>=N) {uint64_t source=end+i-N;d->ola[c][source%OLA]+=d->input[c][(end+i)%N]*d->window_power[i];}
        } else {
            if(!packed)sb_fft_transform(&d->fft,d->re[c],d->im[c],1);
            const double *wave=packed?(c?d->im[0]:d->re[0]):d->re[c];
            for(unsigned i=0;i<N;i++)if(end+i>=N) {uint64_t source=end+i-N;d->ola[c][source%OLA]+=wave[i]*d->window[i]*.5;}
        }
    }
}
static int normalize_enabled(const sb_dsp *d) {return d->config.normalization_mode==2||(d->config.normalization_mode==0&&d->config.voice_boost);}
static double normalization_gain(const sb_dsp *d,double level) {
    if(!normalize_enabled(d)||level<=-36.)return 1.;
    double target=d->config.normalization_mode==2?d->config.target_lufs:-14.;
    return amplitude(clamp(target-level,-40.,20.));
}
/* A stream reset with mono enabled and dry NR has identical downstream
 * channels. Keep one recurrence per biquad; restore its twin before a setting
 * or NR transition can make the channels differ. No audio approximation. */
static void mirror_filter(biquad *b) {b->z1[1]=b->z1[0];b->z2[1]=b->z2[0];}
static void mirror_mono_state(sb_dsp *d) {
    for(size_t i=0;i<d->eq.count;i++)mirror_filter(&d->eq.band[i]);
    for(size_t i=0;i<d->old_eq.count;i++)mirror_filter(&d->old_eq.band[i]);
    mirror_filter(&d->weight_shelf);mirror_filter(&d->weight_hp);
    mirror_filter(&d->bright_hp);mirror_filter(&d->reference_hp);mirror_filter(&d->deess_shelf);
    d->prev_bright[1]=d->prev_bright[0];
}
static void meter_sample(sb_dsp *d,const double x[2]) {
    double sum=0;
    for(unsigned c=0;c<(d->mono_fast?1:d->channels);c++){double v=filter(&d->weight_hp,c,filter(&d->weight_shelf,c,x[c]));sum+=v*v;}
    if(d->mono_fast)sum+=sum;
    /* Energy sum avoids the anti-phase cancellation of a mono analysis feed. */
    d->meter_sum+=sum-d->meter[d->meter_pos];d->meter[d->meter_pos]=sum;d->meter_pos=next_index(d->meter_pos,d->meter_size);
    if(d->meter_fill<d->meter_size)d->meter_fill++;
    if(++d->meter_step<(size_t)(d->fs*.1)||d->meter_fill<d->meter_size)return;
    d->meter_step=0;double l=-.691+10.*log10(fmax(1.e-16,d->meter_sum/d->meter_size));
    if(l>-70.&&l>d->highest-18.) {
        if(l>d->highest)d->highest=l;
        if(d->hist_count<25)d->hist_count++;else d->history_sum-=d->history[d->hist_pos];
        d->history[d->hist_pos]=l;d->history_sum+=l;d->hist_pos=(d->hist_pos+1)%25;d->loudness=d->history_sum/d->hist_count;
    }
    double gain=normalization_gain(d,d->loudness);
    d->norm_target=gain;d->norm_remaining=(size_t)ceil(.05*d->fs);d->norm_step=(gain-d->norm_gain)/d->norm_remaining;
}
static void dynamics(sb_dsp *d,double x[2]) {
    if(d->mono_fast&&d->noise_mix!=0.){mirror_mono_state(d);d->mono_fast=0;}
    double smooth=d->smooth20;
    d->mono_mix+=smooth*((d->config.mono?1.:0.)-d->mono_mix);
    double mono=(x[0]+(d->channels==2?x[1]:x[0]))*.5;
    if(d->mono_fast) {
        double left=x[0]+(mono-x[0])*d->mono_mix;
        double right=x[1]+(mono-x[1])*d->mono_mix;
        /* Preserve the legacy arithmetic even for exponent extremes, where
         * two mathematically equal mono blends can round differently. */
        if(memcmp(&left,&right,sizeof(left))!=0) {mirror_mono_state(d);d->mono_fast=0;}
    }
    unsigned channels=d->mono_fast?1:d->channels;
    double cross=d->eq_fade_length?1.-(double)d->eq_fade/d->eq_fade_length:1.;
    for(unsigned c=0;c<channels;c++) {if(d->mono_mix!=0.)x[c]+=(mono-x[c])*d->mono_mix;double y=equalize(&d->eq,c,x[c]);if(d->eq_fade)y=cross*y+(1.-cross)*equalize(&d->old_eq,c,x[c]);x[c]=y;}
    if(d->eq_fade)d->eq_fade--;
    meter_sample(d,x);
    if(d->norm_remaining){d->norm_gain+=d->norm_step;if(!--d->norm_remaining)d->norm_gain=d->norm_target;}
    double peak=0;for(unsigned c=0;c<channels;c++){x[c]*=d->norm_gain;peak=fmax(peak,fabs(x[c]));}
    double level=db(peak),over=level+18.,reduction=over<=-1.?0.:over>=1.?(1./2.25-1.)*over:(1./2.25-1.)*(over+1.)*(over+1.)/4.;
    double rate=reduction<d->comp_db?d->smooth20:d->smooth50;d->comp_db+=rate*(reduction-d->comp_db);
    d->vb_mix+=smooth*((d->config.voice_boost?1.:0.)-d->vb_mix);
    double cg=amplitude((d->comp_db+4.)*d->vb_mix);
    double be=0,re=0;
    for(unsigned c=0;c<channels;c++){x[c]*=cg;double b=filter(&d->bright_hp,c,x[c]),r=filter(&d->reference_hp,c,x[c]);be+=b*b;re+=r*r;double delta=b-d->prev_bright[c];d->prev_bright[c]=b;d->adaptive_diff+=delta*delta;if(d->mono_fast)d->adaptive_diff+=delta*delta;}
    if(d->mono_fast){be+=be;re+=re;}
    double env=d->smooth10;d->bright_energy+=env*(be-d->bright_energy);d->ref_energy+=env*(re-d->ref_energy);
    double relative=10.*log10((d->bright_energy+1.e-18)/(d->ref_energy+1.e-18));
    double threshold=d->config.deesser==2?-9.:-6.75, maximum=d->config.deesser==2?13.:10.;
    double wanted=(d->config.deesser&&d->ref_energy>1.e-9)?-clamp(relative-threshold,0.,maximum):0.;
    rate=wanted<d->deess_db?d->smooth20:d->smooth10;d->deess_db+=rate*(wanted-d->deess_db);
    d->adaptive_energy+=be;
    if(++d->adaptive_count>=H){
        if(relative>threshold&&d->adaptive_energy>1.e-15){double f=d->fs/PI*asin(fmin(1.,.5*sqrt(d->adaptive_diff/d->adaptive_energy)));d->shelf_frequency+=.25*(clamp(.75*f,3000.,fmin(9000.,d->fs*.45))-d->shelf_frequency);}
        d->adaptive_count=0;d->adaptive_energy=d->adaptive_diff=0;
    }
    if(fabs(d->deess_db-d->shelf_gain)>.1||d->dynamics_frames%H==0){coefficients(&d->deess_shelf,6,d->shelf_frequency,.707,d->deess_db,d->fs);d->shelf_gain=d->deess_db;}
    if(d->config.deesser||fabs(d->deess_db)>1.e-8) {
        for(unsigned c=0;c<channels;c++)x[c]=filter(&d->deess_shelf,c,x[c]);
    } else {
        memset(d->deess_shelf.z1,0,sizeof(d->deess_shelf.z1));
        memset(d->deess_shelf.z2,0,sizeof(d->deess_shelf.z2));
    }
    if(d->mono_fast)x[1]=x[0];
}
/* Each real or drain tick advances exactly one sample. OLA samples are read
   after all four contributing frames finish; the following linked limiter
   delays them another lookahead frames. No leading zero padding escapes. */
static int tick(sb_dsp *d,double left,double right,float *out_left,float *out_right) {
    uint64_t t=d->ticks++;
    double dry[2]={0,0};
    for(unsigned c=0;c<d->channels;c++)dry[c]=d->input[c][t%N];
    d->input[0][t%N]=left;
    if(d->channels==2)d->input[1][t%N]=right;
    if((t+1)%H==0){d->noise_mix+=d->noise_rate*(d->config.noise_reduction-d->noise_mix);spectral_frame(d,t+1);}
    if(t<N)return 0;
    double x[2]={0,0};for(unsigned c=0;c<d->channels;c++){x[c]=d->ola[c][(t-N)%OLA];d->ola[c][(t-N)%OLA]=0;if(d->noise_mix==0.)x[c]=dry[c];}
    dynamics(d,x);uint64_t j=d->dynamics_frames++;
    size_t write=d->delay_position;
    d->delay_position=next_index(write,d->lookahead+1);
    double peak=0;for(unsigned c=0;c<d->channels;c++){d->delay[c][write]=x[c];peak=fmax(peak,fabs(x[c]));}
    while(d->peak_head!=d->peak_tail&&d->peak_index[d->peak_head]+d->lookahead<j)d->peak_head=next_index(d->peak_head,d->peak_capacity);
    while(d->peak_head!=d->peak_tail){size_t prev=d->peak_tail?d->peak_tail-1:d->peak_capacity-1;if(d->peak_value[prev]>peak)break;d->peak_tail=prev;}
    d->peak_value[d->peak_tail]=peak;d->peak_index[d->peak_tail]=j;d->peak_tail=next_index(d->peak_tail,d->peak_capacity);
    if(j<d->lookahead)return 0;
    double required=fmin(1.,d->limiter_ceiling/fmax(d->peak_value[d->peak_head],1.e-20));
    double release=d->smooth10;d->limiter_gain=fmin(required,d->limiter_gain+release*(1.-d->limiter_gain));
    *out_left=(float)(d->delay[0][d->delay_position]*d->limiter_gain);
    if(d->channels==2)*out_right=(float)(d->delay[1][d->delay_position]*d->limiter_gain);
    return 1;
}

sb_dsp *sb_dsp_create(double fs,unsigned channels,size_t max_frames) {
    if(!isfinite(fs)||fs<8000||fs>192000||channels<1||channels>2||!max_frames||max_frames>1048576)return NULL;
    sb_dsp *d=calloc(1,sizeof(*d));if(!d)return NULL;
    d->fs=fs;d->channels=channels;d->max_frames=max_frames;d->lookahead=(size_t)ceil(.01*fs);d->latency=N+d->lookahead;d->peak_capacity=d->lookahead+2;d->meter_size=(size_t)floor(.4*fs);
    sb_fft_prepare(&d->fft);
    d->smooth20=1.-exp(-1./(.02*fs));d->smooth50=1.-exp(-1./(.05*fs));d->smooth10=1.-exp(-1./(.01*fs));
    d->noise_rate=1.-exp(-(double)H/(.05*fs));d->limiter_ceiling=amplitude(-.7);
    d->meter=calloc(d->meter_size,sizeof(double));
    d->delay[0]=calloc(d->lookahead+1,sizeof(double));d->delay[1]=calloc(d->lookahead+1,sizeof(double));
    d->peak_value=calloc(d->peak_capacity,sizeof(double));d->peak_index=calloc(d->peak_capacity,sizeof(uint64_t));
    if(!d->meter||!d->delay[0]||!d->delay[1]||!d->peak_value||!d->peak_index){sb_dsp_destroy(d);return NULL;}
    for(unsigned i=0;i<N;i++){d->window[i]=sin(PI*i/N);d->window_power[i]=d->window[i]*d->window[i]*.5;}
    sb_dsp_reset(d);return d;
}
void sb_dsp_destroy(sb_dsp *d){if(d){sb_fft_destroy(&d->fft);free(d->meter);free(d->delay[0]);free(d->delay[1]);free(d->peak_value);free(d->peak_index);free(d);}}
void sb_dsp_configure(sb_dsp *d,const sb_dsp_config *config) {
    if(!d||!config)return;
    if(d->mono_fast)mirror_mono_state(d);
    sb_dsp_config c=*config;c.voice_boost=!!c.voice_boost;c.mono=!!c.mono;
    c.noise_reduction=isfinite(c.noise_reduction)?(float)clamp(c.noise_reduction,0,1):0;
    c.normalization_mode=c.normalization_mode>=0&&c.normalization_mode<=2?c.normalization_mode:1;
    c.target_lufs=isfinite(c.target_lufs)?(float)clamp(c.target_lufs,-20,-6):-14;
    c.deesser=c.deesser>=0&&c.deesser<=2?c.deesser:0;c.eq_count=c.eq_count>SB_DSP_MAX_EQ_BANDS?SB_DSP_MAX_EQ_BANDS:c.eq_count;
    size_t valid=0;for(size_t i=0;i<c.eq_count;i++){sb_dsp_eq_band b=c.eq[i];if(sanitize_band(&b,d->fs))c.eq[valid++]=b;}c.eq_count=valid;
    int changed=c.voice_boost!=d->config.voice_boost||c.eq_count!=d->config.eq_count;
    for(size_t i=0;i<c.eq_count&&!changed;i++)changed=c.eq[i].type!=d->config.eq[i].type||c.eq[i].frequency!=d->config.eq[i].frequency||c.eq[i].q!=d->config.eq[i].q||c.eq[i].gain_db!=d->config.eq[i].gain_db;
    if(changed)d->old_eq=d->eq;
    d->config=c;
    if(!c.mono)d->mono_fast=0;
    if(changed){make_eq(d,&d->eq);d->eq_fade_length=(size_t)ceil(.05*d->fs);d->eq_fade=d->eq_fade_length;}
    if(!normalize_enabled(d)){d->norm_target=1;d->norm_remaining=(size_t)ceil(.05*d->fs);d->norm_step=(1.-d->norm_gain)/d->norm_remaining;}
}
void sb_dsp_reset(sb_dsp *d) {
    if(!d)return;
    memset(d->input,0,sizeof(d->input));memset(d->ola,0,sizeof(d->ola));memset(d->noise,0,sizeof(d->noise));
    for(unsigned i=0;i<=N/2;i++)d->noise_gain[i]=1.;
    memset(d->meter,0,d->meter_size*sizeof(double));memset(d->history,0,sizeof(d->history));
    for(unsigned c=0;c<2;c++)memset(d->delay[c],0,(d->lookahead+1)*sizeof(double));
    /* Start wet amount at zero so estimator warmup cannot erase first voice. */
    d->mono_fast=d->channels==2&&d->config.mono&&d->config.noise_reduction==0;
    d->noise_initialized=0;d->noise_mix=0;d->mono_mix=d->config.mono?1:0;d->vb_mix=d->config.voice_boost?1:0;
    d->meter_sum=0;d->meter_pos=d->meter_fill=d->meter_step=d->hist_count=d->hist_pos=0;d->history_sum=0;d->highest=d->loudness=-160;
    d->norm_gain=d->norm_target=1;d->norm_step=0;d->norm_remaining=0;d->comp_db=d->bright_energy=d->ref_energy=d->deess_db=0;
    d->shelf_frequency=fmin(6000.,d->fs*.4);d->shelf_gain=0;d->adaptive_energy=d->adaptive_diff=0;memset(d->prev_bright,0,sizeof(d->prev_bright));d->adaptive_count=0;
    d->delay_position=0;
    d->peak_head=d->peak_tail=0;d->limiter_gain=1;d->ticks=d->accepted=d->emitted=d->dynamics_frames=0;d->flushing=0;d->eq_fade=d->eq_fade_length=0;
    make_eq(d,&d->eq);memset(&d->old_eq,0,sizeof(d->old_eq));
    identity(&d->weight_shelf);identity(&d->weight_hp);identity(&d->bright_hp);identity(&d->reference_hp);identity(&d->deess_shelf);
    coefficients(&d->weight_shelf,6,1684,.707,4,d->fs);coefficients(&d->weight_hp,2,100,.5,0,d->fs);
    coefficients(&d->bright_hp,2,4000,.707,0,d->fs);coefficients(&d->reference_hp,2,120,.5,0,d->fs);
}
void sb_dsp_seed_loudness(sb_dsp *d,float level) {
    if(!d||!isfinite(level)||level<-70.f||level>0.f)return;
    memset(d->history,0,sizeof(d->history));
    d->history[0]=level;
    d->history_sum=d->highest=d->loudness=level;
    d->hist_count=1;
    d->hist_pos=1;
    d->norm_gain=d->norm_target=normalization_gain(d,level);
    d->norm_step=0;
    d->norm_remaining=0;
}
size_t sb_dsp_process(sb_dsp *d,const float *const in[2],size_t n,float *const out[2],size_t cap) {
    if(!d||d->flushing||!in||!out||n>d->max_frames||cap<n)return 0;
    for(unsigned c=0;c<d->channels;c++)if(!in[c]||!out[c])return 0;
    size_t produced=0;d->accepted+=n;
    unsigned channels=d->channels;
    for(size_t i=0;i<n;i++) {
        /* Read both source channels before writing: planar output may alias
         * either source channel, as in the original serialized sample loop. */
        double left=clean(in[0][i]),right=channels==2?clean(in[1][i]):0.;
        if(tick(d,left,right,out[0]+produced,channels==2?out[1]+produced:NULL)){produced++;d->emitted++;}
    }
    return produced;
}
size_t sb_dsp_flush(sb_dsp *d,float *const out[2],size_t cap) {
    if(!d||!out||!cap)return 0;for(unsigned c=0;c<d->channels;c++)if(!out[c])return 0;
    d->flushing=1;size_t produced=0;unsigned channels=d->channels;
    while(produced<cap&&d->emitted<d->accepted){if(tick(d,0.,0.,out[0]+produced,channels==2?out[1]+produced:NULL)){produced++;d->emitted++;}}
    return produced;
}
size_t sb_dsp_latency(const sb_dsp *d){return d?d->latency:0;}
float sb_dsp_loudness_db(const sb_dsp *d){return d?(float)d->loudness:-160;}
float sb_dsp_smart_threshold(const sb_dsp *d){
    if(!d)return (float)amplitude(-48);
    /* Recovered controller adds its LINEAR makeup field in the dB expression. */
    double makeup=d->config.voice_boost?amplitude(4.):0.;
    double threshold=normalize_enabled(d)&&d->loudness>-70.?d->loudness+db(d->norm_target)+makeup-30.:-48.-makeup;
    return (float)amplitude(clamp(threshold,-100.,-20.));
}
