/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_func.h"
#include "pdf_lex.h"
#include "pdf_mem.h"
#include <math.h>
#include <string.h>

#define MAX_IN 8
#define PS_STACK 100
#define PS_MAX_CODE 4096

enum ps_op {
	PS_NUM = 0, PS_ADD, PS_SUB, PS_MUL, PS_DIV, PS_IDIV, PS_MOD, PS_NEG, PS_ABS, PS_SQRT, PS_SIN,
	PS_COS, PS_ATAN, PS_EXP, PS_LN, PS_LOG, PS_CVI, PS_CVR, PS_FLOOR, PS_CEIL, PS_ROUND,
	PS_TRUNC, PS_DUP, PS_POP, PS_EXCH, PS_COPY, PS_INDEX, PS_ROLL, PS_EQ, PS_NE, PS_GT, PS_GE,
	PS_LT, PS_LE, PS_AND, PS_OR, PS_NOT, PS_XOR, PS_BITSHIFT, PS_TRUE, PS_FALSE, PS_IF, PS_IFELSE,
	PS_JMP, PS_JZ, /* internal: jump, jump if false */
};

struct ps_instr {
	uint8_t op;
	float val; /* number, or jump target */
};

struct pdf_function {
	int type; /* 0, 2, 3, 4, or -1 for an array of functions */
	int nin, nout;
	float domain[MAX_IN * 2];
	float range[PDF_FUNC_MAX_OUT * 2];
	bool has_range;
	/* type 2 */
	float c0[PDF_FUNC_MAX_OUT], c1[PDF_FUNC_MAX_OUT];
	float n;
	/* type 3 */
	struct pdf_function **sub;
	int nsub;
	float *bounds;
	float *encode;
	/* type 0 */
	uint8_t *samples;
	size_t samples_len;
	int size[MAX_IN];
	int bps;
	float *decode;
	float *encode0;
	/* type 4 */
	struct ps_instr *code;
	int ncode;
	/* array */
	struct pdf_function **parts;
	int nparts;
};

static float interp(float x, float xmin, float xmax, float ymin, float ymax)
{
	if (xmax == xmin) {
		return ymin;
	}
	return ymin + (x - xmin) * (ymax - ymin) / (xmax - xmin);
}

static int load_float_array(struct pdf_doc *doc, struct pdf_obj *arr, float *out, int max)
{
	int n = 0;

	if (arr == NULL || arr->type != PDF_ARRAY) {
		return 0;
	}
	for (size_t i = 0; i < arr->arr.n && n < max; i++) {
		out[n++] = pdf_num(pdf_array_get(doc, arr, i));
	}
	return n;
}

/* ---- type 4 ----------------------------------------------------------- */

struct ps_parser {
	struct ps_instr *code;
	int n;
	bool err;
};

static void emit(struct ps_parser *p, int op, float v)
{
	if (p->n >= PS_MAX_CODE) {
		p->err = true;
		return;
	}
	p->code[p->n].op = (uint8_t)op;
	p->code[p->n].val = v;
	p->n++;
}

static const struct {
	const char *name;
	int op;
} ps_ops[] = {
	{"add", PS_ADD}, {"sub", PS_SUB}, {"mul", PS_MUL}, {"div", PS_DIV}, {"idiv", PS_IDIV},
	{"mod", PS_MOD}, {"neg", PS_NEG}, {"abs", PS_ABS}, {"sqrt", PS_SQRT}, {"sin", PS_SIN},
	{"cos", PS_COS}, {"atan", PS_ATAN}, {"exp", PS_EXP}, {"ln", PS_LN}, {"log", PS_LOG},
	{"cvi", PS_CVI}, {"cvr", PS_CVR}, {"floor", PS_FLOOR}, {"ceiling", PS_CEIL},
	{"round", PS_ROUND}, {"truncate", PS_TRUNC}, {"dup", PS_DUP}, {"pop", PS_POP},
	{"exch", PS_EXCH}, {"copy", PS_COPY}, {"index", PS_INDEX}, {"roll", PS_ROLL}, {"eq", PS_EQ},
	{"ne", PS_NE}, {"gt", PS_GT}, {"ge", PS_GE}, {"lt", PS_LT}, {"le", PS_LE}, {"and", PS_AND},
	{"or", PS_OR}, {"not", PS_NOT}, {"xor", PS_XOR}, {"bitshift", PS_BITSHIFT},
	{"true", PS_TRUE}, {"false", PS_FALSE},
};

/* Parses a { ... } block; lx is positioned after the opening brace. */
static void ps_parse_block(struct ps_parser *p, struct pdf_lex *lx, int depth)
{
	int pending_blocks[2] = {-1, -1};
	int npending = 0;

	if (depth > 32) {
		p->err = true;
		return;
	}
	for (;;) {
		struct pdf_tok t;

		if (pdf_lex_next(lx, &t) < 0 || t.type == TOK_EOF) {
			pdf_tok_free(&t);
			return;
		}
		if (t.type == TOK_BRACE_CLOSE) {
			pdf_tok_free(&t);
			return;
		}
		if (t.type == TOK_NUM) {
			emit(p, PS_NUM, t.fval);
			pdf_tok_free(&t);
			continue;
		}
		if (t.type == TOK_BRACE_OPEN) {
			/* a procedure for if/ifelse: emit a jump-over placeholder */
			if (npending >= 2) {
				p->err = true;
				pdf_tok_free(&t);
				return;
			}
			int jz_at = p->n;

			emit(p, PS_JZ, 0);
			ps_parse_block(p, lx, depth + 1);
			pending_blocks[npending++] = jz_at;
			pdf_tok_free(&t);
			continue;
		}
		if (t.type == TOK_KEYWORD) {
			if (t.len == 2 && memcmp(t.start, "if", 2) == 0) {
				if (npending != 1) {
					p->err = true;
				} else {
					/* JZ placeholder jumps past the block when the condition is false */
					p->code[pending_blocks[0]].val = (float)p->n;
				}
				npending = 0;
			} else if (t.len == 6 && memcmp(t.start, "ifelse", 6) == 0) {
				if (npending != 2) {
					p->err = true;
				} else {
					/*
					 * layout: JZ a; block1; JZ b; block2
					 * wanted: JZ -> block2 start; after block1 jump to end
					 */
					int jz1 = pending_blocks[0], jz2 = pending_blocks[1];

					/* turn the second placeholder into an unconditional jump to the end */
					p->code[jz2].op = PS_JMP;
					p->code[jz2].val = (float)p->n;
					p->code[jz1].val = (float)(jz2 + 1);
				}
				npending = 0;
			} else {
				bool found = false;

				for (size_t i = 0; i < sizeof(ps_ops) / sizeof(ps_ops[0]); i++) {
					if (strlen(ps_ops[i].name) == t.len &&
					    memcmp(ps_ops[i].name, t.start, t.len) == 0) {
						emit(p, ps_ops[i].op, 0);
						found = true;
						break;
					}
				}
				if (!found) {
					p->err = true;
				}
			}
		}
		pdf_tok_free(&t);
	}
}

static int ps_eval(const struct pdf_function *f, const float *in, int nin, float *out, int *nout)
{
	float st[PS_STACK];
	int sp = 0;

	for (int i = 0; i < nin && sp < PS_STACK; i++) {
		st[sp++] = in[i];
	}
	int pc = 0, steps = 0;

	while (pc < f->ncode && steps++ < 100000) {
		const struct ps_instr *ins = &f->code[pc++];
		float a, b;

#define POP() (sp > 0 ? st[--sp] : 0.0f)
#define PUSH(v) do { if (sp < PS_STACK) st[sp++] = (v); } while (0)
		switch (ins->op) {
		case PS_NUM:
			PUSH(ins->val);
			break;
		case PS_JMP:
			pc = (int)ins->val;
			break;
		case PS_JZ:
			a = POP();
			if (a == 0.0f) {
				pc = (int)ins->val;
			}
			break;
		case PS_ADD: b = POP(); a = POP(); PUSH(a + b); break;
		case PS_SUB: b = POP(); a = POP(); PUSH(a - b); break;
		case PS_MUL: b = POP(); a = POP(); PUSH(a * b); break;
		case PS_DIV: b = POP(); a = POP(); PUSH(b != 0.0f ? a / b : 0.0f); break;
		case PS_IDIV: b = POP(); a = POP(); PUSH((int)b != 0 ? (float)((int)a / (int)b) : 0.0f); break;
		case PS_MOD: b = POP(); a = POP(); PUSH((int)b != 0 ? (float)((int)a % (int)b) : 0.0f); break;
		case PS_NEG: a = POP(); PUSH(-a); break;
		case PS_ABS: a = POP(); PUSH(fabsf(a)); break;
		case PS_SQRT: a = POP(); PUSH(sqrtf(a > 0 ? a : 0)); break;
		case PS_SIN: a = POP(); PUSH(sinf(a * 3.14159265f / 180.0f)); break;
		case PS_COS: a = POP(); PUSH(cosf(a * 3.14159265f / 180.0f)); break;
		case PS_ATAN:
			b = POP(); a = POP();
			a = atan2f(a, b) * 180.0f / 3.14159265f;
			if (a < 0) {
				a += 360.0f;
			}
			PUSH(a);
			break;
		case PS_EXP: b = POP(); a = POP(); PUSH(powf(a, b)); break;
		case PS_LN: a = POP(); PUSH(a > 0 ? logf(a) : 0); break;
		case PS_LOG: a = POP(); PUSH(a > 0 ? log10f(a) : 0); break;
		case PS_CVI: a = POP(); PUSH((float)(int)a); break;
		case PS_CVR: break;
		case PS_FLOOR: a = POP(); PUSH(floorf(a)); break;
		case PS_CEIL: a = POP(); PUSH(ceilf(a)); break;
		case PS_ROUND: a = POP(); PUSH(floorf(a + 0.5f)); break;
		case PS_TRUNC: a = POP(); PUSH((float)(long)a); break;
		case PS_DUP: a = POP(); PUSH(a); PUSH(a); break;
		case PS_POP: (void)POP(); break;
		case PS_EXCH: b = POP(); a = POP(); PUSH(b); PUSH(a); break;
		case PS_COPY: {
			int n = (int)POP();

			if (n > 0 && n <= sp && sp + n <= PS_STACK) {
				for (int i = 0; i < n; i++) {
					st[sp + i] = st[sp - n + i];
				}
				sp += n;
			}
			break;
		}
		case PS_INDEX: {
			int n = (int)POP();
			float v = (n >= 0 && n < sp) ? st[sp - 1 - n] : 0.0f;

			PUSH(v);
			break;
		}
		case PS_ROLL: {
			int j = (int)POP(), n = (int)POP();

			if (n > 0 && n <= sp) {
				j %= n;
				if (j < 0) {
					j += n;
				}
				for (int r = 0; r < j; r++) {
					float top = st[sp - 1];

					for (int i = sp - 1; i > sp - n; i--) {
						st[i] = st[i - 1];
					}
					st[sp - n] = top;
				}
			}
			break;
		}
		case PS_EQ: b = POP(); a = POP(); PUSH(a == b); break;
		case PS_NE: b = POP(); a = POP(); PUSH(a != b); break;
		case PS_GT: b = POP(); a = POP(); PUSH(a > b); break;
		case PS_GE: b = POP(); a = POP(); PUSH(a >= b); break;
		case PS_LT: b = POP(); a = POP(); PUSH(a < b); break;
		case PS_LE: b = POP(); a = POP(); PUSH(a <= b); break;
		case PS_AND: b = POP(); a = POP(); PUSH((float)((int)a & (int)b)); break;
		case PS_OR: b = POP(); a = POP(); PUSH((float)((int)a | (int)b)); break;
		case PS_XOR: b = POP(); a = POP(); PUSH((float)((int)a ^ (int)b)); break;
		case PS_NOT: a = POP(); PUSH(a == 0.0f ? 1.0f : (a == 1.0f ? 0.0f : (float)(~(int)a))); break;
		case PS_BITSHIFT: {
			int s = (int)POP(), v = (int)POP();

			PUSH((float)(s >= 0 ? (v << s) : (v >> -s)));
			break;
		}
		case PS_TRUE: PUSH(1.0f); break;
		case PS_FALSE: PUSH(0.0f); break;
		default:
			break;
		}
#undef POP
#undef PUSH
	}
	int n = f->nout > 0 ? f->nout : sp;

	if (n > PDF_FUNC_MAX_OUT) {
		n = PDF_FUNC_MAX_OUT;
	}
	if (n > sp) {
		n = sp;
	}
	for (int i = 0; i < n; i++) {
		out[i] = st[sp - n + i];
	}
	*nout = n;
	return 0;
}

/* ---- type 0 ----------------------------------------------------------- */

static float sample_at(const struct pdf_function *f, size_t index, int j)
{
	size_t bit = (index * (size_t)f->nout + (size_t)j) * (size_t)f->bps;
	size_t byte = bit / 8;
	uint32_t maxv = (f->bps >= 32) ? 0xFFFFFFFFu : ((1u << f->bps) - 1);

	if (byte >= f->samples_len) {
		return 0.0f;
	}
	uint32_t v = 0;

	switch (f->bps) {
	case 8:
		v = f->samples[byte];
		break;
	case 16:
		v = (byte + 1 < f->samples_len) ? ((uint32_t)f->samples[byte] << 8) | f->samples[byte + 1] : 0;
		break;
	case 24:
		v = (byte + 2 < f->samples_len) ? ((uint32_t)f->samples[byte] << 16) |
						  ((uint32_t)f->samples[byte + 1] << 8) | f->samples[byte + 2] : 0;
		break;
	case 32:
		v = (byte + 3 < f->samples_len) ? ((uint32_t)f->samples[byte] << 24) |
						  ((uint32_t)f->samples[byte + 1] << 16) |
						  ((uint32_t)f->samples[byte + 2] << 8) | f->samples[byte + 3] : 0;
		break;
	default: {
		/* 1, 2, 4, 12 bits */
		for (int k = 0; k < f->bps; k++) {
			size_t b = bit + (size_t)k;
			size_t by = b / 8;
			uint32_t bitv = (by < f->samples_len) ? (f->samples[by] >> (7 - (b % 8))) & 1 : 0;

			v = (v << 1) | bitv;
		}
		break;
	}
	}
	return (float)v / (float)maxv;
}

static int sampled_eval(const struct pdf_function *f, const float *in, int nin, float *out, int *nout)
{
	/* multilinear interpolation on the first input, nearest on the others */
	float e[MAX_IN];
	size_t idx0 = 0, idx1 = 0, stride = 1;
	float frac = 0.0f;

	for (int i = 0; i < f->nin && i < nin; i++) {
		float x = in[i];
		float d0 = f->domain[i * 2], d1 = f->domain[i * 2 + 1];

		if (x < d0) {
			x = d0;
		}
		if (x > d1) {
			x = d1;
		}
		float enc0 = f->encode0 ? f->encode0[i * 2] : 0.0f;
		float enc1 = f->encode0 ? f->encode0[i * 2 + 1] : (float)(f->size[i] - 1);

		e[i] = interp(x, d0, d1, enc0, enc1);
		if (e[i] < 0) {
			e[i] = 0;
		}
		if (e[i] > (float)(f->size[i] - 1)) {
			e[i] = (float)(f->size[i] - 1);
		}
		int fl = (int)e[i];

		if (i == 0) {
			frac = e[i] - (float)fl;
			idx0 = (size_t)fl;
			idx1 = (size_t)(fl + 1 < f->size[i] ? fl + 1 : fl);
		} else {
			size_t near = (size_t)(e[i] + 0.5f);

			idx0 += near * stride;
			idx1 += near * stride;
		}
		stride *= (size_t)f->size[i];
	}
	int n = f->nout;

	if (n > PDF_FUNC_MAX_OUT) {
		n = PDF_FUNC_MAX_OUT;
	}
	for (int j = 0; j < n; j++) {
		float s = sample_at(f, idx0, j) * (1.0f - frac) + sample_at(f, idx1, j) * frac;
		float dmin = f->decode ? f->decode[j * 2] : f->range[j * 2];
		float dmax = f->decode ? f->decode[j * 2 + 1] : f->range[j * 2 + 1];

		out[j] = dmin + s * (dmax - dmin);
	}
	*nout = n;
	return 0;
}

/* ---- loading ---------------------------------------------------------- */

static struct pdf_function *load_one(struct pdf_doc *doc, struct pdf_obj *obj, int depth);

static struct pdf_function *load_one(struct pdf_doc *doc, struct pdf_obj *obj, int depth)
{
	obj = pdf_resolve(doc, obj);
	if (obj == NULL || depth > 8) {
		return NULL;
	}
	struct pdf_function *f = pdf_calloc(sizeof(*f));

	if (f == NULL) {
		return NULL;
	}
	if (obj->type == PDF_ARRAY) {
		f->type = -1;
		f->nparts = (int)obj->arr.n;
		if (f->nparts > PDF_FUNC_MAX_OUT) {
			f->nparts = PDF_FUNC_MAX_OUT;
		}
		f->parts = pdf_calloc(sizeof(*f->parts) * (size_t)f->nparts);
		if (f->parts == NULL) {
			pdf_free(f);
			return NULL;
		}
		for (int i = 0; i < f->nparts; i++) {
			f->parts[i] = load_one(doc, pdf_array_get(doc, obj, (size_t)i), depth + 1);
		}
		f->nin = 1;
		f->nout = f->nparts;
		return f;
	}
	if (obj->type != PDF_DICT && obj->type != PDF_STREAM) {
		pdf_free(f);
		return NULL;
	}
	f->type = (int)pdf_int(pdf_get(doc, obj, "FunctionType"));
	int nd = load_float_array(doc, pdf_get(doc, obj, "Domain"), f->domain, MAX_IN * 2);

	f->nin = nd / 2;
	if (f->nin < 1) {
		f->nin = 1;
		f->domain[0] = 0;
		f->domain[1] = 1;
	}
	int nr = load_float_array(doc, pdf_get(doc, obj, "Range"), f->range, PDF_FUNC_MAX_OUT * 2);

	f->has_range = nr >= 2;
	f->nout = nr / 2;

	switch (f->type) {
	case 2: {
		int n0 = load_float_array(doc, pdf_get(doc, obj, "C0"), f->c0, PDF_FUNC_MAX_OUT);
		int n1 = load_float_array(doc, pdf_get(doc, obj, "C1"), f->c1, PDF_FUNC_MAX_OUT);

		if (n0 == 0) {
			f->c0[0] = 0.0f;
			n0 = 1;
		}
		if (n1 == 0) {
			f->c1[0] = 1.0f;
			n1 = 1;
		}
		f->nout = n0 < n1 ? n0 : n1;
		struct pdf_obj *n = pdf_get(doc, obj, "N");

		f->n = n ? pdf_num(n) : 1.0f;
		break;
	}
	case 3: {
		struct pdf_obj *funcs = pdf_get(doc, obj, "Functions");

		f->nsub = (int)pdf_array_len(funcs);
		if (f->nsub <= 0) {
			break;
		}
		f->sub = pdf_calloc(sizeof(*f->sub) * (size_t)f->nsub);
		f->bounds = pdf_calloc(sizeof(float) * (size_t)(f->nsub + 1));
		f->encode = pdf_calloc(sizeof(float) * (size_t)f->nsub * 2);
		if (f->sub == NULL || f->bounds == NULL || f->encode == NULL) {
			break;
		}
		for (int i = 0; i < f->nsub; i++) {
			f->sub[i] = load_one(doc, pdf_array_get(doc, funcs, (size_t)i), depth + 1);
			f->encode[i * 2] = 0;
			f->encode[i * 2 + 1] = 1;
		}
		load_float_array(doc, pdf_get(doc, obj, "Bounds"), f->bounds, f->nsub - 1);
		load_float_array(doc, pdf_get(doc, obj, "Encode"), f->encode, f->nsub * 2);
		if (f->nout == 0 && f->sub[0] != NULL) {
			f->nout = pdf_function_num_outputs(f->sub[0]);
		}
		break;
	}
	case 0: {
		if (obj->type != PDF_STREAM) {
			break;
		}
		struct pdf_obj *size = pdf_get(doc, obj, "Size");

		for (int i = 0; i < f->nin; i++) {
			f->size[i] = (int)pdf_int(pdf_array_get(doc, size, (size_t)i));
			if (f->size[i] < 1) {
				f->size[i] = 1;
			}
		}
		f->bps = (int)pdf_int(pdf_get(doc, obj, "BitsPerSample"));
		if (f->bps <= 0) {
			f->bps = 8;
		}
		struct pdf_obj *dec = pdf_get(doc, obj, "Decode");

		if (dec != NULL && dec->type == PDF_ARRAY && (int)dec->arr.n >= f->nout * 2) {
			f->decode = pdf_calloc(sizeof(float) * (size_t)f->nout * 2);
			if (f->decode) {
				load_float_array(doc, dec, f->decode, f->nout * 2);
			}
		}
		struct pdf_obj *enc = pdf_get(doc, obj, "Encode");

		if (enc != NULL && enc->type == PDF_ARRAY && (int)enc->arr.n >= f->nin * 2) {
			f->encode0 = pdf_calloc(sizeof(float) * (size_t)f->nin * 2);
			if (f->encode0) {
				load_float_array(doc, enc, f->encode0, f->nin * 2);
			}
		}
		if (pdf_stream_decode(doc, obj, &f->samples, &f->samples_len) < 0) {
			f->samples = NULL;
			f->samples_len = 0;
		}
		break;
	}
	case 4: {
		if (obj->type != PDF_STREAM) {
			break;
		}
		uint8_t *src;
		size_t slen;

		if (pdf_stream_decode(doc, obj, &src, &slen) < 0) {
			break;
		}
		f->code = pdf_malloc(sizeof(*f->code) * PS_MAX_CODE);
		if (f->code == NULL) {
			pdf_free(src);
			break;
		}
		struct ps_parser p = {.code = f->code, .n = 0, .err = false};
		struct pdf_lex lx;

		pdf_lex_init(&lx, src, slen, 0);
		pdf_lex_skip_ws(&lx);
		if (lx.pos < lx.len && lx.buf[lx.pos] == '{') {
			lx.pos++;
		}
		ps_parse_block(&p, &lx, 0);
		f->ncode = p.err ? 0 : p.n;
		pdf_free(src);
		break;
	}
	default:
		break;
	}
	return f;
}

struct pdf_function *pdf_function_load(struct pdf_doc *doc, struct pdf_obj *obj)
{
	return load_one(doc, obj, 0);
}

void pdf_function_free(struct pdf_function *f)
{
	if (f == NULL) {
		return;
	}
	for (int i = 0; i < f->nsub; i++) {
		pdf_function_free(f->sub[i]);
	}
	pdf_free(f->sub);
	pdf_free(f->bounds);
	pdf_free(f->encode);
	pdf_free(f->samples);
	pdf_free(f->decode);
	pdf_free(f->encode0);
	pdf_free(f->code);
	for (int i = 0; i < f->nparts; i++) {
		pdf_function_free(f->parts[i]);
	}
	pdf_free(f->parts);
	pdf_free(f);
}

int pdf_function_num_outputs(const struct pdf_function *f)
{
	return f ? f->nout : 0;
}

int pdf_function_eval(const struct pdf_function *f, const float *in, int nin, float *out, int *nout)
{
	float x[MAX_IN];

	*nout = 0;
	if (f == NULL) {
		return -1;
	}
	if (f->type == -1) {
		int n = 0;

		for (int i = 0; i < f->nparts && n < PDF_FUNC_MAX_OUT; i++) {
			float o[PDF_FUNC_MAX_OUT];
			int no = 0;

			if (f->parts[i] != NULL && pdf_function_eval(f->parts[i], in, nin, o, &no) == 0 && no > 0) {
				out[n++] = o[0];
			} else {
				out[n++] = 0.0f;
			}
		}
		*nout = n;
		return 0;
	}
	for (int i = 0; i < f->nin && i < MAX_IN; i++) {
		float v = (i < nin) ? in[i] : 0.0f;

		if (v < f->domain[i * 2]) {
			v = f->domain[i * 2];
		}
		if (v > f->domain[i * 2 + 1]) {
			v = f->domain[i * 2 + 1];
		}
		x[i] = v;
	}
	int rc = -1;

	switch (f->type) {
	case 2: {
		float t = (f->n == 1.0f) ? x[0] : powf(x[0], f->n);

		for (int j = 0; j < f->nout; j++) {
			out[j] = f->c0[j] + t * (f->c1[j] - f->c0[j]);
		}
		*nout = f->nout;
		rc = 0;
		break;
	}
	case 3: {
		if (f->nsub <= 0) {
			break;
		}
		int k = 0;

		while (k < f->nsub - 1 && x[0] >= f->bounds[k]) {
			k++;
		}
		float lo = (k == 0) ? f->domain[0] : f->bounds[k - 1];
		float hi = (k == f->nsub - 1) ? f->domain[1] : f->bounds[k];
		float t = interp(x[0], lo, hi, f->encode[k * 2], f->encode[k * 2 + 1]);

		rc = pdf_function_eval(f->sub[k], &t, 1, out, nout);
		break;
	}
	case 0:
		rc = sampled_eval(f, x, f->nin, out, nout);
		break;
	case 4:
		rc = ps_eval(f, x, f->nin, out, nout);
		break;
	default:
		break;
	}
	if (rc == 0 && f->has_range) {
		for (int j = 0; j < *nout && j < f->nout; j++) {
			if (out[j] < f->range[j * 2]) {
				out[j] = f->range[j * 2];
			}
			if (out[j] > f->range[j * 2 + 1]) {
				out[j] = f->range[j * 2 + 1];
			}
		}
	}
	return rc;
}
