#include "sb_stretcher.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
typedef struct { float *a[2]; double *p,*q; size_t n,cap; } result;
static result allocate(size_t n) {
    result r = {{calloc(n ? n : 1,sizeof(float)),calloc(n ? n : 1,sizeof(float))},
                calloc(n ? n : 1,sizeof(double)),calloc(n ? n : 1,sizeof(double)),0,n};
    CHECK(r.a[0] && r.a[1] && r.p && r.q); return r;
}
static void release(result *r) { free(r->a[0]);free(r->a[1]);free(r->p);free(r->q); }
static result make_signal(size_t n,double fs,int mode) {
    result r=allocate(n);r.n=n;
    for(size_t i=0;i<n;i++) {
        float x=(float)(0.4*sin(2*PI*440*(double)i/fs));
        r.a[0][i]=mode==2?0:x;
        r.a[1][i]=mode==1?-x:r.a[0][i];
        r.q[i]=(i>=n/3?70:0)+(i>=2*n/3?31:0);
        r.p[i]=1000+(double)i+r.q[i];
    }
    return r;
}
static result render_instance(sb_stretcher *s,const result *in,size_t push_chunk,size_t pull_chunk) {
    result out=allocate(in->n*2+4096);size_t accepted=0,turns=0;
    while(!sb_stretcher_is_drained(s)) {
        size_t n=in->n-accepted;if(n>push_chunk)n=push_chunk;
        const float *a[2]={in->a[0]+accepted,in->a[1]+accepted};
        size_t pushed=sb_stretcher_push(s,a,in->p+accepted,in->q+accepted,n);accepted+=pushed;
        CHECK(out.n+pull_chunk<=out.cap);
        float *b[2]={out.a[0]+out.n,out.a[1]+out.n};
        size_t got=sb_stretcher_pull(s,b,out.p+out.n,out.q+out.n,pull_chunk,accepted==in->n);out.n+=got;
        CHECK(pushed||got||sb_stretcher_is_drained(s)||accepted<in->n);
        CHECK(++turns<in->n*4+10000);
    }
    CHECK(accepted==in->n); CHECK(sb_stretcher_buffered_frames(s)==0);
    CHECK(sb_stretcher_writable_frames(s)==0);
    for(size_t i=0;i<out.n;i++) {
        CHECK(isfinite(out.a[0][i])&&isfinite(out.a[1][i])&&isfinite(out.p[i])&&isfinite(out.q[i]));
        if(i) CHECK(out.p[i]>=out.p[i-1]&&out.q[i]>=out.q[i-1]);
    }
    return out;
}
static result render(const result *in,double fs,double rate,size_t push,size_t pull) {
    size_t window=((size_t)floor(1024*fs/44100+0.5)+3)&~(size_t)3;
    sb_stretcher *s=sb_stretcher_create(fs,2,4*window);CHECK(s);CHECK(sb_stretcher_set_rate(s,rate));
    result out=render_instance(s,in,push,pull);sb_stretcher_destroy(s);return out;
}
static double pitch(const result *r,double fs) {
    size_t lo=r->n/5,hi=r->n*4/5,first=0,last=0,crossings=0;
    for(size_t i=lo+1;i<hi;i++)if(r->a[0][i-1]<=0 && r->a[0][i]>0) {
        if(!crossings)first=i;last=i;crossings++;
    }
    CHECK(crossings>5);return fs*(double)(crossings-1)/(double)(last-first);
}
static void tones(void) {
    const double rates[]={0.5,0.75,1,1.1,1.25,1.5,2,2.9,3,5};
    for(size_t f=0;f<2;f++) {
        double fs=f?48000:44100;result in=make_signal((size_t)fs,fs,0);
        for(size_t k=0;k<sizeof(rates)/sizeof(*rates);k++) {
            double rate=rates[k];result out=render(&in,fs,rate,509,127);
            size_t expected=(size_t)ceil((double)in.n/rate-1e-9);
            CHECK(out.n==expected);
            double hz=pitch(&out,fs);CHECK(fabs(hz-440)<3);
            for(size_t i=0;i<out.n;i++) {
                CHECK(out.a[0][i]==out.a[1][i]);
                double clock=(double)i*rate;
                size_t index=(size_t)floor(clock);
                if(index>=in.n)index=in.n-1;
                double fraction=clock-(double)index;
                double saved=in.q[index];
                if(index+1<in.n)saved+=(in.q[index+1]-saved)*fraction;
                CHECK(fabs(out.q[i]-saved)<1e-7);
                CHECK(fabs(out.p[i]-(1000+clock+saved))<1e-7);
            }
            if(rate==1) {
                CHECK(!memcmp(out.a[0],in.a[0],in.n*sizeof(float)));
                CHECK(!memcmp(out.p,in.p,in.n*sizeof(double)));
                CHECK(!memcmp(out.q,in.q,in.n*sizeof(double)));
            }
            printf("tone fs=%.0f rate=%.2f frames=%zu pitch=%.3fHz\n",fs,rate,out.n,hz);release(&out);
        }release(&in);
    }
}
static void chunking(void) {
    const double rates[]={0.5,1,1.1,2,5};result in=make_signal(16013,44100,0);
    for(size_t k=0;k<5;k++) {
        result a=render(&in,44100,rates[k],1,1),b=render(&in,44100,rates[k],4096,2048);
        CHECK(a.n==b.n);CHECK(!memcmp(a.a[0],b.a[0],a.n*sizeof(float)));
        CHECK(!memcmp(a.p,b.p,a.n*sizeof(double)));CHECK(!memcmp(a.q,b.q,a.n*sizeof(double)));
        release(&a);release(&b);
    }release(&in);puts("chunk invariance passed");
}
static void ends(void) {
    const size_t lengths[]={0,1,2,17,255,511,512,513,1023,4095,4096,4097};
    const double rates[]={0.5,1,1.1,5};
    for(size_t j=0;j<sizeof(lengths)/sizeof(*lengths);j++)for(size_t k=0;k<4;k++) {
        result in=make_signal(lengths[j],44100,0);if(in.n)in.a[0][in.n-1]=in.a[1][in.n-1]=0.75f;
        result out=render(&in,44100,rates[k],37,31);
        CHECK(out.n==(size_t)ceil((double)in.n/rates[k]-1e-10));
        if(out.n)CHECK(out.a[0][out.n-1]==0.75f);
        release(&out);release(&in);
    }puts("empty, tiny, partial-hop EOF duration/tail passed");
}
static void silence_phase(void) {
    for(int mode=1;mode<=2;mode++) {
        result in=make_signal(44100,44100,mode),out=render(&in,44100,1.7,333,257);
        for(size_t i=0;i<out.n;i++) {
            if(mode==1)CHECK(out.a[0][i]==-out.a[1][i]);
            else CHECK(out.a[0][i]==0&&out.a[1][i]==0);
        }
        if(mode==1){double hz=pitch(&out,44100);CHECK(fabs(hz-440)<3);printf("antiphase pitch=%.3fHz\n",hz);}
        release(&in);release(&out);
    }puts("silence and stereo antiphase integrity passed");
}
static void constant_short_clips(void) {
    const size_t lengths[]={1,2,17,255,300,511,512,513,700,1023,2049};
    const double rates[]={0.5,0.75,1.25,5};
    for(size_t j=0;j<sizeof(lengths)/sizeof(*lengths);j++)for(size_t k=0;k<4;k++) {
        result in=make_signal(lengths[j],44100,0);
        for(size_t i=0;i<in.n;i++)in.a[0][i]=in.a[1][i]=0.25f;
        result out=render(&in,44100,rates[k],37,31);
        for(size_t i=0;i<out.n;i++) CHECK(fabs(out.a[0][i]-0.25f)<1e-6);
        release(&out);release(&in);
    }puts("short constant clips have no artificial silence/fade passed");
}
static void guards_reset(void) {
    CHECK(!sb_stretcher_create(NAN,2,4096));CHECK(!sb_stretcher_create(44100,3,4096));
    CHECK(!sb_stretcher_create(44100,2,4095));
    sb_stretcher *s=sb_stretcher_create(44100,2,4096);CHECK(s);
    CHECK(!sb_stretcher_set_rate(s,NAN));CHECK(!sb_stretcher_set_rate(s,INFINITY));
    CHECK(!sb_stretcher_set_rate(s,0.49));CHECK(!sb_stretcher_set_rate(s,5.01));
    CHECK(sb_stretcher_set_rate(s,1.25));result in=make_signal(10003,44100,0);
    result a=render_instance(s,&in,81,73);CHECK(!sb_stretcher_set_rate(s,1));
    sb_stretcher_reset(s);result b=render_instance(s,&in,271,131);
    CHECK(a.n==b.n&&!memcmp(a.a[0],b.a[0],a.n*sizeof(float)));release(&a);release(&b);
    sb_stretcher_reset(s);CHECK(sb_stretcher_set_rate(s,1));
    in.a[0][0]=NAN;in.a[1][1]=INFINITY;in.p[0]=NAN;in.p[1]=-1;in.q[0]=INFINITY;
    a=render_instance(s,&in,317,83);CHECK(a.a[0][0]==0&&a.a[1][1]==0);release(&a);release(&in);
    sb_stretcher_destroy(s);puts("guards, nonfinite inputs, reset/EOF passed");
}
static void mono_boundary_rates(void) {
    const double sample_rates[]={8000,384000};
    for(size_t f=0;f<2;f++) {
        double fs=sample_rates[f];
        size_t window=((size_t)floor(1024*fs/44100+0.5)+3)&~(size_t)3;
        sb_stretcher *s=sb_stretcher_create(fs,1,4*window);CHECK(s);
        CHECK(sb_stretcher_set_rate(s,1.3));
        float input[113],output[257];size_t sent=0,produced=0,ninput=4*window+17,turns=0;
        while(!sb_stretcher_is_drained(s)) {
            size_t n=ninput-sent;if(n>113)n=113;
            for(size_t i=0;i<n;i++)input[i]=0.25f;
            const float *src[2]={input,NULL};sent+=sb_stretcher_push(s,src,NULL,NULL,n);
            float *dst[2]={output,NULL};n=sb_stretcher_pull(s,dst,NULL,NULL,257,sent==ninput);
            for(size_t i=0;i<n;i++)CHECK(fabs(output[i]-0.25f)<1e-6);
            produced+=n;CHECK(++turns<100000);
        }
        CHECK(produced==(size_t)ceil(ninput/1.3-1e-9));sb_stretcher_destroy(s);
    }puts("mono and 8/384 kHz window boundaries passed");
}
static void rate_changes(void) {
    sb_stretcher *s=sb_stretcher_create(44100,2,4096);CHECK(s);
    result in=make_signal(44100,44100,0);size_t sent=0,got=0;double prevp=0,prevq=0;
    float a[2][257];double p[257],q[257];size_t turns=0;
    while(!sb_stretcher_is_drained(s)) {
        if(sent<in.n){double rates[]={0.5,1,5,1.7};CHECK(sb_stretcher_set_rate(s,rates[(sent/3000)%4]));}
        size_t n=in.n-sent;if(n>157)n=157;
        const float *src[2]={in.a[0]+sent,in.a[1]+sent};sent+=sb_stretcher_push(s,src,in.p+sent,in.q+sent,n);
        float *dst[2]={a[0],a[1]};n=sb_stretcher_pull(s,dst,p,q,257,sent==in.n);
        for(size_t i=0;i<n;i++){CHECK(isfinite(a[0][i])&&p[i]>=prevp&&q[i]>=prevq);prevp=p[i];prevq=q[i];}got+=n;
        CHECK(++turns<100000);
    }
    CHECK(sent==in.n&&got>0);sb_stretcher_destroy(s);release(&in);puts("dynamic rate transitions and monotonic metadata passed");
}
int main(void) { tones();chunking();ends();constant_short_clips();silence_phase();guards_reset();mono_boundary_rates();rate_changes();puts("ALL STRETCH TESTS PASSED");return 0; }
