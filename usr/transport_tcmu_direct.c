/*
 * TCMU direct-ring transport backend for mhvtl (PROTOTYPE)
 *
 * Starts like transport_tcmu.c: connects to mhvtl_tcmu_handler, which
 * creates the backstore, owns netlink and maps the LUN, relaying the
 * CDBs that arrive during LUN mapping over the socket. A tape daemon
 * asks for a hand-off in MSG_REGISTER; once the LUN is mapped the
 * handler sends MSG_HANDOFF with the UIO fd attached (SCM_RIGHTS).
 * target_core_user allows one open() of the UIO device, so the fd is
 * passed rather than re-opened. From then on this process works on the
 * command ring directly: no socket, one copy each way.
 *
 * MHVTL_TCMU_DIRECT=backoff  ring checked from the existing poll loop
 * MHVTL_TCMU_DIRECT=block    poll() on the UIO fd when the ring is empty
 *
 * License: GPLv2+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#define __LINUX_UIO_H
#include <linux/target_core_user.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "logging.h"
#include "vtllib.h"
#include "transport.h"
#include "tcmu_proto.h"

#define MAX_TCMU_DEVICES 64
#define BLOCK_POLL_MS 100

enum { MODE_RELAY, MODE_BACKOFF, MODE_BLOCK };

struct tdir_state {
	int sock_fd;
	int active;
	int direct;
	int uio_fd;
	struct tcmu_mailbox *mb;
	size_t map_len;
	struct tcmu_cmd_entry *cur;
	struct iovec *iov;
	uint32_t iov_cnt;
	uint32_t iov_max;
	int got_data;
	char bs_name[32];
};

static struct tdir_state tdir_devices[MAX_TCMU_DEVICES];
static int tdir_mode = MODE_RELAY;

static int sock_send(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

static int sock_recv(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0) return -1;
		p += n; len -= n;
	}
	return 0;
}

/* Receive one fixed-size message, picking up a passed fd if present. */
static int sock_recv_fd(int fd, void *buf, size_t len, int *passed_fd)
{
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	struct cmsghdr *c;
	ssize_t n;

	*passed_fd = -1;
	n = recvmsg(fd, &mh, 0);
	if (n <= 0)
		return -1;
	for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(passed_fd, CMSG_DATA(c), sizeof(int));
	if ((size_t)n < len)
		return sock_recv(fd, (uint8_t *)buf + n, len - n);
	return 0;
}

static int cdb_length(const uint8_t *cdb)
{
	switch (cdb[0] >> 5) {
	case 0: return 6;
	case 1: case 2: return 10;
	case 4: return 16;
	case 5: return 12;
	default: return 16;
	}
}

static int take_handoff(struct tdir_state *st, int fd, size_t map_len)
{
	void *map;

	map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		MHVTL_ERR("[%s] mmap UIO (%zu bytes): %s", st->bs_name,
			  map_len, strerror(errno));
		close(fd);
		return -1;
	}
	if (tdir_mode == MODE_BLOCK)
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	st->uio_fd = fd;
	st->mb = map;
	st->map_len = map_len;
	st->direct = 1;
	MHVTL_LOG("[%s] direct ring: map %zu bytes, cmdr %u bytes, mode %s",
		  st->bs_name, map_len, st->mb->cmdr_size,
		  tdir_mode == MODE_BLOCK ? "block" : "backoff");
	return 0;
}

static int tdir_open(unsigned minor, struct mhvtl_ctl *ctl)
{
	struct tdir_state *st;
	struct sockaddr_un addr;
	int fd, dev_type, waited;

	if (minor >= MAX_TCMU_DEVICES)
		return -1;
	st = &tdir_devices[minor];
	if (st->active)
		return -1;

	extern char mhvtl_driver_name[];
	if (strstr(mhvtl_driver_name, "library")) {
		snprintf(st->bs_name, sizeof(st->bs_name), "lib%u", minor);
		dev_type = 8;
	} else {
		snprintf(st->bs_name, sizeof(st->bs_name), "tape%u", minor);
		dev_type = 1;
	}

	fd = -1;
	for (waited = 0; waited < 30; waited++) {
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, TCMU_SOCK_PATH, sizeof(addr.sun_path) - 1);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			break;
		close(fd);
		fd = -1;
		sleep(1);
	}
	if (fd < 0) {
		MHVTL_ERR("Cannot connect to %s after 30s", TCMU_SOCK_PATH);
		return -1;
	}

	struct tcmu_msg_register reg;
	memset(&reg, 0, sizeof(reg));
	reg.hdr.type = MSG_REGISTER;
	reg.hdr.data_len = sizeof(reg) - sizeof(reg.hdr);
	reg.minor = minor;
	reg.dev_type = dev_type;
	reg.channel = ctl->channel;
	reg.target = ctl->id;
	reg.lun = ctl->lun;
	strncpy(reg.bs_name, st->bs_name, sizeof(reg.bs_name) - 1);
	/* Only tape drives take the ring over; the changer stays relayed. */
	if (tdir_mode != MODE_RELAY && dev_type == 1)
		reg.flags = TCMU_REG_DIRECT;

	struct tcmu_msg_hdr resp;
	if (sock_send(fd, &reg, sizeof(reg)) < 0 ||
	    sock_recv(fd, &resp, sizeof(resp)) < 0 ||
	    resp.type != MSG_REGISTER_OK) {
		MHVTL_ERR("Registration failed for %s", st->bs_name);
		close(fd);
		return -1;
	}

	st->sock_fd = fd;
	st->uio_fd = -1;
	st->active = 1;
	MHVTL_LOG("Connected to TCMU handler for %s (direct %s)", st->bs_name,
		  reg.flags ? "requested" : "off");
	return (int)minor;
}

/* Next CMD entry in the ring, skipping PAD entries; NULL if none. */
static struct tcmu_cmd_entry *ring_next(struct tdir_state *st)
{
	struct tcmu_mailbox *mb = st->mb;
	uint32_t head = __atomic_load_n(&mb->cmd_head, __ATOMIC_ACQUIRE);
	uint32_t tail = mb->cmd_tail;

	while (tail != head) {
		struct tcmu_cmd_entry *ent = (void *)mb + mb->cmdr_off + tail;
		uint32_t op = tcmu_hdr_get_op(ent->hdr.len_op);

		if (op == TCMU_OP_CMD)
			return ent;
		if (op != TCMU_OP_PAD)
			ent->hdr.uflags |= TCMU_UFLAG_UNKNOWN_OP;
		tail = (tail + tcmu_hdr_get_len(ent->hdr.len_op)) % mb->cmdr_size;
		__atomic_store_n(&mb->cmd_tail, tail, __ATOMIC_RELEASE);
	}
	return NULL;
}

static void uio_clear_event(struct tdir_state *st)
{
	uint32_t c;
	while (read(st->uio_fd, &c, 4) < 0 && errno == EINTR)
		;
}

static int ring_poll(struct tdir_state *st, struct mhvtl_header *hdr)
{
	struct tcmu_cmd_entry *ent;
	uint32_t i, cnt;
	int len;

	ent = ring_next(st);
	if (!ent && tdir_mode == MODE_BLOCK) {
		struct pollfd pfd = { .fd = st->uio_fd, .events = POLLIN };

		uio_clear_event(st);
		ent = ring_next(st);
		if (!ent && poll(&pfd, 1, BLOCK_POLL_MS) > 0) {
			uio_clear_event(st);
			ent = ring_next(st);
		}
	}
	if (!ent)
		return VTL_IDLE;

	/* rsp overlays req in the entry, so keep the iovecs and CDB now. */
	cnt = ent->req.iov_cnt;
	if (cnt > st->iov_max) {
		struct iovec *n = realloc(st->iov, cnt * sizeof(*n));
		if (!n)
			return -1;
		st->iov = n;
		st->iov_max = cnt;
	}
	for (i = 0; i < cnt; i++) {
		st->iov[i].iov_base = (void *)st->mb +
				      (size_t)ent->req.iov[i].iov_base;
		st->iov[i].iov_len = ent->req.iov[i].iov_len;
	}
	st->iov_cnt = cnt;

	memset(hdr->cdb, 0, MAX_COMMAND_SIZE);
	len = cdb_length((uint8_t *)st->mb + ent->req.cdb_off);
	if (len > MAX_COMMAND_SIZE)
		len = MAX_COMMAND_SIZE;
	memcpy(hdr->cdb, (uint8_t *)st->mb + ent->req.cdb_off, len);
	hdr->serialNo = ent->hdr.cmd_id;
	st->cur = ent;
	st->got_data = 0;
	return VTL_QUEUE_CMD;
}

static int tdir_poll_cmd(int handle, struct mhvtl_header *hdr)
{
	struct tdir_state *st;
	struct tcmu_msg_cdb msg;
	struct pollfd pfd;
	int passed_fd;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return -1;
	st = &tdir_devices[handle];
	if (!st->active)
		return -1;

	if (st->direct)
		return ring_poll(st, hdr);

	pfd.fd = st->sock_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 0) <= 0)
		return VTL_IDLE;

	if (sock_recv_fd(st->sock_fd, &msg, sizeof(msg), &passed_fd) < 0) {
		MHVTL_ERR("socket read failed");
		return -1;
	}
	if (msg.hdr.type == MSG_HANDOFF) {
		if (passed_fd < 0) {
			MHVTL_ERR("[%s] MSG_HANDOFF without an fd", st->bs_name);
			return -1;
		}
		if (take_handoff(st, passed_fd, (size_t)msg.cmd_id) < 0)
			return -1;
		return ring_poll(st, hdr);
	}
	if (passed_fd >= 0)
		close(passed_fd);
	if (msg.hdr.type == MSG_SHUTDOWN)
		return -1;
	if (msg.hdr.type != MSG_CDB) {
		MHVTL_ERR("unexpected message type %u", msg.hdr.type);
		return -1;
	}
	memcpy(hdr->cdb, msg.cdb, MAX_COMMAND_SIZE);
	hdr->serialNo = msg.cmd_id;
	return VTL_QUEUE_CMD;
}

static int tdir_get_data(int handle, struct mhvtl_ds *ds)
{
	struct tdir_state *st;
	uint32_t i, done = 0;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return 0;
	st = &tdir_devices[handle];

	if (st->direct) {
		for (i = 0; i < st->iov_cnt && done < (uint32_t)ds->sz; i++) {
			uint32_t n = st->iov[i].iov_len;
			if (n > (uint32_t)ds->sz - done)
				n = ds->sz - done;
			memcpy((uint8_t *)ds->data + done, st->iov[i].iov_base, n);
			done += n;
		}
		st->got_data = 1;
		return (int)done;
	}

	struct tcmu_msg_data_req req;
	memset(&req, 0, sizeof(req));
	req.hdr.type = MSG_DATA_REQ;
	req.hdr.data_len = sizeof(req) - sizeof(req.hdr);
	req.cmd_id = ds->serialNo;
	req.data_len = ds->sz;
	if (sock_send(st->sock_fd, &req, sizeof(req)) < 0)
		return 0;

	struct tcmu_msg_data_out dout;
	if (sock_recv(st->sock_fd, &dout, sizeof(dout)) < 0)
		return 0;
	if (dout.hdr.type != MSG_DATA_OUT || dout.data_len == 0)
		return 0;
	uint32_t to_read = dout.data_len;
	if (to_read > (uint32_t)ds->sz)
		to_read = ds->sz;
	if (sock_recv(st->sock_fd, ds->data, to_read) < 0)
		return 0;
	return (int)to_read;
}

static void ring_complete(struct tdir_state *st, struct mhvtl_ds *ds)
{
	struct tcmu_mailbox *mb = st->mb;
	struct tcmu_cmd_entry *ent = st->cur;
	uint32_t i, done = 0, zero = 0, tail;

	if (!ent)
		return;

	/* DATA-IN straight into the ring's data area. */
	if (!st->got_data && ds->sz > 0 && ds->data) {
		for (i = 0; i < st->iov_cnt && done < (uint32_t)ds->sz; i++) {
			uint32_t n = st->iov[i].iov_len;
			if (n > (uint32_t)ds->sz - done)
				n = ds->sz - done;
			memcpy(st->iov[i].iov_base, (uint8_t *)ds->data + done, n);
			done += n;
		}
	}

	ent->rsp.scsi_status = ds->sam_stat;
	if (ds->sam_stat == SAM_STAT_CHECK_CONDITION && ds->sense_buf)
		memcpy(ent->rsp.sense_buffer, ds->sense_buf,
		       TCMU_SENSE_BUFFERSIZE);
	/* Short reads: report the real DATA-IN length (#253). */
	if (!st->got_data && (mb->flags & TCMU_MAILBOX_FLAG_CAP_READ_LEN)) {
		ent->rsp.read_len = done;
		ent->hdr.uflags |= TCMU_UFLAG_READ_LEN;
	}

	tail = (mb->cmd_tail + tcmu_hdr_get_len(ent->hdr.len_op)) %
	       mb->cmdr_size;
	__atomic_store_n(&mb->cmd_tail, tail, __ATOMIC_RELEASE);
	st->cur = NULL;

	/* Tell the kernel a completion is ready. */
	while (write(st->uio_fd, &zero, 4) < 0 && errno == EINTR)
		;
}

static void tdir_put_data(int handle, struct mhvtl_ds *ds)
{
	struct tdir_state *st;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return;
	st = &tdir_devices[handle];

	if (st->direct) {
		ring_complete(st, ds);
		ds->sam_stat = 0;
		return;
	}

	struct tcmu_msg_cdb_resp resp;
	memset(&resp, 0, sizeof(resp));
	resp.hdr.type = MSG_CDB_RESP;
	resp.cmd_id = ds->serialNo;
	resp.sam_stat = ds->sam_stat;
	resp.data_len = ds->sz;
	if (ds->sense_buf)
		memcpy(resp.sense, ds->sense_buf, TCMU_MAX_SENSE_SIZE);
	resp.hdr.data_len = sizeof(resp) - sizeof(resp.hdr) + ds->sz;
	sock_send(st->sock_fd, &resp, sizeof(resp));
	if (ds->sz > 0 && ds->data)
		sock_send(st->sock_fd, ds->data, ds->sz);
	ds->sam_stat = 0;
}

static pid_t tdir_add_lu(unsigned minor, struct mhvtl_ctl *ctl)
{
	pid_t pid = fork();
	if (pid == 0) _exit(0);
	return pid > 0 ? pid : 0;
}

static void tdir_remove_lu(int handle, struct mhvtl_ctl *ctl)
{
}

static void tdir_close(int handle)
{
	struct tdir_state *st;

	if (handle < 0 || handle >= MAX_TCMU_DEVICES)
		return;
	st = &tdir_devices[handle];
	if (!st->active)
		return;
	if (st->mb)
		munmap(st->mb, st->map_len);
	if (st->uio_fd >= 0)
		close(st->uio_fd);
	if (st->sock_fd >= 0)
		close(st->sock_fd);
	free(st->iov);
	memset(st, 0, sizeof(*st));
	st->sock_fd = st->uio_fd = -1;
}

static struct vtl_transport tdir_transport = {
	.name      = "tcmu-direct",
	.open      = tdir_open,
	.poll_cmd  = tdir_poll_cmd,
	.get_data  = tdir_get_data,
	.put_data  = tdir_put_data,
	.add_lu    = tdir_add_lu,
	.remove_lu = tdir_remove_lu,
	.close     = tdir_close,
};

struct vtl_transport *transport_tcmu_direct_init(const char *mode)
{
	if (!strcmp(mode, "block"))
		tdir_mode = MODE_BLOCK;
	else if (!strcmp(mode, "backoff"))
		tdir_mode = MODE_BACKOFF;
	else
		tdir_mode = MODE_RELAY;
	return &tdir_transport;
}
