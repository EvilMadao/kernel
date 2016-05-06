/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Manage send buffer
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ursula.braun@de.ibm.com>
 */

#ifndef SMC_TX_H
#define SMC_TX_H

#include <linux/socket.h>
#include <linux/types.h>

#include "smc.h"

void smc_tx_init(struct smc_sock *);
int smc_tx_sendmsg(struct smc_sock *, struct msghdr *, size_t);
int smc_tx_sndbuf_nonempty(struct smc_connection *);
void smc_tx_sndbuf_nonfull(struct smc_sock *);

#endif /* SMC_TX_H */
