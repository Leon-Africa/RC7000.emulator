
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h>

#include "domusobj.h"
#include "rc3600.h"
#include "rc3600_emul.h"

#define AZ(foo)         do { assert((foo) == 0); } while (0)
#define AN(foo)         do { assert((foo) != 0); } while (0)

static int fo;

static int
xfgetc(void *priv)
{
        return fgetc(priv);
}

static void
PC(uint8_t u)
{
	// printf(">>%02x\n", u);
	assert(1 == write(fo, &u, 1));
}

static void
PW(unsigned u)
{
	// printf(">>>>%04x %06o\n", u, u);
	PC(u >> 8);
	PC(u & 0xff);
}

static uint16_t
GW(void)
{
	uint8_t c[2];
	int i;

	i = read(fo, c + 0, 1);
	assert(i == 1);
	i = read(fo, c + 1, 1);
	assert(i == 1);
	return ((c[0] << 8) | c[1]);
}

static void
SendCmd(uint16_t cmd, uint16_t a0, uint16_t a1, uint16_t a2, uint16_t a3)
{
	uint16_t s;

	PW(cmd); s = cmd;
	PW(a0);  s += a0;
	PW(a1);  s += a1;
	PW(a2);  s += a2;
	PW(a3);  s += a3;
	PW(65536-(unsigned)s);
}

static int
DoCmd(uint16_t cmd, uint16_t a0, uint16_t a1, uint16_t a2, uint16_t a3,
    uint16_t *upload, uint16_t uplen, uint16_t *download, uint16_t downlen)
{
	uint16_t s;
	int i;

	PW(cmd); s = cmd;
	PW(a0);  s += a0;
	PW(a1);  s += a1;
	PW(a2);  s += a2;
	PW(a3);  s += a3;
	PW(65536-(unsigned)s);
	printf("%04x(%04x,%04x,%04x,%04x) S|%04x",
	    cmd, a0, a1, a2, a3, 0x10000 - s);

	s = GW();
	if (s == 0)
		return (-1);
	if (s & 0x8000) {
		/* upload */
		printf(" U[%d]", 0x10000 - s);
		i = s;
		i += uplen;
		assert(i == 0x10000);
		for (i = s; i < 0x10000; i++) {
			PW(*upload++);
			uplen--;
		}
		s = GW();
	}
	printf(" D %4d", s);
	assert(!(s & 0x8000));
	assert(s != 0);
	/* download */
	assert(s == downlen);
	for (i = 0; i < s; i++)
		*download++ = GW();
	putchar('\t');
	return (0);
}

static void
Sync(void)
{
	int i;

	printf("SYNC\t");
	i = DoCmd(0, 0, 0, 0, 0, NULL, 0, NULL, 0);
	printf(" => %d\n", i);
	assert(i == -1);
}

static uint16_t
ChkSum(uint16_t from, uint16_t to)
{
	uint16_t in[1];

	assert(from < to);
	printf("CHKSUM\t");
	DoCmd(1, from, to, 0, 0, NULL, 0, in, 1);
	printf(" => S|%04x\n", in[0]);
	return (in[0]);
}

static void
Upload(uint16_t dst, uint16_t *fm, uint16_t len)
{
	uint16_t in[1], s = 0;
	int i;

	printf("UPLOAD\t");
	DoCmd(2, dst, dst+len, 0, 0, fm, len, in, 1);
	for (i = 0; i < len; i++)
		s += fm[i];
	printf(" => S|%04x (%04x)\n", in[0], s);
	assert(s == in[0]);
	ChkSum(dst, dst + len);
}

static void
Download(uint16_t src, uint16_t len, uint16_t *to)
{
	uint16_t in[len + 1], s = 0;
	int i;

	printf("DNLOAD\t");
	DoCmd(3, src, src+len, 0, 0, NULL, 0, in, len + 1);
	for (i = 0; i < len; i++) {
		to[i] = in[i];
		s += in[i];
	}
	printf(" => S|%04x (%04x)\n", in[len], s);
	assert(s == in[len]);
}

static void
Fill(uint16_t from, uint16_t to, uint16_t val)
{
	uint16_t in[1], s = 0;
	int i;

	assert(to > from);
	printf("FILL\t");
	DoCmd(4, from, to, val, 0, NULL, 0, in, 1);
	for (i = 0; i < (to-from); i++)
		s += val;
	printf(" => S|%04x (%04x)\n", in[0], s);
	assert(s == in[0]);
}

/**********************************************************************/

static uint16_t
DKP_recal(void)
{
	uint16_t retval;

	printf("RECAL\t");
	DoCmd(20, 0, 0, 0, 0, NULL, 0, &retval, 1);
	return (retval);
}

static void
DKP_rw(uint16_t doa, uint16_t dob, uint16_t doc, uint16_t *result)
{

	printf("RW\t");
	DoCmd(21, doa, dob, doc, 0, NULL, 0, result, 3);
}

static uint16_t
DKP_seek(uint16_t cyl)
{
	uint16_t retval;

	printf("SEEK\t");
	DoCmd(22, cyl, 0, 0, 0, NULL, 0, &retval, 1);
	return (retval);
}


static uint16_t core[32768];

static void
read_dkp(const char *fn)
{
	FILE *ft = fopen(fn, "w");
	int i, u, cyl,lcyl,hd;
	uint16_t ret[12 * 256];

	assert(ft != NULL);

	printf("Recal: %04x\n", DKP_recal());

	lcyl = 0;
	for (cyl = 0; cyl < 203; cyl++) {
		for (hd = 0; hd < 2; hd++) {
			if (cyl != lcyl) {
				i = DKP_seek(cyl);
				printf("Seek: cyl=%d %04x\n", cyl, i);
				assert(i == 0x4040);
				lcyl = cyl;
			}

			DKP_rw(cyl, 0x1000, 0x0004 | (hd << 8), ret);
			printf("DKP: DIA=%04x", ret[0]);
			assert(ret[0] == 0xc040);
			printf(" DIB=%04x", ret[1]);
			assert(ret[1] == 0x1c00);
			printf(" DIC=%04x\n", ret[2]);
			assert(ret[2] == (0x00c0 | (hd << 8)));

			Download(0x1000, 0x0c00, ret);
			for (u = 0; u < 12*256; u++) {
				i = ret[u];
				fputc(i >> 8, ft);
				fputc(i & 0xff, ft);
			}
			fflush(ft);
		}
	}
}

static void
dkp_write_upload_track(FILE *fi)
{
	uint8_t sect[512];
	uint16_t ws[256];
	int i, j, u, sc,s;

	SendCmd(4, 0x1000, 0x1c00, 0, 0);
	GW();
	for (sc = 0; sc < 12; sc++) {
		i = fread(sect, sizeof sect, 1, fi);
		assert(i == 1);
		for (i = 0, j = 0; i < 256; i++, j+=2)
			ws[i] = sect[j] << 8 | sect[j+1];
		for (j = 256; j > 0; j--)
			if (ws[255] != ws[j - 1])
				break;
		if (j > 200)
			j = 256;

		if (j > 0) {
			s = 0x1000 + 0x100 * sc;
			SendCmd(2, s, s + j, 0, 0);
			GW();
			s = 0;
			for (u = 0; u < j; u++) {
				s += ws[u];
				PW(ws[u]);
			}
			i = GW();
			s &= 0xffff;
			printf(" Upload: %3d %04x %04x", j, i, s);
			assert(s == i);
		}
		if (j != 256 && ws[255] != 0) {
			s = 0x1000 + 0x100 * sc;
			SendCmd(4, s + j, s + 0x100, ws[255], 0);
			printf(" Fill: %3d %04x", 256 - j, GW());
		}
		printf("\n");
	}
}

static void
dkp_write(const char *fn)
{
	FILE *fi = fopen(fn, "r");
	int i, cyl,lcyl,hd;

	SendCmd(5, 0, 0, 0, 0);
	printf("Recal: %04x\n", GW());

	lcyl = 0;
	for (cyl = 0; cyl < 203; cyl++) {
		for (hd = 0; hd < 2; hd++) {
			if (cyl != lcyl) {
				SendCmd(7, cyl, 0, 0, 0);
				i = GW();
				printf(" Seek: cyl=%d %04x\n", cyl, i);
				assert(i == 0x4040);
				lcyl = cyl;
			}
			dkp_write_upload_track(fi);

			printf("c%03d h%d ", cyl, hd);

			SendCmd(6, 0x100 | cyl, 0x1000, 0x0004 | (hd << 8), 0);
			i = GW();
			printf("DKP: DIA=%04x", i);
			assert(i == 0xc040);
			i = GW();
			printf(" DIB=%04x", i);
			assert(i == 0x1c02);
			i = GW();
			printf(" DIC=%04x\n", i);
			assert(i == (0x00c0 | (hd << 8))); // emulator
			//assert(i == (0x0004 | (hd << 8))); // live
		}
	}

}

int
main(int argc, char **argv)
{
        struct domus_obj_file *dof;
        struct domus_obj_obj *doo;
        struct domus_obj_rec *dor;
	FILE *fi;
	unsigned u, a;
	struct termios t;
	uint8_t c;
	int i, j;
	char buf[BUFSIZ];
	uint16_t card[80];

	setbuf(stderr, NULL);
	setbuf(stdout, NULL);
	fi = fopen("../domus/__.SLAVE", "r");

	if (argc > 1)
		fo = open(argv[1], O_RDWR);
	else
		fo = open("/dev/nmdm1B", O_RDWR);
	assert(fo >= 0);
	AZ(tcgetattr(fo, &t));
	cfmakeraw(&t);
	cfsetspeed(&t, B9600);
	t.c_cflag |= CLOCAL | CSTOPB;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 1;
	AZ(tcsetattr(fo, TCSAFLUSH, &t));

	assert(fi != NULL);
	dof = ReadDomusObj(xfgetc, fi, "-", 1);
	doo = TAILQ_FIRST(&dof->objs);
	assert(doo != NULL);
	for (u = 0; u < 64; u++) 
		PC(0);

	PC(0x01);
		
	TAILQ_FOREACH(dor, &doo->recs, list) {
		if (WVAL(dor->w[0]) != 2)
			continue;
		a = WVAL(dor->w[0]);
		printf("---> %04x\n", a);
		for (u = 7; u < dor->nw; u++, a++) {
			assert(WRELOC(dor->w[u]) == 1);
			core[a] = WVAL(dor->w[u]);
			PW(core[a]);
		}
	}

	for (u = 0; u < 3; u++)
		while (read(fo, &c, 1) > 0)
			continue;

	for (u = 0; u < 64; u++) {
		PC(0);
		i = read(fo, &c, 1);
		printf("%d %02x %u\n", i, c, u);
		if (i == 1 && c == 0) {
			i = read(fo, &c, 1);
			printf("  %d %02x %u\n", i, c, u);
			if (i == 1 && c == 0) {
				printf("Sync!\n");
				break;
			}
		}
	}
	t.c_cc[VTIME] = 200;
	AZ(tcsetattr(fo, TCSAFLUSH, &t));

	Sync();
	i = ChkSum(0x0000, 0x0020);
	memset(card, 0x7f, sizeof card);
	Upload(0x1000, card, 80);
	Download(0x1000, 80, card);
	Fill(0x1000, 0x1004, 0x1234);

	if (1) {
		read_dkp("/tmp/_.ty");
		exit (0);
	}
	if (0)
		dkp_write("/tmp/_.cb2");
	
	while (1) {
		printf("Press enter to read card:");
		fgets(buf, sizeof buf, stdin);
		SendCmd(8, 0x1000, 041, 0, 0);
		i = GW();
		printf("Start = %04x\n", i);
		while (1) {
			SendCmd(9, 0x1000, 0, 0, 0);
			i = GW();
			printf("Busy = %04x, OK ? ", i);
			fgets(buf, sizeof buf, stdin);
			if (buf[0] != '\n')
				break;
		}
		SendCmd(10, 0x1000, 0x1000 + 80, 0, 0);
		i = GW();
		
		SendCmd(3, 0x1000, 0x1000 + 80, 0, 0);
		for (u = 0; u < 80; u++)
			card[u] = GW();

		for (u = 0; u < 80; u++)
			printf("%x", (card[u] >> 8) & 0xf);
		printf("\n");
		for (u = 0; u < 80; u++)
			printf("%x", (card[u] >> 4) & 0xf);
		printf("\n");
		for (u = 0; u < 80; u++)
			printf("%x", (card[u] >> 0) & 0xf);
		printf("\n");
		for (j = 0x8000; j; j >>= 1) {
			for (i = 0; i < 80; i++) {
				if (card[i] & j)
					printf("#");
				else
					printf("|");
			}
			printf("  ");
			for (i = 0; i < 80; i += 2) {
				if ((card[i]|card[i+1]) & j)
					printf("#");
				else
					printf("|");
			}
			printf("\n");
		}
		printf("Download: %04x\n", GW());
	}

	return (0);
}
