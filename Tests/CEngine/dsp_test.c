/* Standalone: clang -std=c11 -O1 -g -fsanitize=address,undefined
   -fno-omit-frame-pointer -Wall -Wextra -Werror -pedantic
   -I Packages/SBEngine/Sources/SBEngine Packages/SBEngine/Tests/dsp_test.c
   Packages/SBEngine/Sources/SBEngine/sb_dsp.c -lm -o /tmp/sb-dsp-test */
#include "sb_dsp.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FS 48000
#define PI 3.14159265358979323846
static uint32_t rng=1;
static double random_sample(void){rng=rng*1664525u+1013904223u;return ((rng>>8)/16777216.-.5)*2.;}
static float *buffer(size_t n){float *p=calloc(n,sizeof(*p));assert(p);return p;}
static void render(sb_dsp *d,float *a,float *b,size_t n,size_t chunk,float *x,float *y){
    size_t consumed=0,produced=0;
    while(consumed<n){size_t k=n-consumed<chunk?n-consumed:chunk;const float *in[2]={a+consumed,b+consumed};float *out[2]={x+produced,y+produced};size_t got=sb_dsp_process(d,in,k,out,k);assert(got<=k);consumed+=k;produced+=got;}
    while(produced<n){float *out[2]={x+produced,y+produced};size_t cap=n-produced<chunk?n-produced:chunk;size_t got=sb_dsp_flush(d,out,cap);assert(got&&got<=cap);produced+=got;}
    float *out[2]={x,y};assert(sb_dsp_flush(d,out,chunk)==0);assert(produced==n);
}
static double energy(const float *x,size_t start,size_t end){double e=0;for(size_t i=start;i<end;i++)e+=(double)x[i]*x[i];return e/(end-start);}
static double projection(const float *x,size_t start,size_t end,double frequency){double s=0,c=0;for(size_t i=start;i<end;i++){double w=2*PI*frequency*i/FS;s+=x[i]*sin(w);c+=x[i]*cos(w);}return 2*sqrt(s*s+c*c)/(end-start);}
static void tone(float *a,float *b,size_t n,double frequency,double gain){for(size_t i=0;i<n;i++)a[i]=b[i]=(float)(gain*sin(2*PI*frequency*i/FS));}
static void bypass(void){
    size_t n=FS+137;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n),*z=buffer(n),*w=buffer(n);
    for(size_t i=0;i<n;i++){a[i]=(float)(.5*random_sample());b[i]=(float)(.5*random_sample());}
    sb_dsp *d=sb_dsp_create(FS,2,4096);assert(d);assert(sb_dsp_latency(d)==1504);render(d,a,b,n,4096,x,y);
    assert(memcmp(a,x,n*sizeof(float))==0);assert(memcmp(b,y,n*sizeof(float))==0);
    sb_dsp_reset(d);render(d,a,b,n,17,z,w);assert(memcmp(x,z,n*sizeof(float))==0);assert(memcmp(y,w,n*sizeof(float))==0);
    sb_dsp_reset(d);float *out[2]={x,y};const float *in[2]={a,b};assert(sb_dsp_process(d,in,4097,out,4097)==0);assert(sb_dsp_process(d,in,8,out,7)==0);render(d,a,b,n,1,z,w);assert(memcmp(a,z,n*sizeof(float))==0);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);free(z);free(w);puts("bypass: exact source PCM, chunk 1/17/4096, latency, reset, bounds, EOF passed");
}
static void spectral_noise(void){
    size_t n=FS*3;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);rng=11;
    for(size_t i=0;i<n;i++){a[i]=(float)(.12*sin(2*PI*1500*i/FS)+.04*random_sample());b[i]=-a[i];}
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.noise_reduction=1;c.normalization_mode=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,511,x,y);
    double before=projection(a,FS,n,1500),after=projection(x,FS,n,1500);double noise_before=energy(a,FS,n)-before*before/2,noise_after=energy(x,FS,n)-after*after/2;
    printf("noise: residual %.2f dB, voiced harmonic %.2f dB\n",10*log10(noise_after/noise_before),20*log10(after/before));
    assert(noise_after<noise_before*.7);assert(after>before*.65);
    double onset=projection(x,0,FS/10,1500)/projection(a,0,FS/10,1500);
    printf("noise onset: first 100ms harmonic retained %.1f%%\n",onset*100);assert(onset>.7);
    for(size_t i=0;i<n;i++)assert(x[i]==-y[i]);
    sb_dsp_reset(d);float *z=buffer(n),*w=buffer(n);render(d,a,b,n,4096,z,w);for(size_t i=0;i<n;i++)assert(x[i]==z[i]&&y[i]==w[i]);
    c.mono=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,999,z,w);assert(energy(z,0,n)<1.e-20);assert(energy(w,0,n)<1.e-20);
    free(z);free(w);sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void eq_and_mono(void){
    size_t n=FS;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);tone(a,b,n,1000,.05);
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.normalization_mode=1;c.eq_count=1;c.eq[0]=(sb_dsp_eq_band){0,1000,1,6};sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,733,x,y);
    double gain=20*log10(projection(x,FS/2,n,1000)/.05);printf("parametric EQ: center gain %.3f dB\n",gain);assert(fabs(gain-6)<.02);
    c.eq_count=0;c.mono=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);for(size_t i=0;i<n;i++)b[i]=0;render(d,a,b,n,13,x,y);assert(fabs(projection(x,FS/2,n,1000)-.025)<1.e-5);assert(memcmp(x,y,n*sizeof(float))==0);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void normalization(void){
    size_t n=FS*5;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);tone(a,b,n,1000,.03);
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.normalization_mode=2;c.target_lufs=-14;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,4096,x,y);
    double input_l=sb_dsp_loudness_db(d),gain_db=20*log10(projection(x,FS*4,n,1000)/.03);printf("normalization: input %.2f LUFS, gain %.2f dB, predicted %.2f LUFS\n",input_l,gain_db,input_l+gain_db);assert(fabs(input_l+gain_db+14)<.1);
    c.normalization_mode=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,4096,x,y);assert(memcmp(a,x,n*sizeof(float))==0);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void deesser(void){
    size_t n=FS*2;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);for(size_t i=0;i<n;i++)a[i]=b[i]=(float)(.03*sin(2*PI*800*i/FS)+.14*sin(2*PI*8000*i/FS));
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.normalization_mode=1;c.deesser=2;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,257,x,y);
    double low=20*log10(projection(x,FS,n,800)/.03),high=20*log10(projection(x,FS,n,8000)/.14);printf("deesser: 800 Hz %.2f dB, 8 kHz %.2f dB\n",low,high);assert(high<low-2);assert(low>-.5);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void limiter_and_invalid(void){
    size_t n=FS+5;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);for(size_t i=0;i<n;i++){a[i]=(float)(1.5*sin(2*PI*400*i/FS));b[i]=a[i]*.5f;}a[0]=5;a[n-1]=-5;a[13]=NAN;b[23]=INFINITY;
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.normalization_mode=1;c.eq_count=1;c.eq[0]=(sb_dsp_eq_band){0,1000,1,0};sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,1,x,y);double max=0;for(size_t i=0;i<n;i++){assert(isfinite(x[i])&&isfinite(y[i]));max=fmax(max,fabs(x[i]));assert(fabs(y[i])<.923);}printf("limiter: peak %.6f (ceiling %.6f), first/last impulse %.6f / %.6f\n",max,pow(10,-.7/20),x[0],x[n-1]);assert(max<=pow(10,-.7/20)+1.e-7);assert(fabs(x[0])>.01&&fabs(x[n-1])>.01);
    c.noise_reduction=NAN;c.eq_count=100;c.target_lufs=INFINITY;c.deesser=99;c.normalization_mode=-1;for(size_t i=0;i<SB_DSP_MAX_EQ_BANDS;i++)c.eq[i]=(sb_dsp_eq_band){99,NAN,NAN,NAN};sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,127,x,y);for(size_t i=0;i<n;i++)assert(isfinite(x[i]));
    sb_dsp_destroy(d);assert(!sb_dsp_create(NAN,2,512));assert(!sb_dsp_create(48000,3,512));free(a);free(b);free(x);free(y);
}

static void rates_tiny_and_toggles(void){
    const double rates[]={8000,44100,48000,96000,192000};
    for(size_t r=0;r<sizeof(rates)/sizeof(rates[0]);r++){
        sb_dsp *d=sb_dsp_create(rates[r],1,64);assert(d);
        assert(sb_dsp_latency(d)==1024+(size_t)ceil(.01*rates[r]));
        for(size_t n=0;n<33;n++){
            float a[64]={0},b[64]={0},x[64]={0},y[64]={0};
            for(size_t i=0;i<n;i++)a[i]=(float)(.1*(i+1)/33.);
            sb_dsp_reset(d);render(d,a,b,n,1,x,y);assert(memcmp(a,x,n*sizeof(float))==0);
        }
        sb_dsp_destroy(d);
    }
    size_t n=FS;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n);tone(a,b,n,700,.1);
    sb_dsp *d=sb_dsp_create(FS,2,256);size_t produced=0;
    for(size_t offset=0;offset<n;){
        if(offset%4096==0){
            sb_dsp_config c={0};c.voice_boost=(offset/4096)%2;c.mono=(offset/8192)%2;c.noise_reduction=(float)((offset/4096)%3)*.5f;c.deesser=(offset/4096)%3;c.normalization_mode=1;c.eq_count=1;c.eq[0]=(sb_dsp_eq_band){0,700,1,(float)((offset/4096)%2?9:-9)};sb_dsp_configure(d,&c);
        }
        size_t k=n-offset<256?n-offset:256;const float *in[2]={a+offset,b+offset};float *out[2]={x+produced,y+produced};produced+=sb_dsp_process(d,in,k,out,k);offset+=k;
    }
    while(produced<n){float *out[2]={x+produced,y+produced};produced+=sb_dsp_flush(d,out,n-produced<256?n-produced:256);}
    double delta=0;for(size_t i=1;i<n;i++){assert(isfinite(x[i]));delta=fmax(delta,fabs(x[i]-x[i-1]));assert(fabs(x[i])<.923);}
    printf("sample rates / tiny EOF: 8-192kHz passed; toggles maximum step %.4f\n",delta);assert(delta<.2);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void voice_boost(void){
    size_t n=FS*3;float *a=buffer(n),*b=buffer(n),*x=buffer(n),*y=buffer(n),*u=buffer(n),*v=buffer(n);tone(a,b,n,700,.1);
    sb_dsp *d=sb_dsp_create(FS,2,4096);sb_dsp_config c={0};c.voice_boost=1;c.normalization_mode=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,4073,x,y);
    double quiet=projection(x,FS,n,700)/.1;sb_dsp_reset(d);tone(a,b,n,700,.5);render(d,a,b,n,4096,u,v);double loud=projection(u,FS,n,700)/.5;
    printf("Voice Boost compressor: quiet gain %.2f dB, loud gain %.2f dB\n",20*log10(quiet),20*log10(loud));assert(loud<quiet*.7);
    sb_dsp_reset(d);render(d,a,b,n,31,x,y);assert(memcmp(x,u,n*sizeof(float))==0);assert(memcmp(y,v,n*sizeof(float))==0);
    c.normalization_mode=0;sb_dsp_configure(d,&c);sb_dsp_reset(d);tone(a,b,n,700,.03);render(d,a,b,n,4096,x,y);double with_normalization=energy(x,FS*2,n);
    c.normalization_mode=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);render(d,a,b,n,4096,x,y);assert(with_normalization>energy(x,FS*2,n)*2);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);free(u);free(v);
}

static void seeded_normalization(void){
    sb_dsp *d=sb_dsp_create(FS,2,4096);assert(d);
    size_t latency=sb_dsp_latency(d);assert(latency<4096);
    float *a=buffer(4096),*b=buffer(4096),*x=buffer(4096),*y=buffer(4096);
    for(size_t i=0;i<4096;i++)a[i]=b[i]=.01f;
    const float *in[2]={a,b};float *out[2]={x,y};
    sb_dsp_config c={0};c.normalization_mode=2;c.target_lufs=-14;
    sb_dsp_configure(d,&c);sb_dsp_reset(d);
    const float invalid[]={NAN,INFINITY,-INFINITY,-70.01f,.01f};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)sb_dsp_seed_loudness(d,invalid[i]);
    assert(sb_dsp_loudness_db(d)==-160);
    sb_dsp_seed_loudness(d,-30);
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)sb_dsp_seed_loudness(d,invalid[i]);
    assert(sb_dsp_loudness_db(d)==-30);
    assert(sb_dsp_process(d,in,latency,out,latency)==0);
    assert(sb_dsp_process(d,in,1,out,1)==1);
    double seeded_gain=x[0]/a[0];assert(fabs(20*log10(seeded_gain)-16)<.00001);
    c.normalization_mode=1;sb_dsp_configure(d,&c);sb_dsp_reset(d);sb_dsp_seed_loudness(d,-30);
    assert(sb_dsp_process(d,in,latency,out,latency)==0);assert(sb_dsp_process(d,in,1,out,1)==1);
    assert(x[0]==a[0]&&y[0]==b[0]);
    c.normalization_mode=2;sb_dsp_configure(d,&c);sb_dsp_reset(d);sb_dsp_seed_loudness(d,-36);
    assert(sb_dsp_process(d,in,latency,out,latency)==0);assert(sb_dsp_process(d,in,1,out,1)==1);assert(x[0]==a[0]);
    sb_dsp_reset(d);sb_dsp_seed_loudness(d,-70);assert(sb_dsp_loudness_db(d)==-70);
    sb_dsp_reset(d);sb_dsp_seed_loudness(d,0);assert(sb_dsp_loudness_db(d)==0);
    sb_dsp_seed_loudness(NULL,-30);
    printf("normalization seed: immediate +%.2f dB after %zu-frame latency; invalid ignored, off/guard unity passed\n",20*log10(seeded_gain),latency);
    sb_dsp_destroy(d);free(a);free(b);free(x);free(y);
}
static void inplace_aliasing(void) {
    const size_t n=5013;float *l=buffer(n),*r=buffer(n),*gold_l=buffer(n),*gold_r=buffer(n),*got_l=buffer(n),*got_r=buffer(n);
    for(size_t i=0;i<n;i++){l[i]=(float)(.21*sin(i*.103));r[i]=(float)(.16*cos(i*.071));}
    sb_dsp_config c={0};c.voice_boost=1;c.noise_reduction=.6f;c.deesser=2;c.normalization_mode=2;c.target_lufs=-14;
    c.eq_count=1;c.eq[0]=(sb_dsp_eq_band){0,1700,.8,3};
    sb_dsp *reference=sb_dsp_create(FS,2,4096);assert(reference);sb_dsp_configure(reference,&c);sb_dsp_reset(reference);
    render(reference,l,r,n,4096,gold_l,gold_r);sb_dsp_destroy(reference);
    for(unsigned mode=0;mode<3;mode++) {
        for(size_t i=0;i<n;i++){l[i]=(float)(.21*sin(i*.103));r[i]=(float)(.16*cos(i*.071));}
        sb_dsp *d=sb_dsp_create(FS,2,4096);assert(d);sb_dsp_configure(d,&c);sb_dsp_reset(d);size_t produced=0;
        for(size_t at=0;at<n;) {
            size_t chunk=n-at;if(chunk>4096)chunk=4096;
            const float *input[2]={l+at,r+at};float *output[2]={mode==1?r+at:l+at,mode==0?r+at:l+at};
            size_t got=sb_dsp_process(d,input,chunk,output,chunk);
            memcpy(got_l+produced,output[0],got*sizeof(float));memcpy(got_r+produced,output[1],got*sizeof(float));produced+=got;at+=chunk;
        }
        float a[4096],b[4096];float *output[2]={a,b};size_t got;
        while((got=sb_dsp_flush(d,output,4096))) {memcpy(got_l+produced,a,got*4);memcpy(got_r+produced,b,got*4);produced+=got;}
        assert(produced==n);
        /* Shared output storage follows the original channel-zero then channel-one writes. */
        if(mode==2) {
            size_t body=n-sb_dsp_latency(d);
            assert(memcmp(got_l,gold_r,body*4)==0);assert(memcmp(got_r,gold_r,body*4)==0);
            assert(memcmp(got_l+body,gold_l+body,(n-body)*4)==0);
        } else assert(memcmp(got_l,gold_l,n*4)==0);
        assert(memcmp(got_r,gold_r,n*4)==0);sb_dsp_destroy(d);
    }
    free(l);free(r);free(gold_l);free(gold_r);free(got_l);free(got_r);
    puts("DSP aliasing: planar in-place, crossed source channels and shared destination preserve original sample order");
}
int main(void){inplace_aliasing();bypass();spectral_noise();eq_and_mono();normalization();deesser();limiter_and_invalid();rates_tiny_and_toggles();voice_boost();seeded_normalization();puts("All custom C DSP checks passed.");return 0;}
