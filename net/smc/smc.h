/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the SMC module (socket related)
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <ursula.braun@linux.vnet.ibm.com>
 */
#ifndef _SMC_H
#define _SMC_H

#include <linux/socket.h>
#include <linux/types.h>
#include <net/sock.h>

#include "smc_ib.h"

#define SMCPROTO_SMC		0	/* SMC protocol */

#define smc_stop_received(conn) \
	(conn->local_rx_ctrl.conn_state_flags.sending_done || \
	 conn->local_rx_ctrl.conn_state_flags.abnormal_close || \
	 conn->local_rx_ctrl.conn_state_flags.closed_conn)

#define smc_close_received(conn) \
	(conn->local_rx_ctrl.conn_state_flags.abnormal_close || \
	 conn->local_rx_ctrl.conn_state_flags.closed_conn)

enum smc_state {		/* possible states of an SMC socket */
	SMC_ACTIVE	= 1,
	SMC_INIT	= 2,
	SMC_CLOSED	= 7,
	SMC_LISTEN	= 10,
	SMC_DESTRUCT	= 32
};

struct smc_link_group;

struct smc_wr_rx_hdr {	/* common prefix part of LLC and CDC to demultiplex */
	u8			type;
} __packed;

struct smc_cdc_conn_state_flags {
#if defined(__BIG_ENDIAN_BITFIELD)
	u8	sending_done : 1;	/* Sending done indicator */
	u8	closed_conn : 1;	/* Peer connection closed indicator */
	u8	abnormal_close : 1;	/* Abnormal close indicator */
	u8	reserved : 5;
#elif defined(__LITTLE_ENDIAN_BITFIELD)
	u8	reserved : 5;
	u8	abnormal_close : 1;
	u8	closed_conn : 1;
	u8	sending_done : 1;
#endif
} __packed;

struct smc_cdc_producer_flags {
#if defined(__BIG_ENDIAN_BITFIELD)
	u8	write_blocked : 1;	/* Writing Blocked, no rx buf space */
	u8	urg_data_pending : 1;	/* Urgent Data Pending */
	u8	urg_data_present : 1;	/* Urgent Data Present */
	u8	cons_curs_upd_req : 1;	/* cursor update requested */
	u8	failover_validation : 1;/* message replay due to failover */
	u8	reserved : 3;
#elif defined(__LITTLE_ENDIAN_BITFIELD)
	u8	reserved : 3;
	u8	failover_validation : 1;
	u8	cons_curs_upd_req : 1;
	u8	urg_data_present : 1;
	u8	urg_data_pending : 1;
	u8	write_blocked : 1;
#endif
} __packed;

/* in host byte order */
struct smc_host_cursor {	/* SMC cursor - an offset in an RMBE */
	u16	reserved;
	u16	wrap;		/* window wrap sequence number */
	u32	count;		/* cursor (= offset) part */
} __aligned(8);

/* in host byte order */
union smc_host_cursor_ovl {		/* overlay for atomic cursor handling */
	struct smc_host_cursor	curs;
	u64			acurs;
} __aligned(8);

/* in host byte order, except for flag bitfields in network byte order */
struct smc_host_cdc_msg {		/* Connection Data Control message */
	struct smc_wr_rx_hdr		common; /* .type = 0xFE */
	u8				len;	/* length = 44 */
	u16				seqno;	/* connection seq # */
	u32				token;	/* alert_token */
	union smc_host_cursor_ovl	prod;		/* producer cursor */
	union smc_host_cursor_ovl	cons;		/* consumer cursor,
							 * piggy backed "ack"
							 */
	struct smc_cdc_producer_flags	prod_flags;	/* conn. tx/rx status */
	struct smc_cdc_conn_state_flags	conn_state_flags; /* peer conn. status*/
	u8				reserved[18];
} __packed __aligned(8);

struct smc_connection {
	struct rb_node		alert_node;
	struct smc_link_group	*lgr;		/* link group of connection */
	u32			alert_token_local; /* unique conn. id */
	u8			peer_conn_idx;	/* from tcp handshake */
	int			peer_rmbe_len;	/* size of peer rx buffer */
	atomic_t		peer_rmbe_space;/* remaining free bytes in peer
						 * rmbe
						 */
	int			rtoken_idx;	/* idx to peer RMB rkey/addr */

	struct smc_buf_desc	*sndbuf_desc;	/* send buffer descriptor */
	int			sndbuf_size;	/* sndbuf size <== sock wmem */
	struct smc_buf_desc	*rmb_desc;	/* RMBE descriptor */
	int			rmbe_size;	/* RMBE size <== sock rmem */
	int			rmbe_size_short;/* compressed notation */

	struct smc_host_cdc_msg	local_tx_ctrl;	/* host byte order staging
						 * buffer for CDC msg send
						 * .prod cf. TCP snd_nxt
						 * .cons cf. TCP sends ack
						 */
	union smc_host_cursor_ovl tx_curs_prep;	/* tx - prepared data
						 * snd_max..wmem_alloc
						 */
	union smc_host_cursor_ovl tx_curs_sent;	/* tx - sent data
						 * snd_nxt ?
						 */
	union smc_host_cursor_ovl tx_curs_fin;	/* tx - confirmed by peer
						 * snd-wnd-begin ?
						 */
	atomic_t		sndbuf_space;	/* remaining space in sndbuf */
	u16			tx_cdc_seq;	/* sequence # for CDC send */
	spinlock_t		send_lock;	/* protect wr_sends */
	struct delayed_work	tx_work;	/* retry of smc_cdc_msg_send */

	struct smc_host_cdc_msg	local_rx_ctrl;	/* filled during event_handl.
						 * .prod cf. TCP rcv_nxt
						 * .cons cf. TCP snd_una
						 */
	union smc_host_cursor_ovl rx_curs_confirmed; /* confirmed to peer
						      * source of snd_una ?
						      */
	atomic_t		bytes_to_rcv;	/* arrived data,
						 * not yet received
						 */
};

struct smc_sock {				/* smc sock container */
	struct sock		sk;
	struct socket		*clcsock;	/* internal tcp socket */
	struct smc_connection	conn;		/* smc connection */
	struct sockaddr		*addr;		/* inet connect address */
	struct smc_sock		*listen_smc;	/* listen parent */
	struct work_struct	tcp_listen_work;/* handle tcp socket accepts */
	struct work_struct	smc_listen_work;/* prepare new accept socket */
	struct list_head	accept_q;	/* sockets to be accepted */
	spinlock_t		accept_q_lock;	/* protects accept_q */
	u8			use_fallback : 1, /* fallback to tcp */
				clc_started : 1;/* smc_connect_rdma ran */
};

static inline struct smc_sock *smc_sk(const struct sock *sk)
{
	return (struct smc_sock *)sk;
}

#define SMC_SYSTEMID_LEN		8

extern u8	local_systemid[SMC_SYSTEMID_LEN]; /* unique system identifier */

/* convert an u32 value into network byte order, store it into a 3 byte field */
static inline void hton24(u8 *net, u32 host)
{
	__be32 t;

	t = cpu_to_be32(host);
	memcpy(net, ((u8 *)&t) + 1, 3);
}

/* convert a received 3 byte field into host byte order*/
static inline u32 ntoh24(u8 *net)
{
	__be32 t = 0;

	memcpy(((u8 *)&t) + 1, net, 3);
	return be32_to_cpu(t);
}

#define SMC_RMB_SIZES	16	/* number of distinct sizes for an RMB*/

/* convert the RMB size into the compressed notation - minimum 16K */
static inline u8 smc_compress_bufsize(int size)
{
	u8 compressed = 0;

	size = (size - 1) >> 14;
	compressed = ilog2(size) + 1;
	if (compressed >= SMC_RMB_SIZES)
		compressed = SMC_RMB_SIZES - 1;
	return compressed;
}

/* convert the RMB size from compressed notation into integer */
static inline int smc_uncompress_bufsize(u8 compressed)
{
	u32 size;

	size = 0x00000001 << (((int)compressed) + 14);
	return (int)size;
}

#ifdef CONFIG_XFRM
static inline bool using_ipsec(struct smc_sock *smc)
{
	return (smc->clcsock->sk->sk_policy[0] ||
		smc->clcsock->sk->sk_policy[1]) ? 1 : 0;
}
#else
static inline bool using_ipsec(struct smc_sock *smc)
{
	return 0;
}
#endif

struct smc_clc_msg_local;

int smc_netinfo_by_tcpsk(struct socket *, __be32 *, u8 *);
void smc_conn_free(struct smc_connection *);
int smc_conn_create(struct smc_sock *, __be32, struct smc_ib_device *, u8,
		    struct smc_clc_msg_local *, int);

#endif	/* _SMC_H */
