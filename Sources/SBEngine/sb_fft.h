#ifndef SB_FFT_H
#define SB_FFT_H

#include <math.h>
#include <stdint.h>
#if defined(__APPLE__) && !defined(SB_ENGINE_PORTABLE)
#include <Accelerate/Accelerate.h>
#define SB_FFT_ACCELERATE 1
#endif

#define SB_FFT_SIZE 1024u

/* Instance-owned immutable plan: no allocation, locks, or trigonometry in a transform.
   Generate each stage with the original recurrence, rather than independently
   evaluating each angle, to retain the existing floating-point results. */
typedef struct {
#ifdef SB_FFT_ACCELERATE
    FFTSetupD accelerated;
#endif
    uint16_t reversed[SB_FFT_SIZE];
    double real[2][SB_FFT_SIZE - 1], imag[2][SB_FFT_SIZE - 1];
} sb_fft_plan;

static void sb_fft_prepare(sb_fft_plan *plan) {
#ifdef SB_FFT_ACCELERATE
    plan->accelerated=vDSP_create_fftsetupD(10,kFFTRadix2);
#endif
    for (unsigned i = 0, j = 0; i < SB_FFT_SIZE; i++) {
        plan->reversed[i] = (uint16_t)j;
        unsigned bit = SB_FFT_SIZE >> 1;
        for (; bit && (j & bit); bit >>= 1) j ^= bit;
        j ^= bit;
    }
    for (unsigned inverse = 0; inverse < 2; inverse++) {
        unsigned offset = 0;
        for (unsigned len = 2; len <= SB_FFT_SIZE; len <<= 1) {
            double angle = (inverse ? 2. : -2.) * 3.14159265358979323846 / len;
            double wr0 = cos(angle), wi0 = sin(angle), wr = 1, wi = 0;
            for (unsigned j = 0; j < len / 2; j++) {
                plan->real[inverse][offset + j] = wr;
                plan->imag[inverse][offset + j] = wi;
                double tmp = wr * wr0 - wi * wi0;
                wi = wr * wi0 + wi * wr0;
                wr = tmp;
            }
            offset += len / 2;
        }
    }
}

static void sb_fft_destroy(sb_fft_plan *plan) {
#ifdef SB_FFT_ACCELERATE
    if(plan->accelerated) vDSP_destroy_fftsetupD(plan->accelerated);
#else
    (void)plan;
#endif
}

static void sb_fft_transform(const sb_fft_plan *plan, double *r, double *im, unsigned inverse) {
#ifdef SB_FFT_ACCELERATE
    if(plan->accelerated) {
        DSPDoubleSplitComplex split={r,im};
        vDSP_fft_zipD(plan->accelerated,&split,1,10,inverse?FFT_INVERSE:FFT_FORWARD);
        if(inverse) {double scale=1./SB_FFT_SIZE;vDSP_vsmulD(r,1,&scale,r,1,SB_FFT_SIZE);vDSP_vsmulD(im,1,&scale,im,1,SB_FFT_SIZE);}
        return;
    }
#endif
    for (unsigned i = 1; i < SB_FFT_SIZE; i++) {
        unsigned j = plan->reversed[i];
        if (i < j) {
            double t = r[i]; r[i] = r[j]; r[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    unsigned offset = 0;
    for (unsigned len = 2; len <= SB_FFT_SIZE; len <<= 1) {
        for (unsigned base = 0; base < SB_FFT_SIZE; base += len) {
            for (unsigned j = 0; j < len / 2; j++) {
                unsigned u = base + j, v = u + len / 2;
                double wr = plan->real[inverse][offset + j], wi = plan->imag[inverse][offset + j];
                double tr = wr * r[v] - wi * im[v], ti = wr * im[v] + wi * r[v];
                r[v] = r[u] - tr; im[v] = im[u] - ti;
                r[u] += tr; im[u] += ti;
            }
        }
        offset += len / 2;
    }
    if (inverse) for (unsigned i = 0; i < SB_FFT_SIZE; i++) { r[i] /= SB_FFT_SIZE; im[i] /= SB_FFT_SIZE; }
}

#endif
