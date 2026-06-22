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
ifeq ($(UNAME_S),Darwin)
  INCLUDES += -I/opt/local/include
  LDFLAGS  += -L/opt/local/lib
  LDLIBS += -lm
else
  LDLIBS += -latomic -lbsd -lm
endif

ifeq ($(BUILD),debug)
     DOPTS = -g
else
     DOPTS = -DNDEBUG=1 -O3
endif



COPTS = -std=gnu11 -Wall -funsafe-math-optimizations -fno-math-errno -fcx-limited-range -freciprocal-math -fno-trapping-math -Wextra -Wno-sign-conversion -Wno-int-conversion -MMD -MP
CFLAGS += $(DOPTS) $(ARCHOPTS) $(COPTS) $(INCLUDES)

TARGETS = gen_ft8 decode_ft8 test_ft8

.PHONY: run_tests all clean install

all: $(TARGETS)

run_tests: test_ft8
	@./test_ft8

gen_ft8: gen_ft8.o ft8/constants.o ft8/text.o ft8/pack.o ft8/encode.o ft8/crc.o common/wave.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_ft8:  test_ft8.o ft8/pack.o ft8/encode.o ft8/crc.o ft8/text.o ft8/constants.o fft/kiss_fftr.o fft/kiss_fft.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

decode_ft8: main.o decode_ft8.o fft/kiss_fftr.o fft/kiss_fft.o ft8/decode.o ft8/encode.o ft8/crc.o ft8/ldpc.o ft8/unpack.o ft8/text.o ft8/constants.o common/wave.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

libft8.a: ft8/constants.o ft8/encode.o ft8/pack.o ft8/text.o common/wave.o
	ar rc libft8.a $^

clean:
	rm -f *.o *.a ft8/*.o common/*.o fft/*.o $(TARGETS)

install: all
	install -d -m 0755 $(DESTDIR)$(bindir)
	install $(TARGETS) $(DESTDIR)$(bindir)
#	install libft8.a $(DESTDIR)$(libdir)

