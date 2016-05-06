/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Manage RMBE
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ursula.braun@de.ibm.com>
 */

#ifndef SMC_RX_H
#define SMC_RX_H

#include <linux/socket.h>
#include <linux/types.h>

#include "smc.h"

void smc_rx_init(struct smc_sock *);
int smc_rx_to_read(struct smc_connection *);
int smc_rx_recvmsg(struct smc_sock *, struct msghdr *, size_t, int);
void smc_rx_handler(struct smc_sock *);

#endif /* SMC_RX_H */
