BUILD ?= release
prefix        ?= /usr/local
exec_prefix   ?= $(prefix)
bindir        ?= $(exec_prefix)/bin
sbindir       ?= $(exec_prefix)/sbin
libdir        ?= $(exec_prefix)/lib
datadir       ?= $(prefix)/share
sysconfdir    ?= /etc
localstatedir ?= /var
pkgdatadir    ?= $(datadir)/ka9q-radio
pkglibdir     ?= $(libdir)/ka9q-radio
statedir      ?= $(localstatedir)/lib/ka9q-radio

ifdef DESTDIR
prefix = /usr
endif

export prefix exec_prefix bindir sbindir libdir datadir sysconfdir
export localstatedir pkgdatadir pkglibdir statedir mandir
export DEB_BUILD_ARCH

# eg, /var/lib/ka9q-radio
PATH_FLAGS += -DSTATEDIR=\"$(statedir)\"

# eg, /usr/local/share/ka9q-radio, /usr/share/ka9q-radio
PATH_FLAGS += -DPKGDATADIR=\"$(pkgdatadir)\"

# eg, /usr/local/lib/ka9q-radio, /usr/lib/ka9q-radio
PATH_FLAGS += -DPKGLIBDIR=\"$(pkglibdir)\"

UNAME_S := $(shell uname -s)

CFLAGS = -O3 -ggdb3
CPPFLAGS = -std=c11 -I.
LDFLAGS = -latomic -lbsd -lm

TARGETS = gen_ft8 decode_ft8 test_ft8

.PHONY: run_tests all clean install

all: $(TARGETS)

run_tests: test_ft8
	@./test_ft8

gen_ft8: gen_ft8.o ft8/constants.o ft8/text.o ft8/pack.o ft8/encode.o ft8/crc.o common/wave.o
	$(CXX) -o $@ $^ $(LDFLAGS)

test_ft8:  test_ft8.o ft8/pack.o ft8/encode.o ft8/crc.o ft8/text.o ft8/constants.o fft/kiss_fftr.o fft/kiss_fft.o
	$(CXX) -o $@ $^ $(LDFLAGS)

decode_ft8: main.o decode_ft8.o fft/kiss_fftr.o fft/kiss_fft.o ft8/decode.o ft8/encode.o ft8/crc.o ft8/ldpc.o ft8/unpack.o ft8/text.o ft8/constants.o common/wave.o
	$(CXX) -o $@ $^ $(LDFLAGS)

libft8.a: ft8/constants.o ft8/encode.o ft8/pack.o ft8/text.o common/wave.o
	ar rc libft8.a $^

clean:
	rm -f *.o *.a ft8/*.o common/*.o fft/*.o $(TARGETS)

install: all
	install -d -m 0755 $(DESTDIR)$(bindir)
	install $(TARGETS) $(DESTDIR)$(bindir)
#	install libft8.a $(DESTDIR)$(libdir)

