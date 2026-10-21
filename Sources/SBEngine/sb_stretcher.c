#include "sb_stretcher.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && !defined(SB_ENGINE_PORTABLE)
#include <Accelerate/Accelerate.h>
#define SB_ACCELERATE 1
#endif

#define SB_PI 3.14159265358979323846264338327950288

struct sb_stretcher {
    unsigned channels;
    size_t capacity, window, hop;
    float *audio[2];
    double *positions, *savings;
    float *weights, *tail[2], *reference[2], *search[2], *stage[2];
    double *stage_positions, *stage_savings;
    uint64_t base, total;
    long double nominal, emitted;
    double rate, stage_rate;
    size_t stage_count, stage_read;
    double last_input_position, last_input_saving;
    double last_output_position, last_output_saving;
    double position_step;
    double reference_energy;
#ifdef SB_ACCELERATE
    double *correlation_input, *correlation_reference, *correlation, *correlation_scratch, *correlation_energy;
    double *correlation_reference_imaginary;
    FFTSetupD correlation_fft;
    size_t correlation_fft_size;
    vDSP_Length correlation_fft_log2;
#endif
    int have_input, have_output, have_tail, eof;
};

static size_t slot(const sb_stretcher *s, uint64_t frame) {
    return (size_t)(frame % s->capacity);
}

static float sample(const sb_stretcher *s, unsigned channel, int64_t frame) {
    if (frame < 0 || (uint64_t)frame < s->base) return 0.0f;
    if ((uint64_t)frame >= s->total) {
        /* Boundary extension is audio support only. Replicating the last sample
         * avoids an artificial zero/fade on short slowed clips; nominal output
         * duration remains capped independently. */
        if (!s->eof || !s->total) return 0.0f;
        frame = (int64_t)s->total - 1;
    }
    return s->audio[channel][slot(s, (uint64_t)frame)];
}

static float finite_float(double x) {
    if (!isfinite(x)) return 0.0f;
    if (x > FLT_MAX) return FLT_MAX;
    if (x < -FLT_MAX) return -FLT_MAX;
    return (float)x;
}

/* Retain the complete next search interval plus interpolation neighbours.
 * Previous-grain audio/reference are in separate fixed buffers. */
static void retire_input(sb_stretcher *s) {
    long double keep = floorl(s->nominal) - (long double)s->hop - 2.0L;
    if (keep <= (long double)s->base) return;
    uint64_t next = keep >= (long double)s->total ? s->total : (uint64_t)keep;
    s->base = next;
}

static double repaired_value(double value, double fallback, double minimum) {
    if (!isfinite(value)) value = fallback;
    if (!isfinite(value) || value < minimum) value = minimum;
    return value;
}

static void metadata_at(const sb_stretcher *s, long double point,
                        double *position, double *saving) {
    if (!s->total) { *position = 0.0; *saving = 0.0; return; }
    if (point < (long double)s->base) point = (long double)s->base;
    uint64_t i = point >= (long double)(s->total - 1)
                     ? s->total - 1 : (uint64_t)floorl(point);
    double fraction = (double)(point - (long double)i);
    double p0 = s->positions[slot(s, i)];
    double q0 = s->savings[slot(s, i)];
    if (i + 1 < s->total) {
        double p1 = s->positions[slot(s, i + 1)];
        double q1 = s->savings[slot(s, i + 1)];
        *position = p0 + (p1 - p0) * fraction;
        *saving = q0 + (q1 - q0) * fraction;
    } else {
        /* The final fractional input frame extends toward the source boundary.
         * Savings do not increase merely because a stretcher pads its audio. */
        double extension = s->position_step * fraction;
        *position = isfinite(extension) && extension <= DBL_MAX - p0
                        ? p0 + extension : p0;
        *saving = q0;
    }
    if (!isfinite(*position)) *position = p0;
    if (!isfinite(*saving)) *saving = q0;
}

static void stage_metadata(sb_stretcher *s, size_t count) {
    double previous_position = s->have_output ? s->last_output_position : 0.0;
    double previous_saving = s->have_output ? s->last_output_saving : 0.0;
    for (size_t i = 0; i < count; ++i) {
        double p, q;
        metadata_at(s, s->nominal + (long double)i * s->rate, &p, &q);
        p = repaired_value(p, previous_position, previous_position);
        q = repaired_value(q, previous_saving, previous_saving);
        s->stage_positions[i] = p;
        s->stage_savings[i] = q;
        previous_position = p;
        previous_saving = q;
    }
}

static size_t remaining_output(const sb_stretcher *s) {
    long double remaining = ((long double)s->total - s->nominal) / s->rate;
    long double tolerance = 32.0L * LDBL_EPSILON *
                            ((long double)s->total + 1.0L);
    if (remaining <= tolerance) return 0;
    long double n = ceill(remaining - tolerance);
    return n >= (long double)SIZE_MAX ? SIZE_MAX : (size_t)n;
}

/* Sample selection, not resampling. A short EOF fragment that needs more output
 * than it contains can repeat its PCM; negative wrapping is used only for that
 * end-anchored, shorter-than-a-hop case. */
static float end_sample(const sb_stretcher *s, unsigned c, int64_t frame) {
    if (frame < 0 && s->base == 0 && s->total) {
        int64_t n = (int64_t)s->total;
        frame %= n;
        if (frame < 0) frame += n;
    }
    return sample(s, c, frame);
}

/* The correlation interval starts at or after base and fits in the ring.
 * Copy its contiguous slices once instead of dividing by capacity per sample;
 * retain the original zero/EOF last-sample extension outside logical input. */
static void gather_search(const sb_stretcher *s,unsigned channel,int64_t low,size_t count) {
    size_t available=(uint64_t)low<s->total?(size_t)(s->total-(uint64_t)low):0;
    if(available>count)available=count;
    if(available) {
        size_t start=slot(s,(uint64_t)low),first=s->capacity-start;
        if(first>available)first=available;
        memcpy(s->search[channel],s->audio[channel]+start,first*sizeof(float));
        if(first<available)memcpy(s->search[channel]+first,s->audio[channel],(available-first)*sizeof(float));
    }
    float extension=sample(s,channel,(int64_t)s->total);
    for(size_t i=available;i<count;i++)s->search[channel][i]=extension;
}

static int64_t choose_grain(const sb_stretcher *s, int64_t center) {
    int64_t low = center - (int64_t)s->hop;
    if (low < (int64_t)s->base) low = (int64_t)s->base;
    int64_t high = center + (int64_t)s->hop;
    if (s->eof && high >= (int64_t)s->total) high = (int64_t)s->total - 1;
    if (!s->have_tail || s->reference_energy <= 1e-9 || high <= low)
        return center;

    int64_t chosen = center;
    double best = -DBL_MAX;
    size_t span = (size_t)(high - low) + s->hop;
    double energy = 0.0;
#ifdef SB_ACCELERATE
    double maximum_input=0.,reference_l1=0.;
#endif
    for (unsigned c = 0; c < s->channels; ++c) {
        gather_search(s,c,low,span);
#ifdef SB_ACCELERATE
        float peak=0;
        vDSP_maxmgv(s->search[c],1,&peak,span);
        maximum_input=fmax(maximum_input,(double)peak);
#endif
        for (size_t j = 0; j < s->hop; ++j) {
            double x = s->search[c][j];
            energy += x * x;
#ifdef SB_ACCELERATE
            reference_l1+=fabs(s->reference[c][j]);
#endif
        }
    }
#ifdef SB_ACCELERATE
    /* Correlate all candidate lags together. Double precision preserves the
     * scalar decision except at numerical ties, which are checked below using
     * the original ordered sum. All scratch storage belongs to the instance. */
    size_t candidates = (size_t)(high-low)+1;
    if(s->correlation_fft) {
        size_t size=s->correlation_fft_size;
        int shared=s->channels==1 ||
            (memcmp(s->search[0],s->search[1],span*sizeof(float))==0 &&
             memcmp(s->reference[0],s->reference[1],s->hop*sizeof(float))==0);
        DSPDoubleSplitComplex input={s->correlation_input,s->correlation_scratch};
        shared=shared && maximum_input>0.;
        int input_exponent=0,reference_exponent=0;
        if(shared) {
            /* Pack the two real signals into one forward FFT. Recover their
             * spectra from conjugate pairs before forming their correlation. */
            vDSP_vspdp(s->search[0],1,input.realp,1,span);
            vDSP_vspdp(s->reference[0],1,input.imagp,1,s->hop);
            memset(input.realp+span,0,(size-span)*sizeof(double));
            memset(input.imagp+s->hop,0,(size-s->hop)*sizeof(double));
            /* Exact power-of-two scaling keeps both packed signals comparable.
             * Without it, a loud reference can erase a quiet input (or vice
             * versa) when conjugate pairs are subtracted after the FFT. */
            float reference_maximum=0.;
            vDSP_maxmgv(s->reference[0],1,&reference_maximum,s->hop);
            input_exponent=ilogb(maximum_input);
            reference_exponent=ilogb((double)reference_maximum);
            double input_scale=scalbn(1.,-input_exponent),reference_scale=scalbn(1.,-reference_exponent);
            vDSP_vsmulD(input.realp,1,&input_scale,input.realp,1,span);
            vDSP_vsmulD(input.imagp,1,&reference_scale,input.imagp,1,s->hop);
            vDSP_fft_zipD(s->correlation_fft,&input,1,s->correlation_fft_log2,FFT_FORWARD);
            for(size_t k=0;k<=size/2;k++) {
                size_t mirror=k?size-k:0;
                double zr=input.realp[k],zi=input.imagp[k];
                double wr=input.realp[mirror],wi=input.imagp[mirror];
                double ar=(zr+wr)*.5,ai=(zi-wi)*.5;
                double br=(zi+wi)*.5,bi=(wr-zr)*.5;
                double real=ar*br+ai*bi,imaginary=ai*br-ar*bi;
                input.realp[k]=real;input.imagp[k]=imaginary;
                if(mirror!=k){input.realp[mirror]=real;input.imagp[mirror]=-imaginary;}
            }
        } else {
            /* Re(correlation(L+iR, refL+iRefR)) is the joint stereo dot
             * product. Padding to >= input span is sufficient for these valid
             * nonwrapping lags, so both channels need only three complex FFTs. */
            vDSP_vspdp(s->search[0],1,s->correlation_input,1,span);
            vDSP_vspdp(s->reference[0],1,s->correlation_reference,1,s->hop);
            if(s->channels==2) {
                vDSP_vspdp(s->search[1],1,s->correlation_scratch,1,span);
                vDSP_vspdp(s->reference[1],1,s->correlation_reference_imaginary,1,s->hop);
            } else {
                memset(s->correlation_scratch,0,span*sizeof(double));
                memset(s->correlation_reference_imaginary,0,s->hop*sizeof(double));
            }
            memset(s->correlation_input+span,0,(size-span)*sizeof(double));
            memset(s->correlation_scratch+span,0,(size-span)*sizeof(double));
            memset(s->correlation_reference+s->hop,0,(size-s->hop)*sizeof(double));
            memset(s->correlation_reference_imaginary+s->hop,0,(size-s->hop)*sizeof(double));
            DSPDoubleSplitComplex reference={s->correlation_reference,s->correlation_reference_imaginary};
            vDSP_fft_zipD(s->correlation_fft,&input,1,s->correlation_fft_log2,FFT_FORWARD);
            vDSP_fft_zipD(s->correlation_fft,&reference,1,s->correlation_fft_log2,FFT_FORWARD);
            for(size_t k=0;k<size;k++) {
                double a=input.realp[k]*reference.realp[k]+input.imagp[k]*reference.imagp[k];
                double b=input.imagp[k]*reference.realp[k]-input.realp[k]*reference.imagp[k];
                input.realp[k]=a;input.imagp[k]=b;
            }
        }
        vDSP_fft_zipD(s->correlation_fft,&input,1,s->correlation_fft_log2,FFT_INVERSE);
        double scale=scalbn((shared?s->channels:1.)/size,input_exponent+reference_exponent);
        vDSP_vsmulD(input.realp,1,&scale,s->correlation,1,candidates);
    } else for (unsigned c=0;c<s->channels;c++) {
        /* Allocation of the FFT plan may fail independently of scratch storage. */
        vDSP_vspdp(s->search[c],1,s->correlation_input,1,span);
        vDSP_vspdp(s->reference[c],1,s->correlation_reference,1,s->hop);
        vDSP_convD(s->correlation_input,1,s->correlation_reference,1,
                   s->correlation_scratch,1,candidates,s->hop);
        if(c==0) memcpy(s->correlation,s->correlation_scratch,candidates*sizeof(double));
        else vDSP_vaddD(s->correlation,1,s->correlation_scratch,1,s->correlation,1,candidates);
    }
    double minimum_energy=energy;
    for(size_t k=0;k<candidates;k++) {
        minimum_energy=fmin(minimum_energy,energy);
        s->correlation_energy[k]=energy;
        double score=s->correlation[k]/sqrt(fmax(0.,energy)+1e-9);
        s->correlation[k]=score;
        if(score>best) best=score;
        if(k+1<candidates) for(unsigned c=0;c<s->channels;c++) {
            const float *candidate=s->search[c]+k;
            double old=candidate[0],next=candidate[s->hop];
            energy+=next*next-old*old;
        }
    }
    /* Bound absolute dot-product rounding with max(abs(input))*sum(abs(ref)),
     * rather than trusting rolling energy after loud-to-silent cancellation.
     * The minimum actual denominator also covers negative rolling energies.
     * A generous bound covers both orders, and all near-best candidates are
     * checked using the original sum/denominator and first-best tie rule. */
    double tolerance=64.*DBL_EPSILON*(double)(s->channels*s->hop)*maximum_input*reference_l1
                     /sqrt(fmax(0.,minimum_energy)+1e-9);
    double approximate_best=best;
    best=-DBL_MAX;
    for(size_t k=0;k<candidates;k++) if(approximate_best-s->correlation[k]<=tolerance) {
        double dot=0.;
        for(unsigned c=0;c<s->channels;c++)
            for(size_t j=0;j<s->hop;j++) dot+=(double)s->search[c][k+j]*s->reference[c][j];
        double score=dot/sqrt(fmax(0.,s->correlation_energy[k])+1e-9);
        if(score>best) {best=score;chosen=low+(int64_t)k;}
    }
    return chosen;
#else
    /* Joint channel correlation cannot cancel on antiphase stereo. All channels
     * still share exactly one lag/window, preserving their relative phase. */
    for (int64_t k = low; k <= high; ++k) {
        double dot = 0.0;
        for (unsigned c = 0; c < s->channels; ++c) {
            const float *candidate = s->search[c] + (size_t)(k - low);
            for (size_t j = 0; j < s->hop; ++j)
                dot += (double)candidate[j] * s->reference[c][j];
        }
        double score = dot / sqrt(fmax(0.0, energy) + 1e-9);
        if (score > best) { best = score; chosen = k; }
        if (k < high) {
            for (unsigned c = 0; c < s->channels; ++c) {
                const float *candidate = s->search[c] + (size_t)(k - low);
                double old = candidate[0], next = candidate[s->hop];
                energy += next * next - old * old;
            }
        }
    }
    return chosen;
#endif
}

static int make_stage(sb_stretcher *s) {
    if (s->stage_read < s->stage_count) return 1;
    s->stage_count = s->stage_read = 0;
    if (s->nominal >= (long double)s->total) return 0;

    size_t count = s->hop;
    int final_hop = 0;
    int unity = s->rate == 1.0;
    if (s->eof) {
        size_t remaining = remaining_output(s);
        if (!remaining) return 0;
        if (remaining <= count) { count = remaining; final_hop = 1; }
    } else if (unity) {
        long double available = (long double)s->total - s->nominal;
        if (available < (long double)count) count = (size_t)ceill(available);
        if (!count) return 0;
    } else {
        int64_t center = (int64_t)floorl(s->nominal + 0.5L);
        long double support_end = (long double)center + s->hop + s->window;
        /* Hold one logical output sample for EOF, including at high rates where
         * the correlation lookahead can be shorter than the analysis hop. */
        long double clock_end = s->nominal + (long double)(s->hop + 1) * s->rate;
        if (support_end > (long double)s->total ||
            clock_end > (long double)s->total) return 0;
    }

    stage_metadata(s, count);
    int64_t center = (int64_t)floorl(s->nominal + 0.5L);
    if (center < (int64_t)s->base) center = (int64_t)s->base;
    if (unity) {
        /* A stream that stays at unity never windows/interpolates its audio. */
        int64_t start = (int64_t)floorl(s->nominal);
        for (unsigned c = 0; c < s->channels; ++c) {
            for (size_t j = 0; j < count; ++j) {
                float x = sample(s, c, start + (int64_t)j);
                if (s->have_tail) {
                    double a = final_hop && count > 1
                        ? 0.5 * (1.0 - cos(SB_PI * (double)j / (double)(count - 1)))
                        : s->weights[j];
                    if (count == 1) a = 1.0;
                    x = finite_float((1.0 - a) * s->tail[c][j] + a * x);
                }
                s->stage[c][j] = x;
            }
        }
        s->have_tail = 0;
    } else if (final_hop) {
        /* End-anchor the last fragment rather than losing the enclosure's tail
         * to the final analysis-hop skip. The shorter fade reaches full weight
         * at its last sample; padding never lengthens the output clock. */
        int64_t start = (int64_t)s->total - (int64_t)count;
        for (unsigned c = 0; c < s->channels; ++c) {
            for (size_t j = 0; j < count; ++j) {
                float x = end_sample(s, c, start + (int64_t)j);
                double a = count > 1
                    ? 0.5 * (1.0 - cos(SB_PI * (double)j / (double)(count - 1))) : 1.0;
                s->stage[c][j] = s->have_tail
                    ? finite_float((1.0 - a) * s->tail[c][j] + a * x) : x;
            }
        }
        s->have_tail = 0;
    } else {
        int64_t chosen = choose_grain(s, center);
        for (unsigned c = 0; c < s->channels; ++c) {
            for (size_t j = 0; j < s->hop; ++j) {
                float x = sample(s, c, chosen + (int64_t)j);
                double a = s->weights[j];
                s->stage[c][j] = s->have_tail
                    ? finite_float(a * x + (1.0 - a) * s->tail[c][j]) : x;
                s->tail[c][j] = sample(s, c, chosen + (int64_t)s->hop + (int64_t)j);
            }
        }
        s->reference_energy = 0.0;
        for (unsigned c = 0; c < s->channels; ++c) {
            for (size_t j = 0; j < s->hop; ++j) {
                double x = s->tail[c][j];
                s->reference[c][j] = (float)x;
                s->reference_energy += x * x;
            }
        }
        s->have_tail = 1;
    }
    s->stage_count = count;
    s->stage_rate = s->rate;
    s->nominal += (long double)count * s->rate;
    retire_input(s);
    return 1;
}

sb_stretcher *sb_stretcher_create(double sample_rate, unsigned channels,
                                 size_t max_buffer_frames) {
    if (!isfinite(sample_rate) || sample_rate < 8000.0 || sample_rate > 384000.0 ||
        channels < 1 || channels > 2) return NULL;
    size_t window = ((size_t)floor(1024.0 * sample_rate / 44100.0 + 0.5) + 3u) & ~(size_t)3u;
    if (window < 4 || max_buffer_frames < 4 * window ||
        max_buffer_frames > SIZE_MAX / sizeof(double)) return NULL;
    sb_stretcher *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->channels = channels;
    s->capacity = max_buffer_frames;
    s->window = window;
    s->hop = window / 2;
    s->rate = 1.0;
    s->position_step = 1.0;
    s->positions = calloc(max_buffer_frames, sizeof(double));
    s->savings = calloc(max_buffer_frames, sizeof(double));
#ifdef SB_ACCELERATE
    s->correlation_fft_size=1;
    while(s->correlation_fft_size<window+s->hop){s->correlation_fft_size*=2;s->correlation_fft_log2++;}
    s->correlation_fft=vDSP_create_fftsetupD(s->correlation_fft_log2,kFFTRadix2);
    s->correlation_input=calloc(s->correlation_fft_size,sizeof(double));
    s->correlation_reference=calloc(s->correlation_fft_size,sizeof(double));
    s->correlation_reference_imaginary=calloc(s->correlation_fft_size,sizeof(double));
    s->correlation=calloc(window+1,sizeof(double));
    s->correlation_scratch=calloc(s->correlation_fft_size,sizeof(double));
    s->correlation_energy=calloc(window+1,sizeof(double));
    if(!s->correlation_input||!s->correlation_reference||!s->correlation||!s->correlation_scratch||!s->correlation_energy||!s->correlation_reference_imaginary) {
        sb_stretcher_destroy(s); return NULL;
    }
#endif
    s->weights = calloc(window, sizeof(float));
    s->stage_positions = calloc(s->hop, sizeof(double));
    s->stage_savings = calloc(s->hop, sizeof(double));
    for (unsigned c = 0; c < channels; ++c) {
        s->reference[c] = calloc(s->hop, sizeof(float));
        s->search[c] = calloc(window + s->hop, sizeof(float));
        s->audio[c] = calloc(max_buffer_frames, sizeof(float));
        s->tail[c] = calloc(s->hop, sizeof(float));
        s->stage[c] = calloc(s->hop, sizeof(float));
    }
    if (!s->positions || !s->savings || !s->weights || !s->reference[0] ||
        !s->search[0] || !s->stage_positions || !s->stage_savings || !s->audio[0] || !s->tail[0] ||
        !s->stage[0] || (channels == 2 && (!s->audio[1] || !s->tail[1] || !s->stage[1] ||
        !s->reference[1] || !s->search[1]))) {
        sb_stretcher_destroy(s); return NULL;
    }
    for (size_t j = 0; j < window; ++j)
        s->weights[j] = (float)(0.5 * (1.0 - cos(2.0 * SB_PI * (double)j / (double)window)));
    return s;
}

void sb_stretcher_destroy(sb_stretcher *s) {
    if (!s) return;
    for (unsigned c = 0; c < 2; ++c) {
        free(s->audio[c]); free(s->tail[c]); free(s->stage[c]);
        free(s->reference[c]); free(s->search[c]);
    }
#ifdef SB_ACCELERATE
    if(s->correlation_fft)vDSP_destroy_fftsetupD(s->correlation_fft);
    free(s->correlation_reference_imaginary);
    free(s->correlation_input);free(s->correlation_reference);free(s->correlation);
    free(s->correlation_scratch);free(s->correlation_energy);
#endif
    free(s->positions); free(s->savings); free(s->weights);
    free(s->stage_positions); free(s->stage_savings); free(s);
}

void sb_stretcher_reset(sb_stretcher *s) {
    if (!s) return;
    s->base = s->total = 0;
    s->nominal = s->emitted = 0.0L;
    s->stage_count = s->stage_read = 0;
    s->have_input = s->have_output = s->have_tail = s->eof = 0;
    s->reference_energy = 0.0;
    s->last_input_position = s->last_input_saving = 0.0;
    s->last_output_position = s->last_output_saving = 0.0;
    s->position_step = 1.0;
    for (unsigned c = 0; c < s->channels; ++c) {
        memset(s->tail[c], 0, s->hop * sizeof(float));
        memset(s->reference[c], 0, s->hop * sizeof(float));
    }
}

int sb_stretcher_set_rate(sb_stretcher *s, double rate) {
    if (!s || s->eof || !isfinite(rate) || rate < 0.5 || rate > 5.0) return 0;
    s->rate = rate;
    return 1;
}

size_t sb_stretcher_push(sb_stretcher *s, const float *const input[2],
                         const double *positions, const double *savings, size_t frames) {
    if (!s || s->eof || !input || !input[0] || (s->channels == 2 && !input[1])) return 0;
    retire_input(s);
    size_t room = s->capacity - (size_t)(s->total - s->base);
    if (frames > room) frames = room;
    uint64_t limit = (uint64_t)INT64_MAX / 4;
    if ((uint64_t)frames > limit - s->total) frames = (size_t)(limit - s->total);
    for (size_t j = 0; j < frames; ++j) {
        size_t k = slot(s, s->total);
        for (unsigned c = 0; c < s->channels; ++c) {
            float x = isfinite(input[c][j]) ? input[c][j] : 0.0f;
            s->audio[c][k] = x;
        }
        double p = positions ? positions[j] : (double)s->total;
        double q = savings ? savings[j] : s->last_input_saving;
        p = repaired_value(p, s->have_input ? s->last_input_position : (double)s->total,
                           s->have_input ? s->last_input_position : 0.0);
        q = repaired_value(q, s->last_input_saving, s->last_input_saving);
        if (s->have_input) {
            double step = p - s->last_input_position;
            /* Do not extrapolate a discontinuous Smart Speed jump at EOF. */
            if (step > 0.0 && step <= 4.0) s->position_step = step;
        }
        s->positions[k] = s->last_input_position = p;
        s->savings[k] = s->last_input_saving = q;
        s->have_input = 1;
        ++s->total;
    }
    return frames;
}

size_t sb_stretcher_pull(sb_stretcher *s, float *const output[2],
                         double *positions, double *savings, size_t capacity, int finished) {
    if (!s) return 0;
    if (finished) s->eof = 1;
    if (!capacity || !output || !output[0] || (s->channels == 2 && !output[1])) return 0;
    size_t produced = 0;
    while (produced < capacity && make_stage(s)) {
        size_t n = s->stage_count - s->stage_read;
        if (n > capacity - produced) n = capacity - produced;
        for (unsigned c = 0; c < s->channels; ++c)
            memcpy(output[c] + produced, s->stage[c] + s->stage_read, n * sizeof(float));
        if (positions) memcpy(positions + produced, s->stage_positions + s->stage_read, n * sizeof(double));
        if (savings) memcpy(savings + produced, s->stage_savings + s->stage_read, n * sizeof(double));
        s->last_output_position = s->stage_positions[s->stage_read + n - 1];
        s->last_output_saving = s->stage_savings[s->stage_read + n - 1];
        s->have_output = 1;
        s->emitted += (long double)n * s->stage_rate;
        s->stage_read += n;
        produced += n;
    }
    return produced;
}

size_t sb_stretcher_buffered_frames(const sb_stretcher *s) {
    if (!s) return 0;
    if (s->eof && s->stage_read == s->stage_count && !remaining_output(s)) return 0;
    long double n = (long double)s->total - s->emitted;
    if (n <= 0.0L) return 0;
    n = ceill(n);
    return n >= (long double)SIZE_MAX ? SIZE_MAX : (size_t)n;
}

size_t sb_stretcher_writable_frames(const sb_stretcher *s) {
    if (!s || s->eof) return 0;
    return s->capacity - (size_t)(s->total - s->base);
}

int sb_stretcher_is_drained(const sb_stretcher *s) {
    return s && s->eof && s->stage_read == s->stage_count && !remaining_output(s);
}
