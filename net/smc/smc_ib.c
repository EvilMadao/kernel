/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  IB infrastructure:
 *  Establish SMC-R as an Infiniband Client to be notified about added and
 *  removed IB devices of type RDMA.
 *  Determine device an port characteristics for these IB devices.
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#include <linux/random.h>
#include <rdma/ib_verbs.h>

#include "smc_pnet.h"
#include "smc_ib.h"
#include "smc_core.h"
#include "smc_wr.h"
#include "smc.h"

struct smc_ib_devices smc_ib_devices = {	/* smc-registered ib devices */
	.lock = __SPIN_LOCK_UNLOCKED(smc_ib_devices.lock),
	.list = LIST_HEAD_INIT(smc_ib_devices.list),
};

#define SMC_LOCAL_SYSTEMID_RESET	"%%%%%%%"

u8 local_systemid[SMC_SYSTEMID_LEN] = SMC_LOCAL_SYSTEMID_RESET;	/* unique system
								 * identifier
								 */

void smc_ib_dealloc_protection_domain(struct smc_link *lnk)
{
	ib_dealloc_pd(lnk->roce_pd);
	lnk->roce_pd = NULL;
}

int smc_ib_create_protection_domain(struct smc_link *lnk)
{
	lnk->roce_pd = ib_alloc_pd(lnk->smcibdev->ibdev);
	if (IS_ERR(lnk->roce_pd)) {
		lnk->roce_pd = NULL;
		return (int)PTR_ERR(lnk->roce_pd);
	}
	return 0;
}

static void smc_ib_qp_event_handler(struct ib_event *ibevent, void *priv)
{
	switch (ibevent->event) {
	case IB_EVENT_DEVICE_FATAL:
	case IB_EVENT_GID_CHANGE:
	case IB_EVENT_PORT_ERR:
	case IB_EVENT_QP_ACCESS_ERR:
		/* tbd in follow-on patch:
		 * abnormal close of corresponding connections
		 */
		break;
	default:
		break;
	}
}

void smc_ib_destroy_queue_pair(struct smc_link *lnk)
{
	ib_destroy_qp(lnk->roce_qp);
	lnk->roce_qp = NULL;
}

/* create a queue pair within the protection domain for a link */
int smc_ib_create_queue_pair(struct smc_link *lnk)
{
	struct ib_qp_init_attr qp_attr = {
		.event_handler = smc_ib_qp_event_handler,
		.qp_context = lnk,
		.send_cq = lnk->smcibdev->roce_cq_send,
		.recv_cq = lnk->smcibdev->roce_cq_recv,
		.srq = NULL,
		.cap = {
			.max_send_wr = SMC_WR_BUF_CNT,
				/* include unsolicited rdma_writes as well,
				 * there are max. 2 RDMA_WRITE per 1 WR_SEND
				 */
			.max_recv_wr = SMC_WR_BUF_CNT * 3,
			.max_send_sge = SMC_IB_MAX_SEND_SGE,
			.max_recv_sge = 1,
			.max_inline_data = SMC_WR_TX_SIZE,
		},
		.sq_sig_type = IB_SIGNAL_REQ_WR,
		.qp_type = IB_QPT_RC,
	};

	lnk->roce_qp = ib_create_qp(lnk->roce_pd, &qp_attr);
	if (IS_ERR(lnk->roce_qp)) {
		lnk->roce_qp = NULL;
		return (int)PTR_ERR(lnk->roce_qp);
	}
	smc_wr_remember_qp_attr(lnk);
	return 0;
}

/* map a new TX or RX buffer to DMA */
int smc_ib_buf_map(struct smc_ib_device *smcibdev, int buf_size,
		   struct smc_buf_desc *buf_slot,
		   enum dma_data_direction data_direction)
{
	int rc = 0;

	if (buf_slot->dma_addr[SMC_SINGLE_LINK])
		return rc; /* already mapped */
	buf_slot->dma_addr[SMC_SINGLE_LINK] =
		ib_dma_map_single(smcibdev->ibdev, buf_slot->cpu_addr,
				  buf_size, data_direction);
	if (ib_dma_mapping_error(smcibdev->ibdev,
				 buf_slot->dma_addr[SMC_SINGLE_LINK]))
		rc = -EIO;
	return rc;
}

static int smc_ib_fill_gid_and_mac(struct smc_ib_device *smcibdev, u8 ibport)
{
	struct net_device *ndev;
	int rc;

	rc = ib_query_gid(smcibdev->ibdev, ibport, 0,
			  &smcibdev->gid[ibport - 1], NULL);
	/* the SMC protocol requires specification of the roce MAC address;
	 * if net_device cannot be determined, it can be derived from gid 0
	 */
	ndev = smcibdev->ibdev->get_netdev(smcibdev->ibdev, ibport);
	if (ndev) {
		memcpy(&smcibdev->mac, ndev->dev_addr, ETH_ALEN);
	} else if (!rc) {
		memcpy(&smcibdev->mac[ibport - 1][0],
		       &smcibdev->gid[ibport - 1].raw[8], 3);
		memcpy(&smcibdev->mac[ibport - 1][3],
		       &smcibdev->gid[ibport - 1].raw[13], 3);
		smcibdev->mac[ibport - 1][0] &= ~0x02;
	}
	return rc;
}

/* Create an identifier unique for this instance of SMC-R.
 * The MAC-address of the first active registered IB device
 * plus a random 2-byte number is used to create this identifier.
 * This name is delivered to the peer during connection initialization.
 */
static inline void smc_ib_define_local_systemid(struct smc_ib_device *smcibdev,
						u8 ibport)
{
	memcpy(&local_systemid[2], &smcibdev->mac[ibport - 1],
	       sizeof(smcibdev->mac[ibport - 1]));
	get_random_bytes(&local_systemid[0], 2);
}

bool smc_ib_port_active(struct smc_ib_device *smcibdev, u8 ibport)
{
	return smcibdev->pattr[ibport - 1].state == IB_PORT_ACTIVE;
}

int smc_ib_remember_port_attr(struct smc_ib_device *smcibdev, u8 ibport)
{
	int rc;

	memset(&smcibdev->pattr[ibport - 1], 0,
	       sizeof(smcibdev->pattr[ibport - 1]));
	rc = ib_query_port(smcibdev->ibdev, ibport,
			   &smcibdev->pattr[ibport - 1]);
	if (rc)
		goto out;
	smc_ib_fill_gid_and_mac(smcibdev, ibport);
	if (!strncmp(local_systemid, SMC_LOCAL_SYSTEMID_RESET,
		     sizeof(local_systemid)) &&
	    smc_ib_port_active(smcibdev, ibport))
		/* create unique system identifier */
		smc_ib_define_local_systemid(smcibdev, ibport);
out:
	return rc;
}

static struct ib_client smc_ib_client;

/* callback function for ib_register_client() */
static void smc_ib_add_dev(struct ib_device *ibdev)
{
	struct smc_ib_device *smcibdev;
	int i;

	if (ibdev->node_type != RDMA_NODE_IB_CA)
		return;

	smcibdev = kzalloc(sizeof(*smcibdev), GFP_KERNEL);
	if (!smcibdev)
		return;

	smcibdev->ibdev = ibdev;

	for (i = 1; i <= SMC_MAX_PORTS; i++) {
		if (smc_pnet_exists_in_table(smcibdev, i) &&
		    !smcibdev->initialized) {
			/* dev hotplug: ib device and port is in pnet table */
			if (smc_ib_remember_port_attr(smcibdev, i)) {
				kfree(smcibdev);
				return;
			}
			smcibdev->initialized = 1;
			break;
		}
	}
	spin_lock(&smc_ib_devices.lock);
	list_add_tail(&smcibdev->list, &smc_ib_devices.list);
	spin_unlock(&smc_ib_devices.lock);
	ib_set_client_data(ibdev, &smc_ib_client, smcibdev);
}

/* callback function for ib_register_client() */
static void smc_ib_remove_dev(struct ib_device *ibdev, void *client_data)
{
	struct smc_ib_device *smcibdev;

	smcibdev = ib_get_client_data(ibdev, &smc_ib_client);
	ib_set_client_data(ibdev, &smc_ib_client, NULL);
	spin_lock(&smc_ib_devices.lock);
	list_del_init(&smcibdev->list); /* remove from smc_ib_devices */
	spin_unlock(&smc_ib_devices.lock);
	kfree(smcibdev);
}

static struct ib_client smc_ib_client = {
	.name	= "smc_ib",
	.add	= smc_ib_add_dev,
	.remove = smc_ib_remove_dev,
};

int __init smc_ib_register_client(void)
{
	return ib_register_client(&smc_ib_client);
}

void __exit smc_ib_unregister_client(void)
{
	ib_unregister_client(&smc_ib_client);
}
