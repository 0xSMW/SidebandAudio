/* Exercise the private accelerated decision against the original ordered search. */
#include "../../Sources/SBEngine/sb_stretcher.c"
#include <assert.h>
#include <stdio.h>
static int64_t original_grain(const sb_stretcher *s,int64_t center) {
    int64_t low=center-(int64_t)s->hop;if(low<(int64_t)s->base)low=(int64_t)s->base;
    int64_t high=center+(int64_t)s->hop;if(s->eof&&high>=(int64_t)s->total)high=(int64_t)s->total-1;
    if(!s->have_tail||s->reference_energy<=1e-9||high<=low)return center;
    double energy=0,best=-DBL_MAX;int64_t chosen=center;
    for(unsigned c=0;c<s->channels;c++)for(size_t j=0;j<s->hop;j++){double x=sample(s,c,low+(int64_t)j);energy+=x*x;}
    for(int64_t k=low;k<=high;k++) {
        double dot=0;
        for(unsigned c=0;c<s->channels;c++)for(size_t j=0;j<s->hop;j++)dot+=(double)sample(s,c,k+(int64_t)j)*s->reference[c][j];
        double score=dot/sqrt(fmax(0.,energy)+1e-9);
        if(score>best){best=score;chosen=k;}
        if(k<high)for(unsigned c=0;c<s->channels;c++){double old=sample(s,c,k),next=sample(s,c,k+(int64_t)s->hop);energy+=next*next-old*old;}
    }
    return chosen;
}
static void gathered_ring_edges(void) {
    sb_stretcher *s=sb_stretcher_create(44100,2,4096);assert(s);
    s->base=1000;s->total=5000;
    for(uint64_t i=s->base;i<s->total;i++)for(unsigned c=0;c<2;c++)s->audio[c][slot(s,i)]=(float)((double)i*.001*(c?-1:1));
    const int64_t starts[]={1000,3500,4999,5000};const size_t lengths[]={1,20,1500};
    for(unsigned eof=0;eof<2;eof++)for(unsigned c=0;c<2;c++)for(unsigned a=0;a<4;a++)for(unsigned b=0;b<3;b++) {
        s->eof=eof;gather_search(s,c,starts[a],lengths[b]);
        for(size_t j=0;j<lengths[b];j++)assert(s->search[c][j]==sample(s,c,starts[a]+(int64_t)j));
    }
    sb_stretcher_destroy(s);puts("Correlation ring gather: exact wrapped slices, logical end, zero extension and EOF replication");
}
int main(void) {
    gathered_ring_edges();
    const double rates[]={8000,44100,48000,96000,192000};uint32_t rng=17;unsigned cases=0;
    for(unsigned f=0;f<5;f++)for(unsigned channels=1;channels<=2;channels++) {
        size_t window=((size_t)floor(1024*rates[f]/44100+.5)+3)&~(size_t)3;
        sb_stretcher *s=sb_stretcher_create(rates[f],channels,window*8);assert(s);
        for(unsigned mode=0;mode<28;mode++) {
            sb_stretcher_reset(s);size_t n=window*5;
            for(size_t i=0;i<n;i++)for(unsigned c=0;c<channels;c++) {
                rng=rng*1664525u+1013904223u;float random=(float)((double)(rng>>8)/16777216.-.5);
                float x=(float)(.3*sin((double)i*.091)+.11*cos((double)i*.027));
                if(mode%14==0)x=0;else if(mode%14==1)x=.25f;else if(mode%14==2)x=(float)((i%8)*.025);else if(mode%14==3)x=random;else if(mode%14==4)x=i%200==0?.7f:0;
                else if(mode%14==5)x+=(float)(random*1e-7);
                else if(mode%14==12)x=i<window?100000.f:random*1e-10f;
                else if(mode%14==13)x=i<window?1.f:random*1e-16f;
                s->audio[c][i]=(c && mode<14)?-x:x;
            }
            if(mode>=14 && channels==2)memcpy(s->audio[1],s->audio[0],n*sizeof(float));
            s->total=n;s->have_tail=1;s->reference_energy=0;
            for(unsigned c=0;c<channels;c++)for(size_t j=0;j<s->hop;j++) {
                float x=s->audio[c][s->hop+j+37];
                if(mode%14==6)x=(float)sin(j*.077); /* reference absent from the candidate region */
                if(mode%14==7)x=-x;
                s->reference[c][j]=x;s->reference_energy+=(double)x*x;
            }
            for(unsigned edge=0;edge<3;edge++) {
                s->eof=edge==2;int64_t center=edge==0?(int64_t)s->hop:edge==1?(int64_t)(window*2):(int64_t)(n-s->hop/2);
                int64_t want=original_grain(s,center),got=choose_grain(s,center);
                if(got!=want){fprintf(stderr,"lag mismatch fs=%.0f ch=%u mode=%u edge=%u got=%lld want=%lld\n",rates[f],channels,mode,edge,(long long)got,(long long)want);abort();}
                cases++;
            }
        }
        sb_stretcher_destroy(s);
    }
    printf("Correlation: %u exact chosen-lag comparisons including silence, constants, periodic near ties, shared stereo, antiphase, transients, absent references and EOF\n",cases);
    return 0;
}
