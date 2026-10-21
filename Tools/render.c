/* Offline PCM16 WAV renderer. No platform audio frameworks or whole-file load. */
#include "SBEngine.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096u
#define INPUT_LIMIT (100u * 1024u * 1024u)
typedef struct { unsigned channels, rate, align; uint32_t bytes; long offset; } Wav;
typedef struct { FILE *file; unsigned channels; uint64_t frames, clipped; double peak, source, saved; } Writer;
static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void put16(unsigned char *p,uint16_t n) { p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8); }
static void put32(unsigned char *p,uint32_t n) { for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i)); }
static int read_wav(FILE *f,Wav *w) {
    unsigned char h[16];
    if(fseek(f,0,SEEK_END))return 0;
    long size=ftell(f);
    if(size<12 || size>(long)INPUT_LIMIT || fseek(f,0,SEEK_SET))return 0;
    if(fread(h,1,12,f)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4))return 0;
    uint32_t riff=u32(h+4);
    if(riff<4 || (uint64_t)riff+8>(uint64_t)size)return 0;
    long end=(long)riff+8,pos=12;int fmt=0,data=0;
    while(pos<end) {
        if(end-pos<8 || fseek(f,pos,SEEK_SET) || fread(h,1,8,f)!=8)return 0;
        uint32_t n=u32(h+4);uint64_t next=(uint64_t)pos+8+n+(n&1u);
        if(next>(uint64_t)end)return 0;
        if(!memcmp(h,"fmt ",4)) {
            if(fmt || n<16 || fread(h,1,16,f)!=16)return 0;
            unsigned format=u16(h),channels=u16(h+2),rate=u32(h+4),align=u16(h+12),bits=u16(h+14);
            if(format!=1 || (channels!=1 && channels!=2) || rate<8000 || rate>192000 ||
               bits!=16 || align!=channels*2 || u32(h+8)!=rate*align)return 0;
            w->channels=channels;w->rate=rate;w->align=align;fmt=1;
        } else if(!memcmp(h,"data",4)) {
            if(data)return 0;
            w->offset=pos+8;w->bytes=n;data=1;
        }
        pos=(long)next;
    }
    return fmt && data && w->bytes%w->align==0 && !fseek(f,w->offset,SEEK_SET);
}
static int header(FILE *f,const Wav *w,uint64_t frames) {
    uint64_t bytes=frames*w->align;
    if(bytes>UINT32_MAX-36u)return 0;
    unsigned char h[44]={0};
    memcpy(h,"RIFF",4);put32(h+4,(uint32_t)bytes+36);memcpy(h+8,"WAVEfmt ",8);
    put32(h+16,16);put16(h+20,1);put16(h+22,(uint16_t)w->channels);
    put32(h+24,w->rate);put32(h+28,w->rate*w->align);put16(h+32,(uint16_t)w->align);put16(h+34,16);
    memcpy(h+36,"data",4);put32(h+40,(uint32_t)bytes);
    return !fseek(f,0,SEEK_SET) && fwrite(h,1,sizeof h,f)==sizeof h;
}
static int write_frames(Writer *w,const float *l,const float *r,const double *source,const double *saved,size_t n) {
    unsigned char bytes[CHUNK*4];
    if(n>CHUNK || w->frames+n>(UINT32_MAX-36u)/(w->channels*2u))return 0;
    for(size_t i=0;i<n;i++)for(unsigned c=0;c<w->channels;c++) {
        double x=c?r[i]:l[i];
        if(!isfinite(x))x=0;
        if(fabs(x)>w->peak)w->peak=fabs(x);
        if(x>32767.0/32768.0 || x< -1)w->clipped++;
        if(x>32767.0/32768.0)x=32767.0/32768.0;
        if(x< -1)x=-1;
        int value=(int)lrint(x*32768.0);
        put16(bytes+(i*w->channels+c)*2,(uint16_t)(value<0?value+65536:value));
    }
    if(fwrite(bytes,w->channels*2u,n,w->file)!=n)return 0;
    w->frames+=n;if(n){w->source=source[n-1];w->saved=saved[n-1];}return 1;
}
static int number(const char *s,double low,double high,double *value) {
    char *end;errno=0;double x=strtod(s,&end);
    if(errno || end==s || *end || !isfinite(x) || x<low || x>high)return 0;
    *value=x;return 1;
}
static int flag(const char *s) { return !strcmp(s,"0") || !strcmp(s,"1"); }
int main(int argc,char **argv) {
    double rate,noise=0;
    if((argc!=6 && argc!=7) || !number(argv[3],.5,5,&rate) || !flag(argv[4]) || !flag(argv[5]) ||
       (argc==7 && !number(argv[6],0,1,&noise))) {
        fprintf(stderr,"usage: %s input.wav output.wav rate[0.5..5] smart[0|1] voice[0|1] [noise[0..1]]\n",argv[0]);return 2;
    }
    FILE *input=fopen(argv[1],"rb");if(!input){perror("open input");return 1;}
    Wav wav={0};
    if(!read_wav(input,&wav)) {
        fprintf(stderr,"input must be a bounded RIFF/WAVE PCM16LE file, mono/stereo 8000..192000 Hz, <=100 MiB; invalid/truncated chunks rejected\n");
        fclose(input);return 1;
    }
    SBEngine *engine=sb_engine_create(wav.rate,wav.channels,CHUNK);
    SBEngineConfig config=sb_engine_default_config();config.rate=(float)rate;
    config.smart_speed=argv[4][0]-'0';config.voice_boost=argv[5][0]-'0';config.noise_reduction=(float)noise;
    if(!engine || !sb_engine_configure(engine,&config)){fprintf(stderr,"cannot configure SBEngine\n");sb_engine_destroy(engine);fclose(input);return 1;}
    /* Start in the selected settings rather than fading from create defaults. */
    sb_engine_reset(engine,0);
    /* Exclusive create protects both the source and previously rendered files. */
    FILE *output=fopen(argv[2],"wbx");
    if(!output){perror("create output (must not exist)");sb_engine_destroy(engine);fclose(input);return 1;}
    Writer writer={output,wav.channels,0,0,0,0,0};int ok=header(output,&wav,0);
    unsigned char pcm[CHUNK*4];float in[2][CHUNK],out[2][CHUNK];double source[CHUNK],saved[CHUNK];
    uint64_t remaining=wav.bytes/wav.align,accepted=0;
    while(ok && remaining) {
        size_t n=remaining>CHUNK?CHUNK:(size_t)remaining;
        if(fread(pcm,wav.align,n,input)!=n){fprintf(stderr,"input read failed\n");ok=0;break;}
        for(size_t i=0;i<n;i++)for(unsigned c=0;c<wav.channels;c++) {
            unsigned raw=u16(pcm+(i*wav.channels+c)*2);
            int value=raw>=32768?(int)raw-65536:(int)raw;
            in[c][i]=(float)(value/32768.0);
        }
        size_t offset=0;
        while(ok && offset<n) {
            size_t pushed=sb_engine_push(engine,in[0]+offset,wav.channels==2?in[1]+offset:NULL,n-offset);
            offset+=pushed;accepted+=pushed;size_t drained=0,got;
            do {
                got=sb_engine_read(engine,out[0],wav.channels==2?out[1]:NULL,source,saved,CHUNK);
                if(got && !write_frames(&writer,out[0],out[1],source,saved,got)){ok=0;break;}
                drained+=got;
            } while(got);
            if(!pushed && !drained){fprintf(stderr,"SBEngine stalled before accepting input\n");ok=0;}
        }
        remaining-=n;
    }
    if(ok) {
        sb_engine_finish(engine);unsigned idle=0;
        while(ok && !sb_engine_is_drained(engine)) {
            size_t got=sb_engine_read(engine,out[0],wav.channels==2?out[1]:NULL,source,saved,CHUNK);
            if(got){idle=0;ok=write_frames(&writer,out[0],out[1],source,saved,got);}
            else if(++idle>256){fprintf(stderr,"SBEngine stalled while draining EOF\n");ok=0;}
        }
    }
    if(ok)ok=header(output,&wav,writer.frames) && !fflush(output);
    if(fclose(output))ok=0;
    if(fclose(input))ok=0;
    if(ok)fprintf(stderr,"input=%llu frames %.6fs output=%llu frames %.6fs rate=%.6g smart=%d voice=%d noise=%.3g peak=%.6f clipped_samples=%llu source_last=%.3f saved=%.3f frames (%.6fs) loudness=%.3f LUFS\n",
        (unsigned long long)accepted,accepted/(double)wav.rate,(unsigned long long)writer.frames,writer.frames/(double)wav.rate,
        (double)config.rate,config.smart_speed,config.voice_boost,noise,writer.peak,(unsigned long long)writer.clipped,
        writer.source,writer.saved,writer.saved/wav.rate,(double)sb_engine_loudness(engine));
    else fprintf(stderr,"render failed; incomplete output may remain at %s\n",argv[2]);
    sb_engine_destroy(engine);return ok?0:1;
}
