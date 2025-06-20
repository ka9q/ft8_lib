#ifndef _INCLUDE_DECODE_H_
#define _INCLUDE_DECODE_H_

#include <stdint.h>
#include <stdbool.h>

#include "constants.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /// Input structure to ft8_find_sync() function. This structure describes stored waterfall data over the whole message slot.
    /// Fields time_osr and freq_osr specify additional oversampling rate for time and frequency resolution.
    /// If time_osr=1, FFT magnitude data is collected once for every symbol transmitted, i.e. every 1/6.25 = 0.16 seconds.
    /// Values time_osr > 1 mean each symbol is further subdivided in time.
    /// If freq_osr=1, each bin in the FFT magnitude data corresponds to 6.25 Hz, which is the tone spacing.
    /// Values freq_osr > 1 mean the tone spacing is further subdivided by FFT analysis.
    typedef struct
    {
        int max_blocks;          ///< number of blocks (symbols) allocated in the mag array
        int num_blocks;          ///< number of blocks (symbols) stored in the mag array
        int num_bins;            ///< number of FFT bins in terms of 6.25 Hz
        int time_osr;            ///< number of time subdivisions
        int freq_osr;            ///< number of frequency subdivisions
        uint8_t* mag;            ///< FFT magnitudes stored as uint8_t[blocks][time_osr][freq_osr][num_bins]
        int block_stride;        ///< Helper value = time_osr * freq_osr * num_bins
        ftx_protocol_t protocol; ///< Indicate if using FT4 or FT8
    } waterfall_t;

    /// Output structure of ft8_find_sync() and input structure of ft8_decode().
    /// Holds the position of potential start of a message in time and frequency.
    typedef struct
    {
        int16_t score;       ///< Candidate score (non-negative number; higher score means higher likelihood)
        int16_t time_offset; ///< Index of the time block
        int16_t freq_offset; ///< Index of the frequency bin
        uint8_t time_sub;    ///< Index of the time subdivision used
        uint8_t freq_sub;    ///< Index of the frequency subdivision used
    } candidate_t;

    /// Structure that holds the decoded message
    typedef struct
    {
        // TODO: check again that this size is enough
        char text[25]; ///< Plain text
        uint16_t hash; ///< Hash value to be used in hash table and quick checking for duplicates
      // Store so we can display them after sorting
      float freq_hz;   // We will sort on this
      float time_sec;
      int score;
    } message_t;

    /// Structure that contains the status of various steps during decoding of a message
    typedef struct
    {
        int ldpc_errors;         ///< Number of LDPC errors during decoding
        uint16_t crc_extracted;  ///< CRC value recovered from the message
        uint16_t crc_calculated; ///< CRC value calculated over the payload
        int unpack_status;       ///< Return value of the unpack routine
    } decode_status_t;

    /// Localize top N candidates in frequency and time according to their sync strength (looking at Costas symbols)
    /// We treat and organize the candidate list as a min-heap (empty initially).
    /// @param[in] power Waterfall data collected during message slot
    /// @param[in] sync_pattern Synchronization pattern
    /// @param[in] num_candidates Number of maximum candidates (size of heap array)
    /// @param[in,out] heap Array of candidate_t type entries (with num_candidates allocated entries)
    /// @param[in] min_score Minimal score allowed for pruning unlikely candidates (can be zero for no effect)
    /// @return Number of candidates filled in the heap
    int ft8_find_sync(const waterfall_t* power, int num_candidates, candidate_t heap[], int min_score);

    /// Attempt to decode a message candidate. Extracts the bit probabilities, runs LDPC decoder, checks CRC and unpacks the message in plain text.
    /// @param[in] power Waterfall data collected during message slot
    /// @param[in] cand Candidate to decode
    /// @param[out] message message_t structure that will receive the decoded message
    /// @param[in] max_iterations Maximum allowed LDPC iterations (lower number means faster decode, but less precise)
    /// @param[out] status decode_status_t structure that will be filled with the status of various decoding steps
    /// @return True if the decoding was successful, false otherwise (check status for details)
    bool ft8_decode(const waterfall_t* power, const candidate_t* cand, message_t* message, int max_iterations, decode_status_t* status);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_DECODE_H_
