/* clang -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
   -Wall -Wextra -Werror -pedantic -I Packages/SBEngine/Sources/SBEngine
   Packages/SBEngine/Tests/eq_response_test.c Packages/SBEngine/Sources/SBEngine/{sb_dsp,sb_engine,sb_stretcher}.c
   -lm -o /tmp/sb-eq-response-test */
#include "include/SBEngine.h"
#include "sb_dsp.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
#define BLOCK 4096u
static double measured_gain(const sb_dsp_config *config,double fs,double frequency) {
    size_t n=(size_t)fs;float *input=calloc(n,sizeof(float)),*output=calloc(n,sizeof(float));assert(input&&output);
    for(size_t i=0;i<n;i++)input[i]=(float)(.025*sin(2.*PI*frequency*i/fs));
    sb_dsp *dsp=sb_dsp_create(fs,1,BLOCK);assert(dsp);sb_dsp_configure(dsp,config);sb_dsp_reset(dsp);
    size_t produced=0;
    for(size_t i=0;i<n;) {
        size_t count=n-i<BLOCK?n-i:BLOCK;const float *in[2]={input+i,NULL};float *out[2]={output+produced,NULL};
        produced+=sb_dsp_process(dsp,in,count,out,count);i+=count;
    }
    while(produced<n) {
        size_t capacity=n-produced<BLOCK?n-produced:BLOCK;float *out[2]={output+produced,NULL};
        size_t count=sb_dsp_flush(dsp,out,capacity);assert(count&&count<=capacity);produced+=count;
    }
    double ss=0,cc=0,sc=0,ys=0,yc=0;
    for(size_t i=n/2;i<n;i++) {
        double w=2.*PI*frequency*i/fs,s=sin(w),c=cos(w);ss+=s*s;cc+=c*c;sc+=s*c;ys+=output[i]*s;yc+=output[i]*c;
    }
    double determinant=ss*cc-sc*sc,a=(ys*cc-yc*sc)/determinant,b=(yc*ss-ys*sc)/determinant;
    double result=20.*log10(hypot(a,b)/.025);sb_dsp_destroy(dsp);free(input);free(output);return result;
}
static void compare_pcm(int type,float cutoff,float q,float gain,double frequency,double fs) {
    sb_dsp_config dsp={0};dsp.normalization_mode=1;dsp.eq_count=1;dsp.eq[0]=(sb_dsp_eq_band){type,cutoff,q,gain};
    SBEngineConfig engine=sb_engine_default_config();sb_engine_config_set_band(&engine,0,type,cutoff,q,gain);
    sb_dsp_config dsp_before=dsp;SBEngineConfig engine_before=engine;
    double response=sb_engine_custom_eq_gain_db(&engine,fs,frequency),direct=sb_dsp_custom_eq_gain_db(&dsp,fs,frequency);
    assert(isfinite(response)&&fabs(response-direct)<1.e-12);
    assert(memcmp(&dsp,&dsp_before,sizeof(dsp))==0&&memcmp(&engine,&engine_before,sizeof(engine))==0);
    double measured=measured_gain(&dsp,fs,frequency);assert(fabs(response-measured)<.0001);
    if(type==0&&frequency==cutoff)assert(fabs(response-gain)<1.e-9);
    if(type!=0&&frequency==cutoff)assert(fabs(response+3.01029995664)<.000001);
    printf("%.0fHz type%d f%.0f Q%.3f tone%.0f response %.6f dB measured %.6f dB\n",fs,type,cutoff,q,frequency,response,measured);
}
int main(void) {
    SBEngineConfig config=sb_engine_default_config();
    /* Default Voice Boost/normalization/Smart Speed do not belong in this custom-only response. */
    assert(sb_engine_custom_eq_gain_db(&config,48000,1000)==0);
    assert(isnan(sb_engine_custom_eq_gain_db(NULL,48000,1000)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,NAN,1000)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,7999,1000)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,192001,1000)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,48000,NAN)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,48000,0)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,48000,-1)));
    assert(isnan(sb_engine_custom_eq_gain_db(&config,48000,24000)));
    config.eq_count=13;assert(isnan(sb_engine_custom_eq_gain_db(&config,48000,1000)));
    config=sb_engine_default_config();sb_engine_config_set_band(&config,0,99,1000,1,-12);
    assert(sb_engine_custom_eq_gain_db(&config,48000,1000)==0);
    sb_engine_config_set_band(&config,0,0,NAN,1,-12);assert(sb_engine_custom_eq_gain_db(&config,48000,1000)==0);
    sb_engine_config_set_band(&config,0,0,100000,-1,-100);
    double clamped=sb_engine_custom_eq_gain_db(&config,48000,20000);
    sb_engine_config_set_band(&config,0,0,22800,.1f,-24);assert(clamped==sb_engine_custom_eq_gain_db(&config,48000,20000));
    const double rates[]={44100,48000};const float qs[]={.707f,4,8};
    for(size_t r=0;r<2;r++) {
        for(size_t q=0;q<3;q++) {
            compare_pcm(0,1000,qs[q],-12,750,rates[r]);
            compare_pcm(0,1000,qs[q],-12,1000,rates[r]);
            compare_pcm(0,1000,qs[q],-12,1250,rates[r]);
        }
        compare_pcm(2,80,.70710678f,0,20,rates[r]);compare_pcm(2,80,.70710678f,0,80,rates[r]);
        compare_pcm(2,1000,.70710678f,0,250,rates[r]);compare_pcm(2,1000,.70710678f,0,1000,rates[r]);
        compare_pcm(1,1000,.70710678f,0,1000,rates[r]);compare_pcm(1,1000,.70710678f,0,4000,rates[r]);
        compare_pcm(1,12000,.70710678f,0,12000,rates[r]);compare_pcm(1,12000,.70710678f,0,20000,rates[r]);
    }
    sb_dsp_config cascade={0};cascade.normalization_mode=1;cascade.eq_count=2;
    cascade.eq[0]=(sb_dsp_eq_band){2,80,.70710678f,0};cascade.eq[1]=(sb_dsp_eq_band){0,1000,4,-12};
    assert(fabs(sb_dsp_custom_eq_gain_db(&cascade,48000,250)-measured_gain(&cascade,48000,250))<.0001);
    puts("Custom EQ response: flat, invalid input, shared clamps, -12dB centers, HP/LP and cascade match PCM.");
    return 0;
}
