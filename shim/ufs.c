/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"


static int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}
#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */

#define REG_CAP		0x00
#define REG_MCQCAP	0x04
#define REG_VER		0x08
#define REG_EXTCAP	0x0c
#define REG_PID		0x10
#define REG_MID		0x14
#define REG_IS		0x20
#define REG_IE		0x24
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UECPA	0x38
#define REG_UECDL	0x3c
#define REG_UECN	0x40
#define REG_UECT	0x44
#define REG_UECDME	0x48
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
/* UFSHCI 2.x/3.x/4.x: the UIC command block moved to 0x90 (in 1.x it was
 * 0x10, which is CONTROLLER_PID in this layout and read-only). */
#define REG_UICCMD	0x90
#define REG_UICARG0	0x94
#define REG_UICARG1	0x98
#define REG_UICARG2	0x9c

#define IS_UIC_COMP	(1u << 10)	/* UIC command completion */

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

/* UIC DME commands (JEDEC UFS 4.0, table "UIC commands"). */
#define UIC_DME_GET		0x01
#define UIC_DME_SET		0x02
#define UIC_DME_PEER_GET	0x03
#define UIC_DME_POWERON		0x10
#define UIC_DME_POWEROFF	0x11
#define UIC_DME_ENABLE		0x12
#define UIC_DME_RESET		0x14
#define UIC_DME_LINK_STARTUP	0x16

/* PA layer MIB attributes (UniPro 1.8 / JESD220). */
#define PA_ACTIVETXDATALANES	0x1560
#define PA_CONNECTEDTXDATALANES	0x1561
#define PA_TXPWRSTATUS		0x1567
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156a
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_CONNECTEDRXDATALANES	0x1581
#define PA_RXPWRSTATUS		0x1582
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584
#define PA_MAXRXPWMGEAR		0x1586
#define PA_MAXRXHSGEAR		0x1587
#define PWRMODE_FAST		0x11	/* (rx<<4)|tx, FAST = 1 */
#define PA_HS_MODE_B		2

#define BLOCK		4096u

/* ---- experiment log (kept in RAM, written to the UFS at the end) ---- */
static char logbuf[16384];
static unsigned loglen;

static void log_str(const char *t)
{
	while (*t && loglen < sizeof(logbuf) - 2)
		logbuf[loglen++] = *t++;
	logbuf[loglen] = 0;
}

static void log_num(const char *tag, uint32_t v)
{
	char t[12];
	int k = 0;

	log_str(tag);
	log_str("=");
	if (!v)
		t[k++] = '0';
	while (v) {
		t[k++] = "0123456789abcdef"[v & 15];
		v >>= 4;
	}
	while (k)
		logbuf[loglen++] = t[--k];
	log_str("\n");
}

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[4 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | p[1];
}

/* Response UPIU: 12-byte header, then the SCSI part. The sense data length
 * is at offset 32 and the 18 sense bytes follow (Linux struct utp_cmd_rsp). */
static void sense_dump(void)
{
	con_hex2("RSP", "d0_3=", be32(ucd.resp), "d4_7=", be32(ucd.resp + 4));
	con_hex2("RSP", "d8_b=", be32(ucd.resp + 8), "len=", be16(ucd.resp + 32));
	con_hex2("SENSE", "d0_3=", be32(ucd.resp + 34), "d4_7=", be32(ucd.resp + 38));
	con_hex2("SENSE", "d8_b=", be32(ucd.resp + 42), "dc_f=", be32(ucd.resp + 46));
	con_hex2("SENSE", "d10=", be16(ucd.resp + 50), "d12=", be16(ucd.resp + 52));
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out, int write)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = write ? 0x20 : (len ? 0x40 : 0x00);	/* flags: W or R */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	/* CT=1 (UFS storage), DD: 0=none 1=write 2=read, I=1 */
	utrl[0].dw0 = (1u << 28) | ((write ? 1u : (len ? 2u : 0u)) << 25) | (1u << 24);
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out) {
		sense_dump();
		log_num("scsi_ocs", *ocs_out);
		return 2;
	}
	log_num("rsp_type", ucd.resp[0]);
	log_num("rsp_6_7", (ucd.resp[6] << 8) | ucd.resp[7]);
	log_num("rsp_0_3", be32(ucd.resp));
	log_num("rsp_4_7", be32(ucd.resp + 4));
	log_num("sense_len", be16(ucd.resp + 32));
	log_num("sense_0_3", be32(ucd.resp + 34));
	log_num("sense_4_7", be32(ucd.resp + 38));
	log_num("sense_8_b", be32(ucd.resp + 42));
	log_num("sense_c_f", be32(ucd.resp + 46));
	log_num("sense_10", be16(ucd.resp + 50));
	log_num("sense_12", be16(ucd.resp + 52));
	if (ucd.resp[0] != 0x21) {		/* RESPONSE UPIU */
		sense_dump();
		return 3;
	}
	if (*status_out) {
		sense_dump();
		return 4;
	}
	return 0;
}

/*
 * One UIC (Unipro) command. The command register is 0x90 and the arguments
 * 0x94/0x98/0x9c in the UFSHCI 2.x/3.x/4.x layout; the completion is IS
 * bit 10 and the configuration result code comes back in ARG_2 (0x98).
 * Returns the result code, or 0xffffffff on timeout.
 */
static uint32_t uic(uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
	ufs_wr(REG_IS, IS_UIC_COMP);		/* clear a stale completion */
	ufs_wr(REG_UICARG0, arg0);
	ufs_wr(REG_UICARG1, arg1);
	ufs_wr(REG_UICARG2, arg2);
	ufs_wr(REG_UICCMD, cmd);
	uint64_t start = ticks();
	while (!(ufs_rd(REG_IS) & IS_UIC_COMP)) {
		if (ticks() - start > 19200000) {	/* 1 s */
			con_hex2("UIC", "cmd=", cmd, "timeout is=", ufs_rd(REG_IS));
			return 0xffffffffu;
		}
	}
	ufs_wr(REG_IS, IS_UIC_COMP);
	uint32_t res = ufs_rd(REG_UICARG1) & 0xff;
	con_hex2("UIC", "cmd=", cmd, "res=", res);
	return res;
}

/* DME GET: the value comes back in UIC ARG_3 (0x9c). */
static uint32_t dme_get(uint32_t mib, uint32_t *val)
{
	uint32_t v = 0;
	uint32_t r = uic(UIC_DME_GET, mib << 16, 0, 0);

	v = ufs_rd(REG_UICARG2);
	if (val)
		*val = v;
	log_num("dme_get", (mib << 16) | (r & 0xff));
	return r;
}

/* DME SET: arg1 = MIB << 16, arg3 = the value. */
static uint32_t dme_set(uint32_t mib, uint32_t val)
{
	uint32_t r = uic(UIC_DME_SET, mib << 16, 0, val);

	log_num("dme_set", (mib << 16) | (r & 0xff));
	return r;
}


/* Write one 4 KiB block to LUN 0 (host to device). */
static uint32_t write_blocks(uint32_t lba, const void *buf, uint32_t count)
{
	uint8_t cdb[10] = { 0x2a, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0,
			    (uint8_t)(count >> 8), (uint8_t)count, 0 };
	uint32_t ocs, st;

	return scsi(0, cdb, sizeof(cdb), (void *)buf, count * BLOCK, &ocs, &st, 1);
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st, 0);
}

/* Wait for HCS to show the UniPro link ready (UIC_COMMAND_READY). */
static uint32_t wait_ucrdy(void)
{
	uint32_t hcs = 0;
	for (int i = 0; i < 1000000; i++) {
		hcs = ufs_rd(REG_HCS);
		if (hcs & HCS_UCRDY)
			break;
	}
	return hcs;
}

/*
 * Bring the UniPro link up again the way a host driver does it after a
 * reset: DME RESET, DME ENABLE, DME LINK STARTUP. The device answers our
 * SCSI commands with "function not supported" and an empty sense, which
 * is what a device that never saw the enable/link-startup sequence
 * looks like. Returns 0 when the link is up afterwards.
 */
static int ufs_recover_link(void)
{
	uint32_t r;

	con_str("UFS", "recovering the UniPro link (UIC)");
	r = uic(UIC_DME_RESET, 0, 0, 0);
	con_hex2("UIC", "dme_reset res=", r, "is=", ufs_rd(REG_IS));
	con_hex2("UFS", "hcs after reset=", wait_ucrdy(), "hcs=", ufs_rd(REG_HCS));
	r = uic(UIC_DME_ENABLE, 0, 0, 0);
	con_hex2("UIC", "dme_enable res=", r, "is=", ufs_rd(REG_IS));
	r = uic(UIC_DME_LINK_STARTUP, 0, 0, 0);
	con_hex2("UIC", "link_startup res=", r, "is=", ufs_rd(REG_IS));
	con_hex2("UFS", "hcs after startup=", wait_ucrdy(), "hcs=", ufs_rd(REG_HCS));
	con_hex2("UFS ERR", "phy=", ufs_rd(REG_UECPA), "dme=", ufs_rd(REG_UECDME));
	return (ufs_rd(REG_HCS) & HCS_UCRDY) ? 0 : -1;
}



/* UPIU_TRANSACTION_QUERY_REQ and the query opcodes (JESD220). */
#define UPIU_QUERY_REQ		0x16
#define QUERY_OP_READ_FLAG	0x05
#define QUERY_OP_SET_FLAG	0x06
#define QUERY_IDN_FDEVICEINIT	0x01
#define QUERY_FUNC_READ		0x01
#define QUERY_FUNC_WRITE	0x81

/*
 * One QUERY request.  The UPIU payload is "struct utp_upiu_query":
 * opcode, idn, index, selector, reserved, length, value, reserved.
 * The answer arrives in the QUERY response UPIU (type 0x36); its value
 * field sits at offset 20 (12 byte header + 8).
 */
static uint32_t query(uint8_t opcode, uint8_t idn, uint32_t value, uint32_t *rsp_value)
{
	static uint8_t tag;
	static int qlog;
	uint32_t ocs = 0, st = 0, rc, rtype, rcode;

	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	ucd.cmd[0] = UPIU_QUERY_REQ;
	ucd.cmd[1] = 0x00;
	ucd.cmd[2] = 0x00;			/* LUN */
	ucd.cmd[3] = ++tag;
	ucd.cmd[4] = 0x00;			/* command set type / IID */
	ucd.cmd[5] = opcode == QUERY_OP_READ_FLAG ? QUERY_FUNC_READ : QUERY_FUNC_WRITE;
	/*
	 * The query payload starts right behind the 12 byte UPIU header
	 * (struct utp_upiu_query): opcode, idn, index, selector, reserved,
	 * length, value.  (The command UPIU has a data length field there,
	 * the query UPIU does not.)
	 */
	ucd.cmd[12] = opcode;
	ucd.cmd[13] = idn;
	ucd.cmd[14] = 0;			/* index */
	ucd.cmd[15] = 0;			/* selector */
	ucd.cmd[18] = 0;			/* length */
	ucd.cmd[19] = 0;
	ucd.cmd[20] = value >> 24;
	ucd.cmd[21] = value >> 16;
	ucd.cmd[22] = value >> 8;
	ucd.cmd[23] = value;

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | (1u << 24);	/* CT=1 (UFS storage), DD=0, I=1 */
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			log_num("query_timeout", opcode);
			return 0xffffffffu;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");

	rc = utrl[0].dw2 & 0xff;
	rtype = ucd.resp[0];
	rcode = ucd.resp[6];
	if (rsp_value)
		*rsp_value = be32(ucd.resp + 20);
	if (qlog < 4) {
		qlog++;
		log_num("query_ocs", rc);
		log_num("query_type", rtype);
		log_num("query_rc", rcode);
	}
	(void)ocs;
	(void)st;
	return rc ? rc : rcode;
}


/*
 * The NOP OUT UPIU (type 0x00): the reference driver sends it to find out
 * whether the device has finished its own initialisation.
 */
static uint32_t nop_out(void)
{
	static uint8_t tag;
	uint32_t ocs = 0, st = 0, rc;

	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	ucd.cmd[0] = 0x00;			/* NOP OUT UPIU */
	ucd.cmd[3] = ++tag;

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | (1u << 24);	/* CT=1, DD=0, I=1 */
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			log_num("nop_out_timeout", 1);
			return 0xffffffffu;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");

	rc = utrl[0].dw2 & 0xff;
	log_num("nop_out_ocs", rc);
	log_num("nop_out_type", ucd.resp[0]);
	(void)ocs;
	(void)st;
	return rc;
}

/*
 * A QUERY READ_DESC of the device descriptor (idn 0).  The descriptor
 * follows the query answer in the response UPIU.
 */
static uint32_t read_device_descriptor(void)
{
	uint32_t v = 0, r;

	r = query(0x01, 0x00, 0, &v);		/* READ_DESC, idn 0 (device) */
	log_num("read_desc_rc", r);
	con_hex2("UFS DESC", "type=", ucd.resp[0], "code=", ucd.resp[6]);
	con_hex2("UFS DESC", "d0_3=", be32(ucd.resp + 32), "d4_7=", be32(ucd.resp + 36));
	return r;
}

/*
 * The UFS 3.0/4.0 device initialisation handshake: the host sets the
 * fDeviceInit flag and waits for the device to clear it again.  Linux does
 * this in ufshcd_complete_dev_init() before it talks to the device.
 */
static void fdeviceinit_handshake(void)
{
	uint32_t v = 0, rc;

	log_str("=== fDeviceInit handshake ===\n");
	rc = query(QUERY_OP_SET_FLAG, QUERY_IDN_FDEVICEINIT, 0, 0);
	log_num("set_flag_rc", rc);
	for (int i = 0; i < 5; i++) {
		v = 0xffffffff;
		rc = query(QUERY_OP_READ_FLAG, QUERY_IDN_FDEVICEINIT, 0, &v);
		log_num("read_flag_rc", rc);
		log_num("fdeviceinit", v);
		if (rc == 0 && !(v & 1))
			break;
	}
}

/*
 * Bring the UniPro link and the device into the state a host driver sets up
 * after a reset, then see whether the device answers SCSI commands.
 *
 * The UFS 4.0 device descriptor says bInitialPowerMode = 01h, i.e. the device
 * comes up in Sleep power mode and the host has to move it to Fast/Active
 * with a DME SET of the PA PowerMode attribute.  The reference driver
 * (Linux ufshcd_dme_change_power_mode) does:
 *   PA_RXGEAR / PA_ACTIVERXDATALANES / PA_RXTERMINATION /
 *   PA_TXGEAR / PA_ACTIVETXDATALANES / PA_TXTERMINATION / PA_HSSERIES
 *   and finally PA_PWRMODE = (FAST<<4)|FAST.
 * Steps 1..n are tried one after another, each one followed by a TUR, so a
 * single boot can find out which one the device needs.
 */
static uint32_t try_tur(const char *what)
{
	static const uint8_t tur[6] = { 0x00 };
	uint32_t ocs = 0, st = 0;
	uint32_t rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st, 0);

	log_str(what);
	log_str(":");
	log_num("  tur_rc", rc);
	log_num("  ocs_st", (ocs << 8) | st);
	return rc;
}

/*
 * Take over the transfer request list: stop, point it at our own
 * descriptors, run.  A UIC DME RESET or LINK STARTUP invalidates the
 * host side list, so this has to be redone after every link command.
 */
static int take_over_utrl(void)
{
	uintptr_t list = (uintptr_t)utrl;

	ufs_wr(REG_UTRLRSR, 0);
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return -1;
	}
	return 0;
}

static void ufs_power_mode_experiment(void)
{
	uint32_t lanes_rx = 0, lanes_tx = 0, gear_rx = 0, gear_tx = 0, v = 0;

	log_str("=== UFS 4.0 device init candidates ===\n");
	dme_get(PA_CONNECTEDRXDATALANES, &lanes_rx);
	dme_get(PA_CONNECTEDTXDATALANES, &lanes_tx);
	dme_get(PA_MAXRXHSGEAR, &gear_rx);
	dme_get(PA_MAXRXPWMGEAR, &v);
	log_num("lanes_rx", lanes_rx);
	log_num("lanes_tx", lanes_tx);
	log_num("gear_rx", gear_rx);
	dme_get(PA_PWRMODE, &v);
	log_num("pwrmode_before", v);

	try_tur("before");

	/* C1: the reference probe order: NOP OUT, fDeviceInit, descriptor,
	 * then the power mode change. */
	nop_out();
	fdeviceinit_handshake();
	read_device_descriptor();
	dme_set(PA_RXGEAR, gear_rx);
	dme_set(PA_ACTIVERXDATALANES, lanes_rx);
	dme_set(PA_RXTERMINATION, 1);
	dme_set(PA_TXGEAR, gear_tx ? gear_tx : gear_rx);
	dme_set(PA_ACTIVETXDATALANES, lanes_tx);
	dme_set(PA_TXTERMINATION, 1);
	dme_set(PA_HSSERIES, PA_HS_MODE_B);
	dme_set(PA_PWRMODE, PWRMODE_FAST);
	dme_get(PA_PWRMODE, &v);
	log_num("pwrmode_after_c1", v);
	try_tur("c1_probe_order");

	/* C2: a link startup (the link comes up in PWM-G1/1 lane) and only
	 * then the power mode change, as the reference driver does. */
	uic(UIC_DME_ENABLE, 0, 0, 0);
	uic(UIC_DME_LINK_STARTUP, 0, 0, 0);
	take_over_utrl();
	dme_set(PA_RXGEAR, gear_rx);
	dme_set(PA_ACTIVERXDATALANES, lanes_rx);
	dme_set(PA_RXTERMINATION, 1);
	dme_set(PA_TXGEAR, gear_tx ? gear_tx : gear_rx);
	dme_set(PA_ACTIVETXDATALANES, lanes_tx);
	dme_set(PA_TXTERMINATION, 1);
	dme_set(PA_HSSERIES, PA_HS_MODE_B);
	dme_set(PA_PWRMODE, PWRMODE_FAST);
	dme_get(PA_PWRMODE, &v);
	log_num("pwrmode_after_c2", v);
	try_tur("c2_link_startup");

	/* C3: the same but asking for slow/fast instead of fast/fast. */
	dme_set(PA_PWRMODE, 0x22);
	dme_get(PA_PWRMODE, &v);
	log_num("pwrmode_after_c3", v);
	try_tur("c3_pwrmode_22");

	/* C4: a full reset and the complete bring up again. */
	uic(UIC_DME_RESET, 0, 0, 0);
	uic(UIC_DME_ENABLE, 0, 0, 0);
	uic(UIC_DME_LINK_STARTUP, 0, 0, 0);
	take_over_utrl();
	nop_out();
	fdeviceinit_handshake();
	dme_set(PA_RXGEAR, gear_rx);
	dme_set(PA_ACTIVERXDATALANES, lanes_rx);
	dme_set(PA_RXTERMINATION, 1);
	dme_set(PA_TXGEAR, gear_rx);
	dme_set(PA_ACTIVETXDATALANES, lanes_tx);
	dme_set(PA_TXTERMINATION, 1);
	dme_set(PA_HSSERIES, PA_HS_MODE_B);
	dme_set(PA_PWRMODE, PWRMODE_FAST);
	try_tur("c4_full_reset");

	/* C5: START STOP UNIT with the active power condition on the well
	 * known LUN (UPIU LUN 0xd0). */
	{
		static const uint8_t ssu[6] = { 0x1e, 0, 0, 0, 0x00, 0 };
		uint32_t ocs = 0, st = 0;
		uint32_t rc = scsi(0xd0, ssu, sizeof(ssu), 0, 0, &ocs, &st, 0);

		log_num("ssu_wlun_rc", rc);
		log_num("ssu_wlun_st", (ocs << 8) | st);
		try_tur("c5_start_stop_unit");
	}
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

static uint32_t cache_first, cache_last;	/* the scratch ext4 partition we log into */

/* The last 4 KiB block of the "cache" partition (sda29, 128 MiB at LBA
 * 176128): a scratch area the recovery can read back with dd. */
#define LOG_LBA		208895u

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!strcmp(name, "cache")) {
			cache_first = (uint32_t)first;
			cache_last = (uint32_t)last;
		}
		if (!strcmp(name, "userdata") || !strcmp(name, "cust") ||
		    !strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	con_hex2("UFS", "pid=", ufs_rd(REG_PID), "mid=", ufs_rd(REG_MID));
	con_hex2("UFS", "mcqcap=", ufs_rd(REG_MCQCAP), "extcap=", ufs_rd(REG_EXTCAP));
	/* The UIC error codes say why the last UIC command failed. */
	con_hex2("UFS ERR", "phy=", ufs_rd(REG_UECPA), "dlink=", ufs_rd(REG_UECDL));
	con_hex2("UFS ERR", "net=", ufs_rd(REG_UECN), "trans=", ufs_rd(REG_UECT));
	con_hex2("UFS ERR", "dme=", ufs_rd(REG_UECDME), "ie=", ufs_rd(REG_IE));

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	if (take_over_utrl())
		return;

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st, 0);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	log_num("tur_rc", rc);
	log_num("tur_st", ocs << 8 | st);

	/*
	 * The device answers but refuses every command: it is still in the
	 * Sleep power mode it came up in (bInitialPowerMode = 01h).  Walk
	 * through the candidate ways of waking it up.
	 */
	if (rc) {
		ufs_power_mode_experiment();
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st, 0);
		con_hex2("UFS TUR2", "rc=", rc, "ocs/st=", ocs << 8 | st);
		log_num("tur2_rc", rc);
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st, 0);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st, 0);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		goto log_it;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		goto log_it;
	list_partitions(elba, nent);
log_it:

	/*
	 * Publish the log into the scratch "cache" partition so that it can be
	 * read back from the recovery (the phone resets into the recovery with
	 * the boot reason it was started with).
	 */
	if (1) {
		uint8_t *blk = data;
		uint32_t ocs = 0, st = 0, wrc;
		unsigned n = loglen < 4 * BLOCK - 1 ? loglen : 4 * BLOCK - 1;

		for (unsigned i = 0; i < 4 * BLOCK; i++)
			blk[i] = 0;
		for (unsigned i = 0; i < n; i++)
			blk[i] = logbuf[i];
		wrc = write_blocks(LOG_LBA, blk, 4);
		con_hex2("UFS LOG", "lba=", LOG_LBA, "rc=", wrc);
		log_num("log_write_rc", wrc);
	}
	con_str("UFS", "log written to cache partition");
}

/* The whole log on the panel, 48 chars per line. */
void ufs_print_log(void)
{
	unsigned i = 0;

	con_clear(YELLOW);
	con_str("UFSLOG", "experiment log");
	while (i < loglen) {
		unsigned j = i;
		char line[49];

		while (j < loglen && j - i < 48 && logbuf[j] != '\n')
			j++;
		unsigned n = j - i;
		if (n > 48)
			n = 48;
		for (unsigned k = 0; k < n; k++)
			line[k] = logbuf[i + k];
		line[n] = 0;
		con_str("", line);
		if (j < loglen && logbuf[j] == '\n')
			j++;
		i = j;
	}
}

