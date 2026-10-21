#include "SBEngine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
typedef struct { float *l,*r; double *source,*saved; size_t n,capacity; } Render;
static Render render(const float *l,const float *r,size_t n,SBEngineConfig config,size_t chunk,size_t pull,int64_t base) {
    SBEngine *e=sb_engine_create(44100,2,17); CHECK(e);
    CHECK(sb_engine_configure(e,&config)); sb_engine_reset(e,base);
    Render out={calloc(n*2+4096,sizeof(float)),calloc(n*2+4096,sizeof(float)),
                calloc(n*2+4096,sizeof(double)),calloc(n*2+4096,sizeof(double)),0,n*2+4096};
    CHECK(out.l&&out.r&&out.source&&out.saved);
    size_t accepted=0,turns=0;
    int finished=0;
    while (!sb_engine_is_drained(e)) {
        size_t count=n-accepted; if(count>chunk)count=chunk;
        size_t pushed=count?sb_engine_push(e,l+accepted,r+accepted,count):0;
        accepted+=pushed;
        if(accepted==n&&!finished){sb_engine_finish(e);finished=1;}
        CHECK(out.n+pull<=out.capacity);
        size_t got=sb_engine_read(e,out.l+out.n,out.r+out.n,out.source+out.n,out.saved+out.n,pull);
        out.n+=got;
        CHECK(pushed||got||sb_engine_is_drained(e));
        CHECK(++turns<n*4+10000);
    }
    CHECK(accepted==n);
    CHECK(sb_engine_push(e,l,r,n)==0);
    for(size_t i=0;i<out.n;i++) {
        CHECK(isfinite(out.l[i])&&isfinite(out.r[i]));
        CHECK(isfinite(out.source[i])&&isfinite(out.saved[i]));
        CHECK(out.source[i]>=base&&out.source[i]<=base+n+5);
        CHECK(out.saved[i]>=0&&out.saved[i]<=n);
        if(i){CHECK(out.source[i]>=out.source[i-1]);CHECK(out.saved[i]>=out.saved[i-1]);}
    }
    sb_engine_reset(e,base);
    CHECK(sb_engine_configure(e,&config));
    sb_engine_destroy(e);
    return out;
}
static void release(Render *r){free(r->l);free(r->r);free(r->source);free(r->saved);}
static SBEngineConfig bypass(void){SBEngineConfig c=sb_engine_default_config();c.smart_speed=0;c.voice_boost=0;c.normalization_mode=1;return c;}
/* Reference the original maximum-window algorithm across voiced/quiet boundaries,
   exact-threshold samples, stereo vetoes, block edges, and a partial EOF block. */
static void mixed_smart_windows(void) {
    const size_t n=4096*3+149;
    float *l=calloc(n,sizeof(float)),*r=calloc(n,sizeof(float));CHECK(l&&r);
    float threshold=(float)pow(10.,-48./20.);
    uint32_t rng=71;
    for(size_t i=0;i<n;i++) {
        rng=rng*1664525u+1013904223u;
        unsigned pattern=(unsigned)(i/173)%7;
        l[i]=pattern<4?threshold*.5f:pattern==4?threshold:nextafterf(threshold,INFINITY);
        r[i]=(rng%509==0)?-.1f:-l[i];
    }
    SBEngineConfig config=bypass();config.smart_speed=1;
    Render actual=render(l,r,n,config,997,257,5000);
    size_t total=0;
    for(size_t base=0;base<n;base+=4096) {
        size_t count=n-base<4096?n-base:4096;
        float x[4096],y[4096],peaks[4096];double source[4096];
        for(size_t i=0;i<count;i++){x[i]=l[base+i];y[i]=r[base+i];source[i]=5000+base+i;peaks[i]=fmaxf(fabsf(x[i]),fabsf(y[i]));}
        size_t cursor=0,removed=0;
        while(cursor+148<count) {
            float peak=0;for(size_t j=cursor;j<cursor+148;j++)peak=fmaxf(peak,peaks[j]);
            if(peak>threshold){cursor++;continue;}
            size_t dst=cursor-removed;
            for(size_t j=0;j<46;j++){float blend=(float)j/45.f;x[dst+j]=x[dst+j]*(1-blend)+x[dst+102+j]*blend;y[dst+j]=y[dst+j]*(1-blend)+y[dst+102+j]*blend;}
            memmove(x+dst+46,x+dst+148,(count-cursor-148)*sizeof(float));
            memmove(y+dst+46,y+dst+148,(count-cursor-148)*sizeof(float));
            memmove(source+dst,source+dst+102,(count-cursor-102)*sizeof(double));
            removed+=102;cursor+=148;
        }
        for(size_t i=0;i<count-removed;i++) {
            CHECK(total<actual.n);CHECK(actual.l[total]==x[i]&&actual.r[total]==y[i]);
            CHECK(actual.source[total]==source[i]);CHECK(actual.saved[total]==source[i]-5000-total);total++;
        }
    }
    CHECK(total==actual.n);CHECK(actual.n<n);
    release(&actual);free(l);free(r);
    puts("Smart Speed mixed windows: exact original PCM, splice positions and clocks");
}
int main(void) {
    mixed_smart_windows();
    size_t n=44100*2+113;
    float *l=calloc(n,sizeof(float)),*r=calloc(n,sizeof(float));CHECK(l&&r);
    for(size_t i=0;i<n;i++){l[i]=(float)(.2*sin(i*2*3.141592653589793*440/44100));r[i]=-l[i];}
    SBEngineConfig c=bypass();
    for(size_t tiny=0;tiny<33;tiny++){
        Render a=render(l,r,tiny,c,1,7,12345);CHECK(a.n==tiny);
        for(size_t i=0;i<tiny;i++){CHECK(a.l[i]==l[i]&&a.r[i]==r[i]);CHECK(a.source[i]==12345+i&&a.saved[i]==0);}
        release(&a);
    }
    Render a=render(l,r,n,c,17,257,5000), b=render(l,r,n,c,32768,4096,5000);
    CHECK(a.n==n&&b.n==n);
    CHECK(!memcmp(a.l,l,n*sizeof(float))&&!memcmp(a.r,r,n*sizeof(float)));
    CHECK(!memcmp(a.l,b.l,n*sizeof(float))&&!memcmp(a.source,b.source,n*sizeof(double)));
    release(&a);release(&b);
    const float rates[]={.5f,1.5f,2.f,3.f,5.f};
    for(size_t k=0;k<sizeof(rates)/sizeof(rates[0]);k++){
        c=bypass();c.rate=rates[k];a=render(l,r,n,c,31,113,0);
        CHECK(a.n==(size_t)ceil(n/(double)c.rate));
        for(size_t i=0;i<a.n;i++){CHECK(fabs(a.l[i]+a.r[i])<1.e-7);CHECK(a.saved[i]==0);}
        printf("rate %.2f: %zu frames, no tempo counted as silence\n",c.rate,a.n);release(&a);
    }
    /* Fixed-size silence blocks: 27 complete 148→46 cuts per full block. */
    memset(l,0,n*sizeof(float));memset(r,0,n*sizeof(float));
    c=bypass();c.smart_speed=1;
    a=render(l,r,8192,c,17,31,5000);b=render(l,r,8192,c,8192,4096,5000);
    CHECK(a.n==8192-54*102);CHECK(a.n==b.n);
    CHECK(!memcmp(a.source,b.source,a.n*sizeof(double)));
    CHECK(a.saved[a.n-1]==54*102);
    CHECK(a.source[a.n-1]==5000+8191);
    printf("Smart Speed: 8192 -> %zu frames; saved %.0f source frames\n",a.n,a.saved[a.n-1]);
    release(&a);release(&b);
    c.rate=2;a=render(l,r,8192,c,4096,31,0);
    CHECK(a.n==(8192-54*102)/2);CHECK(a.saved[a.n-1]==54*102);release(&a);
    /* A loud right channel must veto silence cuts despite a silent left channel. */
    for(size_t i=0;i<8192;i++)r[i]=.1f;
    c=bypass();c.smart_speed=1;a=render(l,r,8192,c,37,128,0);CHECK(a.n==8192);release(&a);
    /* Every independent control is safe with Voice Boost off, including a short final block. */
    c=bypass();c.mono=1;c.noise_reduction=.5;c.normalization_mode=2;c.target_lufs=-18;c.deesser=2;
    sb_engine_config_set_band(&c,0,0,1700,.8f,-3);
    a=render(l,r,8192+57,c,4096,256,8000);
    CHECK(a.n==8192+57);for(size_t i=0;i<a.n;i++)CHECK(a.l[i]==a.r[i]);release(&a);
    CHECK(!sb_engine_create(NAN,2,17));CHECK(!sb_engine_create(44100,3,17));
    free(l);free(r);puts("SBEngine integration gates passed");return 0;
}
