#ifndef _INCLUDE_WAVE_H_
#define _INCLUDE_WAVE_H_

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

  // Save signal in floating point format (-1 .. +1) as a WAVE file using 16-bit signed integers.
  void save_wav(const float* signal, int num_samples, int sample_rate, const char* path);

  // Load signal in floating point format (-1 .. +1) as a WAVE file using 16-bit signed integers.
  // Now mallocs signal array, places in *signal, caller must free
  int load_wav(float** signal, int* num_samples, int* sample_rate, const char* path,int fd);

  // base_freq = radio frequency in Hz corresponding to zero frequency here (receiver is always USB)
  // tmp = UTC @ signal[0]
  // fsec = fractional second in UTC @ signal[0]
  int process_buffer(float const *signal,int sample_rate, int num_samples, bool is_ft8, float base_freq, struct tm const *tmp, double fsec);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_WAVE_H_
