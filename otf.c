#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "trfn.h"

#define NGLYPHS		(1 << 14)
#define GNLEN		(64)
#define BUFLEN		(1 << 23)

#define U32(buf, off)		(htonl(*(u32 *) ((buf) + (off))))
#define U16(buf, off)		(htons(*(u16 *) ((buf) + (off))))
#define U8(buf, off)		(*(u8 *) ((buf) + (off)))
#define S16(buf, off)		((s16) htons(*(u16 *) ((buf) + (off))))
#define S32(buf, off)		((s32) htonl(*(u32 *) ((buf) + (off))))

#define OTFLEN		12	/* otf header length */
#define OTFRECLEN	16	/* otf header record length */
#define CMAPLEN		4	/* cmap header length */
#define CMAPRECLEN	8	/* cmap record length */
#define CMAP4LEN	8	/* format 4 cmap subtable header length */

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
typedef short s16;

static char glyph_name[NGLYPHS][GNLEN];
static int glyph_code[NGLYPHS];
static int glyph_bbox[NGLYPHS][4];
static int glyph_wid[NGLYPHS];
static int glyph_n;
static int upm;			/* units per em */
static int res;			/* device resolution */

static char *macset[];

static int owid(int w)
{
	return (w < 0 ? w * 1000 - upm / 2 : w * 1000 + upm / 2) / upm;
}

static int uwid(int w)
{
	int d = 7200 / res;
	return (w < 0 ? owid(w) - d / 2 : owid(w) + d / 2) / d;
}

/* find the otf table with the given name */
static void *otf_table(void *otf, char *name)
{
	void *recs = otf + OTFLEN;	/* otf table records */
	void *rec;			/* beginning of a table record */
	int nrecs = U16(otf, 4);
	int i;
	for (i = 0; i < nrecs; i++) {
		rec = recs + i * OTFRECLEN;
		if (!strncmp(rec, name, 4))
			return otf + U32(rec, 8);
	}
	return NULL;
}

/* parse otf cmap format 4 subtable */
static void otf_cmap4(void *otf, void *cmap4)
{
	int nsegs;
	void *ends, *begs, *deltas, *offsets;
	void *idarray;
	int beg, end, delta, offset;
	int i, j;
	nsegs = U16(cmap4, 6) / 2;
	ends = cmap4 + 14;
	begs = ends + 2 * nsegs + 2;
	deltas = begs + 2 * nsegs;
	offsets = deltas + 2 * nsegs;
	idarray = offsets + 2 * nsegs;
	for (i = 0; i < nsegs; i++) {
		beg = U16(begs, 2 * i);
		end = U16(ends, 2 * i);
		delta = U16(deltas, 2 * i);
		offset = U16(offsets, 2 * i);
		if (offset) {
			for (j = beg; j <= end; j++)
				glyph_code[U16(offsets + i * 2,
						offset + (j - beg) * 2)] = j;
		} else {
			for (j = beg; j <= end; j++)
				glyph_code[(j + delta) & 0xffff] = j;
		}
	}
}

/* parse otf cmap header */
static void otf_cmap(void *otf, void *cmap)
{
	void *recs = cmap + CMAPLEN;	/* cmap records */
	void *rec;			/* a cmap record */
	void *tab;			/* a cmap subtable */
	int plat, enc;
	int fmt;
	int nrecs = U16(cmap, 2);
	int i;
	for (i = 0; i < nrecs; i++) {
		rec = recs + i * CMAPRECLEN;
		plat = U16(rec, 0);
		enc = U16(rec, 2);
		tab = cmap + U32(rec, 4);
		fmt = U16(tab, 0);
		if (plat == 3 && enc == 1 && fmt == 4)
			otf_cmap4(otf, tab);
	}
}

static void otf_post(void *otf, void *post)
{
	void *post2;			/* version 2.0 header */
	void *index;			/* glyph name indices */
	void *names;			/* glyph names */
	int i, idx;
	int cname = 0;
	if (U32(post, 0) != 0x00020000)
		return;
	post2 = post + 32;
	glyph_n = U16(post2, 0);
	index = post2 + 2;
	names = index + 2 * glyph_n;
	for (i = 0; i < glyph_n; i++) {
		idx = U16(index, 2 * i);
		if (idx <= 257) {
			strcpy(glyph_name[i], macset[idx]);
		} else {
			memcpy(glyph_name[i], names + cname + 1,
				U8(names, cname));
			glyph_name[i][U8(names, cname)] = '\0';
			cname += U8(names, cname) + 1;
		}
	}
}

static void otf_glyf(void *otf, void *glyf)
{
	void *maxp = otf_table(otf, "maxp");
	void *head = otf_table(otf, "head");
	void *loca = otf_table(otf, "loca");
	void *gdat;
	void *gdat_next;
	int n = U16(maxp, 4);
	int fmt = U16(head, 50);
	int i, j;
	for (i = 0; i < n; i++) {
		if (fmt) {
			gdat = glyf + U32(loca, 4 * i);
			gdat_next = glyf + U32(loca, 4 * (i + 1));
		} else {
			gdat = glyf + U16(loca, 2 * i) * 2;
			gdat_next = glyf + U16(loca, 2 * (i + 1)) * 2;
		}
		if (gdat < gdat_next)
			for (j = 0; j < 4; j++)
				glyph_bbox[i][j] = S16(gdat, 2 + 2 * j);
	}
}

static void otf_hmtx(void *otf, void *hmtx)
{
	void *hhea = otf_table(otf, "hhea");
	int n;
	int i;
	n = U16(hhea, 34);
	for (i = 0; i < n; i++)
		glyph_wid[i] = U16(hmtx, i * 4);
	for (i = n; i < glyph_n; i++)
		glyph_wid[i] = glyph_wid[n - 1];
}

static void otf_kern(void *otf, void *kern)
{
	int n;		/* number of kern subtables */
	void *tab;	/* a kern subtable */
	int off = 4;
	int npairs;
	int cov;
	int i, j;
	int c1, c2, val;
	n = U16(kern, 2);
	for (i = 0; i < n; i++) {
		tab = kern + off;
		off += U16(tab, 2);
		cov = U16(tab, 4);
		if ((cov >> 8) == 0 && (cov & 1)) {	/* format 0 */
			npairs = U16(tab, 6);
			for (j = 0; j < npairs; j++) {
				c1 = U16(tab, 14 + 6 * j);
				c2 = U16(tab, 14 + 6 * j + 2);
				val = S16(tab, 14 + 6 * j + 4);
				trfn_kern(glyph_name[c1], glyph_name[c2],
					owid(val));
			}
		}
	}
}

static int coverage(void *cov, int *out)
{
	int fmt = U16(cov, 0);
	int n = U16(cov, 2);
	int beg, end;
	int ncov = 0;
	int i, j;
	if (fmt == 1) {
		for (i = 0; i < n; i++)
			out[ncov++] = U16(cov, 4 + 2 * i);
	}
	if (fmt == 2) {
		for (i = 0; i < n; i++) {
			beg = U16(cov, 4 + 6 * i);
			end = U16(cov, 4 + 6 * i + 2);
			for (j = beg; j <= end; j++)
				out[ncov++] = j;
		}
	}
	return ncov;
}

static int valuerecord_len(int fmt)
{
	int off = 0;
	int i;
	for (i = 0; i < 8; i++)
		if (fmt & (1 << i))
			off += 2;
	return off;
}

static void valuerecord_print(int fmt, void *rec)
{
	int vals[8] = {0};
	int off = 0;
	int i;
	for (i = 0; i < 8; i++) {
		if (fmt & (1 << i)) {
			vals[i] = uwid(S16(rec, off));
			off += 2;
		}
	}
	if (fmt)
		printf(":%+d%+d%+d%+d", vals[0], vals[1], vals[2], vals[3]);
}

static void otf_gpostype1(void *otf, char *feat, char *sub)
{
	int fmt = U16(sub, 0);
	int vfmt = U16(sub, 4);
	int cov[NGLYPHS];
	int ncov, nvals;
	int vlen = valuerecord_len(vfmt);
	int i;
	ncov = coverage(sub + U16(sub, 2), cov);
	if (fmt == 1) {
		for (i = 0; i < ncov; i++) {
			printf("gpos %s %s", feat, glyph_name[cov[i]]);
			valuerecord_print(vfmt, sub + 6);
			printf("\n");
		}
	}
	if (fmt == 2) {
		nvals = U16(sub, 6);
		for (i = 0; i < nvals; i++) {
			printf("gpos %s %s", feat, glyph_name[cov[i]]);
			valuerecord_print(vfmt, sub + 8 + i * vlen);
			printf("\n");
		}
	}
}

static void otf_gpostype2(void *otf, char *feat, char *sub)
{
	int fmt = U16(sub, 0);
	int vfmt1 = U16(sub, 4);
	int vfmt2 = U16(sub, 6);
	int c2len;
	int nc1 = U16(sub, 8);
	int cov[NGLYPHS];
	void *c2;
	int ncov, nc2, second;
	int i, j;
	if (fmt != 1)
		return;
	ncov = coverage(sub + U16(sub, 2), cov);
	c2len = 2 + valuerecord_len(vfmt1) + valuerecord_len(vfmt2);
	for (i = 0; i < nc1; i++) {
		c2 = sub + U16(sub, 10 + 2 * i);
		nc2 = U16(c2, 0);
		for (j = 0; j < nc2; j++) {
			printf("gpos %s 2", feat);
			second = U16(c2 + 2 + c2len * j, 0);
			printf(" %s", glyph_name[cov[i]]);
			valuerecord_print(vfmt1, c2 + 2 + c2len * j + 2);
			printf(" %s", glyph_name[second]);
			valuerecord_print(vfmt2, c2 + 2 + c2len * j + 2 +
					valuerecord_len(vfmt1));
			printf("\n");
		}
	}
}

static void otf_gpostype3(void *otf, char *feat, char *sub)
{
	int fmt = U16(sub, 0);
	int cov[NGLYPHS];
	int ncov, i, n;
	ncov = coverage(sub + U16(sub, 2), cov);
	if (fmt != 1)
		return;
	n = U16(sub, 4);
	for (i = 0; i < n; i++) {
		int prev = U16(sub, 6 + 4 * i);
		int next = U16(sub, 6 + 4 * i + 2);
		printf("gcur %s %s", feat, glyph_name[cov[i]]);
		if (prev)
			printf(" %d %d", uwid(S16(sub, prev + 2)),
					uwid(S16(sub, prev + 4)));
		else
			printf(" - -");
		if (next)
			printf(" %d %d", uwid(S16(sub, next + 2)),
					uwid(S16(sub, next + 4)));
		else
			printf(" - -");
		printf("\n");
	}
}

static void otf_gposfeatrec(void *otf, void *gpos, void *featrec)
{
	void *feats = gpos + U16(gpos, 6);
	void *lookups = gpos + U16(gpos, 8);
	void *feat, *lookup, *tab;
	int nlookups, type, flag, ntabs;
	char tag[8] = "";
	int i, j;
	memcpy(tag, featrec, 4);
	feat = feats + U16(featrec, 4);
	nlookups = U16(feat, 2);
	for (i = 0; i < nlookups; i++) {
		lookup = lookups + U16(lookups, 2 + 2 * U16(feat, 4 + 2 * i));
		type = U16(lookup, 0);
		flag = U16(lookup, 2);
		ntabs = U16(lookup, 4);
		for (j = 0; j < ntabs; j++) {
			tab = lookup + U16(lookup, 6 + 2 * j);
			if (type == 1)
				otf_gpostype1(otf, tag, tab);
			if (type == 2)
				otf_gpostype2(otf, tag, tab);
			if (type == 3)
				otf_gpostype3(otf, tag, tab);
		}
	}
}

static void otf_gposlang(void *otf, void *gpos, void *lang)
{
	void *feats = gpos + U16(gpos, 6);
	int featidx = U16(lang, 2);
	int nfeat = U16(lang, 4);
	int i;
	if (featidx != 0xffff)
		otf_gposfeatrec(otf, gpos, feats + 2 + 6 * featidx);
	for (i = 0; i < nfeat; i++)
		otf_gposfeatrec(otf, gpos,
				feats + 2 + 6 * U16(lang, 6 + 2 * i));
}

static void otf_gpos(void *otf, void *gpos)
{
	void *scripts = gpos + U16(gpos, 4);
	int nscripts, nlangs;
	void *script;
	void *grec;
	int i, j;
	nscripts = U16(scripts, 0);
	for (i = 0; i < nscripts; i++) {
		grec = scripts + 2 + 6 * i;
		script = scripts + U16(grec, 4);
		if (U16(script, 0))
			otf_gposlang(otf, gpos, script + U16(script, 0));
		nlangs = U16(script, 2);
		for (j = 0; j < nlangs; j++)
			otf_gposlang(otf, gpos, script +
					U16(script, 4 + 6 * j + 4));
	}
}

static void otf_gsubtype1(void *otf, char *feat, char *sub)
{
	int cov[NGLYPHS];
	int fmt = U16(sub, 0);
	int ncov;
	int n;
	int i;
	ncov = coverage(sub + U16(sub, 2), cov);
	if (fmt == 1) {
		for (i = 0; i < ncov; i++)
			printf("gsub %s 2 -%s +%s\n",
				feat, glyph_name[cov[i]],
				glyph_name[cov[i] + S16(sub, 4)]);
	}
	if (fmt == 2) {
		n = U16(sub, 4);
		for (i = 0; i < n; i++)
			printf("gsub %s 2 -%s +%s\n",
				feat, glyph_name[cov[i]],
				glyph_name[U16(sub, 6 + 2 * i)]);
	}
}

static void otf_gsubtype3(void *otf, char *feat, char *sub)
{
	int cov[NGLYPHS];
	int fmt = U16(sub, 0);
	int ncov, n, i, j;
	if (fmt != 1)
		return;
	ncov = coverage(sub + U16(sub, 2), cov);
	n = U16(sub, 4);
	for (i = 0; i < n; i++) {
		void *alt = sub + U16(sub, 6 + 2 * i);
		int nalt = U16(alt, 0);
		for (j = 0; j < nalt; j++)
			printf("gsub %s 2 -%s +%s\n",
				feat, glyph_name[cov[i]],
				glyph_name[U16(alt, 2 + 2 * j)]);
	}
}

static void otf_gsubtype4(void *otf, char *feat, char *sub)
{
	int fmt = U16(sub, 0);
	int cov[NGLYPHS];
	int ncov, n, i, j, k;
	if (fmt != 1)
		return;
	ncov = coverage(sub + U16(sub, 2), cov);
	n = U16(sub, 4);
	for (i = 0; i < n; i++) {
		void *set = sub + U16(sub, 6 + 2 * i);
		int nset = U16(set, 0);
		for (j = 0; j < nset; j++) {
			void *lig = set + U16(set, 2 + 2 * j);
			int nlig = U16(lig, 2);
			printf("gsub %s %d -%s",
				feat, nlig + 1, glyph_name[cov[i]]);
			for (k = 0; k < nlig - 1; k++)
				printf(" -%s", glyph_name[U16(lig, 4 + 2 * k)]);
			printf(" +%s\n", glyph_name[U16(lig, 0)]);
		}
	}
}

static void otf_gsubfeatrec(void *otf, void *gsub, void *featrec)
{
	void *feats = gsub + U16(gsub, 6);
	void *lookups = gsub + U16(gsub, 8);
	void *feat, *lookup, *tab;
	int nlookups, type, flag, ntabs;
	char tag[8] = "";
	int i, j;
	memcpy(tag, featrec, 4);
	feat = feats + U16(featrec, 4);
	nlookups = U16(feat, 2);
	for (i = 0; i < nlookups; i++) {
		lookup = lookups + U16(lookups, 2 + 2 * U16(feat, 4 + 2 * i));
		type = U16(lookup, 0);
		flag = U16(lookup, 2);
		ntabs = U16(lookup, 4);
		for (j = 0; j < ntabs; j++) {
			tab = lookup + U16(lookup, 6 + 2 * j);
			if (type == 1)
				otf_gsubtype1(otf, tag, tab);
			if (type == 3)
				otf_gsubtype3(otf, tag, tab);
			if (type == 4)
				otf_gsubtype4(otf, tag, tab);
		}
	}
}

static void otf_gsublang(void *otf, void *gsub, void *lang)
{
	void *feats = gsub + U16(gsub, 6);
	int featidx = U16(lang, 2);
	int nfeat = U16(lang, 4);
	int i;
	if (featidx != 0xffff)
		otf_gsubfeatrec(otf, gsub, feats + 2 + 6 * featidx);
	for (i = 0; i < nfeat; i++)
		otf_gsubfeatrec(otf, gsub,
				feats + 2 + 6 * U16(lang, 6 + 2 * i));
}

static void otf_gsub(void *otf, void *gsub)
{
	void *scripts = gsub + U16(gsub, 4);
	int nscripts, nlangs;
	void *script;
	int i, j;
	nscripts = U16(scripts, 0);
	for (i = 0; i < nscripts; i++) {
		script = scripts + U16(scripts + 2 + 6 * i, 4);
		nlangs = U16(script, 2);
		if (U16(script, 0))
			otf_gsublang(otf, gsub, script + U16(script, 0));
		for (j = 0; j < nlangs; j++)
			otf_gsublang(otf, gsub, script +
					U16(script, 4 + 6 * j + 4));
	}
}

int xread(int fd, char *buf, int len)
{
	int nr = 0;
	while (nr < len) {
		int ret = read(fd, buf + nr, len - nr);
		if (ret == -1 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (ret <= 0)
			break;
		nr += ret;
	}
	return nr;
}

static char buf[BUFLEN];

int otf_read(void)
{
	int i;
	if (xread(0, buf, sizeof(buf)) <= 0)
		return 1;
	upm = U16(otf_table(buf, "head"), 18);
	otf_cmap(buf, otf_table(buf, "cmap"));
	otf_post(buf, otf_table(buf, "post"));
	if (otf_table(buf, "glyf"))
		otf_glyf(buf, otf_table(buf, "glyf"));
	otf_hmtx(buf, otf_table(buf, "hmtx"));
	for (i = 0; i < glyph_n; i++) {
		trfn_char(glyph_name[i], -1,
			glyph_code[i] != 0xffff ? glyph_code[i] : 0,
			owid(glyph_wid[i]),
			owid(glyph_bbox[i][0]), owid(glyph_bbox[i][1]),
			owid(glyph_bbox[i][2]), owid(glyph_bbox[i][3]));
	}
	if (otf_table(buf, "kern"))
		otf_kern(buf, otf_table(buf, "kern"));
	return 0;
}

void otf_feat(int r)
{
	res = r;
	if (otf_table(buf, "GSUB"))
		otf_gsub(buf, otf_table(buf, "GSUB"));
	if (otf_table(buf, "GPOS"))
		otf_gpos(buf, otf_table(buf, "GPOS"));
}

static char *macset[] = {
	".notdef", ".null", "nonmarkingreturn", "space", "exclam",
	"quotedbl", "numbersign", "dollar", "percent", "ampersand",
	"quotesingle", "parenleft", "parenright", "asterisk", "plus",
	"comma", "hyphen", "period", "slash", "zero",
	"one", "two", "three", "four", "five",
	"six", "seven", "eight", "nine", "colon",
	"semicolon", "less", "equal", "greater", "question",
	"at", "A", "B", "C", "D",
	"E", "F", "G", "H", "I",
	"J", "K", "L", "M", "N",
	"O", "P", "Q", "R", "S",
	"T", "U", "V", "W", "X",
	"Y", "Z", "bracketleft", "backslash", "bracketright",
	"asciicircum", "underscore", "grave", "a", "b",
	"c", "d", "e", "f", "g",
	"h", "i", "j", "k", "l",
	"m", "n", "o", "p", "q",
	"r", "s", "t", "u", "v",
	"w", "x", "y", "z", "braceleft",
	"bar", "braceright", "asciitilde", "Adieresis", "Aring",
	"Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis",
	"aacute", "agrave", "acircumflex", "adieresis", "atilde",
	"aring", "ccedilla", "eacute", "egrave", "ecircumflex",
	"edieresis", "iacute", "igrave", "icircumflex", "idieresis",
	"ntilde", "oacute", "ograve", "ocircumflex", "odieresis",
	"otilde", "uacute", "ugrave", "ucircumflex", "udieresis",
	"dagger", "degree", "cent", "sterling", "section",
	"bullet", "paragraph", "germandbls", "registered", "copyright",
	"trademark", "acute", "dieresis", "notequal", "AE",
	"Oslash", "infinity", "plusminus", "lessequal", "greaterequal",
	"yen", "mu", "partialdiff", "summation", "product",
	"pi", "integral", "ordfeminine", "ordmasculine", "Omega",
	"ae", "oslash", "questiondown", "exclamdown", "logicalnot",
	"radical", "florin", "approxequal", "Delta", "guillemotleft",
	"guillemotright", "ellipsis", "nonbreakingspace", "Agrave", "Atilde",
	"Otilde", "OE", "oe", "endash", "emdash",
	"quotedblleft", "quotedblright", "quoteleft", "quoteright", "divide",
	"lozenge", "ydieresis", "Ydieresis", "fraction", "currency",
	"guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl",
	"periodcentered", "quotesinglbase", "quotedblbase", "perthousand", "Acircumflex",
	"Ecircumflex", "Aacute", "Edieresis", "Egrave", "Iacute",
	"Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex",
	"apple", "Ograve", "Uacute", "Ucircumflex", "Ugrave",
	"dotlessi", "circumflex", "tilde", "macron", "breve",
	"dotaccent", "ring", "cedilla", "hungarumlaut", "ogonek",
	"caron", "Lslash", "lslash", "Scaron", "scaron",
	"Zcaron", "zcaron", "brokenbar", "Eth", "eth",
	"Yacute", "yacute", "Thorn", "thorn", "minus",
	"multiply", "onesuperior", "twosuperior", "threesuperior", "onehalf",
	"onequarter", "threequarters", "franc", "Gbreve", "gbreve",
	"Idotaccent", "Scedilla", "scedilla", "Cacute", "cacute",
	"Ccaron", "ccaron", "dcroat",
};