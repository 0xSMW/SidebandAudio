/* Double-precision FFT accuracy against original arithmetic, including complex input. */
#include "sb_fft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define N SB_FFT_SIZE
#define PI 3.14159265358979323846
static void fft(double *r,double *im,int inverse) {
    for(unsigned i=1,j=0;i<N;i++) { unsigned bit=N>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j) {double t=r[i];r[i]=r[j];r[j]=t;t=im[i];im[i]=im[j];im[j]=t;} }
    for(unsigned len=2;len<=N;len<<=1) {
        double angle=(inverse?2.:-2.)*PI/len,wr0=cos(angle),wi0=sin(angle);
        for(unsigned base=0;base<N;base+=len) { double wr=1,wi=0;
            for(unsigned j=0;j<len/2;j++) { unsigned u=base+j,v=u+len/2;double tr=wr*r[v]-wi*im[v],ti=wr*im[v]+wi*r[v];r[v]=r[u]-tr;im[v]=im[u]-ti;r[u]+=tr;im[u]+=ti;double tmp=wr*wr0-wi*wi0;wi=wr*wi0+wi*wr0;wr=tmp; }
        }
    }
    if(inverse)for(unsigned i=0;i<N;i++){r[i]/=N;im[i]/=N;}
}

int main(void) {
    sb_fft_plan plan;
    sb_fft_prepare(&plan);
    uint32_t rng=1;
    double maximum_error=0;
    for (unsigned pass=0;pass<40;pass++) {
        double r[N],im[N],expected_r[N],expected_im[N];
        for(unsigned i=0;i<N;i++) {
            rng=rng*1664525u+1013904223u;r[i]=((double)(rng>>8)/16777216.-.5)*2.;
            rng=rng*1664525u+1013904223u;im[i]=pass%3?((double)(rng>>8)/16777216.-.5)*2.:0.;
        }
        memcpy(expected_r,r,sizeof(r));memcpy(expected_im,im,sizeof(im));
        fft(expected_r,expected_im,pass%2);
        sb_fft_transform(&plan,r,im,pass%2);
#ifdef SB_FFT_ACCELERATE
        for(unsigned i=0;i<N;i++) {
            double error=fmax(fabs(r[i]-expected_r[i]),fabs(im[i]-expected_im[i]));
            maximum_error=fmax(maximum_error,error);
            assert(error<2.e-11);
        }
#else
        assert(memcmp(r,expected_r,sizeof(r))==0);
        assert(memcmp(im,expected_im,sizeof(im))==0);
#endif
    }
    sb_fft_destroy(&plan);
    printf("FFT plan: 40 real/complex forward/inverse transforms, max double error %.3g (limit 2e-11; portable arithmetic exact)\n",maximum_error);
    return 0;
}
