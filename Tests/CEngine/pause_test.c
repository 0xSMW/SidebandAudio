#include "SBEngine.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FATAL %s:%d %s\n",__FILE__,__LINE__,#x); exit(2); } } while (0)
static unsigned failures;
#define EXPECT(x, label) do { if (!(x)) { fprintf(stderr,"FAIL %s (%.0f Hz): %s\n",label,fs,#x); failures++; } } while (0)
typedef struct { float *l,*r; size_t n; } Input;
typedef struct { float *l,*r; double *source,*saved; size_t n,capacity; } Render;
static Input input(size_t n) {
    Input in={calloc(n?n:1,sizeof(float)),calloc(n?n:1,sizeof(float)),n};
    REQUIRE(in.l&&in.r);return in;
}
static void free_input(Input *in) { free(in->l);free(in->r); }
static void free_render(Render *out) { free(out->l);free(out->r);free(out->source);free(out->saved); }
static SBEngineConfig configuration(float sensitivity,float rate) {
    SBEngineConfig c=sb_engine_default_config();c.voice_boost=0;c.normalization_mode=1;
    c.rate=rate;c.smart_speed=1;c.silence_sensitivity=sensitivity;return c;
}
static Render render(Input in,double fs,unsigned channels,float sensitivity,float rate,size_t chunk,size_t pull) {
    const int64_t base=12345;
    SBEngine *engine=sb_engine_create(fs,channels,8192);REQUIRE(engine);
    SBEngineConfig c=configuration(sensitivity,rate);REQUIRE(sb_engine_configure(engine,&c));
    sb_engine_reset(engine,base);
    size_t capacity=in.n*2+8192+pull;
    Render out={calloc(capacity,sizeof(float)),calloc(capacity,sizeof(float)),
        calloc(capacity,sizeof(double)),calloc(capacity,sizeof(double)),0,capacity};
    REQUIRE(out.l&&out.r&&out.source&&out.saved);
    size_t accepted=0,turns=0;int finished=0;
    while (!sb_engine_is_drained(engine)) {
        size_t count=in.n-accepted;if(count>chunk)count=chunk;
        size_t pushed=count?sb_engine_push(engine,in.l+accepted,channels==2?in.r+accepted:NULL,count):0;
        accepted+=pushed;
        if(accepted==in.n&&!finished){sb_engine_finish(engine);finished=1;}
        REQUIRE(out.n+pull<=out.capacity);
        size_t got=sb_engine_read(engine,out.l+out.n,channels==2?out.r+out.n:NULL,
            out.source+out.n,out.saved+out.n,pull);
        out.n+=got;
        REQUIRE(pushed||got||sb_engine_is_drained(engine));
        REQUIRE(++turns<in.n*4+10000);
    }
    REQUIRE(accepted==in.n);
    for(size_t i=0;i<out.n;i++) {
        REQUIRE(isfinite(out.l[i])&&isfinite(out.r[i]));
        REQUIRE(isfinite(out.source[i])&&isfinite(out.saved[i]));
        REQUIRE(out.source[i]>=base&&out.source[i]<=base+(double)in.n+1);
        REQUIRE(out.saved[i]>=0&&out.saved[i]<=in.n);
        if(i){REQUIRE(out.source[i]>=out.source[i-1]);REQUIRE(out.saved[i]>=out.saved[i-1]);}
    }
    sb_engine_destroy(engine);return out;
}
static uint32_t random_state;
static float noise(void) {
    random_state=random_state*1664525u+1013904223u;
    return (float)((double)(random_state>>8)/8388608.-1.);
}
static float voiced(double t,float amplitude) {
    return amplitude*(float)(.75*sin(2*PI*180*t)+.18*sin(2*PI*360*t)+.07*sin(2*PI*540*t));
}
/* All segment boundaries are aligned to source time, not caller push boundaries. */
static Input noisy_pause(double fs,double gap_seconds) {
    Input in=input((size_t)llround(fs*(1.4+gap_seconds)));random_state=71;
    for(size_t i=0;i<in.n;i++) {
        double t=(double)i/fs;
        float x=(t<.8||t>=.8+gap_seconds)?voiced(t,.22f):.012f*noise();
        in.l[i]=in.r[i]=x;
    }
    return in;
}
static void require_preserved(Input in,double fs,unsigned channels,const char *label) {
    Render out=render(in,fs,channels,.5f,1,997,257);
    EXPECT(out.n==in.n,label);
    int intact=out.n==in.n;
    for(size_t i=0;i<out.n&&intact;i++)
        intact=out.source[i]==12345+(double)i&&out.saved[i]==0&&out.l[i]==in.l[i]&&
            (channels==1||out.r[i]==in.r[i]);
    EXPECT(intact,label);
    if(out.n!=in.n)fprintf(stderr,"  retained %zu/%zu; saved %.3f seconds\n",out.n,in.n,(double)(in.n-out.n)/fs);
    free_render(&out);
}
static void noisy_pause_and_guards(double fs) {
    Input in=noisy_pause(fs,1.2);
    Render a=render(in,fs,2,.5f,1,137,113),b=render(in,fs,2,.5f,1,8192,4096);
    EXPECT(a.n<in.n-(size_t)(fs*.25),"broadband pause shortened");
    EXPECT(a.n==b.n,"chunk/pull invariant length");
    if(a.n==b.n) {
        EXPECT(!memcmp(a.l,b.l,a.n*sizeof(float))&&!memcmp(a.r,b.r,a.n*sizeof(float)),"chunk/pull invariant PCM");
        EXPECT(!memcmp(a.source,b.source,a.n*sizeof(double))&&!memcmp(a.saved,b.saved,a.n*sizeof(double)),"chunk/pull invariant metadata");
    }
    int guarded=1;size_t jumps=0;
    const double first_cut=fs*.86,last_cut=fs*1.94;
    for(size_t i=0;i<a.n;i++) {
        double source=a.source[i]-12345.;
        if(i&&a.source[i]>a.source[i-1]+1) {
            double removed_start=a.source[i-1]-12345.+1;
            double removed_end=source-1;
            if(removed_start<first_cut||removed_end>=last_cut)guarded=0;
            jumps++;
        }
        if((source<first_cut||source>=last_cut)&&a.l[i]!=in.l[(size_t)source])guarded=0;
        if(a.saved[i]!=(a.source[i]-12345.-(double)i))guarded=0;
    }
    EXPECT(jumps>0&&guarded,"60 ms speech boundary guards and exact saved clock");
    Render legacy=render(in,fs,2,0,1,997,257);
    EXPECT(legacy.n==in.n,"strict legacy does not remove broadband room noise");
    printf("broadband pause %.0f Hz: %.3f s removed; %zu guarded jumps\n",fs,(double)(in.n-a.n)/fs,jumps);
    free_render(&legacy);free_render(&a);free_render(&b);free_input(&in);
}
static void speech_vetoes(double fs) {
    Input in=input((size_t)llround(fs*2.1));
    for(size_t i=0;i<in.n;i++) {
        double t=(double)i/fs;
        in.l[i]=in.r[i]=voiced(t,t<.7?.22f:.0015f);
    }
    require_preserved(in,fs,2,"quiet sustained harmonic vowel");
    for(size_t i=0;i<in.n;i++) { in.r[i]=in.l[i];in.l[i]=0; }
    require_preserved(in,fs,2,"right-only quiet speech veto");
    for(size_t i=0;i<in.n;i++)in.l[i]=-in.r[i];
    require_preserved(in,fs,2,"antiphase quiet speech veto");
    /* Breathy consonant-like high-band bursts, 60 ms on / 40 ms off, above room noise. */
    random_state=1337;double previous=0;
    for(size_t i=0;i<in.n;i++) {
        double t=(double)i/fs;float x;
        if(t<.7)x=voiced(t,.22f);
        else {
            double white=noise(),high=white-previous;previous=white;
            int active=fmod(t-.7,.1)<.06;
            x=(float)(.001*noise()+(active?.014*high:0));
        }
        in.l[i]=in.r[i]=x;
    }
    require_preserved(in,fs,2,"whisper-like high-band bursts");
    for(unsigned frequency=50;frequency<=60;frequency+=10) {
        for(size_t i=0;i<in.n;i++) {
            double t=(double)i/fs;
            in.l[i]=in.r[i]=t<.7?voiced(t,.22f):(float)(.004*sin(2*PI*frequency*t));
        }
        require_preserved(in,fs,2,frequency==50?"50 Hz hum veto":"60 Hz hum veto");
    }
    free_input(&in);
}
static void short_gaps_and_tiny_eof(double fs) {
    SBEngine *engine=sb_engine_create(fs,1,8192);REQUIRE(engine);
    SBEngineConfig c=configuration(0,1);REQUIRE(sb_engine_configure(engine,&c));
    size_t legacy_latency=sb_engine_latency(engine);
    c.silence_sensitivity=.5f;REQUIRE(sb_engine_configure(engine,&c));
    EXPECT(sb_engine_latency(engine)==legacy_latency+24*(size_t)llround(fs*.01),"240 ms aligned detector lookahead");
    sb_engine_destroy(engine);
    Input in=noisy_pause(fs,.19);
    require_preserved(in,fs,2,"noisy gap below 200 ms");
    size_t first=(size_t)llround(fs*.8),last=first+(size_t)llround(fs*.19);
    for(size_t i=first;i<last;i++)in.l[i]=in.r[i]=0;
    require_preserved(in,fs,2,"digital-zero gap below 200 ms");free_input(&in);
    size_t hop=(size_t)llround(fs*.01);
    const size_t lengths[]={0,1,7,hop-1,hop,hop+1,hop*19+1};
    for(size_t k=0;k<sizeof(lengths)/sizeof(lengths[0]);k++) {
        in=input(lengths[k]);
        Render out=render(in,fs,1,.5f,1,1,7);
        EXPECT(out.n==in.n,"startup and tiny/partial EOF retains unconfirmed quiet");
        for(size_t i=0;i<out.n;i++)REQUIRE(out.l[i]==0&&out.source[i]==12345+(double)i&&out.saved[i]==0);
        free_render(&out);free_input(&in);
    }
    in=input((size_t)llround(fs*.6));
    for(size_t i=0;i<in.n;i++)in.l[i]=voiced((double)i/fs,.1f);
    const float rates[]={.5f,2.f};
    for(size_t k=0;k<sizeof(rates)/sizeof(rates[0]);k++) {
        Render out=render(in,fs,1,.5f,rates[k],257,113);
        EXPECT(out.n==(size_t)ceil((double)in.n/rates[k]),"tempo keeps voiced duration");
        int no_savings=1;for(size_t i=0;i<out.n;i++)if(out.saved[i]!=0)no_savings=0;
        EXPECT(no_savings,"tempo is not silence savings");free_render(&out);
    }
    free_input(&in);
}
int main(void) {
    const double rates[]={8000,44100,48000,192000};
    REQUIRE(sb_engine_default_config().silence_sensitivity==.5f);
    for(size_t i=0;i<sizeof(rates)/sizeof(rates[0]);i++) {
        noisy_pause_and_guards(rates[i]);speech_vetoes(rates[i]);short_gaps_and_tiny_eof(rates[i]);
    }
    if(failures){fprintf(stderr,"Adaptive pause gate: %u failed expectations\n",failures);return 1;}
    puts("Adaptive pause gate: all rates, guards, speech vetoes, metadata and streaming invariance passed");return 0;
}
