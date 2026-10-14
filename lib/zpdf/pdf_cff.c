/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "pdf_cff.h"
#include "pdf_mem.h"
#include <string.h>

#define NSTD_STRINGS 391

static const char *const std_strings[NSTD_STRINGS] = {
	".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand",
	"quoteright", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period",
	"slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
	"colon", "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E",
	"F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
	"Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore",
	"quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p",
	"q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright",
	"asciitilde", "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section",
	"currency", "quotesingle", "quotedblleft", "guillemotleft", "guilsinglleft",
	"guilsinglright", "fi", "fl", "endash", "dagger", "daggerdbl", "periodcentered",
	"paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright",
	"ellipsis", "perthousand", "questiondown", "grave", "acute", "circumflex", "tilde",
	"macron", "breve", "dotaccent", "dieresis", "ring", "cedilla", "hungarumlaut", "ogonek",
	"caron", "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE", "ordmasculine", "ae",
	"dotlessi", "lslash", "oslash", "oe", "germandbls", "onesuperior", "logicalnot", "mu",
	"trademark", "Eth", "onehalf", "plusminus", "Thorn", "onequarter", "divide", "brokenbar",
	"degree", "thorn", "threequarters", "twosuperior", "registered", "minus", "eth",
	"multiply", "threesuperior", "copyright", "Aacute", "Acircumflex", "Adieresis", "Agrave",
	"Aring", "Atilde", "Ccedilla", "Eacute", "Ecircumflex", "Edieresis", "Egrave", "Iacute",
	"Icircumflex", "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex", "Odieresis",
	"Ograve", "Otilde", "Scaron", "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute",
	"Ydieresis", "Zcaron", "aacute", "acircumflex", "adieresis", "agrave", "aring", "atilde",
	"ccedilla", "eacute", "ecircumflex", "edieresis", "egrave", "iacute", "icircumflex",
	"idieresis", "igrave", "ntilde", "oacute", "ocircumflex", "odieresis", "ograve", "otilde",
	"scaron", "uacute", "ucircumflex", "udieresis", "ugrave", "yacute", "ydieresis", "zcaron",
	"exclamsmall", "Hungarumlautsmall", "dollaroldstyle", "dollarsuperior", "ampersandsmall",
	"Acutesmall", "parenleftsuperior", "parenrightsuperior", "twodotenleader",
	"onedotenleader", "zerooldstyle", "oneoldstyle", "twooldstyle", "threeoldstyle",
	"fouroldstyle", "fiveoldstyle", "sixoldstyle", "sevenoldstyle", "eightoldstyle",
	"nineoldstyle", "commasuperior", "threequartersemdash", "periodsuperior", "questionsmall",
	"asuperior", "bsuperior", "centsuperior", "dsuperior", "esuperior", "isuperior",
	"lsuperior", "msuperior", "nsuperior", "osuperior", "rsuperior", "ssuperior", "tsuperior",
	"ff", "ffi", "ffl", "parenleftinferior", "parenrightinferior", "Circumflexsmall",
	"hyphensuperior", "Gravesmall", "Asmall", "Bsmall", "Csmall", "Dsmall", "Esmall", "Fsmall",
	"Gsmall", "Hsmall", "Ismall", "Jsmall", "Ksmall", "Lsmall", "Msmall", "Nsmall", "Osmall",
	"Psmall", "Qsmall", "Rsmall", "Ssmall", "Tsmall", "Usmall", "Vsmall", "Wsmall", "Xsmall",
	"Ysmall", "Zsmall", "colonmonetary", "onefitted", "rupiah", "Tildesmall",
	"exclamdownsmall", "centoldstyle", "Lslashsmall", "Scaronsmall", "Zcaronsmall",
	"Dieresissmall", "Brevesmall", "Caronsmall", "Dotaccentsmall", "Macronsmall",
	"figuredash", "hypheninferior", "Ogoneksmall", "Ringsmall", "Cedillasmall",
	"questiondownsmall", "oneeighth", "threeeighths", "fiveeighths", "seveneighths",
	"onethird", "twothirds", "zerosuperior", "foursuperior", "fivesuperior", "sixsuperior",
	"sevensuperior", "eightsuperior", "ninesuperior", "zeroinferior", "oneinferior",
	"twoinferior", "threeinferior", "fourinferior", "fiveinferior", "sixinferior",
	"seveninferior", "eightinferior", "nineinferior", "centinferior", "dollarinferior",
	"periodinferior", "commainferior", "Agravesmall", "Aacutesmall", "Acircumflexsmall",
	"Atildesmall", "Adieresissmall", "Aringsmall", "AEsmall", "Ccedillasmall", "Egravesmall",
	"Eacutesmall", "Ecircumflexsmall", "Edieresissmall", "Igravesmall", "Iacutesmall",
	"Icircumflexsmall", "Idieresissmall", "Ethsmall", "Ntildesmall", "Ogravesmall",
	"Oacutesmall", "Ocircumflexsmall", "Otildesmall", "Odieresissmall", "OEsmall",
	"Oslashsmall", "Ugravesmall", "Uacutesmall", "Ucircumflexsmall", "Udieresissmall",
	"Yacutesmall", "Thornsmall", "Ydieresissmall", "001.000", "001.001", "001.002", "001.003",
	"Black", "Bold", "Book", "Light", "Medium", "Regular", "Roman", "Semibold"};

/* StandardEncoding: code -> SID for the codes above 126 (32..126 map to code - 31). */
static const uint8_t std_enc_high[][2] = {
	{161, 96}, {162, 97}, {163, 98}, {164, 99}, {165, 100}, {166, 101}, {167, 102},
	{168, 103}, {169, 104}, {170, 105}, {171, 106}, {172, 107}, {173, 108}, {174, 109},
	{175, 110}, {177, 111}, {178, 112}, {179, 113}, {180, 114}, {182, 115}, {183, 116},
	{184, 117}, {185, 118}, {186, 119}, {187, 120}, {188, 121}, {189, 122}, {191, 123},
	{193, 124}, {194, 125}, {195, 126}, {196, 127}, {197, 128}, {198, 129}, {199, 130},
	{200, 131}, {202, 132}, {203, 133}, {205, 134}, {206, 135}, {207, 136}, {208, 137},
	{225, 138}, {227, 139}, {232, 140}, {233, 141}, {234, 142}, {235, 143}, {241, 144},
	{245, 145}, {248, 146}, {249, 147}, {250, 148}, {251, 149}};

struct index {
	uint32_t count;
	uint8_t offsize;
	const uint8_t *offsets; /* count + 1 entries */
	const uint8_t *data;    /* base for offsets (offset 1 == data) */
	size_t end;             /* absolute end of the index in the file */
};

struct priv {
	struct index subrs;
	bool has_subrs;
};

struct cff_font {
	const uint8_t *d;
	size_t len;
	struct index charstrings, gsubrs, strings;
	struct priv priv;
	bool is_cid;
	uint16_t nglyphs;
	float fm[6];
	/* charset: gid -> SID (or CID) */
	uint16_t *charset;
	/* encoding: code -> gid (0 = none), only for non-CID fonts */
	uint16_t *encoding;
	bool std_encoding;
	/* CID: FDSelect gid -> fd, FDArray privates */
	uint8_t *fdselect;
	struct priv *fdprivs;
	int nfd;
};

static uint32_t rdn(const uint8_t *p, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}

static int read_index(const struct cff_font *f, size_t pos, struct index *ix)
{
	memset(ix, 0, sizeof(*ix));
	if (pos + 2 > f->len) {
		return -1;
	}
	ix->count = (uint32_t)rdn(f->d + pos, 2);
	if (ix->count == 0) {
		ix->end = pos + 2;
		return 0;
	}
	if (pos + 3 > f->len) {
		return -1;
	}
	ix->offsize = f->d[pos + 2];
	if (ix->offsize < 1 || ix->offsize > 4) {
		return -1;
	}
	ix->offsets = f->d + pos + 3;
	size_t off_bytes = (size_t)(ix->count + 1) * ix->offsize;

	if (pos + 3 + off_bytes > f->len) {
		return -1;
	}
	ix->data = ix->offsets + off_bytes - 1;
	uint32_t last = rdn(ix->offsets + (size_t)ix->count * ix->offsize, ix->offsize);

	ix->end = (size_t)(ix->data - f->d) + last;
	if (ix->end > f->len) {
		return -1;
	}
	return 0;
}

static const uint8_t *index_get(const struct index *ix, uint32_t i, size_t *len)
{
	if (i >= ix->count) {
		*len = 0;
		return NULL;
	}
	uint32_t a = rdn(ix->offsets + (size_t)i * ix->offsize, ix->offsize);
	uint32_t b = rdn(ix->offsets + (size_t)(i + 1) * ix->offsize, ix->offsize);

	if (b < a) {
		*len = 0;
		return NULL;
	}
	*len = b - a;
	return ix->data + a;
}

/* DICT parsing: operands are collected until an operator; we keep what we need. */
struct dict_vals {
	int32_t charstrings, charset, encoding, private_size, private_off, fdarray, fdselect, subrs;
	bool has_ros;
	float fm[6];
	bool has_fm;
};

static float dict_real(const uint8_t *p, size_t n, size_t *used)
{
	/* nibble encoded real */
	char buf[32];
	size_t bi = 0, i = 1;
	bool done = false;

	while (i < n && !done) {
		uint8_t b = p[i++];

		for (int k = 0; k < 2 && !done; k++) {
			uint8_t nib = k == 0 ? b >> 4 : b & 15;

			if (nib <= 9) {
				if (bi < sizeof(buf) - 1) {
					buf[bi++] = (char)('0' + nib);
				}
			} else if (nib == 0xa) {
				if (bi < sizeof(buf) - 1) {
					buf[bi++] = '.';
				}
			} else if (nib == 0xb) {
				if (bi < sizeof(buf) - 1) {
					buf[bi++] = 'E';
				}
			} else if (nib == 0xc) {
				if (bi < sizeof(buf) - 2) {
					buf[bi++] = 'E';
					buf[bi++] = '-';
				}
			} else if (nib == 0xe) {
				if (bi < sizeof(buf) - 1) {
					buf[bi++] = '-';
				}
			} else if (nib == 0xf) {
				done = true;
			}
		}
	}
	buf[bi] = 0;
	*used = i;
	/* tiny strtod replacement */
	float v = 0, frac = 0, scale = 1;
	int exp = 0, expsign = 1;
	bool neg = false, in_frac = false, in_exp = false;

	for (size_t k = 0; k < bi; k++) {
		char c = buf[k];

		if (c == '-') {
			if (in_exp) {
				expsign = -1;
			} else {
				neg = true;
			}
		} else if (c == '.') {
			in_frac = true;
		} else if (c == 'E') {
			in_exp = true;
		} else if (c >= '0' && c <= '9') {
			if (in_exp) {
				exp = exp * 10 + (c - '0');
			} else if (in_frac) {
				scale *= 0.1f;
				frac += (c - '0') * scale;
			} else {
				v = v * 10 + (c - '0');
			}
		}
	}
	v += frac;
	while (exp-- > 0) {
		v = expsign > 0 ? v * 10 : v * 0.1f;
	}
	return neg ? -v : v;
}

static void parse_dict(const uint8_t *p, size_t n, struct dict_vals *dv)
{
	float ops[48];
	int nops = 0;
	size_t i = 0;

	while (i < n) {
		uint8_t b = p[i];

		if (b <= 21) {
			int op = b;

			i++;
			if (b == 12 && i < n) {
				op = 1200 + p[i++];
			}
			switch (op) {
			case 17:
				dv->charstrings = nops ? (int32_t)ops[0] : 0;
				break;
			case 15:
				dv->charset = nops ? (int32_t)ops[0] : 0;
				break;
			case 16:
				dv->encoding = nops ? (int32_t)ops[0] : 0;
				break;
			case 18:
				if (nops >= 2) {
					dv->private_size = (int32_t)ops[0];
					dv->private_off = (int32_t)ops[1];
				}
				break;
			case 19:
				dv->subrs = nops ? (int32_t)ops[0] : 0;
				break;
			case 1207:
				if (nops >= 6) {
					for (int k = 0; k < 6; k++) {
						dv->fm[k] = ops[k];
					}
					dv->has_fm = true;
				}
				break;
			case 1230:
				dv->has_ros = true;
				break;
			case 1236:
				dv->fdarray = nops ? (int32_t)ops[0] : 0;
				break;
			case 1237:
				dv->fdselect = nops ? (int32_t)ops[0] : 0;
				break;
			default:
				break;
			}
			nops = 0;
		} else if (b == 28) {
			if (i + 3 > n) {
				break;
			}
			if (nops < 48) {
				ops[nops++] = (float)(int16_t)((p[i + 1] << 8) | p[i + 2]);
			}
			i += 3;
		} else if (b == 29) {
			if (i + 5 > n) {
				break;
			}
			if (nops < 48) {
				ops[nops++] = (float)(int32_t)rdn(p + i + 1, 4);
			}
			i += 5;
		} else if (b == 30) {
			size_t used;
			float v = dict_real(p + i, n - i, &used);

			if (nops < 48) {
				ops[nops++] = v;
			}
			i += used;
		} else if (b >= 32 && b <= 246) {
			if (nops < 48) {
				ops[nops++] = (float)((int)b - 139);
			}
			i++;
		} else if (b >= 247 && b <= 250) {
			if (i + 2 > n) {
				break;
			}
			if (nops < 48) {
				ops[nops++] = (float)(((int)b - 247) * 256 + p[i + 1] + 108);
			}
			i += 2;
		} else if (b >= 251 && b <= 254) {
			if (i + 2 > n) {
				break;
			}
			if (nops < 48) {
				ops[nops++] = (float)(-((int)b - 251) * 256 - p[i + 1] - 108);
			}
			i += 2;
		} else {
			i++;
		}
	}
}

static void load_private(struct cff_font *f, int32_t off, int32_t size, struct priv *pv)
{
	memset(pv, 0, sizeof(*pv));
	if (off <= 0 || size < 0 || (size_t)off + (size_t)size > f->len) {
		return;
	}
	struct dict_vals dv = {0};

	parse_dict(f->d + off, (size_t)size, &dv);
	if (dv.subrs > 0 && read_index(f, (size_t)off + (size_t)dv.subrs, &pv->subrs) == 0) {
		pv->has_subrs = true;
	}
}

static int load_charset(struct cff_font *f, int32_t off)
{
	f->charset = pdf_calloc(sizeof(uint16_t) * (size_t)f->nglyphs);
	if (f->charset == NULL) {
		return -1;
	}
	if (off == 0) {
		/* ISOAdobe: identity SIDs */
		for (uint16_t g = 0; g < f->nglyphs; g++) {
			f->charset[g] = g;
		}
		return 0;
	}
	if (off == 1 || off == 2 || (size_t)off >= f->len) {
		/* expert charsets: not supported, identity */
		for (uint16_t g = 0; g < f->nglyphs; g++) {
			f->charset[g] = g;
		}
		return 0;
	}
	const uint8_t *p = f->d + off;
	size_t n = f->len - (size_t)off;
	uint8_t fmt = p[0];
	size_t i = 1;
	uint16_t gid = 1;

	f->charset[0] = 0;
	if (fmt == 0) {
		while (gid < f->nglyphs && i + 2 <= n) {
			f->charset[gid++] = (uint16_t)rdn(p + i, 2);
			i += 2;
		}
	} else if (fmt == 1 || fmt == 2) {
		int nl = fmt == 1 ? 1 : 2;

		while (gid < f->nglyphs && i + 2 + (size_t)nl <= n) {
			uint16_t first = (uint16_t)rdn(p + i, 2);
			uint32_t nleft = rdn(p + i + 2, nl);

			i += 2 + (size_t)nl;
			for (uint32_t k = 0; k <= nleft && gid < f->nglyphs; k++) {
				f->charset[gid++] = (uint16_t)(first + k);
			}
		}
	}
	return 0;
}

static uint16_t gid_for_sid(const struct cff_font *f, uint16_t sid)
{
	for (uint16_t g = 0; g < f->nglyphs; g++) {
		if (f->charset[g] == sid) {
			return g;
		}
	}
	return 0;
}

static int load_encoding(struct cff_font *f, int32_t off)
{
	f->encoding = pdf_calloc(sizeof(uint16_t) * 256);
	if (f->encoding == NULL) {
		return -1;
	}
	if (off == 0 || off == 1) {
		/* standard (or expert, treated as standard) encoding by names */
		f->std_encoding = true;
		for (int c = 32; c <= 126; c++) {
			f->encoding[c] = gid_for_sid(f, (uint16_t)(c - 31));
		}
		for (size_t k = 0; k < sizeof(std_enc_high) / sizeof(std_enc_high[0]); k++) {
			f->encoding[std_enc_high[k][0]] = gid_for_sid(f, std_enc_high[k][1]);
		}
		return 0;
	}
	if ((size_t)off >= f->len) {
		return 0;
	}
	const uint8_t *p = f->d + off;
	size_t n = f->len - (size_t)off;
	uint8_t fmt = p[0];
	size_t i = 1;

	if ((fmt & 0x7f) == 0) {
		uint8_t ncodes = i < n ? p[i++] : 0;

		for (uint16_t gid = 1; gid <= ncodes && i < n; gid++) {
			f->encoding[p[i++]] = gid;
		}
	} else if ((fmt & 0x7f) == 1) {
		uint8_t nranges = i < n ? p[i++] : 0;
		uint16_t gid = 1;

		for (uint8_t r = 0; r < nranges && i + 2 <= n; r++) {
			uint8_t first = p[i], nleft = p[i + 1];

			i += 2;
			for (int k = 0; k <= nleft; k++) {
				int c = first + k;

				if (c < 256 && gid < f->nglyphs) {
					f->encoding[c] = gid++;
				}
			}
		}
	}
	if (fmt & 0x80) {
		uint8_t nsups = i < n ? p[i++] : 0;

		for (uint8_t s = 0; s < nsups && i + 3 <= n; s++) {
			uint8_t code = p[i];
			uint16_t sid = (uint16_t)rdn(p + i + 1, 2);

			f->encoding[code] = gid_for_sid(f, sid);
			i += 3;
		}
	}
	return 0;
}

static int load_fdselect(struct cff_font *f, int32_t off)
{
	f->fdselect = pdf_calloc((size_t)f->nglyphs);
	if (f->fdselect == NULL) {
		return -1;
	}
	if (off <= 0 || (size_t)off >= f->len) {
		return 0;
	}
	const uint8_t *p = f->d + off;
	size_t n = f->len - (size_t)off;

	if (p[0] == 0) {
		for (uint16_t g = 0; g < f->nglyphs && (size_t)g + 1 < n; g++) {
			f->fdselect[g] = p[1 + g];
		}
	} else if (p[0] == 3) {
		if (n < 3) {
			return 0;
		}
		uint16_t nr = (uint16_t)rdn(p + 1, 2);
		size_t i = 3;

		for (uint16_t r = 0; r < nr && i + 5 <= n; r++) {
			uint16_t first = (uint16_t)rdn(p + i, 2);
			uint8_t fd = p[i + 2];
			uint16_t next = (uint16_t)rdn(p + i + 3, 2);

			for (uint32_t g = first; g < next && g < f->nglyphs; g++) {
				f->fdselect[g] = fd;
			}
			i += 3;
		}
	}
	return 0;
}

struct cff_font *cff_open(const uint8_t *data, size_t len)
{
	struct cff_font *f = pdf_calloc(sizeof(*f));

	if (f == NULL) {
		return NULL;
	}
	f->d = data;
	f->len = len;
	f->fm[0] = 0.001f;
	f->fm[3] = 0.001f;

	/* OpenType wrapper: find the CFF table */
	if (len > 12 && memcmp(data, "OTTO", 4) == 0) {
		uint16_t ntab = (uint16_t)rdn(data + 4, 2);

		for (uint16_t i = 0; i < ntab; i++) {
			size_t rec = 12 + (size_t)i * 16;

			if (rec + 16 > len) {
				break;
			}
			if (memcmp(data + rec, "CFF ", 4) == 0) {
				uint32_t off = rdn(data + rec + 8, 4), tl = rdn(data + rec + 12, 4);

				if (off < len) {
					f->d = data + off;
					f->len = tl < len - off ? tl : len - off;
				}
				break;
			}
		}
	}
	if (f->len < 4) {
		goto fail;
	}
	uint8_t hdrsize = f->d[2];
	struct index names, topdicts;

	if (read_index(f, hdrsize, &names) < 0 || read_index(f, names.end, &topdicts) < 0 ||
	    read_index(f, topdicts.end, &f->strings) < 0 || read_index(f, f->strings.end, &f->gsubrs) < 0) {
		goto fail;
	}
	size_t tl;
	const uint8_t *td = index_get(&topdicts, 0, &tl);
	struct dict_vals dv = {0};

	if (td == NULL) {
		goto fail;
	}
	parse_dict(td, tl, &dv);
	if (dv.has_fm) {
		memcpy(f->fm, dv.fm, sizeof(f->fm));
	}
	f->is_cid = dv.has_ros;
	if (dv.charstrings <= 0 || read_index(f, (size_t)dv.charstrings, &f->charstrings) < 0) {
		goto fail;
	}
	f->nglyphs = (uint16_t)(f->charstrings.count > 65535 ? 65535 : f->charstrings.count);
	load_private(f, dv.private_off, dv.private_size, &f->priv);
	if (load_charset(f, dv.charset) < 0) {
		goto fail;
	}
	if (f->is_cid) {
		struct index fdarray;

		if (dv.fdarray > 0 && read_index(f, (size_t)dv.fdarray, &fdarray) == 0 && fdarray.count > 0) {
			f->nfd = (int)(fdarray.count > 256 ? 256 : fdarray.count);
			f->fdprivs = pdf_calloc(sizeof(*f->fdprivs) * (size_t)f->nfd);
			if (f->fdprivs == NULL) {
				goto fail;
			}
			for (int i = 0; i < f->nfd; i++) {
				size_t fl;
				const uint8_t *fd = index_get(&fdarray, (uint32_t)i, &fl);
				struct dict_vals fdv = {0};

				if (fd != NULL) {
					parse_dict(fd, fl, &fdv);
					load_private(f, fdv.private_off, fdv.private_size, &f->fdprivs[i]);
				}
			}
		}
		if (load_fdselect(f, dv.fdselect) < 0) {
			goto fail;
		}
	} else {
		if (load_encoding(f, dv.encoding) < 0) {
			goto fail;
		}
	}
	return f;
fail:
	cff_free(f);
	return NULL;
}

void cff_free(struct cff_font *f)
{
	if (f == NULL) {
		return;
	}
	pdf_free(f->charset);
	pdf_free(f->encoding);
	pdf_free(f->fdselect);
	pdf_free(f->fdprivs);
	pdf_free(f);
}

bool cff_is_cid(const struct cff_font *f)
{
	return f->is_cid;
}

uint16_t cff_nglyphs(const struct cff_font *f)
{
	return f->nglyphs;
}

void cff_font_matrix(const struct cff_font *f, float m[6])
{
	memcpy(m, f->fm, sizeof(f->fm));
}

uint16_t cff_gid_for_cid(const struct cff_font *f, uint32_t cid)
{
	if (!f->is_cid) {
		return (uint16_t)(cid < f->nglyphs ? cid : 0);
	}
	for (uint16_t g = 0; g < f->nglyphs; g++) {
		if (f->charset[g] == cid) {
			return g;
		}
	}
	return 0;
}

static int sid_for_name(const struct cff_font *f, const char *name)
{
	for (int i = 0; i < NSTD_STRINGS; i++) {
		if (strcmp(std_strings[i], name) == 0) {
			return i;
		}
	}
	size_t nl = strlen(name);

	for (uint32_t i = 0; i < f->strings.count; i++) {
		size_t sl;
		const uint8_t *s = index_get(&f->strings, i, &sl);

		if (s != NULL && sl == nl && memcmp(s, name, nl) == 0) {
			return NSTD_STRINGS + (int)i;
		}
	}
	return -1;
}

uint16_t cff_gid_for_name(const struct cff_font *f, const char *name)
{
	if (f->is_cid) {
		return 0;
	}
	int sid = sid_for_name(f, name);

	if (sid < 0) {
		return 0;
	}
	return gid_for_sid(f, (uint16_t)sid);
}

uint16_t cff_gid_for_code(const struct cff_font *f, uint8_t code)
{
	if (f->encoding == NULL) {
		return 0;
	}
	return f->encoding[code];
}

/* ---------------------------------------------------------------------- */
/* Type 2 charstrings                                                     */

struct t2ctx {
	const struct cff_font *f;
	const struct cff_outline_cb *cb;
	void *ud;
	const float *m;
	float st[48];
	int sp;
	float x, y;
	int nstems;
	bool width_parsed;
	bool open;
	int depth;
	const struct priv *pv;
	float trans[32];
};

static void t2_xf(struct t2ctx *c, float x, float y, float *ox, float *oy)
{
	*ox = c->m[0] * x + c->m[2] * y + c->m[4];
	*oy = c->m[1] * x + c->m[3] * y + c->m[5];
}

static void t2_moveto(struct t2ctx *c, float x, float y)
{
	float ox, oy;

	if (c->open) {
		c->cb->close(c->ud);
	}
	t2_xf(c, x, y, &ox, &oy);
	c->cb->move(c->ud, ox, oy);
	c->open = true;
}

static void t2_lineto(struct t2ctx *c, float x, float y)
{
	float ox, oy;

	if (!c->open) {
		t2_moveto(c, c->x, c->y);
	}
	t2_xf(c, x, y, &ox, &oy);
	c->cb->line(c->ud, ox, oy);
}

static void t2_curveto(struct t2ctx *c, float x1, float y1, float x2, float y2, float x3, float y3)
{
	float a, b, cc, d, e, g;

	if (!c->open) {
		t2_moveto(c, c->x, c->y);
	}
	t2_xf(c, x1, y1, &a, &b);
	t2_xf(c, x2, y2, &cc, &d);
	t2_xf(c, x3, y3, &e, &g);
	c->cb->cubic(c->ud, a, b, cc, d, e, g);
}

static int bias(uint32_t count)
{
	return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768);
}

static void run_charstring(struct t2ctx *c, const uint8_t *p, size_t n);

static void take_width(struct t2ctx *c, int even)
{
	/* the first stack-clearing operator may carry the advance width as an extra argument */
	if (!c->width_parsed) {
		c->width_parsed = true;
		if ((c->sp % 2) == 1 && even == 0) {
			memmove(c->st, c->st + 1, (size_t)(c->sp - 1) * sizeof(float));
			c->sp--;
		} else if (even > 0 && c->sp > even) {
			memmove(c->st, c->st + 1, (size_t)(c->sp - 1) * sizeof(float));
			c->sp--;
		}
	}
}

static void run_charstring(struct t2ctx *c, const uint8_t *p, size_t n)
{
	size_t i = 0;

	if (c->depth > 10) {
		return;
	}
	while (i < n) {
		uint8_t b = p[i++];

		if (b >= 32 || b == 28) {
			float v;

			if (b == 28) {
				if (i + 2 > n) {
					return;
				}
				v = (float)(int16_t)((p[i] << 8) | p[i + 1]);
				i += 2;
			} else if (b <= 246) {
				v = (float)((int)b - 139);
			} else if (b <= 250) {
				if (i >= n) {
					return;
				}
				v = (float)(((int)b - 247) * 256 + p[i++] + 108);
			} else if (b <= 254) {
				if (i >= n) {
					return;
				}
				v = (float)(-((int)b - 251) * 256 - p[i++] - 108);
			} else {
				if (i + 4 > n) {
					return;
				}
				v = (float)(int32_t)rdn(p + i, 4) / 65536.0f;
				i += 4;
			}
			if (c->sp < 48) {
				c->st[c->sp++] = v;
			}
			continue;
		}
		switch (b) {
		case 1:  /* hstem */
		case 3:  /* vstem */
		case 18: /* hstemhm */
		case 23: /* vstemhm */
			if (!c->width_parsed && (c->sp % 2) == 1) {
				c->sp--;
				memmove(c->st, c->st + 1, (size_t)c->sp * sizeof(float));
			}
			c->width_parsed = true;
			c->nstems += c->sp / 2;
			c->sp = 0;
			break;
		case 19: /* hintmask */
		case 20: /* cntrmask */
			if (!c->width_parsed && (c->sp % 2) == 1) {
				c->sp--;
			}
			c->width_parsed = true;
			c->nstems += c->sp / 2;
			c->sp = 0;
			i += (size_t)(c->nstems + 7) / 8;
			break;
		case 21: /* rmoveto */
			take_width(c, 2);
			if (c->sp >= 2) {
				c->x += c->st[c->sp - 2];
				c->y += c->st[c->sp - 1];
			}
			t2_moveto(c, c->x, c->y);
			c->sp = 0;
			break;
		case 22: /* hmoveto */
			take_width(c, 1);
			if (c->sp >= 1) {
				c->x += c->st[c->sp - 1];
			}
			t2_moveto(c, c->x, c->y);
			c->sp = 0;
			break;
		case 4: /* vmoveto */
			take_width(c, 1);
			if (c->sp >= 1) {
				c->y += c->st[c->sp - 1];
			}
			t2_moveto(c, c->x, c->y);
			c->sp = 0;
			break;
		case 5: /* rlineto */
			for (int k = 0; k + 1 < c->sp; k += 2) {
				c->x += c->st[k];
				c->y += c->st[k + 1];
				t2_lineto(c, c->x, c->y);
			}
			c->sp = 0;
			break;
		case 6: /* hlineto */
		case 7: /* vlineto */ {
			bool horiz = (b == 6);

			for (int k = 0; k < c->sp; k++) {
				if (horiz) {
					c->x += c->st[k];
				} else {
					c->y += c->st[k];
				}
				t2_lineto(c, c->x, c->y);
				horiz = !horiz;
			}
			c->sp = 0;
			break;
		}
		case 8: /* rrcurveto */
			for (int k = 0; k + 5 < c->sp; k += 6) {
				float x1 = c->x + c->st[k], y1 = c->y + c->st[k + 1];
				float x2 = x1 + c->st[k + 2], y2 = y1 + c->st[k + 3];

				c->x = x2 + c->st[k + 4];
				c->y = y2 + c->st[k + 5];
				t2_curveto(c, x1, y1, x2, y2, c->x, c->y);
			}
			c->sp = 0;
			break;
		case 24: /* rcurveline */ {
			int k = 0;

			for (; k + 5 < c->sp - 2; k += 6) {
				float x1 = c->x + c->st[k], y1 = c->y + c->st[k + 1];
				float x2 = x1 + c->st[k + 2], y2 = y1 + c->st[k + 3];

				c->x = x2 + c->st[k + 4];
				c->y = y2 + c->st[k + 5];
				t2_curveto(c, x1, y1, x2, y2, c->x, c->y);
			}
			if (k + 1 < c->sp) {
				c->x += c->st[k];
				c->y += c->st[k + 1];
				t2_lineto(c, c->x, c->y);
			}
			c->sp = 0;
			break;
		}
		case 25: /* rlinecurve */ {
			int k = 0;

			for (; k + 1 < c->sp - 6; k += 2) {
				c->x += c->st[k];
				c->y += c->st[k + 1];
				t2_lineto(c, c->x, c->y);
			}
			if (k + 5 < c->sp) {
				float x1 = c->x + c->st[k], y1 = c->y + c->st[k + 1];
				float x2 = x1 + c->st[k + 2], y2 = y1 + c->st[k + 3];

				c->x = x2 + c->st[k + 4];
				c->y = y2 + c->st[k + 5];
				t2_curveto(c, x1, y1, x2, y2, c->x, c->y);
			}
			c->sp = 0;
			break;
		}
		case 26: /* vvcurveto */
		case 27: /* hhcurveto */ {
			int k = 0;
			float d1 = 0;

			if (c->sp % 4 == 1) {
				d1 = c->st[0];
				k = 1;
			}
			for (; k + 3 < c->sp; k += 4) {
				float x1, y1;

				if (b == 26) {
					x1 = c->x + d1;
					y1 = c->y + c->st[k];
				} else {
					x1 = c->x + c->st[k];
					y1 = c->y + d1;
				}
				float x2 = x1 + c->st[k + 1], y2 = y1 + c->st[k + 2];

				if (b == 26) {
					c->x = x2;
					c->y = y2 + c->st[k + 3];
				} else {
					c->x = x2 + c->st[k + 3];
					c->y = y2;
				}
				t2_curveto(c, x1, y1, x2, y2, c->x, c->y);
				d1 = 0;
			}
			c->sp = 0;
			break;
		}
		case 30: /* vhcurveto */
		case 31: /* hvcurveto */ {
			bool horiz = (b == 31);
			int k = 0;

			while (k + 3 < c->sp) {
				float x1, y1, x2, y2;
				bool last = (k + 8 > c->sp);
				float lastarg = (last && k + 4 < c->sp) ? c->st[k + 4] : 0;

				if (horiz) {
					x1 = c->x + c->st[k];
					y1 = c->y;
					x2 = x1 + c->st[k + 1];
					y2 = y1 + c->st[k + 2];
					c->y = y2 + c->st[k + 3];
					c->x = x2 + lastarg;
				} else {
					x1 = c->x;
					y1 = c->y + c->st[k];
					x2 = x1 + c->st[k + 1];
					y2 = y1 + c->st[k + 2];
					c->x = x2 + c->st[k + 3];
					c->y = y2 + lastarg;
				}
				t2_curveto(c, x1, y1, x2, y2, c->x, c->y);
				horiz = !horiz;
				k += 4;
			}
			c->sp = 0;
			break;
		}
		case 10: /* callsubr */
		case 29: /* callgsubr */ {
			if (c->sp < 1) {
				break;
			}
			int idx = (int)c->st[--c->sp];
			const struct index *ix = (b == 10) ? (c->pv && c->pv->has_subrs ? &c->pv->subrs : NULL)
							   : &c->f->gsubrs;

			if (ix == NULL) {
				break;
			}
			idx += bias(ix->count);
			if (idx < 0 || (uint32_t)idx >= ix->count) {
				break;
			}
			size_t sl;
			const uint8_t *s = index_get(ix, (uint32_t)idx, &sl);

			if (s != NULL) {
				c->depth++;
				run_charstring(c, s, sl);
				c->depth--;
			}
			break;
		}
		case 11: /* return */
			return;
		case 14: /* endchar */
			if (!c->width_parsed && (c->sp == 1 || c->sp == 5)) {
				c->sp--;
				memmove(c->st, c->st + 1, (size_t)c->sp * sizeof(float));
			}
			c->width_parsed = true;
			if (c->sp >= 4) {
				/* seac-like accent composition: adx ady bchar achar */
				float adx = c->st[0], ady = c->st[1];
				int bchar = (int)c->st[2], achar = (int)c->st[3];

				c->sp = 0;
				if (c->open) {
					c->cb->close(c->ud);
					c->open = false;
				}
				if (c->depth < 5 && bchar >= 0 && bchar < 256 && achar >= 0 && achar < 256) {
					uint16_t bg = 0, ag = 0;

					/* standard encoding codes */
					if (bchar >= 32 && bchar <= 126) {
						bg = gid_for_sid(c->f, (uint16_t)(bchar - 31));
					}
					if (achar >= 32 && achar <= 126) {
						ag = gid_for_sid(c->f, (uint16_t)(achar - 31));
					}
					for (size_t k = 0; k < sizeof(std_enc_high) / sizeof(std_enc_high[0]); k++) {
						if (std_enc_high[k][0] == bchar) {
							bg = gid_for_sid(c->f, std_enc_high[k][1]);
						}
						if (std_enc_high[k][0] == achar) {
							ag = gid_for_sid(c->f, std_enc_high[k][1]);
						}
					}
					float m2[6];

					memcpy(m2, c->m, sizeof(m2));
					cff_glyph_outline(c->f, bg, m2, c->cb, c->ud);
					m2[4] = c->m[0] * adx + c->m[2] * ady + c->m[4];
					m2[5] = c->m[1] * adx + c->m[3] * ady + c->m[5];
					cff_glyph_outline(c->f, ag, m2, c->cb, c->ud);
				}
				return;
			}
			if (c->open) {
				c->cb->close(c->ud);
				c->open = false;
			}
			return;
		case 12: {
			if (i >= n) {
				return;
			}
			uint8_t b2 = p[i++];

			switch (b2) {
			case 35: /* flex */
				if (c->sp >= 13) {
					float x1 = c->x + c->st[0], y1 = c->y + c->st[1];
					float x2 = x1 + c->st[2], y2 = y1 + c->st[3];
					float jx = x2 + c->st[4], jy = y2 + c->st[5];

					t2_curveto(c, x1, y1, x2, y2, jx, jy);
					float x3 = jx + c->st[6], y3 = jy + c->st[7];
					float x4 = x3 + c->st[8], y4 = y3 + c->st[9];

					c->x = x4 + c->st[10];
					c->y = y4 + c->st[11];
					t2_curveto(c, x3, y3, x4, y4, c->x, c->y);
				}
				c->sp = 0;
				break;
			case 34: /* hflex */
				if (c->sp >= 7) {
					float y0 = c->y;
					float x1 = c->x + c->st[0], y1 = c->y;
					float x2 = x1 + c->st[1], y2 = y1 + c->st[2];
					float jx = x2 + c->st[3], jy = y2;

					t2_curveto(c, x1, y1, x2, y2, jx, jy);
					float x3 = jx + c->st[4], y3 = y2;
					float x4 = x3 + c->st[5], y4 = y0;

					c->x = x4 + c->st[6];
					c->y = y0;
					t2_curveto(c, x3, y3, x4, y4, c->x, c->y);
				}
				c->sp = 0;
				break;
			case 36: /* hflex1 */
				if (c->sp >= 9) {
					float y0 = c->y;
					float x1 = c->x + c->st[0], y1 = c->y + c->st[1];
					float x2 = x1 + c->st[2], y2 = y1 + c->st[3];
					float jx = x2 + c->st[4], jy = y2;

					t2_curveto(c, x1, y1, x2, y2, jx, jy);
					float x3 = jx + c->st[5], y3 = y2;
					float x4 = x3 + c->st[6], y4 = y3 + c->st[7];

					c->x = x4 + c->st[8];
					c->y = y0;
					t2_curveto(c, x3, y3, x4, y4, c->x, c->y);
				}
				c->sp = 0;
				break;
			case 37: /* flex1 */
				if (c->sp >= 11) {
					float sx = c->x, sy = c->y;
					float dx = 0, dy = 0;

					for (int k = 0; k < 10; k += 2) {
						dx += c->st[k];
						dy += c->st[k + 1];
					}
					float x1 = c->x + c->st[0], y1 = c->y + c->st[1];
					float x2 = x1 + c->st[2], y2 = y1 + c->st[3];
					float jx = x2 + c->st[4], jy = y2 + c->st[5];

					t2_curveto(c, x1, y1, x2, y2, jx, jy);
					float x3 = jx + c->st[6], y3 = jy + c->st[7];
					float x4 = x3 + c->st[8], y4 = y3 + c->st[9];

					if (dx < 0) {
						dx = -dx;
					}
					if (dy < 0) {
						dy = -dy;
					}
					if (dx > dy) {
						c->x = x4 + c->st[10];
						c->y = sy;
					} else {
						c->x = sx;
						c->y = y4 + c->st[10];
					}
					t2_curveto(c, x3, y3, x4, y4, c->x, c->y);
				}
				c->sp = 0;
				break;
			default:
				/* arithmetic and storage operators: rarely used, clear the stack */
				c->sp = 0;
				break;
			}
			break;
		}
		default:
			c->sp = 0;
			break;
		}
	}
}

int cff_glyph_outline(const struct cff_font *f, uint16_t gid, const float m[6],
		      const struct cff_outline_cb *cb, void *ud)
{
	if (gid >= f->nglyphs) {
		return -1;
	}
	size_t len;
	const uint8_t *cs = index_get(&f->charstrings, gid, &len);

	if (cs == NULL) {
		return -1;
	}
	struct t2ctx c;

	memset(&c, 0, sizeof(c));
	c.f = f;
	c.cb = cb;
	c.ud = ud;
	c.m = m;
	c.pv = &f->priv;
	if (f->is_cid && f->fdprivs != NULL && f->fdselect != NULL) {
		int fd = f->fdselect[gid];

		if (fd < f->nfd) {
			c.pv = &f->fdprivs[fd];
		}
	}
	run_charstring(&c, cs, len);
	if (c.open) {
		cb->close(ud);
	}
	return 0;
}
