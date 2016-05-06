/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Work Requests exploiting Infiniband API
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Steffen Maier <maier@linux.vnet.ibm.com>
 */

#ifndef SMC_WR_H
#define SMC_WR_H

#include <rdma/ib_verbs.h>

#include "smc.h"
#include "smc_core.h"

#define SMC_WR_MAX_CQE 32768	/* max. # of completion queue elements */
#define SMC_WR_BUF_CNT 16	/* # of ctrl buffers per link */

#define SMC_WR_TX_WAIT_FREE_SLOT_TIME	HZ
#define SMC_WR_TX_WAIT_PENDING_TIME	(5 * HZ)

#define SMC_WR_TX_SIZE 44 /* actual size of wr_send data (<=SMC_WR_BUF_SIZE) */

#define SMC_WR_TX_PEND_PRIV_SIZE 32

struct smc_wr_tx_pend_priv {
	u8			priv[SMC_WR_TX_PEND_PRIV_SIZE];
};

typedef void (*smc_wr_tx_handler)(struct smc_wr_tx_pend_priv *,
				  struct smc_link *,
				  enum ib_wc_status);

struct smc_wr_rx_handler {
	struct hlist_node	list;	/* hash table collision resolution */
	void			(*handler)(struct ib_wc *, void *);
	u8			type;
};

struct smc_wr_tx_pend {		/* control data for a pending send request */
	u64			wr_id;		/* work request id sent */
	smc_wr_tx_handler	handler;
	enum ib_wc_status	wc_status;	/* CQE status */
	struct smc_link		*link;
	u32			idx;
	struct smc_wr_tx_pend_priv priv;
};

/* Only used by RDMA write WRs.
 * All other WRs (CDC/LLC) use smc_wr_tx_send handling WR_ID implicitly
 */
static inline u64 smc_wr_tx_get_next_wr_id(struct smc_link *link)
{
	return atomic64_inc_return(&link->wr_tx_id);
}

/* post a new receive work request to fill a completed old work request entry */
static inline int smc_wr_rx_post(struct smc_link *link)
{
	struct ib_recv_wr *bad_recv_wr = NULL;
	int rc = 0;
	u64 wr_id;
	u32 index;

	wr_id = ++link->wr_rx_id; /* tasklet context, thus not atomic */
	index = wr_id % link->wr_rx_cnt;
	link->wr_rx_ibs[index].wr_id = wr_id;
	rc = ib_post_recv(link->roce_qp, &link->wr_rx_ibs[index], &bad_recv_wr);
	return rc;
}

struct smc_connection;

int smc_wr_create_lgr(struct smc_link *);
int smc_wr_alloc_link_mem(struct smc_link *);
void smc_wr_free_link(struct smc_link *);
void smc_wr_free_link_mem(struct smc_link *);
void smc_wr_remember_qp_attr(struct smc_link *);
void smc_wr_cq_event_handler(struct ib_event *, void *);
void smc_wr_remove_dev(struct smc_ib_device *);
void smc_wr_add_dev(struct smc_ib_device *);

int smc_wr_tx_wait_no_pending_on_link(struct smc_link *);
int smc_wr_tx_get_free_slot(struct smc_link *, smc_wr_tx_handler,
			    struct smc_wr_buf **,
			    struct smc_wr_tx_pend_priv **);
int smc_wr_tx_put_slot(struct smc_link *, struct smc_wr_tx_pend_priv *);
int smc_wr_tx_send(struct smc_link *, struct smc_connection *,
		   struct smc_wr_tx_pend_priv *);
void smc_wr_tx_tasklet_fn(unsigned long);
void smc_wr_tx_cq_handler(struct ib_cq *, void *);

int smc_wr_rx_register_handler(struct smc_wr_rx_handler *);
int smc_wr_rx_post_init(struct smc_link *);
void smc_wr_rx_tasklet_fn(unsigned long);
void smc_wr_rx_cq_handler(struct ib_cq *, void *);

#endif /* SMC_WR_H */
