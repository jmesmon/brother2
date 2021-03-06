#define BACKEND_NAME bro2
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef SANE_DLL
#include "sane/sanei_backend.h"
#endif

#include "sane/sanei_debug.h"

#include <stddef.h>
#include <sane/sane.h>
#include <sane/saneopts.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <netdb.h>

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <penny/mem.h>
#include <penny/math.h>
#include <penny/print.h>

#include <ccan/net/net.h>
#include <ccan/array_size/array_size.h>
#include <ccan/list/list.h>

#include "bro2.h"

#define memstr(haystack, h_size, needle_str) memmem(haystack, h_size, needle_str, strlen(needle_str))

#if 0
#ifndef DBG
#ifndef NDEBUG
#include <stdio.h>
#define DBG(n, ...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(n, ...)
#endif
#endif
#endif

static SANE_Auth_Callback auth;

#define SETTING_STR_LEN 8

enum opts {
	/* Integer options */
	OPT_NUM,
	OPT_X_RES,
	OPT_Y_RES,
	OPT_TL_X,
	OPT_TL_Y,
	OPT_BR_X,
	OPT_BR_Y,
	OPT_B,
	OPT_C,
	/* String options */
	OPT_FIRST_STR,
	OPT_MODE = OPT_FIRST_STR,
	OPT_COMPRESS,
	OPT_D
};

struct bro2_device {
	struct list_head list;
	int fd;
	const char *addr;
	struct addrinfo *res;

	/* settings */
	union {
		struct {
			int x_res, y_res;
			int tl_x, tl_y, br_x, br_y;
			int brightness, contrast;
		};
		int int_opts[OPT_FIRST_STR];
	};

	union {
		struct {
			char mode[SETTING_STR_LEN];
			char compress[SETTING_STR_LEN];
			char d[SETTING_STR_LEN];
		};
		char str_opts[OPT_D - OPT_FIRST_STR + 1][SETTING_STR_LEN];
	};

	SANE_Parameters param;

	size_t line_buffer_pos;
	uint8_t line_buffer[BRO2_MAX_LINE_MSG_SZ]; /* ~64kbytes, ~16 pages*/
};

SANE_Status sane_init(SANE_Int *ver, SANE_Auth_Callback authorize)
{
	if (ver)
		*ver = SANE_VERSION_CODE(SANE_CURRENT_MAJOR, 0, 0);
	auth = authorize;
	DBG_INIT();
	return SANE_STATUS_GOOD;
}

void sane_exit(void)
{
	/* TODO: required that all allocations are freed */
}

static const char *vendor_str = "Brother";
static const char *type_str = "flatbed scanner";
static size_t num_devs = 0;
static SANE_Device **device_list = NULL;

static int add_device(SANE_Device *d)
{
	device_list = realloc(device_list, sizeof(*device_list) * (num_devs + 2));
	if (!device_list)
		return -1;

	device_list[num_devs] = d;
	device_list[num_devs + 1] = NULL;
	num_devs ++;

	return 0;
}

static SANE_Device *_new_device(const char *host, const char *model)
{
	SANE_Device *d = malloc(sizeof(*d));
	if (!d)
		return NULL;

	d->name = strdup(host);
	if (!d->name)
		goto c1;
	d->model = strdup(model);
	if (!d->model)
		goto c2;
	d->vendor = vendor_str;
	d->type = type_str;

	return d;
c2:
	free((char *)d->name);
c1:
	free(d);
	return NULL;
}

static int new_device(const char *host, const char *model)
{
	SANE_Device *d = _new_device(host, model);
	if (!d)
		return -1;

	return add_device(d);
}

static int bro2_snmp_async_cb(int operation, struct snmp_session *sp, int reqid,
			struct snmp_pdu *pdu, void *data)
{
	if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
		DBG(2, "snmp timeout\n");
		return 1;
	}

	int ix;
	char buf[1024];
	struct variable_list *vp;
	char host[128], serv[128];
	netsnmp_indexed_addr_pair *addr_pair = pdu->transport_data;

	if (sizeof(*addr_pair) != pdu->transport_data_length) {
		DBG(1, "unexpected transport data len: got %d, want %zu\n",
				pdu->transport_data_length, sizeof(*addr_pair));
		return 1;
	}

	int r = getnameinfo(&addr_pair->remote_addr.sa, sizeof(addr_pair->remote_addr),
		host, sizeof(host), serv, sizeof(serv),
		NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
	if (r != 0) {
		DBG(1, "getnameinfo failed: %s\n", gai_strerror(r));
		return 1;
	}

	DBG(10, "if_index: %d host: %s serv: %s\n", addr_pair->if_index, host, serv);

	vp = pdu->variables;

	if (pdu->errstat != SNMP_ERR_NOERROR) {
		/* FIXME: Some type of error occured??? */
		for (ix = 1; vp && ix != pdu->errindex; vp = vp->next_variable, ix++)
			;
		if (vp)
			snprint_objid(buf, sizeof(buf), vp->name, vp->name_length);
		else
			strcpy(buf, "(none)");
		DBG(1, "%s: %s: %s\n",
				host, buf, snmp_errstring(pdu->errstat));
		return 1;
	}

	struct variable_list *vp_tmp = vp;
	while (vp) {
		snprint_variable(buf, sizeof(buf), vp->name, vp->name_length, vp);
		DBG(1, "%s: (type=%d)  %s\n", host, vp->type, buf);
		vp = vp->next_variable;
	}
	vp = vp_tmp;

	if (!vp)
		goto non_bro2;

	if (vp->type != 4)
		goto non_bro2;

	/* OK, that is enough checking for now, add a device */
	char *maybe_model = memstr(vp->val.string, vp->val_len, ";MDL:");
	const char *model;
	char mbuf[256];
	if (!maybe_model)
		model = "UNKNOWN";
	else {
		maybe_model = maybe_model + 5;
		char *end = memchr(maybe_model, ';', vp->val_len - (maybe_model - (char *)vp->val.string));
		if (!end)
			end = (char *)vp->val.string + vp->val_len;

		size_t model_len = MIN(sizeof(mbuf) - 1, end - maybe_model - 1);
		memcpy(mbuf, maybe_model, model_len);
		mbuf[model_len] = '\0';
		model = mbuf;
	}

	new_device(host, model);

non_bro2:
	return 1;
}

static void bro2_snmp_probe_all(void)
{
	struct snmp_session session, *ss;
	struct snmp_pdu *pdu;

	/* TODO: figure out if we can reuse these between snmp_add_null_var()
	 * calls */
	oid init_oids[5][MAX_OID_LEN];
	size_t init_oid_lens[5];

	const char *oid_strs[5] = {
		/* SNMPv2-SMI::enterprises.2435.2.3.9.1.1.7.0
		 * Brother specific
		 * STRING: "MFG:Brother;CMD:PJL,PCL,PCLXL,POSTSCRIPT;MDL:MFC-7820N;CLS:PRINTER;"
		 */
		".1.3.6.1.4.1.2435.2.3.9.1.1.7.0",
		/* SNMPv2-MIB::sysName.0 = STRING: BRN_HIJKLM */
		".1.3.6.1.2.1.1.5.0",
		/* IF-MIB::ifPhysAddress.1 = STRING: 0:80:77:HI:JK:LM
		 * MAC Address, note corespondance with sysName.0 ("HIJKLM") */
		".1.3.6.1.2.1.2.2.1.6.1",
		/* SNMPv2-SMI::enterprises.2435.2.4.3.1240.1.3.0
		 * Brother specific, meaning unknown.
		 * INTEGER: 2198 */
		".1.3.6.1.4.1.2435.2.4.3.1240.1.3.0",
		/* SNMPv2-MIB::sysDescr.0 =
		 * STRING: Brother NC-6200h, Firmware Ver.H  ,MID 8C5-A15,FID 2
		 */
		".1.3.6.1.2.1.1.1.0"
	};

	bool timed_out = false;

	init_snmp("brother2");
	snmp_sess_init(&session);

	session.peername = (char *)"255.255.255.255";
	session.flags |= SNMP_FLAGS_UDP_BROADCAST;
	session.version = SNMP_VERSION_1;
	session.community = (unsigned char *)"public";
	session.community_len = strlen((char *)session.community);

	SOCK_STARTUP;

	ss = snmp_open(&session);
	if (!ss) {
		snmp_perror("ack");
		snmp_log(LOG_ERR, "failed to open session\n");
		goto out_setup;
	}

	pdu = snmp_pdu_create(SNMP_MSG_GET);

	int i;
	for (i = 0; i < ARRAY_SIZE(oid_strs); i++) {
		init_oid_lens[i] = MAX_OID_LEN;
		read_objid(oid_strs[i], init_oids[i], &init_oid_lens[i]);
		snmp_add_null_var(pdu, init_oids[i], init_oid_lens[i]);
	}

	int reqid = snmp_async_send(ss, pdu, bro2_snmp_async_cb, &timed_out);
	if (reqid == 0) {
		DBG(1, "failed to send broadcast snmp\n");
		goto out_close;
	}

	DBG(4, "async send reqid = %d\n", reqid);

	/* FIXME: netsnmp doesn't know how to handle reciving multiple
	 * responses from a single packet.
	 * - Indicating "failure" in the callback means that
	 *   snmp_resend_request() gets called until the retries are used up.
	 * - Indicating "success" or having all the retries used up results in
	 *   the request being destroyed and the pdu being freed.
	 *
	 */
	time_t endtime = time(NULL) + 2;
	while (time(NULL) < endtime) {
		int fds = 0, block = 0;
		fd_set fdset;
		struct timeval timeout = { .tv_usec = 5000 };
		FD_ZERO(&fdset);
		snmp_select_info(&fds, &fdset, &timeout, &block);
		fds = select(fds, &fdset, NULL, NULL, &timeout);
		if (fds)
			snmp_read(&fdset);
		else
			snmp_timeout(); /* calls the callback if timeout has occured. */
	}

out_close:
	snmp_close(ss);
out_setup:
	SOCK_CLEANUP;
	return;
}

static void free_device_list(void)
{
	if (!device_list)
		return;

	free(device_list);
}

/* return an empty list of detected devices */
SANE_Status sane_get_devices(const SANE_Device ***dev_list,
			     SANE_Bool local_only)
{
	free_device_list();
	DBG_INIT();

	if (local_only)
		return SANE_STATUS_GOOD;

	errno = 0;
	bro2_snmp_probe_all();
	*dev_list = (const SANE_Device **)device_list;

	if (errno == ENOMEM)
		return SANE_STATUS_NO_MEM;
	return SANE_STATUS_GOOD;
}

static int bro2_connect(struct bro2_device *dev)
{
	/* Hook up the connection */
	struct addrinfo *res = net_client_lookup(dev->addr, BRO2_PORT_STR,
						AF_UNSPEC, SOCK_STREAM);

	if (!res) {
		DBG(1, "failed to resolve %s\n",
				dev->addr);
		return -1;
	}

	int fd = net_connect(res);

	dev->fd = fd;
	dev->res = res;

	if (fd == -1) {
		return -2;
	}
	return 0;
}

static void bro2_set_area(struct bro2_device *dev)
{
}

static void bro2_init(struct bro2_device *dev, const char *addr)
{
	*dev = (typeof(*dev)) {
		.fd = -1,
		.addr = addr,

		/* defaults */
		.x_res = 300,
		.y_res = 300,
		.brightness = 50,
		.contrast = 50,
		.mode = "CGREY",
		.d = "SIN",
		.compress = "NONE",

		.param = {
			.format = SANE_FRAME_GRAY, /* XXX: fixed to this for debugging */
			.last_frame = SANE_FALSE,  /* will need to be modified
						      to switch when scan is
						      complete */
			.bytes_per_line = 0,  /* FIXME: unknown */
			.pixels_per_line = 0, /* FIXME: unknown */
			.lines = -1, /* -1 == unknown, call sane_read() until SANE_STATUS_EOF */
			.depth = 1,	      /* Or 8? */
		},
	};
}

/* Must be called immediately after connecting, status is only sent at that
 * time. 
 * Returns a positive status, or a negative error code.
 * */
static int bro2_read_status(struct bro2_device *dev)
{
	char buf[512];
	ssize_t r = read(dev->fd, buf, sizeof(buf) - 1);

	if (r == 0) {
		DBG(1, "eof?\n");
		return -1;
	} else if (r == -1) {
		DBG(1, "error?\n");
		return -1;
	}

	buf[r] = '\0';

	if (r <= 4) {
		DBG(1, "String too short.\n");
		return -1;
	}

	bool not_good = false;
	if (memstarts_str(buf, r, "-NG ")) {
		not_good = true;
	} else if (!memstarts_str(buf, r, "+OK ")) {
		DBG(1, "Status string not \"+OK\" or \"+NG\" => \"%.*s\"\n", (int)(r - 2), buf);
		return -1;
	}

	char *end;
	errno = 0;
	long it = strtol(buf + 4, &end, 10);

	if (errno) {
		DBG(1, "Could not get number.\n");
		return -1;
	}

	if (it < 0) {
		DBG(1, "negative status: %ld\n", it);
	}

	if (strncmp("\r\n", end, 2)) {
		DBG(1, "not terminated properly.\n");
		return -1;
	}

	if (not_good && it == 200)
		return 9001;

	return it;
}

static int bro2_send_I(struct bro2_device *dev)
{
	char buf[512];
	int l = snprintf(buf, sizeof(buf),
			"\x1bI\n"
			"R=%u,%u\n"
			"M=%s\n"
			"\x80",
			dev->x_res, dev->y_res, dev->mode);

	if (l > sizeof(buf)) {
		DBG(1, "too much for buffer.\n");
		return -1;
	}

	ssize_t r = write(dev->fd, buf, l);
	if (r != l) {
		DBG(1, "write failed.\n");
		return -1;
	}

	return 0;
}

static void bro2_update_param(struct bro2_device *dev)
{
	dev->param.pixels_per_line = dev->br_x - dev->tl_x;
	dev->param.bytes_per_line  = dev->param.pixels_per_line * dev->param.depth;
}

static int bro2_send_X(struct bro2_device *dev)
{
	char buf[512];
	int l = snprintf(buf, sizeof(buf),
			"\x1bX\n"
			"R=%u,%u\n"
			"M=%s\n"
			"C=%s\n"
			"B=%u\n"
			"N=%u\n"
			"A=%u,%u,%u,%u"
			"D=%s\n"
			"\x80",
			dev->x_res, dev->y_res,
			dev->mode,
			dev->compress,
			dev->brightness,
			dev->contrast,
			dev->tl_x, dev->tl_y, dev->br_x, dev->br_y,
			dev->d
			);

	if (l > sizeof(buf)) {
		DBG(1, "too much for buffer.\n");
		return -1;
	}

	ssize_t r = write(dev->fd, buf, l);
	if (r != l) {
		DBG(1, "write failed: %zd %s\n", r, strerror(errno));
		return -1;
	}

	bro2_update_param(dev);

	return 0;
}

static int bro2_send_R(struct bro2_device *dev)
{
	char buf[] = "\x1bR\n";
	ssize_t r = write(dev->fd, buf, sizeof(buf) - 1);
	if (r != (sizeof(buf) - 1)) {
		DBG(1, "write failed: %zd %s\n", r, strerror(errno));
		return -1;
	}

	return 0;
}

/* Parses a comma seperated list of integers, which is terminated by
 * a newline or the end of the buffer */
static int parse_num_list(void const *v_buf, size_t buf_bytes, int *nums,
		size_t num_ct, char **end)
{
	char const *buf = v_buf;
	int n = 0, i = 0, p = 0;
	while (p < buf_bytes && i < num_ct) {
		int c = buf[p];
		p++;

		if (c == ',') {
			nums[i] = n;
			i ++;
			n = 0;
			continue;
		}

		if (c == '\n') {
			break;
		}

		if (!isdigit(c)) {
			DBG(1, "killed on a %x '%c' (%d %zu) \n", c, c, p, buf_bytes);
			return -1;
		}

		n *= 10;
		n += c - '0';
	}

	if (i < num_ct) {
		nums[i] = n;
		i++;
	}

	*end = (char *)buf + p;

	return i;
}

static int bro2_recv_I_response(struct bro2_device *dev)
{
	char buf[512];
	/* FIXME: timeout at some point. */
	ssize_t l = read(dev->fd, buf, sizeof(buf) - 1);

	if (l <= 0) {
		DBG(1, "error in read\n");
		return -1;
	}

	if (buf[0] != 0x1b) {
		DBG(1, "wrong start byte: %c %x\n", buf[0], buf[0]);
		return -1;
	}

	if (buf[1] != 0x00) {
		DBG(1, "wrong 2nd byte: %c %x\n", buf[1], buf[1]);
		return -1;
	}

	int nums[7];
	char *end;
	int c = parse_num_list(buf + 2, l - 2, nums, 7, &end);
	if (c != 7) {
		DBG(1, "Did not get enough numbers: %d\n", c);
		return -1;
	}

	DBG(1, "Nums: %d %d %d %d %d %d %d\n",
			nums[0],nums[1],nums[2],nums[3],nums[4],nums[5],nums[6]);

	if (end != buf + l) {
		DBG(1, "end doesn't match up.\n");
		return -1;
	}

	/* Fixup the resolution based on info */
	dev->x_res = nums[BRO2_MSG_I_XRES];
	dev->y_res = nums[BRO2_MSG_I_YRES];

	dev->br_x = nums[BRO2_MSG_I_MAX_X];
	dev->br_y = nums[BRO2_MSG_I_MAX_Y];

	return 0;
}

static int bro2_connect_and_get_status(struct bro2_device *dev)
{
	int r = bro2_connect(dev);
	if (r)
		return SANE_STATUS_IO_ERROR;

	r = bro2_read_status(dev);
	if (r == 401) {
		/* should we retry? */
		return SANE_STATUS_DEVICE_BUSY;
	} else if (r != 200) {
		return SANE_STATUS_IO_ERROR;
	}

	return 0;
}

#define STR(x) STR_(x)
#define STR_(x) #x

SANE_Status sane_open(SANE_String_Const name, SANE_Handle *h)
{
	struct bro2_device *dev = malloc(sizeof(*dev));
	*h = dev;

	bro2_init(dev, name);
	int r = bro2_connect_and_get_status(dev);
	if (r)
		return r;

	return SANE_STATUS_GOOD;
}

void sane_close(SANE_Handle h)
{}

#define SANE_STR(thing)		\
	.name = SANE_NAME_##thing,	\
	.title = SANE_TITLE_##thing,	\
	.desc = SANE_DESC_##thing

static SANE_Range range_percent = {
	.min = 0,
	.max = 100,
	.quant = 1
};

#define OPT_PX_CORD(it)				\
	SANE_STR(SCAN_##it),			\
	.type = SANE_TYPE_INT,			\
	.unit = SANE_UNIT_PIXEL,		\
	.constraint_type = SANE_CONSTRAINT_NONE,\
	.cap = SANE_CAP_SOFT_SELECT

static SANE_Option_Descriptor mfc7820n_opts [] = {
	{
		SANE_STR(SCAN_X_RESOLUTION),
		/* 300, ??? */
		.type = SANE_TYPE_INT,
		.unit = SANE_UNIT_DPI,
		.size = sizeof(SANE_Int),
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_NONE,
	}, {
		SANE_STR(SCAN_Y_RESOLUTION),
		.type = SANE_TYPE_INT,
		.unit = SANE_UNIT_DPI,
		.size = sizeof(SANE_Int),
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_NONE,
	}, {
		OPT_PX_CORD(TL_X),
	}, {
		OPT_PX_CORD(TL_Y),
	}, {
		OPT_PX_CORD(BR_X),
	}, {
		OPT_PX_CORD(BR_Y),
	}, {
		SANE_STR(BRIGHTNESS),
		.type = SANE_TYPE_INT,
		.unit = SANE_UNIT_NONE,
		.size = sizeof(SANE_Int),
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_RANGE,
		.constraint = { .range = &range_percent }
	}, {
		SANE_STR(CONTRAST),
		.type = SANE_TYPE_INT,
		.unit = SANE_UNIT_NONE,
		.size = sizeof(SANE_Int),
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_RANGE,
		.constraint = { .range = &range_percent }
	}, {
		SANE_STR(SCAN_MODE),
		.type = SANE_TYPE_STRING,
		.unit = SANE_UNIT_NONE,
		.size = SETTING_STR_LEN,
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_NONE,
	}, {
		.name = "compession",
		.title = "Image Compression Type",
		.desc = "Image Compression Type. NONE or RLENGTH or JPEG",
		.type = SANE_TYPE_STRING,
		.unit = SANE_UNIT_NONE,
		.size = SETTING_STR_LEN,
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_NONE,
	}, {
		.name = "d",
		.title = "D value",
		.desc = "The D value. Only \"SIN\" has been observed.",
		.type = SANE_TYPE_STRING,
		.unit = SANE_UNIT_NONE,
		.size = SETTING_STR_LEN,
		.cap = SANE_CAP_SOFT_SELECT,
		.constraint_type = SANE_CONSTRAINT_NONE,
	}
};

static SANE_Option_Descriptor num_opts_opt = {
	.name = "",
	.title = SANE_TITLE_NUM_OPTIONS,
	.desc = SANE_DESC_NUM_OPTIONS,
	.type = SANE_TYPE_INT,
	.unit = SANE_UNIT_NONE,
	.size = sizeof(SANE_Word),
	.cap = SANE_CAP_SOFT_DETECT,
	.constraint_type = SANE_CONSTRAINT_NONE,
	.constraint = { .range = 0 },
};
const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle h, SANE_Int n)
{
	if (n == 0)
		return &num_opts_opt;

	n--;

	if (n < ARRAY_SIZE(mfc7820n_opts)) {
		return &mfc7820n_opts[n];
	}

	return NULL;
}

SANE_Status sane_control_option(SANE_Handle h, SANE_Int n, SANE_Action a, void *v, SANE_Int *i)
{
	if (i) *i = 0;
	struct bro2_device *dev = h;

	switch (a) {
	case SANE_ACTION_GET_VALUE:
		switch (n) {
		case OPT_NUM:
			*(SANE_Int *)v = ARRAY_SIZE(mfc7820n_opts) + 1;
			break;
		case OPT_X_RES:
		case OPT_Y_RES:
		case OPT_TL_X:
		case OPT_TL_Y:
		case OPT_BR_X:
		case OPT_BR_Y:
		case OPT_B:
		case OPT_C:
			*(SANE_Int *)v = dev->int_opts[n-1];
			break;
		case OPT_MODE:
		case OPT_COMPRESS:
		case OPT_D:
			strcpy(v, dev->str_opts[n-OPT_FIRST_STR]);
			break;
		default:
			return SANE_STATUS_INVAL;
		}
		return SANE_STATUS_GOOD;
	case SANE_ACTION_SET_VALUE:
		switch (n) {
		case OPT_X_RES:
		case OPT_Y_RES:
		case OPT_TL_X:
		case OPT_TL_Y:
		case OPT_BR_X:
		case OPT_BR_Y:
		case OPT_B:
		case OPT_C:
			dev->int_opts[n-1] = *(SANE_Int *)v;
			break;
		case OPT_MODE:
		case OPT_COMPRESS:
		case OPT_D:
			strcpy(dev->str_opts[n-OPT_FIRST_STR], v);
			break;
		default:
			return SANE_STATUS_INVAL;
		}
		return SANE_STATUS_GOOD;
	case SANE_ACTION_SET_AUTO:
		return SANE_STATUS_INVAL;
	}

	return SANE_STATUS_IO_ERROR;
}

SANE_Status sane_get_parameters(SANE_Handle h, SANE_Parameters *p)
{
	struct bro2_device *dev = h;
	*p = dev->param;
	return SANE_STATUS_GOOD;
}

SANE_Status sane_start(SANE_Handle h)
{
#if 0
SANE STATUS CANCELLED: The operation was cancelled through a call to sane cancel.
SANE STATUS DEVICE BUSY: The device is busy. The operation should be retried later.
SANE STATUS JAMMED: The document feeder is jammed.
SANE STATUS NO DOCS: The document feeder is out of documents.
SANE STATUS COVER OPEN: The scanner cover is open.
SANE STATUS IO ERROR: An error occurred while communicating with the device.
SANE STATUS NO MEM: An insufficent amount of memory is available.
SANE STATUS INVAL: The scan cannot be started with the current set of options. The fron-
tend should reload the option descriptors, as if SANE INFO RELOAD OPTIONS had been
returned from a call to sane control option(), since the device’s capabilities may
have changed.
#endif

	struct bro2_device *dev = h;
	/* negotiate parameters */
	int r = bro2_send_I(dev);

	if (r) {
		DBG(1, "send I failed\n");
		return SANE_STATUS_IO_ERROR;
	}

	r = bro2_recv_I_response(dev);
	if (r) {
		DBG(1, "handle I resp failed\n");
		return SANE_STATUS_IO_ERROR;
	}

	r = bro2_send_X(dev);
	if (r) {
		DBG(1, "send X failed\n");
		return SANE_STATUS_IO_ERROR;
	}

	return SANE_STATUS_GOOD;
}

SANE_Status sane_read(SANE_Handle h, SANE_Byte *buf, SANE_Int maxlen, SANE_Int *len)
{
#if 0
SANE STATUS CANCELLED: The operation was cancelled through a call to sane cancel.
SANE STATUS EOF: No more data is available for the current frame.
SANE STATUS JAMMED: The document feeder is jammed.
SANE STATUS NO DOCS: The document feeder is out of documents.
SANE STATUS COVER OPEN: The scanner cover is open.
SANE STATUS IO ERROR: An error occurred while communicating with the device.
SANE STATUS NO MEM: An insufficent amount of memory is available.
SANE STATUS ACCESS DENIED: Access to the device has been denied due to insufficient
or invalid authentication.
#endif
	struct bro2_device *dev = h;

	DBG(1, "reading...\n");
	ssize_t r = read(dev->fd, dev->line_buffer + dev->line_buffer_pos, sizeof(dev->line_buffer) - dev->line_buffer_pos);
	DBG(1, "done reading, got %zd bytes\n", r);

	if (r == -1) {
		switch (errno) {
		case EAGAIN:
			/* apparently we are non-blocking */
			*len = 0;
			return SANE_STATUS_GOOD;
		default:
			DBG(1, "sane_read fail: %d %s\n", errno, strerror(errno));
			return SANE_STATUS_IO_ERROR;
		}
	} else if (r == 0) {
		/* we've been disconnected, probably */
		close(dev->fd);
		dev->fd = -1;
		return SANE_STATUS_IO_ERROR;
	}

	dev->line_buffer_pos += r;
	print_hex_dump(dev->line_buffer, dev->line_buffer_pos, stdout);
	putchar('\n');

	if (dev->line_buffer_pos < 3) {
		/* not enough data */
		*len = 0;
		return SANE_STATUS_GOOD;
	}

	/* Determine the type */
	DBG(1, "line type: %d\n", dev->line_buffer[0]);
	uint16_t line_len = dev->line_buffer[1] + (dev->line_buffer[2] << 8);
	DBG(1, "line length: %d\n", line_len);

	if (dev->line_buffer_pos - 3 < line_len) {
		/* not enough data */
		*len = 0;
		return SANE_STATUS_GOOD;
	}

	/* enough data, write it out */

	if (dev->line_buffer_pos - 3 > maxlen) {
		/* not enough space */
		*len = 0;
		return SANE_STATUS_NO_MEM;
	}

	memcpy(buf, dev->line_buffer + 3, dev->line_buffer_pos - 3);

	*len = dev->line_buffer_pos - 3;
	return SANE_STATUS_GOOD;
}

void sane_cancel(SANE_Handle h)
{
	/* TODO: close & reopen? */
	struct bro2_device *dev = h;
	if (dev->fd != -1) {
		bro2_send_R(dev);
		close(dev->fd);
		dev->fd = -1;
	}
}

SANE_Status sane_set_io_mode(SANE_Handle h, SANE_Bool m)
{
#if 0
SANE STATUS INVAL: No image acquisition is pending.
SANE STATUS UNSUPPORTED: The backend does not support the requested I/O mode.
#endif
	struct bro2_device *dev = h;
	if (dev->fd == -1)
		return SANE_STATUS_INVAL;

	if (m == SANE_FALSE)
		return SANE_STATUS_GOOD;

	return SANE_STATUS_UNSUPPORTED;
}

SANE_Status sane_get_select_fd(SANE_Handle h, SANE_Int *fd)
{
#if 0
SANE STATUS INVAL: No image acquisition is pending.
SANE STATUS UNSUPPORTED: The backend does not support this operation.
#endif
	struct bro2_device *dev = h;
	if (dev->fd == -1) {
		return SANE_STATUS_INVAL;
	}

	*fd = dev->fd;
	return SANE_STATUS_GOOD;
}


