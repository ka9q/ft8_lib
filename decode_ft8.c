// Actual decoding code factored out by KA9Q June 2025
// File/directory handling code is now in main.c

#define _GNU_SOURCE 1
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <libgen.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>

#include "ft8/unpack.h"
#include "ft8/ldpc.h"
#include "ft8/decode.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/crc.h"

#include "common/common.h"
#include "common/wave.h"
#include "common/debug.h"
#include "fft/kiss_fftr.h"

#define LOG_LEVEL LOG_FATAL

const int kMin_score = 10; // Minimum sync score threshold for candidates
const int kMax_candidates = 120;
const int kLDPC_iterations = 20;

const int kMax_decoded_messages = 50;

const int kFreq_osr = 2; // Frequency oversampling rate (bin subdivision)
const int kTime_osr = 2; // Time oversampling rate (symbol subdivision)
static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

static float hamming_i(int i, int N)
{
    const float a0 = (float)25 / 46;
    const float a1 = 1 - a0;

    float x1 = cosf(2 * (float)M_PI * i / N);
    return a0 - a1 * x1;
}

static float blackman_i(int i, int N)
{
    const float alpha = 0.16f; // or 2860/18608
    const float a0 = (1 - alpha) / 2;
    const float a1 = 1.0f / 2;
    const float a2 = alpha / 2;

    float x1 = cosf(2 * (float)M_PI * i / N);
    float x2 = 2 * x1 * x1 - 1; // Use double angle formula

    return a0 - a1 * x1 + a2 * x2;
}

void waterfall_init(waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
    size_t mag_size = max_blocks * time_osr * freq_osr * num_bins * sizeof(me->mag[0]);
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->block_stride = (time_osr * freq_osr * num_bins);
    me->mag = (uint8_t  *)malloc(mag_size);
    LOG(LOG_DEBUG, "Waterfall size = %zu\n", mag_size);
}

void waterfall_free(waterfall_t* me)
{
    free(me->mag);
}

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    waterfall_t wf;      ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // KISS FFT housekeeping variables
    void* fft_work;        ///< Work area required by Kiss FFT
    kiss_fftr_cfg fft_cfg; ///< Kiss FFT housekeeping object
} monitor_t;

void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == PROTO_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == PROTO_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    me->window = (float *)malloc(me->nfft * sizeof(me->window[0]));
    for (int i = 0; i < me->nfft; ++i)
    {
        // window[i] = 1;
        me->window[i] = hann_i(i, me->nfft);
        // me->window[i] = blackman_i(i, me->nfft);
        // me->window[i] = hamming_i(i, me->nfft);
        // me->window[i] = (i < len_window) ? hann_i(i, len_window) : 0;
    }
    me->last_frame = (float *)malloc(me->nfft * sizeof(me->last_frame[0]));

    size_t fft_work_size;
    kiss_fftr_alloc(me->nfft, 0, 0, &fft_work_size);

    LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);
    LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);
    LOG(LOG_DEBUG, "FFT work area = %zu\n", fft_work_size);

    me->fft_work = malloc(fft_work_size);
    me->fft_cfg = kiss_fftr_alloc(me->nfft, 0, me->fft_work, &fft_work_size);

    const int max_blocks = (int)(slot_time / symbol_period);
    const int num_bins = (int)(cfg->sample_rate * symbol_period / 2);
    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.protocol = cfg->protocol;
    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
}

void monitor_free(monitor_t* me)
{
    waterfall_free(&me->wf);
    free(me->fft_work);
    free(me->last_frame);
    free(me->window);
}

// Compute FFT magnitudes (log wf) for a frame in the signal and update waterfall data
void monitor_process(monitor_t* me, const float* frame)
{
    // Check if we can still store more waterfall data
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    int offset = me->wf.num_blocks * me->wf.block_stride;
    int frame_pos = 0;

    // Loop over block subdivisions
    for (int time_sub = 0; time_sub < me->wf.time_osr; ++time_sub)
    {
        kiss_fft_scalar timedata[me->nfft];
        kiss_fft_cpx freqdata[me->nfft / 2 + 1];

        // Shift the new data into analysis frame
        for (int pos = 0; pos < me->nfft - me->subblock_size; ++pos)
        {
            me->last_frame[pos] = me->last_frame[pos + me->subblock_size];
        }
        for (int pos = me->nfft - me->subblock_size; pos < me->nfft; ++pos)
        {
            me->last_frame[pos] = frame[frame_pos];
            ++frame_pos;
        }

        // Compute windowed analysis frame
        for (int pos = 0; pos < me->nfft; ++pos)
        {
            timedata[pos] = me->fft_norm * me->window[pos] * me->last_frame[pos];
        }

        kiss_fftr(me->fft_cfg, timedata, freqdata);

        // Loop over two possible frequency bin offsets (for averaging)
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = 0; bin < me->wf.num_bins; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                float mag2 = (freqdata[src_bin].i * freqdata[src_bin].i) + (freqdata[src_bin].r * freqdata[src_bin].r);
                float db = 10.0f * log10f(1E-12f + mag2);
                // Scale decibels to unsigned 8-bit range and clamp the value
                // Range 0-240 covers -120..0 dB in 0.5 dB steps
                int scaled = (int)(2 * db + 240);

                me->wf.mag[offset] = (scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled);
                ++offset;

                if (db > me->max_mag)
                    me->max_mag = db;
            }
        }
    }

    ++me->wf.num_blocks;
}

void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = 0;
}

// Process a buffer already loaded from a file
// 'path' is passed only to extract date & time, the file is actually read in main()
int process_buffer(float const *signal,int sample_rate, int num_samples, bool is_ft8, float base_freq, char const *path){

  LOG(LOG_INFO, "Sample rate %d Hz, %d samples, %.3f seconds\n", sample_rate, num_samples, (double)num_samples / sample_rate);

  // Compute FFT over the whole signal and store it
  monitor_t mon = {0};
  monitor_config_t mon_cfg = {
    .f_min = 100,
    .f_max = sample_rate/2 - 500, // allow room for the receiver filter rolloff
    .sample_rate = sample_rate,
    .time_osr = kTime_osr,
    .freq_osr = kFreq_osr,
    .protocol = is_ft8 ? PROTO_FT8 : PROTO_FT4
  };
  monitor_init(&mon, &mon_cfg);
  LOG(LOG_DEBUG, "Waterfall allocated %d symbols\n", mon.wf.max_blocks);
  for (int frame_pos = 0; frame_pos + mon.block_size <= num_samples; frame_pos += mon.block_size)
    {
      // Process the waveform data frame by frame - you could have a live loop here with data from an audio device
      // (cool, but we'd have to get the decoding time from something other than the file name -- KA9Q)
      monitor_process(&mon, signal + frame_pos);
    }
  LOG(LOG_DEBUG, "Waterfall accumulated %d symbols\n", mon.wf.num_blocks);
  LOG(LOG_INFO, "Max magnitude: %.1f dB\n", mon.max_mag);

  // Find top candidates by Costas sync score and localize them in time and frequency
  int const candidate_size = (mon_cfg.f_max * kMax_candidates) / 3000; // Scale by bandwidth relative to the original 3 kHz
  candidate_t candidate_list[candidate_size];
  int num_candidates = ft8_find_sync(&mon.wf, candidate_size, candidate_list, kMin_score);

  // Hash table for decoded messages (to check for duplicates)
  int num_decoded = 0;
  message_t decoded[kMax_decoded_messages];
  message_t* decoded_hashtable[kMax_decoded_messages];
  memset(decoded,0,kMax_decoded_messages * sizeof(message_t));
  memset(decoded_hashtable,0, kMax_decoded_messages * sizeof(message_t *));

  // Go over candidates and attempt to decode messages
  for (int idx = 0; idx < num_candidates; ++idx)
    {
      const candidate_t* cand = &candidate_list[idx];
      if (cand->score < kMin_score)
	continue;

      float freq_hz = (cand->freq_offset + (float)cand->freq_sub / mon.wf.freq_osr) / mon.symbol_period;
      float time_sec = (cand->time_offset + (float)cand->time_sub / mon.wf.time_osr) * mon.symbol_period;

      message_t message;
      decode_status_t status;
      if (!ft8_decode(&mon.wf, cand, &message, kLDPC_iterations, &status))
        {
	  // printf("000000 %3d %+4.2f %4.0f ~  ---\n", cand->score, time_sec, freq_hz);
	  if (status.ldpc_errors > 0)
            {
	      LOG(LOG_DEBUG, "LDPC decode: %d errors\n", status.ldpc_errors);
            }
	  else if (status.crc_calculated != status.crc_extracted)
            {
	      LOG(LOG_DEBUG, "CRC mismatch!\n");
            }
	  else if (status.unpack_status != 0)
            {
	      LOG(LOG_DEBUG, "Error while unpacking!\n");
            }
	  continue;
        }

      LOG(LOG_DEBUG, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
      int idx_hash = message.hash % kMax_decoded_messages;
      bool found_empty_slot = false;
      bool found_duplicate = false;
      do
        {
	  if (decoded_hashtable[idx_hash] == NULL)
            {
	      LOG(LOG_DEBUG, "Found an empty slot\n");
	      found_empty_slot = true;
            }
	  else if ((decoded_hashtable[idx_hash]->hash == message.hash) && (0 == strcmp(decoded_hashtable[idx_hash]->text, message.text)))
            {
	      LOG(LOG_DEBUG, "Found a duplicate [%s]\n", message.text);
	      found_duplicate = true;
            }
	  else
            {
	      LOG(LOG_DEBUG, "Hash table clash!\n");
	      // Move on to check the next entry in hash table
	      idx_hash = (idx_hash + 1) % kMax_decoded_messages;
            }
        } while (!found_empty_slot && !found_duplicate);

      if (found_empty_slot)
        {
	  // Fill the empty hashtable slot
	  memcpy(&decoded[idx_hash], &message, sizeof(message));
	  decoded_hashtable[idx_hash] = &decoded[idx_hash];
	  ++num_decoded;

	  // Hacked by KA9Q to emit time prefix and actual frequency
	  // Assumes file name of the form 20250505T043345...
	  {
	    // Temp copy to let basename modify it
	    char *npath = strdup(path);
	    char const *bn = basename(npath);
	    int year,mon,day,hr,minute,sec;
	    char junk;
	    sscanf(bn,"%04d%02d%02d%c%02d%02d%02d",&year,&mon,&day,&junk,&hr,&minute,&sec);
	    free(npath);

	    fprintf(stdout,"%4d/%02d/%02d %02d:%02d:%02d %3d %+4.2f %'.1lf ~ %s\n",
		    year,mon,day,hr,minute,sec,
		    cand->score,
		    time_sec,
		    1.0e6 * base_freq + freq_hz,
		    message.text);
	  }
        }
    }
  LOG(LOG_INFO, "Decoded %d messages\n", num_decoded);

  monitor_free(&mon);
  return 0;
}
