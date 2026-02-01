# Use -DUSE_KISS if you want the kiss fft library
# By default, this builds both a decode_ft8 (using FFTW3) and decode_ft8_kiss (using the KISS library)
# It is only the decode_ft8.c file that depends on the USE_KISS define.

CFLAGS = -O3 -ggdb3 -march=native -flto -ffast-math 
CPPFLAGS = -std=c11 -I. $(shell pkg-config --cflags fftw3f)
LDFLAGS = -latomic -lbsd -lm -flto $(shell pkg-config --libs fftw3f)
LDFLAGS_KISS = -latomic -lbsd -lm -flto 

TARGETS = gen_ft8 decode_ft8 test decode_ft8_kiss

.PHONY: run_tests all clean

all: $(TARGETS)

run_tests: test
	@./test

gen_ft8: gen_ft8.o ft8/constants.o ft8/text.o ft8/pack.o ft8/encode.o ft8/crc.o common/wave.o
	$(CC) -o $@ $^ $(LDFLAGS)

test:  test.o ft8/pack.o ft8/encode.o ft8/crc.o ft8/text.o ft8/constants.o
	$(CC) -o $@ $^ $(LDFLAGS)

decode_ft8: main.o decode_ft8.o ft8/decode.o ft8/encode.o ft8/crc.o ft8/ldpc.o ft8/unpack.o ft8/text.o ft8/constants.o common/wave.o
	$(CC) -o $@ $^ $(LDFLAGS) 

decode_ft8_kiss: main.o decode_ft8_kiss.o ft8/decode.o ft8/encode.o ft8/crc.o ft8/ldpc.o ft8/unpack.o ft8/text.o ft8/constants.o common/wave.o fft/kiss_fft.o fft/kiss_fftr.o
	$(CC) -o $@ $^ $(LDFLAGS_KISS) 

decode_ft8_kiss.o: decode_ft8.c
	$(CC) -c -o $@ $(CFLAGS) $(CCFLAGS) -DUSE_KISS $^

clean:
	rm -f *.o *.a ft8/*.o common/*.o fft/*.o $(TARGETS)
install:
	$(AR) rc libft8.a ft8/constants.o ft8/encode.o ft8/pack.o ft8/text.o common/wave.o
#	install libft8.a /usr/local/lib/libft8.a
	install decode_ft8 /usr/local/bin
