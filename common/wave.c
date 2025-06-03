#define _GNU_SOURCE 1
#include "wave.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <sys/file.h>


// Save signal in floating point format (-1 .. +1) as a WAVE file using 16-bit signed integers.
void save_wav(const float* signal, int num_samples, int sample_rate, const char* path)
{
    char subChunk1ID[4] = { 'f', 'm', 't', ' ' };
    uint32_t subChunk1Size = 16; // 16 for PCM
    uint16_t audioFormat = 1; // PCM = 1
    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t sampleRate = sample_rate;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t byteRate = sampleRate * blockAlign;

    char subChunk2ID[4] = { 'd', 'a', 't', 'a' };
    uint32_t subChunk2Size = num_samples * blockAlign;

    char chunkID[4] = { 'R', 'I', 'F', 'F' };
    uint32_t chunkSize = 4 + (8 + subChunk1Size) + (8 + subChunk2Size);
    char format[4] = { 'W', 'A', 'V', 'E' };

    int16_t* raw_data = (int16_t*)malloc(num_samples * blockAlign);
    for (int i = 0; i < num_samples; i++)
    {
        float x = signal[i];
        if (x > 1.0)
            x = 1.0;
        else if (x < -1.0)
            x = -1.0;
        raw_data[i] = (int)(0.5 + (x * 32767.0));
    }

    FILE* f = fopen(path, "wb");
    if(f == NULL){
      fprintf(stderr,"can't write %s: %s\n",path,strerror(errno));
      return;
    }

    // NOTE: works only on little-endian architecture
    fwrite(chunkID, sizeof(chunkID), 1, f);
    fwrite(&chunkSize, sizeof(chunkSize), 1, f);
    fwrite(format, sizeof(format), 1, f);

    fwrite(subChunk1ID, sizeof(subChunk1ID), 1, f);
    fwrite(&subChunk1Size, sizeof(subChunk1Size), 1, f);
    fwrite(&audioFormat, sizeof(audioFormat), 1, f);
    fwrite(&numChannels, sizeof(numChannels), 1, f);
    fwrite(&sampleRate, sizeof(sampleRate), 1, f);
    fwrite(&byteRate, sizeof(byteRate), 1, f);
    fwrite(&blockAlign, sizeof(blockAlign), 1, f);
    fwrite(&bitsPerSample, sizeof(bitsPerSample), 1, f);

    fwrite(subChunk2ID, sizeof(subChunk2ID), 1, f);
    fwrite(&subChunk2Size, sizeof(subChunk2Size), 1, f);

    fwrite(raw_data, blockAlign, num_samples, f);

    fclose(f);

    free(raw_data);
}

// Load signal in floating point format (-1 .. +1) as a WAVE file using 16-bit signed integers.
// Rewritten 4 May 2025 KA9Q to be more tolerant of variant headers
// Expects to be called with the file already open for reading on fd. path used only for error messages
int load_wav(float **signal, int* num_samples, int* sample_rate, const char* path,int fd){
  if(signal == NULL || num_samples == NULL || sample_rate == NULL || path == NULL)
    return -1;

  FILE *f = fdopen(fd, "rb");
  if(f == NULL){
    fprintf(stderr,"fdopen(%s) failed: %s\n",path,strerror(errno));
    return -1;
  }
  // NOTE: works only on little-endian architecture
  char chunkID[4]; // = {'R', 'I', 'F', 'F'};
  if(fread((void*)chunkID, sizeof(chunkID), 1, f) != 1)
    goto quit;
  uint32_t chunkSize; // = 4 + (8 + subChunk1Size) + (8 + subChunk2Size);
  if(fread((void*)&chunkSize, sizeof(chunkSize), 1, f) != 1)
    goto quit;
  char format[4]; // = {'W', 'A', 'V', 'E'};
  if(fread((void*)format, sizeof(format), 1, f) != 1)
    goto quit;
  if(feof(f)){
    fprintf(stderr,"%s: premature EOF 1\n",path);
    goto quit;
  }
  if(strncmp(chunkID,"RIFF",4) != 0){
    fprintf(stderr,"%s: not RIFF\n",path);
    goto quit;
  }

  if(strncmp(format,"WAVE",4) !=0 ){
    fprintf(stderr,"%s: not WAVE\n",path);
    goto quit;
  }
  uint16_t audioFormat = 0; // = 1;     // PCM = 1
  uint16_t numChannels = 0; // = 1;
  uint16_t bitsPerSample = 0; // = 16;
  uint32_t sampleRate = 0;
  uint16_t blockAlign = 0; // = numChannels * bitsPerSample / 8;
  uint32_t byteRate = 0; // = sampleRate * blockAlign;

  while(!feof(f)){
    // Will normally hit EOF when trying to read the next chunk ID after data
    if(fread((void*)chunkID, sizeof(chunkID), 1, f) != 1)
      break;
    if(fread((void*)&chunkSize, sizeof(chunkSize), 1, f) != 1)
      break;
    if(feof(f))
      break;
    if(strncmp(chunkID,"fmt ",4) == 0){
      if(chunkSize < 16){
	fprintf(stderr,"%s: chunkSize %d too small\n",path,chunkSize);
	goto quit;
      }
      // Standard part of header
      if(fread((void*)&audioFormat, sizeof(audioFormat), 1, f) != 1)
	goto quit;
      if(fread((void*)&numChannels, sizeof(numChannels), 1, f) != 1)
	goto quit;
      if(fread((void*)&sampleRate, sizeof(sampleRate), 1, f) != 1)
	goto quit;
      if(fread((void*)&byteRate, sizeof(byteRate), 1, f) != 1)
	goto quit;
      if(fread((void*)&blockAlign, sizeof(blockAlign), 1, f) != 1)
	goto quit;
      if(fread((void*)&bitsPerSample, sizeof(bitsPerSample), 1, f) != 1)
	goto quit;
      if(feof(f)){
	fprintf(stderr,"%s: premature EOF 3\n",path);
	goto quit;
      }
      if(chunkSize > 16){
	// Skip the rest of the longer fmt header
	fseek(f,chunkSize-16,SEEK_CUR); // Skip unsupported chunk
      }
      if (numChannels != 1){
	fprintf(stderr,"%s: numChannels %d, must be 1\n", path,numChannels);
	goto quit;
      }
    } else if(strncmp(chunkID,"data",4) == 0){
      // Process data
      if(chunkSize != 0xffffffff) // typical placeholder for "indeterminate"
	*num_samples = chunkSize / blockAlign;
      else
	*num_samples = 10000000; // wing it: 10 million = 15 sec * 667 kHz

      *sample_rate = sampleRate;
      if(*signal == NULL) // What if it's not null? We don't know what it is, should it be freed?
	*signal = malloc(sizeof(float) * numChannels * *num_samples);

      switch(audioFormat){
      case 1: // 16-bit signed int
	{
	  if(bitsPerSample != 16){
	    fprintf(stderr,"%s: bits per sample %d for PCM; must be 16\n",path,bitsPerSample);
	    goto quit;
	  }
	  int count;
	  for(count = 0; count < *num_samples; count++){ // numChannels must be 1
	    int16_t s;
	    if(fread(&s,sizeof s, 1, f) != 1)
	      break;

	    (*signal)[count] = s / 32768.0f; // compiler should optimize
	  }
	  if(count != *num_samples){
	    // Trim buffer and return the actual count
	    *signal = reallocf(*signal,sizeof(float) * numChannels * count);
	    *num_samples = count;
	  }
	}
	break;
      case 3: // 32-bit float
	{
	  if(bitsPerSample != 32){
	    fprintf(stderr,"%s: bits per sample %d for float; must be 32\n",path,bitsPerSample);
	    goto quit;
	  }
	  int const count = fread(*signal,blockAlign,*num_samples, f); // Read floating point directly
	  if(count != *num_samples){
	    // Trim buffer and return the actual count
	    *signal = reallocf(*signal,sizeof(float) * numChannels * count);
	    *num_samples = count;
	  }
	}
	break;
      default:
	fprintf(stderr,"%s: unsupported audio format %d\n",path,audioFormat);
	goto quit;
      }
      // If we haven't read all the data in the file, skip over the rest of the chunk
      // There will probably not be another chunk, but this will force the eof when we loop back to
      // read another chunk
      if(chunkSize / blockAlign > *num_samples){
	long extra = chunkSize - *num_samples * blockAlign;
	fseek(f,extra,SEEK_CUR);
      }
    } else {
      fseek(f,chunkSize,SEEK_CUR); // Skip unsupported subchunk
    }
  }
  fclose(f);
  return 0;
 quit:
  if(f != NULL)
    fclose(f);
  return -1;
}
