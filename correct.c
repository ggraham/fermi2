#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

/****************************
 *** Hard coded constants ***
 ****************************/

#define FMC_Q0        31
#define FMC_NOHIT_PEN 63
#define FMC_MAX_Q     41

/******************
 *** Parameters ***
 ******************/

int fmc_verbose = 3;

typedef struct {
	int k, suf_len, min_occ, n_threads, defQ;
	int gap_penalty;
	int max_heap_size;
	int max_penalty;
	int max_penalty_diff;
	int64_t batch_size;

	double a1, a2, err, prior;
} fmc_opt_t;

void fmc_opt_init(fmc_opt_t *opt)
{
	memset(opt, 0, sizeof(fmc_opt_t));
	opt->k = 17;
	opt->suf_len = 1;
	opt->min_occ = 3;
	opt->n_threads = 1;
	opt->defQ = 20;
	opt->gap_penalty = 40;
	opt->max_heap_size = 256;
	opt->max_penalty = 120;
	opt->max_penalty_diff = 60;
	opt->batch_size = (1ULL<<30) - (1ULL<<20);

	opt->a1 = 0.05;
	opt->a2 = 10;
	opt->err = 0.005;
	opt->prior = 0.99;
}

void kt_for(int n_threads, void (*func)(void*,int,int), void *shared, int n_items);
double cputime(void);
double realtime(void);
void liftrlimit(void);

/****************************
 *** Consensus generation ***
 ****************************/

#include <math.h>

double kf_lgamma(double z)
{
	double x = 0;
	x += 0.1659470187408462e-06 / (z+7);
	x += 0.9934937113930748e-05 / (z+6);
	x -= 0.1385710331296526     / (z+5);
	x += 12.50734324009056      / (z+4);
	x -= 176.6150291498386      / (z+3);
	x += 771.3234287757674      / (z+2);
	x -= 1259.139216722289      / (z+1);
	x += 676.5203681218835      / z;
	x += 0.9999999999995183;
	return log(x) - 5.58106146679532777 - z + (z-0.5) * log(z+6.5);
}

double fmc_beta_binomial(int n, int k, double a, double b)
{
	double x, y, z;
	x = lgamma(n + 1) - (lgamma(k + 1) + lgamma(n - k + 1));
	y = lgamma(k + a) + lgamma(n - k + b) - lgamma(n + a + b);
	z = lgamma(a + b) - (lgamma(a) + lgamma(b));
	return exp(x + y + z);
}

uint8_t *fmc_precal_qtab(int max, double e1, double e2, double a1, double a2, double prior1)
{
	int n, k;
	uint8_t *qtab;
	double b1 = a1 * (1 - e1) / e1, b2 = a2 * (1 - e2) / e2;

	qtab = calloc(max * max, 1);
	for (n = 1; n < max; ++n) {
		uint8_t *qn = &qtab[n*max];
		//fprintf(stderr, "=> %d <=", n);
		for (k = 0; k < n; ++k) {
			double p1, p2;
			int q;
			p1 = fmc_beta_binomial(n, k, a1, b1);
			p2 = fmc_beta_binomial(n, k, a2, b2);
			q = -4.343 * log(1. - p1 * prior1 / (p1 * prior1 + p2 * (1-prior1)));
			qn[k] = q < 255? q : 255;
			//fprintf(stderr, "\t%d:%d", k, q);
		}
		//fprintf(stderr, "\n");
	}
	return qtab;
}

/******************
 *** Hash table ***
 ******************/

#include "kvec.h"
#include "khash.h"

#define fmc_cell_get_key(x) ((x)>>28)
#define fmc_cell_get_val(x, is_right) ((x)>>((is_right)?14:0)&0x3fff)
#define fmc_cell_set_keyval(key, val0, val1) ((x)<<28|(val1)<<14|(val0))
#define fmc_cell_set_val(b1, b2, q1, q2) ((b1)<<12|(b2)<<10|(q1)<<5|(q2))

#define fmc_cell_get_b1(v) ((v)>>12&0x3)
#define fmc_cell_get_b2(v) ((v)>>10&0x3)
#define fmc_cell_get_q1(v) (((v)>>5&0x1f)<<1)
#define fmc_cell_get_q2(v) (((v)&0x1f)<<1)

#define fmc_cell_has_b2(v) (((v)>>5&0x1f) != FMC_Q0)

static inline uint64_t hash_64(uint64_t key)
{
	key += ~(key << 32);
	key ^= (key >> 22);
	key += ~(key << 13);
	key ^= (key >> 8);
	key += (key << 3);
	key ^= (key >> 15);
	key += ~(key << 27);
	key ^= (key >> 31);
	return key;
}

#define fmc_hash_func(a) hash_64(fmc_cell_get_key(a))
#define fmc_eq_func(a, b) (fmc_cell_get_key(a) == fmc_cell_get_key(b))
KHASH_INIT(fmc, uint64_t, char, 0, fmc_hash_func, fmc_eq_func)

typedef khash_t(fmc) fmc_hash_t;
typedef kvec_t(uint64_t) fmc64_v;

typedef struct {
	uint64_t suf:63, missing:1;
	uint64_t x;
} longcell_t;

#define fmc_longcell_get_key(a) ((a).suf<<36 | fmc_cell_get_key((a).x))
#define fmc_hash_func_long(a) hash_64(fmc_longcell_get_key(a))
#define fmc_eq_func_long(a, b) (fmc_longcell_get_key(a) == fmc_longcell_get_key(b))
KHASH_INIT(kache, longcell_t, char, 0, fmc_hash_func_long, fmc_eq_func_long)

typedef khash_t(kache) kmercache_t;

/*********************************
 *** Collect k-mer information ***
 *********************************/

#include "rld0.h"

typedef kvec_t(rldintv_t) rldintv_v;

rldintv_t *fmc_traverse(const rld_t *e, int depth)
{
	rldintv_v stack = {0,0,0};
	rldintv_t *p, *ret;
	uint64_t x;

	ret = calloc(1<<depth*2, sizeof(rldintv_t));
	kv_pushp(rldintv_t, stack, &p);
	p->x[0] = p->x[1] = 0, p->x[2] = e->mcnt[0], p->info = 0;
	x = 0;
	while (stack.n) {
		rldintv_t top = kv_pop(stack);
		if (top.info>>2 > 0) {
			int shift = ((top.info>>2) - 1) << 1;
			x = (x & ~(3ULL<<shift)) | (uint64_t)(top.info&3)<<shift;
		}
		if (top.info>>2 != depth) {
			int c;
			rldintv_t t[6];
			rld_extend(e, &top, t, 1);
			for (c = 1; c < 5; ++c) {
				if (t[c].x[2] == 0) continue;
				t[c].info = ((top.info>>2) + 1) << 2 | (c - 1);
				kv_push(rldintv_t, stack, t[c]);
			}
		} else ret[x] = top, ret[x].info = x;
	}
	free(stack.a);
	return ret;
}

int fmc_intv2tip(uint8_t *qtab[2], const rldintv_t t[6])
{
	int c, max_c, max_c2, q1, q2;
	uint64_t max, max2, rest, rest2, sum;
	for (c = 1, max = max2 = 0, max_c = max_c2 = 1, sum = 0; c <= 4; ++c) {
		if (t[c].x[2] > max) max2 = max, max_c2 = max_c, max = t[c].x[2], max_c = c;
		else if (t[c].x[2] > max2) max2 = t[c].x[2], max_c2 = c;
		sum += t[c].x[2];
	}
	rest = sum - max; rest2 = sum - max - max2;
	if (sum > 255) {
		rest  = (int)(255. * rest  / sum + .499);
		rest2 = (int)(255. * rest2 / sum + .499);
		sum = 255;
	}
	q1 = qtab[0][sum<<8|rest];
	q1  = rest? (q1 < FMC_MAX_Q? q1 : FMC_MAX_Q) >> 1 : FMC_Q0;
	q2 = qtab[1][sum<<8|rest2];
	q2 = rest2? (q2 < FMC_MAX_Q? q2 : FMC_MAX_Q) >> 1 : FMC_Q0;
	return fmc_cell_set_val(4-max_c, 4-max_c2, q1, q2);
}

void fmc_collect1(const rld_t *e, uint8_t *qtab[2], int suf_len, int depth, int min_occ, const rldintv_t *start, fmc64_v *a)
{
	rldintv_v stack = {0,0,0};
	uint64_t x = 0, *p;

	kv_push(rldintv_t, stack, *start);
	stack.a[0].info = 0;
	a->n = 0;
	while (stack.n) {
		rldintv_t top = kv_pop(stack);
		if (top.info>>2 > 0) {
			int shift = ((top.info>>2) - 1) << 1;
			x = (x & ~(3ULL<<shift)) | (uint64_t)(top.info&3)<<shift;
		}
		if (top.info>>2 == depth) {
			rldintv_t t[6];
			int val[2];
			kv_pushp(uint64_t, *a, &p);
			rld_extend(e, &top, t, 1);
			val[0] = fmc_intv2tip(qtab, t);
			rld_extend(e, &top, t, 0);
			val[1] = fmc_intv2tip(qtab, t);
			*p = fmc_cell_set_keyval(x, val[0], val[1]);
		} else {
			int c, end = (suf_len + (top.info>>2)) == (suf_len + depth) / 2? 2 : 4;
			rldintv_t t[6];
			rld_extend(e, &top, t, 1);
			for (c = 1; c <= end; ++c) {
				if (t[c].x[2] < min_occ) continue;
				t[c].info = ((top.info>>2) + 1) << 2 | (c - 1);
				kv_push(rldintv_t, stack, t[c]);
			}
		}
	}
	free(stack.a);
}

typedef struct {
	const rld_t *e;
	uint8_t *qtab[2];
	rldintv_t *suf;
	fmc64_v *kmer;
	int suf_len, depth, min_occ;
} for_collect_t;

static void collect_func(void *shared, int i, int tid)
{
	for_collect_t *s = (for_collect_t*)shared;
	fmc_collect1(s->e, s->qtab, s->suf_len, s->depth, s->min_occ, &s->suf[i], &s->kmer[i]);
}

void fmc_kmer_stat(int suf_len, const fmc64_v *a)
{
	int i, j, k, n_suf = 1<<suf_len*2;
	int64_t tot = 0, n_Q1 = 0, n_Q5 = 0, n_Qmax = 0;
	for (i = 0; i < n_suf; ++i) {
		const fmc64_v *ai = &a[i];
		tot += ai->n<<1;
		for (j = 0; j < ai->n; ++j) {
			for (k = 0; k < 2; ++k) {
				int val, q;
				val = fmc_cell_get_val(ai->a[j], k);
				q = fmc_cell_get_q1(val);
				n_Q1 += (q < 1);
				n_Q5 += (q < 10);
				n_Qmax += (q < FMC_Q0<<1);
			}
		}
	}
	fprintf(stderr, "[M::%s] %ld k-mers; %.2f%% <Q1; %.2f%% <Q10; %.2f%% <Qmax\n", __func__, (long)tot,
			100.*n_Q1/tot, 100.*n_Q5/tot, 100.*n_Qmax/tot);
}

fmc64_v *fmc_collect(fmc_opt_t *opt, const char *fn_fmi)
{
	rld_t *e;
	double tc, tr;
	int depth = opt->k - opt->suf_len, n_suf = 1 << opt->suf_len*2;
	for_collect_t f;

	assert(0 < depth && depth <= 18);

	fprintf(stderr, "[M::%s] reading the FMD-index... ", __func__);
	tc = cputime(); tr = realtime();
	e = rld_restore(fn_fmi);
	fprintf(stderr, " in %.3f sec (%.3f CPU sec)\n", realtime() - tr, cputime() - tc);

	fprintf(stderr, "[M::%s] collecting high occurrence k-mers... ", __func__);
	tc = cputime(); tr = realtime();
	f.suf = fmc_traverse(e, opt->suf_len);
	f.qtab[0] = fmc_precal_qtab(1<<8, opt->err, 0.5,      opt->a1, opt->a2, opt->prior);
	f.qtab[1] = fmc_precal_qtab(1<<8, opt->err, 0.333333, opt->a1, opt->a2, opt->prior);
	f.e = e, f.suf_len = opt->suf_len, f.depth = depth, f.min_occ = opt->min_occ;
	f.kmer = calloc(n_suf, sizeof(fmc64_v));
	kt_for(opt->n_threads, collect_func, &f, n_suf);
	rld_destroy(e);
	free(f.qtab[0]); free(f.qtab[1]); free(f.suf);
	fprintf(stderr, "in %.3f sec (%.3f CPU sec)\n", realtime() - tr, cputime() - tc);

	fmc_kmer_stat(opt->suf_len, f.kmer);
	return f.kmer;
}

/****************************
 *** Write/read kmer list ***
 ****************************/

void fmc_kmer_write(FILE *fp, const fmc_opt_t *opt, const fmc64_v *a)
{
	int i, n = 1<<opt->suf_len*2;
	fwrite(opt, sizeof(fmc_opt_t), 1, fp);
	for (i = 0; i < n; ++i) {
		fwrite(&a[i].n, sizeof(size_t), 1, fp);
		fwrite(a[i].a, 8, a[i].n, fp);
	}
}

fmc64_v *fmc_kmer_read(FILE *fp, fmc_opt_t *opt)
{
	int i, n;
	fmc64_v *a;
	fread(opt, sizeof(fmc_opt_t), 1, fp);
	n = 1<<opt->suf_len*2;
	a = malloc(sizeof(fmc64_v) * n);
	for (i = 0; i < n; ++i) {
		fread(&a[i].n, sizeof(size_t), 1, fp);
		a[i].m = a[i].n;
		a[i].a = malloc(8 * a[i].n);
		fread(a[i].a, 8, a[i].n, fp);
	}
	return a;
}

fmc_hash_t **fmc_kmer2hash(const fmc_opt_t *opt, fmc64_v *a)
{
	int i, n = 1 << opt->suf_len*2;
	fmc_hash_t **h;
	double t;
	t = cputime();
	fprintf(stderr, "[M::%s] constructing the hash table... ", __func__);
	h = calloc(n, sizeof(void*));
	for (i = 0; i < n; ++i) {
		size_t j;
		int absent;
		fmc64_v *ai = &a[i];
		h[i] = kh_init(fmc);
		kh_resize(fmc, h[i], (int)(ai->n / .7 + 1.));
		for (j = 0; j < ai->n; ++j)
			kh_put(fmc, h[i], ai->a[j], &absent);
		assert(kh_size(h[i]) == ai->n);
		free(ai->a);
	}
	fprintf(stderr, "in %.3f CPU sec\n", cputime() - t);
	free(a);
	return h;
}

/************************
 *** Sequence reading ***
 ************************/

#include <zlib.h>
#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

unsigned char seq_nt6_table[128] = {
    0, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 1, 5, 2,  5, 5, 5, 3,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 5, 5, 5,  4, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 1, 5, 2,  5, 5, 5, 3,  5, 5, 5, 5,  5, 5, 5, 5,
    5, 5, 5, 5,  4, 5, 5, 5,  5, 5, 5, 5,  5, 5, 5, 5
};

typedef struct {
	int n, m;
	char **s, **q, **name;
} fmc_batch_t;

fmc_batch_t *fmc_batch_read(kseq_t *ks, int64_t batch_size)
{
	int64_t l = 0;
	fmc_batch_t *b;
	b = calloc(1, sizeof(fmc_batch_t));
	while (l < batch_size && kseq_read(ks) >= 0) {
		if (b->n >= b->m) {
			b->m = b->n + 1;
			kroundup32(b->m);
			b->s = realloc(b->s, b->m * sizeof(void*));
			b->q = realloc(b->q, b->m * sizeof(void*));
			b->name = realloc(b->name, b->m * sizeof(void*));
		}
		b->s[b->n] = strdup(ks->seq.s);
		b->q[b->n] = ks->qual.l? strdup(ks->qual.s) : 0;
		b->name[b->n++] = strdup(ks->name.s);
		l += ks->seq.l;
	}
	if (l == 0) {
		free(b);
		return 0;
	} else return b;
}

void fmc_batch_destroy(fmc_batch_t *b)
{
	int i;
	for (i = 0; i < b->n; ++i) {
		free(b->name[i]); free(b->s[i]); free(b->q[i]);
	}
	free(b->name); free(b->s); free(b->q); free(b);
}

#define STATE_N 0
#define STATE_M 1
#define STATE_I 2
#define STATE_D 3

typedef struct {
	uint8_t b:4, state:4;
	uint8_t q;
	int i;
} ecbase_t;

typedef kvec_t(ecbase_t) ecseq_t;

int fmc_seq_conv(const char *s, const char *q, int defQ, ecseq_t *seq)
{
	int i, l;
	l = strlen(s);
	kv_resize(ecbase_t, *seq, l);
	seq->n = l;
	for (i = 0; i < l; ++i) {
		ecbase_t *c = &seq->a[i];
		c->b = seq_nt6_table[(int)s[i]] - 1;
		c->q = q? *q - 33 : defQ;
		c->q = c->q < FMC_MAX_Q? c->q : FMC_MAX_Q;
		c->state = STATE_M;
		c->i = i;
	}
	return l;
}

void fmc_seq_cpy(ecseq_t *dst, const ecseq_t *src)
{
	kv_resize(ecbase_t, *dst, src->n);
	dst->n = src->n;
	memcpy(dst->a, src->a, src->n * sizeof(ecbase_t));
}

static inline ecbase_t ecbase_comp(const ecbase_t *b)
{
	ecbase_t r = *b;
	r.b = b->b < 4? 3 - b->b : 4;
	return r;
}

void fmc_seq_revcomp(ecseq_t *seq)
{
	int i;
	for (i = 0; i < seq->n>>1; ++i) {
		ecbase_t tmp;
		tmp = ecbase_comp(&seq->a[i]);
		seq->a[i] = ecbase_comp(&seq->a[seq->n - 1 - i]);
		seq->a[seq->n - 1 - i] = tmp;
	}
	if (seq->n&1) seq->a[i] = ecbase_comp(&seq->a[i]);
}

/************************
 *** Error correction ***
 ************************/

#include "ksort.h"

typedef struct {
	uint64_t kmer[2];
	int penalty;
	int k; // pos on the stack
	int i; // pos of the next-to-add base on the sequence 
	int state;
} echeap1_t;

#define echeap1_lt(a, b) ((a).penalty > (b).penalty)
KSORT_INIT(ec, echeap1_t, echeap1_lt)

typedef struct {
	int parent;
	int i, penalty;
	uint32_t state:3, base:3, qual:8, ipen:18;
} ecstack1_t;

typedef kvec_t(echeap1_t)  echeap_t;
typedef kvec_t(ecstack1_t) ecstack_t;

typedef struct {
	ecseq_t ori, tmp[2], seq;
	echeap_t heap;
	ecstack_t stack;
	kmercache_t *cache;
} fmc_aux_t;

fmc_aux_t *fmc_aux_init()
{
	fmc_aux_t *a;
	a = calloc(1, sizeof(fmc_aux_t));
	a->cache = kh_init(kache);
	return a;
}

void fmc_aux_destroy(fmc_aux_t *a)
{
	free(a->seq.a); free(a->ori.a); free(a->tmp[0].a); free(a->tmp[1].a);
	free(a->heap.a); free(a->stack.a);
	kh_destroy(kache, a->cache);
	free(a);
}

static inline void append_to_kmer(int k, uint64_t kmer[2], int a)
{
	kmer[0] = (kmer[0]<<2 & ((1ULL<<(k<<1)) - 1)) | a;
	kmer[1] = kmer[1]>>2 | (uint64_t)(3 - a) << ((k-1)<<1);
}

static inline int kmer_lookup(int k, int suf_len, uint64_t kmer[2], fmc_hash_t **h, kmercache_t *cache)
{
	int absent, i = (kmer[0]>>(k>>1<<1)&3) < 2? 0 : 1;
	khint_t kh, kc;
	longcell_t x, *p;

	x.suf = kmer[i] & ((1<<(suf_len<<1)) - 1);
	x.x = kmer[i] >> (suf_len<<1) << 28;
	kc = kh_put(kache, cache, x, &absent);
	p = &kh_key(cache, kc);
	if (absent) {
		fmc_hash_t *g = h[x.suf];
		kh = kh_get(fmc, g, x.x);
		if (kh == kh_end(g)) p->missing = 1;
		else p->missing = 0, p->x = kh_key(g, kh);
	}
	if (fmc_verbose >= 6) {
		int i, which = (kmer[0]>>(k>>1<<1)&3) < 2? 0 : 1;
		int val = p->missing? -1 : fmc_cell_get_val(p->x, !which);
		fprintf(stderr, "?? ");
		for (i = k-1; i >= 0; --i) fputc("ACGT"[kmer[0]>>2*i&3], stderr); fprintf(stderr, " - ");
		for (i = k-1; i >= 0; --i) fputc("ACGT"[kmer[1]>>2*i&3], stderr);
		fprintf(stderr, " - [%d]%lx", which, (long)kmer[which]);
		if (p->missing) fprintf(stderr, " - NOHIT\n");
		else fprintf(stderr, " - %c%d\n", "ACGTN"[fmc_cell_get_b1(val)], fmc_cell_get_q1(val));
	}
	return p->missing? -1 : fmc_cell_get_val(p->x, !i);
}

static inline void update_aux(int k, fmc_aux_t *a, const echeap1_t *p, int b, int state, int penalty, int qual)
{
	ecstack1_t *q;
	echeap1_t *r;
	// update the stack
	kv_pushp(ecstack1_t, a->stack, &q);
	q->parent = p->k;
	q->i = p->i;
	q->base = b;
	q->qual = qual;
	q->state = state;
	q->penalty = p->penalty + penalty;
	q->ipen = penalty;
	// update the heap
	kv_pushp(echeap1_t, a->heap, &r);
	r->penalty = q->penalty;
	r->k = a->stack.n - 1;
	r->kmer[0] = p->kmer[0], r->kmer[1] = p->kmer[1];
	r->state = state;
	r->i = state == STATE_I? p->i : p->i + 1;
	if (fmc_verbose >= 6)fprintf(stderr, "+> [%d] i=%d, b=%c, ipen=%d, state=%c, w=%d\n", r->k, r->i, "ACGTN"[b], penalty, "NMID"[state], q->penalty);
	if (state != STATE_D) append_to_kmer(k, r->kmer, b);
	ks_heapup_ec(a->heap.n, a->heap.a);
}

static void path_backtrack(const ecstack_t *a, int start, const ecseq_t *o, ecseq_t *s)
{
	int i = start, last;
	s->n = 0;
//	fprintf(stderr, "===> start (%d) <===\n", a->a[i].penalty);
	while (i >= 0) {
		ecbase_t *c;
		ecstack1_t *p = &a->a[i];
		if (p->state != STATE_D) {
			kv_pushp(ecbase_t, *s, &c);
			c->b = p->base; c->state = p->state;
			c->q = p->qual < FMC_MAX_Q? p->qual : FMC_MAX_Q;
			c->i = o->a[p->i].i;
//			fprintf(stderr, "[%d] %d,%d,%c; %c => %c\n", i, c->i, p->ipen, "NMID"[p->state], "ACGTN"[o->a[p->i].b], "ACGTN"[c->b]);
		}
		last = p->i;
		i = p->parent;
	}
	for (i = last - 1; i >= 0; --i)
		kv_push(ecbase_t, *s, o->a[i]);
	for (i = 0; i < s->n>>1; ++i) {
		ecbase_t tmp = s->a[i];
		s->a[i] = s->a[s->n - 1 - i];
		s->a[s->n - 1 - i] = tmp;
	}
}

static void path_adjustq(int diff, ecseq_t *s1, const ecseq_t *s2)
{
	int i1 = 0, i2 = 0;
	while (i1 < s1->n && i2 < s2->n) {
		ecbase_t *b1;
		const ecbase_t *b2;
		b1 = &s1->a[i1];
		b2 = &s2->a[i2];
		if (b1->b != b2->b || b1->i != b2->i) {
			b1->q = b1->q > b2->q? b1->q - b2->q : 0;
			b1->q = b1->q < diff? b1->q : diff;
		}
		if (b1->state == STATE_I && b2->state != STATE_I) ++i1;
		else if (b2->state == STATE_I && b1->state != STATE_I) ++i2;
		else ++i1, ++i2;
	}
	for (; i1 < s1->n; ++i1) {
		ecbase_t *b = &s1->a[i1];
		b->q = b->q < diff? b->q : diff;
	}
}

static int fmc_correct1_aux(const fmc_opt_t *opt, fmc_hash_t **h, fmc_aux_t *a)
{
	echeap1_t z;
	int l, path_end[2] = {-1,-1}, max_i = 0;

	kh_clear(kache, a->cache);
	a->heap.n = a->stack.n = 0;
	// find the first k-mer
	memset(&z, 0, sizeof(echeap1_t));
	for (z.i = 0, l = 0; z.i < a->seq.n && l < opt->k; ++z.i)
		if (a->seq.a[z.i].b > 3) l = 0, z.kmer[0] = z.kmer[1] = 0;
		else ++l, append_to_kmer(opt->k, z.kmer, a->seq.a[z.i].b);
	if (z.i == a->seq.n) return -1;
	z.k = -1; // the first k-mer is not on the stack
	kv_push(echeap1_t, a->heap, z);
	// search for the best path
	while (a->heap.n) {
		ecbase_t *c;
		int val, is_excessive;
		z = a->heap.a[0];
		a->heap.a[0] = kv_pop(a->heap);
		ks_heapdown_ec(0, a->heap.n, a->heap.a);
		if (path_end[0] >= 0 && z.penalty > a->stack.a[path_end[0]].penalty + opt->max_penalty_diff) break;
		if (z.i == a->seq.n) { // end of sequence
			if (fmc_verbose >= 6) fprintf(stderr, "** penalty=%d\n", z.penalty);
			if (path_end[0] >= 0) {
				path_end[1] = z.k;
				break;
			} else path_end[0] = z.k;
			continue;
		}
		if (fmc_verbose >= 6) fprintf(stderr, "<- [%d] i=%d, size=%ld, b=%c, penalty=%d, state=%d\n", z.k, z.i, a->heap.n, "ACGTN"[a->seq.a[z.i].b], z.penalty, z.k>=0? a->stack.a[z.k].state : -1);
		c = &a->seq.a[z.i];
		max_i = max_i > z.i? max_i : z.i;
		is_excessive = (a->heap.n >= max_i * (opt->gap_penalty? 5 : 2));
		val = kmer_lookup(opt->k, opt->suf_len, z.kmer, h, a->cache);
		if (val >= 0) { // present in the hash table
			int b1 = fmc_cell_get_b1(val);
			int b2 = fmc_cell_has_b2(val)? fmc_cell_get_b2(val) : 4;
			int q1 = fmc_cell_get_q1(val);
			int q2 = fmc_cell_get_q2(val);
			if (b1 == c->b) { // read base matching the consensus
				update_aux(opt->k, a, &z, b1, STATE_M, 0, c->q + q1);
			} else if (c->b > 3) { // read base is "N"
				update_aux(opt->k, a, &z, b1, STATE_M, 3, q1);
				if (b2 < 4 && !is_excessive) update_aux(opt->k, a, &z, b2, STATE_M, q1, 0);
			} else if (b2 >= 4 || b2 == c->b) { // no second base or the second base is the read base; two branches
				int diff = (int)c->q - q1;
				if (!is_excessive || q1 <= c->q)
					update_aux(opt->k, a, &z, c->b, STATE_M, q1,   diff > 0? diff : 0);
				if (!is_excessive || q1 >= c->q)
					update_aux(opt->k, a, &z, b1,   STATE_M, c->q, diff > 0? 0 : -diff);
				if (opt->gap_penalty > 0 && z.i < a->seq.n - 1 && !is_excessive) {
					if (z.state != STATE_D)
						update_aux(opt->k, a, &z, b1,STATE_I, opt->gap_penalty, diff > 0? 0 : -diff);
					if (z.state != STATE_I)
						update_aux(opt->k, a, &z, b1,STATE_D, opt->gap_penalty, diff > 0? 0 : -diff);
				}
			} else {
				int diff = (int)c->q - (q1 + q2);
				if (!is_excessive || q1 + q2 <= c->q)
					update_aux(opt->k, a, &z, c->b, STATE_M, q1 + q2,              diff > 0? diff : 0);
				if (!is_excessive || q1 + q2 >= c->q)
					update_aux(opt->k, a, &z, b1,   STATE_M, c->q,                 diff > 0? 0 : -diff < q1? -diff : q1);
				if (!is_excessive)
					update_aux(opt->k, a, &z, b2,   STATE_M, c->q > q1? c->q : q1, 0);
				if (opt->gap_penalty > 0 && z.i < a->seq.n - 1 && !is_excessive) {
					if (z.state != STATE_D)
						update_aux(opt->k, a, &z, b1,STATE_I, opt->gap_penalty,    diff > 0? 0 : -diff < q1? -diff : q1);
					if (z.state != STATE_I)
						update_aux(opt->k, a, &z, b1,STATE_D, opt->gap_penalty,    diff > 0? 0 : -diff < q1? -diff : q1);
				}
			}
		} else update_aux(opt->k, a, &z, c->b < 4? c->b : lrand48()&4, STATE_N, FMC_NOHIT_PEN, c->q); // no present in the hash table
		if (fmc_verbose >= 6) fprintf(stderr, "//\n");
	}
	// backtrack
	if (path_end[0] >= 0) {
//		int i;
		path_backtrack(&a->stack, path_end[0], &a->seq, &a->tmp[0]);
//		for (i = 0; i < a->tmp[0].n; ++i) fputc("ACGTN"[a->tmp[0].a[i].b], stderr); fputc('\n', stderr);
//		for (i = 0; i < a->tmp[0].n; ++i) fputc(a->tmp[0].a[i].q+33, stderr); fputc('\n', stderr);
		if (path_end[1] >= 0) {
			path_backtrack(&a->stack, path_end[1], &a->seq, &a->tmp[1]);
			path_adjustq(a->stack.a[path_end[1]].penalty - a->stack.a[path_end[0]].penalty, &a->tmp[0], &a->tmp[1]);
//			for (i = 0; i < a->tmp[1].n; ++i) fputc("ACGTN"[a->tmp[1].a[i].b], stderr); fputc('\n', stderr);
//			for (i = 0; i < a->tmp[0].n; ++i) fputc("ACGTN"[a->tmp[0].a[i].b], stderr); fputc('\n', stderr);
//			for (i = 0; i < a->tmp[0].n; ++i) fputc(a->tmp[0].a[i].q+33, stderr); fputc('\n', stderr);
		}
		fmc_seq_cpy(&a->seq, &a->tmp[0]);
	}
	return 0;
}

void fmc_correct1(const fmc_opt_t *opt, fmc_hash_t **h, char **s, char **q, fmc_aux_t *a)
{
	fmc_aux_t *_a = 0;
	int i;
	if (a == 0) a = _a = fmc_aux_init();
	fmc_seq_conv(*s, *q, opt->defQ, &a->ori);
	fmc_seq_cpy(&a->seq, &a->ori);
	fmc_correct1_aux(opt, h, a);
	fmc_seq_revcomp(&a->seq);
	fmc_correct1_aux(opt, h, a);
	fmc_seq_revcomp(&a->seq);
	if (a->seq.n > a->ori.n) {
		*s = realloc(*s, a->seq.n + 1);
		*q = realloc(*q, a->seq.n + 1);
	} else if (!*q) *q = calloc(a->seq.n + 1, 1);
	for (i = 0; i < a->seq.n; ++i) {
		ecbase_t *b = &a->seq.a[i];
		if (b->state != STATE_N || a->ori.a[b->i].b < 4) {
			(*s)[i] = b->b == a->ori.a[b->i].b? "ACGTN"[b->b] : "acgtn"[b->b];
			(*q)[i] = (b->q < FMC_MAX_Q? b->q : FMC_MAX_Q) + 33;
		} else (*s)[i] = 'N', (*q)[i] = 33;
	}
	(*s)[i] = (*q)[i] = 0;
	if (_a) fmc_aux_destroy(_a);
}

typedef struct {
	const fmc_opt_t *opt;
	fmc_hash_t **h;
	char **name, **s, **q;
	fmc_aux_t **a;
} for_correct_t;

static void correct_func(void *data, int i, int tid)
{
	for_correct_t *f = (for_correct_t*)data;
	if (fmc_verbose >= 5) fprintf(stderr, ">%s\n", f->name[i]);
	fmc_correct1(f->opt, f->h, &f->s[i], &f->q[i], f->a[tid]);
}

void fmc_correct(const fmc_opt_t *opt, fmc_hash_t **h, int n, char **s, char **q, char **name)
{
	for_correct_t f;
	int i;

	f.a = calloc(opt->n_threads, sizeof(void*));
	f.opt = opt, f.h = h, f.name = name, f.s = s, f.q = q;
	for (i = 0; i < opt->n_threads; ++i)
		f.a[i] = fmc_aux_init();
	if (opt->n_threads == 1) {
		for (i = 0; i < n; ++i)
			correct_func(&f, i, 0);
	} else kt_for(opt->n_threads, correct_func, &f, n);
	for (i = 0; i < n; ++i) {
		putchar('>'); puts(f.name[i]);
		puts(f.s[i]); putchar('+'); putchar('\n');
		puts(f.q[i]);
	}
	for (i = 0; i < opt->n_threads; ++i)
		fmc_aux_destroy(f.a[i]);
	free(f.a);
}

/*********************
 *** Main function ***
 *********************/

int main_correct(int argc, char *argv[])
{
	int c;
	fmc_opt_t opt;
	fmc64_v *kmer;
	char *fn_kmer = 0;

	liftrlimit();

	fmc_opt_init(&opt);
	while ((c = getopt(argc, argv, "k:o:t:h:g:v:p:e:")) >= 0) {
		if (c == 'k') opt.k = atoi(optarg);
		else if (c == 'o') opt.min_occ = atoi(optarg);
		else if (c == 't') opt.n_threads = atoi(optarg);
		else if (c == 'h') fn_kmer = optarg;
		else if (c == 'g') opt.gap_penalty = atoi(optarg);
		else if (c == 'v') fmc_verbose = atoi(optarg);
		else if (c == 'p') opt.prior = atof(optarg);
		else if (c == 'e') opt.err = atof(optarg);
	}
	if (!(opt.k&1)) {
		++opt.k;
		fprintf(stderr, "[W::%s] -k must be an odd number; change -k to %d\n", __func__, opt.k);
	}
	if (optind == argc) {
		fprintf(stderr, "Usage: fermi2 correct [-k kmer=%d] [-o minOcc=%d] [-t nThreads=%d] in.fmd\n",
				opt.k, opt.min_occ, opt.n_threads);
		return 1;
	}
	opt.suf_len = opt.k > 18? opt.k - 18 : 1;

	if (fn_kmer) {
		FILE *fp;
		fp = strcmp(fn_kmer, "-")? fopen(fn_kmer, "rb") : stdin;
		assert(fp);
		kmer = fmc_kmer_read(fp, &opt);
		fclose(fp);
	} else kmer = fmc_collect(&opt, argv[optind]);

	if (optind + 2 > argc) {
		int i;
		fmc_kmer_write(stdout, &opt, kmer);
		for (i = 0; i < 1<<opt.suf_len*2; ++i)
			free(kmer[i].a);
		free(kmer);
		return 0;
	} else {
		fmc_hash_t **h;
		int i;
		kseq_t *ks;
		gzFile fp;
		fmc_batch_t *b;

		h = fmc_kmer2hash(&opt, kmer); // kmer is deallocated here
		fp = gzopen(argv[optind+1], "r");
		ks = kseq_init(fp);
		while ((b = fmc_batch_read(ks, opt.batch_size)) != 0) {
			fmc_correct(&opt, h, b->n, b->s, b->q, b->name);
			fmc_batch_destroy(b);
		}
		kseq_destroy(ks);
		gzclose(fp);
		for (i = 0; i < 1<<opt.suf_len*2; ++i)
			kh_destroy(fmc, h[i]);
		free(h);
	}
	return 0;
}