CFLAGS = -O3 -ggdb3 -march=native -flto -ffast-math
CPPFLAGS = -std=c11 -I. $(shell pkg-config --cflags fftw3f)
LDFLAGS = -latomic -lbsd -lm -flto $(shell pkg-config --libs fftw3f)

TARGETS = gen_ft8 decode_ft8 test

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

clean:
	rm -f *.o *.a ft8/*.o common/*.o fft/*.o $(TARGETS)
install:
	$(AR) rc libft8.a ft8/constants.o ft8/encode.o ft8/pack.o ft8/text.o common/wave.o
#	install libft8.a /usr/local/lib/libft8.a
	install decode_ft8 /usr/local/bin
