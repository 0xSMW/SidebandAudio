/* Compare collapsed mono recurrences with both original channel recurrences. */
#include "../../Sources/SBEngine/sb_dsp.c"
#include <assert.h>
#include <stdio.h>
static float input[2][997],a[2][2048],b[2][2048];
static size_t compare(sb_dsp *fast,sb_dsp *reference,size_t n) {
    const float *in[2]={input[0],input[1]};float *left[2]={a[0],a[1]},*right[2]={b[0],b[1]};
    size_t x=sb_dsp_process(fast,in,n,left,2048),y=sb_dsp_process(reference,in,n,right,2048);assert(x==y);
    assert(memcmp(a[0],b[0],x*sizeof(float))==0);assert(memcmp(a[1],b[1],x*sizeof(float))==0);return x;
}
static void shifted_alias_fallback(void) {
    const size_t n=4096,capacity=6200;unsigned cases=0;
    for(unsigned shape=0;shape<4;shape++) {
        float *input[2],*reference_input[2];
        for(unsigned c=0;c<2;c++) {
            input[c]=calloc(capacity,sizeof(float));reference_input[c]=calloc(capacity,sizeof(float));assert(input[c]&&reference_input[c]);
            for(size_t i=0;i<capacity;i++)input[c][i]=reference_input[c][i]=(float)(.17*sin(i*(c?.079:.113)));
        }
        size_t source_offset=shape==1?17:0,destination_offset=shape==1?0:1505;
        const float *src[2]={input[0]+source_offset,input[1]+source_offset};
        const float *gold_src[2]={reference_input[0]+source_offset,reference_input[1]+source_offset};
        unsigned swap=shape>=2;
        float *dst[2]={input[swap]+destination_offset,input[!swap]+destination_offset};
        float *gold_dst[2]={reference_input[swap]+destination_offset,reference_input[!swap]+destination_offset};
        if(shape==3){dst[1]=dst[0];gold_dst[1]=gold_dst[0];}
        sb_dsp_config c={0};c.mono=1;c.voice_boost=1;c.deesser=1;c.normalization_mode=1;
        sb_dsp *fast=sb_dsp_create(48000,2,n),*reference=sb_dsp_create(48000,2,n);assert(fast&&reference);
        sb_dsp_configure(fast,&c);sb_dsp_configure(reference,&c);sb_dsp_reset(fast);sb_dsp_reset(reference);reference->mono_fast=0;
        size_t got=sb_dsp_process(fast,src,n,dst,n),want=sb_dsp_process(reference,gold_src,n,gold_dst,n);assert(got==want);
        for(unsigned ch=0;ch<2;ch++)assert(memcmp(input[ch],reference_input[ch],capacity*sizeof(float))==0);
        float x[2][256],y[2][256];float *out[2]={x[0],x[1]},*gold[2]={y[0],y[1]};
        do {got=sb_dsp_flush(fast,out,256);want=sb_dsp_flush(reference,gold,256);assert(got==want);assert(memcmp(x[0],y[0],got*4)==0);assert(memcmp(x[1],y[1],got*4)==0);}while(got);
        sb_dsp_destroy(fast);sb_dsp_destroy(reference);
        for(unsigned ch=0;ch<2;ch++){free(input[ch]);free(reference_input[ch]);}cases++;
    }
    printf("Shifted DSP aliases: %u forward/backward/cross/shared-output layouts preserve scalar source-read/write order\n",cases);
}
int main(void) {
    shifted_alias_fallback();
    const double rates[]={8000,48000,192000};unsigned blocks=0;
    for(unsigned f=0;f<3;f++)for(unsigned shape=0;shape<8;shape++) {
        sb_dsp *fast=sb_dsp_create(rates[f],2,997),*reference=sb_dsp_create(rates[f],2,997);assert(fast&&reference);
        sb_dsp_config c={0};c.mono=1;c.voice_boost=1;c.deesser=1;c.normalization_mode=1;
        sb_dsp_configure(fast,&c);sb_dsp_configure(reference,&c);sb_dsp_reset(fast);sb_dsp_reset(reference);reference->mono_fast=0;
        for(unsigned block=0;block<80;block++) {
            if(block==3){c.eq_count=1;c.eq[0]=(sb_dsp_eq_band){0,1800,.8,5};}
            if(block==7)c.voice_boost=0;
            if(block==10)c.noise_reduction=1;
            if(block==15)c.noise_reduction=0; /* tiny residual must not reenter the mono path */
            if(block==25)c.mono=0;
            if(block==30)c.mono=1;
            if(block==40){c.noise_reduction=0;sb_dsp_configure(fast,&c);sb_dsp_configure(reference,&c);sb_dsp_reset(fast);sb_dsp_reset(reference);reference->mono_fast=0;}
            if(block==50)c.normalization_mode=2;
            if(block==60)c.deesser=2;
            sb_dsp_configure(fast,&c);sb_dsp_configure(reference,&c);
            for(size_t i=0;i<997;i++) {
                float x=(float)(.31*sin((block*997+i)*.13)),y=(float)(.2*cos((block*997+i)*.031));
                if(shape==0)y=1.e-35f;
                if(shape==1){x=1.e-35f;y=-.7f;}
                if(shape==2)y=-x;
                if(shape==3)y=nextafterf(x,INFINITY);
                if(shape==4){x=i%2?64.f:-64.f;y=i%3?1.e-40f:-1.e-40f;}
                if(shape==5){x=i%2?1.e30f:-1.e30f;y=-x;}
                if(shape==6){x=0.f;y=-0.f;}
                input[0][i]=x;input[1][i]=y;
            }
            compare(fast,reference,997);blocks++;
        }
        float *left[2]={a[0],a[1]},*right[2]={b[0],b[1]};size_t n,m;
        do {n=sb_dsp_flush(fast,left,2048);m=sb_dsp_flush(reference,right,2048);assert(n==m);assert(memcmp(a[0],b[0],n*4)==0);assert(memcmp(a[1],b[1],n*4)==0);}while(n);
        sb_dsp_destroy(fast);sb_dsp_destroy(reference);
    }
    printf("Mono recurrence: %u blocks bit-identical across exponent extremes, antiphase, signed zero, EQ/mono/NR/normalization/de-esser transitions, resets and EOF\n",blocks);return 0;
}
