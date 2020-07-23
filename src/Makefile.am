AM_CFLAGS = -Wall $(LIBOSMOCORE_CFLAGS) -fvisibility=hidden -mcpu=cortex-a9 -mtune=cortex-a9 -march=armv7-a -mfpu=neon -mfloat-abi=softfp -marm -O3

noinst_LTLIBRARIES = libconvtest.la

libconvtest_la_SOURCES = \
	encode.c \
	viterbi.c \
	viterbi_gen.c \
	viterbi_sse.c \
	viterbi_neon.c
