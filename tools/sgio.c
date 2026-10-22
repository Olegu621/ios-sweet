/*
 * sgio - send one raw SCSI CDB to a /dev/sgN device (freestanding aarch64).
 *
 * Usage: sgio <sgdev> <cdb-hex> [in|out] [datalen]
 *
 * Prints the SCSI status, the host/driver status and the sense buffer, so
 * the result can be read back over adb.  Built with
 *   clang --target=aarch64-none-elf -nostdlib -static -fuse-ld=lld
 */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;

#define SYS_ioctl 29
#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93

#define AT_FDCWD (-100)
#define O_RDONLY 0

#define SG_IO 0x2285
#define SG_DXFER_NONE 0
#define SG_DXFER_TO_DEV 1
#define SG_DXFER_FROM_DEV 2

struct sg_io_hdr {
	int interface_id;
	int dxfer_direction;
	unsigned char cmd_len;
	unsigned char mx_sb_len;
	unsigned short iovec_count;
	unsigned int dxfer_len;
	void *dxferp;
	unsigned char *cmdp;
	unsigned char *sbp;
	unsigned int timeout;
	unsigned int flags;
	int pack_id;
	void *usr_ptr;
	unsigned char status;
	unsigned char masked_status;
	unsigned char msg_status;
	unsigned char sb_len_wr;
	unsigned short host_status;
	unsigned short driver_status;
	int resid;
	unsigned int duration;
	unsigned int info;
};

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
			 : "memory", "cc");
	return x0;
}

static void out(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	sys6(SYS_write, 1, (long)s, n, 0, 0, 0);
}

static void outnum(unsigned long v)
{
	char t[24];
	int k = 0;
	if (!v)
		t[k++] = '0';
	while (v) {
		t[k++] = '0' + v % 10;
		v /= 10;
	}
	while (k)
		out((char[]){ t[--k], 0 });
}

static void hexbyte(unsigned char v)
{
	char b[3];
	b[0] = "0123456789abcdef"[v >> 4];
	b[1] = "0123456789abcdef"[v & 15];
	b[2] = 0;
	out(b);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void *memset_(void *p, int v, unsigned long n)
{
	unsigned char *b = p;
	while (n--)
		*b++ = (unsigned char)v;
	return p;
}

static unsigned char cmdbuf[16];
static unsigned char sb[64];
static unsigned char databuf[4096];

int main(long argc, char **argv);

void _start(void)
{
	long *sp;
	long argc = 0;
	char **argv = 0;
	int q;

	__asm__ volatile("mov %0, sp" : "=r"(sp));

	/*
	 * The aarch64 kernel puts the auxiliary vector at the top of the
	 * stack and leaves sp pointing into it, so the classic
	 * argc/argv array sits a few words above sp.  Find argv[0] by
	 * looking for the first word that points at a string in the
	 * argument string area.
	 */
	for (q = 0; q < 1024; q++) {
		unsigned long w = (unsigned long)sp[q];
		if (w > (unsigned long)sp + 512 && w < (unsigned long)sp + 65536 &&
		    *(char *)w == '/') {
			argc = sp[q - 1];
			argv = (char **)&sp[q];
			break;
		}
	}
	if (!argv) {
		out("no argv\n");
		sys6(SYS_exit, 1, 0, 0, 0, 0, 0);
	}
	sys6(SYS_exit, main(argc, argv), 0, 0, 0, 0, 0);
	for (;;)
		;
}

int main(long argc, char **argv)
{
	struct sg_io_hdr h;
	const char *dev, *cdb, *dir;
	int fd, i, n = 0, dlen = 0, rc;
	unsigned int v;

	if (argc < 3) {
		out("usage: sgio <sgdev> <cdb-hex> [in|out] [datalen]\n");
		return 2;
	}
	dev = argv[1];
	cdb = argv[2];
	dir = argc > 3 ? argv[3] : "none";
	if (argc > 4) {
		v = 0;
		for (i = 0; argv[4][i]; i++)
			v = v * 10 + (argv[4][i] - '0');
		dlen = (int)v;
	}

	for (i = 0; cdb[i] && cdb[i + 1] && n < 16; i += 2) {
		int hi = hexval(cdb[i]), lo = hexval(cdb[i + 1]);
		if (hi < 0 || lo < 0)
			break;
		cmdbuf[n++] = (unsigned char)((hi << 4) | lo);
	}

	fd = (int)sys6(SYS_openat, AT_FDCWD, (long)dev, O_RDONLY, 0, 0, 0);
	if (fd < 0) {
		out("open failed\n");
		return 3;
	}

	memset_(&h, 0, sizeof(h));
	memset_(sb, 0, sizeof(sb));
	memset_(databuf, 0, sizeof(databuf));
	h.interface_id = 'S';
	h.cmd_len = (unsigned char)n;
	h.mx_sb_len = sizeof(sb);
	h.cmdp = cmdbuf;
	h.sbp = sb;
	h.timeout = 10000;
	if (str_eq(dir, "in")) {
		h.dxfer_direction = SG_DXFER_FROM_DEV;
		h.dxfer_len = (unsigned)dlen;
		h.dxferp = databuf;
	} else if (str_eq(dir, "out")) {
		h.dxfer_direction = SG_DXFER_TO_DEV;
		h.dxfer_len = (unsigned)dlen;
		h.dxferp = databuf;
	} else {
		h.dxfer_direction = SG_DXFER_NONE;
	}

	rc = (int)sys6(SYS_ioctl, fd, SG_IO, (long)&h, 0, 0, 0);
	out("ioctl rc=");
	outnum(rc < 0 ? (unsigned long)(-rc) : (unsigned long)rc);
	out(" status=");
	hexbyte(h.status);
	out(" masked=");
	hexbyte(h.masked_status);
	out(" msg=");
	hexbyte(h.msg_status);
	out(" host=");
	hexbyte((unsigned char)h.host_status);
	hexbyte((unsigned char)(h.host_status >> 8));
	out(" drv=");
	hexbyte((unsigned char)h.driver_status);
	hexbyte((unsigned char)(h.driver_status >> 8));
	out(" sb_len=");
	hexbyte(h.sb_len_wr);
	out(" resid=");
	outnum((unsigned long)h.resid);
	out("\nsense=");
	for (i = 0; i < h.sb_len_wr && i < (int)sizeof(sb); i++)
		hexbyte(sb[i]);
	out("\n");
	if (h.dxfer_direction == SG_DXFER_FROM_DEV) {
		out("data=");
		for (i = 0; i < (int)h.dxfer_len && i < 64; i++)
			hexbyte(databuf[i]);
		out("\n");
	}
	sys6(SYS_close, fd, 0, 0, 0, 0, 0);
	return 0;
}
