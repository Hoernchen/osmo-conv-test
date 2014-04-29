/*
 * Viterbi decoder
 * Copyright (C) 2013, 2014 Thomas Tsou <tom@tsou.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <osmocom/core/conv.h>

/* Forward Metric Units */
void gen_metrics_k5_n2(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
void gen_metrics_k5_n3(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
void gen_metrics_k5_n4(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
void gen_metrics_k7_n2(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
void gen_metrics_k7_n3(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
void gen_metrics_k7_n4(const int8_t *seq, const int16_t *out,
		       int16_t *sums, int16_t *paths, int norm);
/* Trellis State
 *     state - Internal lshift register value
 *     prev  - Register values of previous 0 and 1 states
 */
struct vstate {
	unsigned state;
	unsigned prev[2];
};

/* Trellis Object
 *     num_states - Number of states in the trellis
 *     sums       - Accumulated path metrics
 *     outputs    - Trellis ouput values
 *     vals       - Input value that led to each state
 */
struct vtrellis {
	int num_states;
	int16_t *sums;
	int16_t *outputs;
	uint8_t *vals;
};

/* Viterbi Decoder
 *     n         - Code order
 *     k         - Constraint length
 *     len       - Horizontal length of trellis
 *     recursive - Set to '1' if the code is recursive
 *     intrvl    - Normalization interval
 *     trellis   - Trellis object
 *     punc      - Puncturing sequence
 *     paths     - Trellis paths
 */
struct vdecoder {
	int n;
	int k;
	int len;
	int recursive;
	int intrvl;
	struct vtrellis *trellis;
	int *punc;
	int16_t **paths;

	void (*metric_func)(const int8_t *, const int16_t *,
			    int16_t *, int16_t *, int);
};

/* Aligned Memory Allocator
 *     SSE requires 16-byte memory alignment. We store relevant trellis values
 *     (accumulated sums, outputs, and path decisions) as 16 bit signed integers
 *     so the allocated memory is casted as such.
 */
#define SSE_ALIGN	16

static int16_t *vdec_malloc(size_t n)
{
#ifdef HAVE_SSE3
	return (int16_t *) memalign(SSE_ALIGN, sizeof(int16_t) * n);
#else
	return (int16_t *) malloc(sizeof(int16_t) * n);
#endif
}

/* Accessor calls */
inline int conv_code_recursive(const struct osmo_conv_code *code)
{
	return code->next_term_output ? 1 : 0;
}

/* Left shift and mask for finding the previous state */
static unsigned vstate_lshift(unsigned reg, int k, int val)
{
	unsigned mask;

	if (k == 5)
		mask = 0x0e;
	else if (k == 7)
		mask = 0x3e;
	else
		mask = 0;

	return ((reg << 1) & mask) | val;
}

/* Bit endian manipulators */
inline unsigned bitswap2(unsigned v)
{
	return ((v & 0x02) >> 1) | ((v & 0x01) << 1);
}

inline unsigned bitswap3(unsigned v)
{
	return ((v & 0x04) >> 2) | ((v & 0x02) >> 0) |
	       ((v & 0x01) << 2);
}

inline unsigned bitswap4(unsigned v)
{
	return ((v & 0x08) >> 3) | ((v & 0x04) >> 1) |
	       ((v & 0x02) << 1) | ((v & 0x01) << 3);
}

inline unsigned bitswap5(unsigned v)
{
	return ((v & 0x10) >> 4) | ((v & 0x08) >> 2) | ((v & 0x04) >> 0) |
	       ((v & 0x02) << 2) | ((v & 0x01) << 4);
}

inline unsigned bitswap6(unsigned v)
{
	return ((v & 0x20) >> 5) | ((v & 0x10) >> 3) | ((v & 0x08) >> 1) |
	       ((v & 0x04) << 1) | ((v & 0x02) << 3) | ((v & 0x01) << 5);
}

static unsigned bitswap(unsigned v, unsigned n)
{
	switch (n) {
	case 1:
		return v;
	case 2:
		return bitswap2(v);
	case 3:
		return bitswap3(v);
	case 4:
		return bitswap4(v);
	case 5:
		return bitswap5(v);
	case 6:
		return bitswap6(v);
	default:
		break;
	}

	return 0;
}

/* Generate non-recursive state output from generator state table
 *     Note that the shift register moves right (i.e. the most recent bit is
 *     shifted into the register at k-1 bit of the register), which is typical
 *     textbook representation. The API transition table expects the most recent
 *     bit in the low order bit, or left shift. A bitswap operation is required
 *     to accommodate the difference.
 */
static unsigned gen_output(struct vstate *state, int val,
			   const struct osmo_conv_code *code)
{
	unsigned out, prev;

	prev = bitswap(state->prev[0], code->K - 1);
	out = code->next_output[prev][val];
	out = bitswap(out, code->N);

	return out;
}

#define BIT2NRZ(REG,N) (((REG >> N) & 0x01) * 2 - 1) * -1

/* Populate non-recursive trellis state
 *     For a given state defined by the k-1 length shift register, find the
 *     value of the input bit that drove the trellis to that state. Also
 *     generate the N outputs of the generator polynomial at that state.
 */
static int gen_state_info(uint8_t *val, unsigned reg,
			  int16_t *output, const struct osmo_conv_code *code)
{
	int i;
	unsigned out;
	struct vstate state;

	/* Previous '0' state */
	state.state = reg;
	state.prev[0] = vstate_lshift(reg, code->K, 0);
	state.prev[1] = vstate_lshift(reg, code->K, 1);

	*val = (reg >> (code->K - 2)) & 0x01;

	/* Transition output */
	out = gen_output(&state, *val, code);

	/* Unpack to NRZ */
	for (i = 0; i < code->N; i++)
		output[i] = BIT2NRZ(out, i);

	return 0;
}

/* Generate recursive state output from generator state table */
static unsigned gen_recursive_output(struct vstate *state,
				     uint8_t *val, unsigned reg,
				     const struct osmo_conv_code *code, int pos)
{
	int val0, val1;
	unsigned out, prev;

	/* Previous '0' state */
	prev = vstate_lshift(reg, code->K, 0);
	prev = bitswap(prev, code->K - 1);

	/* Input value */
	val0 = (reg >> (code->K - 2)) & 0x01;
	val1 = (code->next_term_output[prev] >> pos) & 0x01;
	*val = val0 == val1 ? 0 : 1;

	/* Wrapper for osmocom state access */
	prev = bitswap(state->prev[0], code->K - 1);

	/* Compute the transition output */
	out = code->next_output[prev][*val];
	out = bitswap(out, code->N);

	return out;
}

#define NUM_STATES(K) (K == 7 ? 64 : 16)

/* Populate recursive trellis state
 *     The bit position of the systematic bit is not explicitly marked by the
 *     API, so it must be extracted from the generator table. Otherwise,
 *     populate the trellis similar to the non-recursive version.
 *     Non-systematic recursive codes are not supported.
 */
static int gen_recursive_state_info(uint8_t *val,
				    unsigned reg,
				    int16_t *output,
				    const struct osmo_conv_code *code)
{
	int i, j, pos = -1;
	int ns = NUM_STATES(code->K);
	unsigned out;
	struct vstate state;

	/* Previous '0' and '1' states */
	state.state = reg;
	state.prev[0] = vstate_lshift(reg, code->K, 0);
	state.prev[1] = vstate_lshift(reg, code->K, 1);

	/* Find recursive bit location */
	for (i = 0; i < code->N; i++) {
		for (j = 0; j < ns; j++) {
			if ((code->next_output[j][0] >> i) & 0x01)
				break;
		}
		if (j == ns) {
			pos = i;
			break;
		}
	}

	/* Non-systematic recursive code not supported */
	if (pos < 0)
		return -EPROTO;

	/* Transition output */
	out = gen_recursive_output(&state, val, reg, code, pos);

	/* Unpack to NRZ */
	for (i = 0; i < code->N; i++)
		output[i] = BIT2NRZ(out, i);

	return 0;
}

/* Release the trellis */
static void free_trellis(struct vtrellis *trellis)
{
	if (!trellis)
		return;

	free(trellis->vals);
	free(trellis->outputs);
	free(trellis->sums);
	free(trellis);
}

/* Allocate and initialize the trellis object
 *     Initialization consists of generating the outputs and output value of a
 *     given state. Due to trellis symmetry and anti-symmetry, only one of the
 *     transition paths is utilized by the butterfly operation in the forward
 *     recursion, so only one set of N outputs is required per state variable.
 */
static struct vtrellis *generate_trellis(const struct osmo_conv_code *code)
{
	int i, rc = -1;
	struct vtrellis *trellis;
	int16_t *outputs;

	int ns = NUM_STATES(code->K);
	int recursive = conv_code_recursive(code);
	int olen = (code->N == 2) ? 2 : 4;

	trellis = (struct vtrellis *) calloc(1, sizeof(struct vtrellis));
	trellis->num_states = ns;
	trellis->sums =	vdec_malloc(ns);
	trellis->outputs = vdec_malloc(ns * olen);
	trellis->vals = (uint8_t *) malloc(ns * sizeof(uint8_t));

	if (!trellis->sums || !trellis->outputs)
		goto fail;

	/* Populate the trellis state objects */
	for (i = 0; i < ns; i++) {
		outputs = &trellis->outputs[olen * i];

		if (recursive)
			rc = gen_recursive_state_info(&trellis->vals[i],
						      i, outputs, code);
		else
			rc = gen_state_info(&trellis->vals[i],
					    i, outputs, code);
	}

	if (rc < 0)
		goto fail;

	return trellis;
fail:
	free_trellis(trellis);
	return NULL;
}

/* Reset decoder
 *     Set accumulated path metrics to zero. For termination other than
 *     tail-biting, initialize the zero state as the encoder starting state.
 *     Intialize with the maximum accumulated sum at length equal to the
 *     constraint length.
 */
static void reset_decoder(struct vdecoder *dec, int term)
{
	int ns = dec->trellis->num_states;

	memset(dec->trellis->sums, 0, sizeof(int16_t) * ns);

	if (term != CONV_TERM_TAIL_BITING)
		dec->trellis->sums[0] = INT8_MAX * dec->n * dec->k;
}

static void _traceback(struct vdecoder *dec,
		       unsigned state, uint8_t *out, int len)
{
	int i;
	unsigned path;

	for (i = len - 1; i >= 0; i--) {
		path = dec->paths[i][state] + 1;
		out[i] = dec->trellis->vals[state];
		state = vstate_lshift(state, dec->k, path);
	}
}

static void _traceback_rec(struct vdecoder *dec,
			   unsigned state, uint8_t *out, int len)
{
	int i;
	unsigned path;

	for (i = len - 1; i >= 0; i--) {
		path = dec->paths[i][state] + 1;
		out[i] = path ^ dec->trellis->vals[state];
		state = vstate_lshift(state, dec->k, path);
	}
}

/* Traceback and generate decoded output
 *     Find the largest accumulated path metric at the final state except for
 *     the zero terminated case, where we assume the final state is always zero.
 */
static int traceback(struct vdecoder *dec, uint8_t *out, int term, int len)
{
	int i, sum, max = -1;
	unsigned path, state = 0;

	if (term != CONV_TERM_FLUSH) {
		for (i = 0; i < dec->trellis->num_states; i++) {
			sum = dec->trellis->sums[i];
			if (sum > max) {
				max = sum;
				state = i;
			}
		}

		if (max < 0)
			return -EPROTO;
	}

	for (i = dec->len - 1; i >= len; i--) {
		path = dec->paths[i][state] + 1;
		state = vstate_lshift(state, dec->k, path);
	}

	if (dec->recursive)
		_traceback_rec(dec, state, out, len);
	else
		_traceback(dec, state, out, len);

	return 0;
}

/* Release decoder object */
static void free_vdec(struct vdecoder *dec)
{
	if (!dec)
		return;

	free(dec->paths[0]);
	free(dec->paths);
	free_trellis(dec->trellis);
	free(dec);
}

/* Allocate decoder object
 *     Subtract the constraint length K on the normalization interval to
 *     accommodate the initialization path metric at state zero.
 */
static struct vdecoder *alloc_vdec(const struct osmo_conv_code *code)
{
	int i, ns;
	struct vdecoder *dec;

	ns = NUM_STATES(code->K);

	dec = (struct vdecoder *) calloc(1, sizeof(struct vdecoder));
	dec->n = code->N;
	dec->k = code->K;
	dec->recursive = conv_code_recursive(code);
	dec->intrvl = INT16_MAX / (dec->n * INT8_MAX) - dec->k;

	if (dec->k == 5) {
		switch (dec->n) {
		case 2:
			dec->metric_func = gen_metrics_k5_n2;
			break;
		case 3:
			dec->metric_func = gen_metrics_k5_n3;
			break;
		case 4:
			dec->metric_func = gen_metrics_k5_n4;
			break;
		default:
			goto fail;
		}
	} else if (dec->k == 7) {
		switch (dec->n) {
		case 2:
			dec->metric_func = gen_metrics_k7_n2;
			break;
		case 3:
			dec->metric_func = gen_metrics_k7_n3;
			break;
		case 4:
			dec->metric_func = gen_metrics_k7_n4;
			break;
		default:
			goto fail;
		}
	} else {
		goto fail;
	}

	if (code->term == CONV_TERM_FLUSH)
		dec->len = code->len + code->K - 1;
	else
		dec->len = code->len;

	dec->trellis = generate_trellis(code);
	if (!dec->trellis)
		goto fail;

	dec->paths = (int16_t **) malloc(sizeof(int16_t *) * dec->len);
	dec->paths[0] = vdec_malloc(ns * dec->len);
	for (i = 1; i < dec->len; i++)
		dec->paths[i] = &dec->paths[0][i * ns];

	return dec;
fail:
	free_vdec(dec);
	return NULL;
}

/* Depuncture sequence with nagative value terminated puncturing matrix */
static int depuncture(const int8_t *in, const int *punc, int8_t *out, int len)
{
	int i, n = 0, m = 0;

	for (i = 0; i < len; i++) {
		if (i == punc[n]) {
			out[i] = 0;
			n++;
			continue;
		}

		out[i] = in[m++];
	}

	return 0;
}

/* Forward trellis recursion
 *     Generate branch metrics and path metrics with a combined function. Only
 *     accumulated path metric sums and path selections are stored. Normalize on
 *     the interval specified by the decoder.
 */
static void _conv_decode(struct vdecoder *dec, const int8_t *seq, int _cnt)
{
	int i, len = dec->len;
	struct vtrellis *trellis = dec->trellis;

	for (i = 0; i < len; i++) {
		dec->metric_func(&seq[dec->n * i],
				 trellis->outputs,
				 trellis->sums,
				 dec->paths[i],
				 !(i % dec->intrvl));
	}
}

/* Convolutional decode with a decoder object
 *     Initial puncturing run if necessary followed by the forward recursion.
 *     For tail-biting perform a second pass before running the backward
 *     traceback operation.
 */
static int conv_decode(struct vdecoder *dec, const int8_t *seq,
		       const int *punc, uint8_t *out, int len, int term)
{
	int cnt = 0;
	int8_t depunc[dec->len * dec->n];

	reset_decoder(dec, term);

	if (punc) {
		depuncture(seq, punc, depunc, dec->len * dec->n);
		seq = depunc;
	}

	/* Propagate through the trellis with interval normalization */
	_conv_decode(dec, seq, cnt);

	if (term == CONV_TERM_TAIL_BITING)
		_conv_decode(dec, seq, cnt);

	return traceback(dec, out, term, len);
}

/* All-in-one viterbi decoding  */
int test_conv_decode(const struct osmo_conv_code *code,
		     const sbit_t *input, ubit_t *output)
{
	int rc;
	struct vdecoder *vdec;

	if ((code->N < 2) || (code->N > 4) || (code->len < 1) ||
	    ((code->K != 5) && (code->K != 7)))
		return -EINVAL;

	vdec = alloc_vdec(code);
	if (!vdec)
		return -EFAULT;

	rc = conv_decode(vdec, input, code->puncture,
			 output, code->len, code->term);

	free_vdec(vdec);

	return rc;
}
