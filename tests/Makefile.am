AUTOMAKE_OPTIONS = serial-tests

AM_CFLAGS = -Wall $(LIBOSMOCORE_CFLAGS) -I$(top_builddir)/src -pthread -mcpu=cortex-a9 -mtune=cortex-a9 -march=armv7-a -mfpu=neon -mfloat-abi=softfp -marm -O3

check_PROGRAMS = conv_test

conv_test_SOURCES = conv_test.c noise.c codes.c
conv_test_LDADD = -lpthread -lm \
	$(top_builddir)/src/libconvtest.la \
	$(LIBOSMOCORE_LIBS)

TESTS = $(check_PROGRAMS)

noinst_HEADERS = codes.h noise.h
