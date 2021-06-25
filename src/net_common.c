// Copyright 2014 Carnegie Mellon University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "net_common.h"
#include "util.h"
#include "stopwatch.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_flow.h>

#define MEHCACHED_MBUF_ENTRY_SIZE (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
// #define MEHCACHED_MBUF_SIZE (MEHCACHED_MAX_PORTS * MEHCACHED_MAX_QUEUES * 2048)     // TODO: need to divide by numa node count
#define MEHCACHED_MBUF_SIZE (32768)     // TODO: need to divide by numa node count

#define MEHCACHED_MAX_PKT_BURST (32)

#define MEHCACHED_RX_PTHRESH (8)
#define MEHCACHED_RX_HTHRESH (8)
#define MEHCACHED_RX_WTHRESH (4)

#define MEHCACHED_TX_PTHRESH (36)
#define MEHCACHED_TX_HTHRESH (0)
#define MEHCACHED_TX_WTHRESH (0)

static uint16_t mehcached_num_rx_desc = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t mehcached_num_tx_desc = RTE_TEST_TX_DESC_DEFAULT;

//#define MEHCACHED_USE_QUICK_SLEEP
//#define MEHCACHED_USE_DEEP_SLEEP

static const struct rte_eth_conf mehcached_port_conf = {
	.rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
		.split_hdr_size = 0,
		.offloads = DEV_RX_OFFLOAD_CHECKSUM | DEV_RX_OFFLOAD_SCATTER,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
		.offloads = DEV_TX_OFFLOAD_MULTI_SEGS,
	},
};

struct mehcached_queue_state {
	struct rte_mbuf *rx_mbufs[MEHCACHED_MAX_PKT_BURST];
	uint16_t rx_length;
	uint16_t rx_next_to_use;

#ifdef MEHCACHED_USE_QUICK_SLEEP
	uint16_t rx_quick_sleep;
	uint16_t rx_full_quick_sleep_count;
#endif
#ifdef MEHCACHED_USE_DEEP_SLEEP
	uint64_t rx_last_seen;
	uint64_t rx_deep_sleep_until;
	uint64_t rx_inter_batch_time;
#endif

	struct rte_mbuf *tx_mbufs[MEHCACHED_MAX_PKT_BURST];
	uint16_t tx_length;

	uint64_t num_rx_burst;
	uint64_t num_rx_received;

	uint64_t num_tx_burst;
	uint64_t num_tx_sent;
	uint64_t num_tx_dropped;
} __rte_cache_aligned;

static struct rte_mempool *mehcached_pktmbuf_pool[MEHCACHED_MAX_NUMA_NODES];

static uint16_t mehcached_lcore_to_queue[MEHCACHED_MAX_LCORES];
//static struct ether_addr mehcached_eth_addr[MEHCACHED_MAX_PORTS];

static struct mehcached_queue_state *mehcached_queue_states[MEHCACHED_MAX_QUEUES * MEHCACHED_MAX_PORTS];

struct rte_mbuf *
mehcached_packet_alloc()
{
	return rte_pktmbuf_alloc(mehcached_pktmbuf_pool[rte_socket_id()]);
}

void
mehcached_packet_free(struct rte_mbuf *mbuf)
{
	rte_pktmbuf_free(mbuf);
}

struct rte_mbuf *
mehcached_receive_packet(uint8_t port_id)
{
	uint32_t lcore = rte_lcore_id();
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	assert(queue != (uint16_t)-1);
	// uint16_t queue = (uint16_t)lcore;
	struct mehcached_queue_state *state = mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id];

	if (state->rx_next_to_use == state->rx_length)
	{
#ifdef MEHCACHED_USE_QUICK_SLEEP
		if (state->rx_quick_sleep > 0)
		{
			// struct rte_mbuf *t = mehcached_packet_alloc();
			// if (t == NULL)
			// 	printf("cannot alloc mbuf\n");
			// mehcached_packet_free(t);
			state->rx_quick_sleep--;
			return NULL;
		}
#endif

#ifdef MEHCACHED_USE_DEEP_SLEEP
		uint64_t now = mehcached_stopwatch_now();

		// too small value makes deep sleep ineffective
		// too large value may incorrectly penalize a queue with occasional underflows
		const uint64_t max_deep_sleep_time = mehcached_stopwatch_1_usec * 50;

		// still need to sleep?
		if (state->rx_deep_sleep_until - now <= max_deep_sleep_time)
		{
			// assumed invariant: rx_deep_sleep_until <= now + max_deep_sleep_time
			//   (when no overflow happens)
			// the condition in the if statement checks the sleep time correctly under this invariant
			return NULL;
		}
#endif

		state->rx_length = rte_eth_rx_burst(port_id, queue, state->rx_mbufs, MEHCACHED_MAX_PKT_BURST);
		state->num_rx_received += state->rx_length;
		state->rx_next_to_use = 0;
		state->num_rx_burst++;

#ifdef MEHCACHED_USE_QUICK_SLEEP
		// sleep if no enough RX packets were received
		// this helps reduce PCIe traffic when # of RX packets is imbalanced across queues used by the same core
		state->rx_quick_sleep = (uint16_t)(MEHCACHED_MAX_PKT_BURST - state->rx_length);
		if (state->rx_length != 0)
			state->rx_full_quick_sleep_count = 0;
		else
		{
			if (state->rx_full_quick_sleep_count < 1024)
				state->rx_full_quick_sleep_count++;
			state->rx_quick_sleep = (uint16_t)(state->rx_quick_sleep * state->rx_full_quick_sleep_count);
		}

#endif

#ifdef MEHCACHED_USE_DEEP_SLEEP
		uint64_t to_sleep;
		uint64_t inter_batch_time;

		// adjust sleep time so that the next rx_burst can get MEHCACHED_MAX_PKT_BURST packets
		// note (state->rx_length + 1): this makes inter_batch_time slightly smaller than actual expectation
		// because we do not know whether there are additional subsequent batches
		inter_batch_time = (now - state->rx_last_seen) * MEHCACHED_MAX_PKT_BURST / (state->rx_length + 1);
		if (inter_batch_time > max_deep_sleep_time)
			inter_batch_time = max_deep_sleep_time;
		state->rx_last_seen = now;

		state->rx_inter_batch_time = (state->rx_inter_batch_time * 7 + inter_batch_time * 1) / 8;

		// deep sleep to prevent excessive PCIe traffic when RX across cores is imbalanced
		state->rx_deep_sleep_until = now + state->rx_inter_batch_time;

		// for debugging batch size
		// if ((state->num_rx_burst & 0xffffUL) == 0)
		// {
		// 	printf("port = %zu, queue = %zu; average_batch size = %lf, inter batch time = %lf us\n", port, queue, (double)state->num_rx_received / (double)state->num_rx_burst, (double)state->rx_inter_batch_time / (double)mehcached_stopwatch_1_usec);
		// 	state->num_rx_received = 0;
		// 	state->num_rx_burst = 0;
		// }
#endif
	}

	if (state->rx_next_to_use < state->rx_length)
    {
#ifndef NDEBUG
        //printf("mehcached_receive_packet: lcore=%zu, port=%zu, queue=%zu\n", lcore, port, queue);
#endif
		return state->rx_mbufs[state->rx_next_to_use++];
    }
	else
		return NULL;
}

void
mehcached_receive_packets(uint8_t port_id, struct rte_mbuf **mbufs, size_t *in_out_num_mbufs)
{
	uint32_t lcore = rte_lcore_id();
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	// assert(queue != (uint16_t)-1);
	//uint16_t queue = (uint16_t)lcore;
	struct mehcached_queue_state *state = mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id];

	// printf("reading %d pkts\n", (uint16_t)*in_out_num_mbufs);
	*in_out_num_mbufs = (size_t)rte_eth_rx_burst(port_id, queue, mbufs, (uint16_t)*in_out_num_mbufs);
	// if (*in_out_num_mbufs)
	// 	printf("received %d pkts\n", (uint16_t)*in_out_num_mbufs);
	state->num_rx_received += *in_out_num_mbufs;
	state->num_rx_burst++;
}

void
mehcached_send_packet(uint8_t port_id, struct rte_mbuf *mbuf)
{
	uint32_t lcore = rte_lcore_id();
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	// assert(queue != (uint16_t)-1);
	// uint16_t queue = (uint16_t)lcore;
	struct mehcached_queue_state *state = mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id];

#ifndef NDEBUG
    //printf("mehcached_send_packet: lcore=%zu, port=%zu, queue=%zu\n", lcore, port, queue);
#endif

	state->tx_mbufs[state->tx_length++] = mbuf;
	if (state->tx_length == MEHCACHED_MAX_PKT_BURST)
	{
		// printf("sending port %d lcore %d queue %d\n", port_id, lcore, queue);
		uint16_t count = rte_eth_tx_burst(port_id, queue, state->tx_mbufs, MEHCACHED_MAX_PKT_BURST);
		state->num_tx_sent += count;
		state->num_tx_dropped += (uint64_t)(MEHCACHED_MAX_PKT_BURST - count);
		for (; count < MEHCACHED_MAX_PKT_BURST; count++)
			rte_pktmbuf_free(state->tx_mbufs[count]);
		state->tx_length = 0;
		state->num_tx_burst++;
	}
}

void
mehcached_send_packet_flush(uint8_t port_id)
{
	uint32_t lcore = rte_lcore_id();
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	// assert(queue != (uint16_t)-1);
	// uint16_t queue = (uint16_t)lcore;
	struct mehcached_queue_state *state = mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id];

	if (state->tx_length > 0)
	{
		uint16_t count = rte_eth_tx_burst(port_id, queue, state->tx_mbufs, state->tx_length);
		state->num_tx_sent += count;
		state->num_tx_dropped += (uint64_t)(state->tx_length - count);
		for (; count < state->tx_length; count++)
			rte_pktmbuf_free(state->tx_mbufs[count]);
		state->tx_length = 0;
		state->num_tx_burst++;
	}
}

void
mehcached_get_stats(uint8_t port_id, uint64_t *out_num_rx_burst, uint64_t *out_num_rx_received, uint64_t *out_num_tx_burst, uint64_t *out_num_tx_sent, uint64_t *out_num_tx_dropped)
{
	mehcached_get_stats_lcore(port_id, rte_lcore_id(), out_num_rx_burst, out_num_rx_received, out_num_tx_burst, out_num_tx_sent, out_num_tx_dropped);
}

void
mehcached_get_stats_lcore(uint8_t port_id, uint32_t lcore, uint64_t *out_num_rx_burst, uint64_t *out_num_rx_received, uint64_t *out_num_tx_burst, uint64_t *out_num_tx_sent, uint64_t *out_num_tx_dropped)
{
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	// assert(queue != (uint16_t)-1);
	// uint16_t queue = (uint16_t)lcore;
	struct mehcached_queue_state *state = mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id];

	if (out_num_rx_burst)
		*out_num_rx_burst = state->num_rx_burst;
	if (out_num_rx_received)
		*out_num_rx_received = state->num_rx_received;
	if (out_num_tx_burst)
		*out_num_tx_burst = state->num_tx_burst;
	if (out_num_tx_sent)
		*out_num_tx_sent = state->num_tx_sent;
	if (out_num_tx_dropped)
		*out_num_tx_dropped = state->num_tx_dropped;

    //struct rte_eth_stats stats;
    //rte_eth_stats_get(port, &stats);
    //printf("port %zu i %lu o %lu ie %lu oe %lu\n", port, stats.ipackets, stats.opackets, stats.ierrors, stats.oerrors);
}

struct rte_mbuf *
mehcached_clone_packet(struct rte_mbuf *mbuf_src)
{
	return rte_pktmbuf_clone(mbuf_src, mehcached_pktmbuf_pool[rte_socket_id()]);
}

bool
mehcached_init_network(uint64_t cpu_mask, uint64_t port_mask, uint8_t *out_num_ports)
{
	int ret;
	size_t i;

	size_t num_numa_nodes = 0;
	uint16_t num_queues = 0;

	assert(rte_lcore_count() <= MEHCACHED_MAX_LCORES);

	RTE_LCORE_FOREACH(i){
		printf("active core %d\n", i);
		num_queues++;
	}

	// count required queues
	// for (i = 0; i < rte_lcore_count(); i++)
	// {
	// 	if ((cpu_mask & ((uint64_t)1 << i)) != 0)
	// 		num_queues++;
	// }
	assert(num_numa_nodes <= MEHCACHED_MAX_QUEUES);

	// count numa nodes
	// for (i = 0; i < rte_lcore_count(); i++)
	RTE_LCORE_FOREACH(i)
	{
		uint32_t socket_id = (uint32_t)rte_lcore_to_socket_id((unsigned int)i);
		if (num_numa_nodes <= socket_id)
			num_numa_nodes = socket_id + 1;
	}
	printf("num numa nodes %d\n", num_numa_nodes);
	assert(num_numa_nodes <= MEHCACHED_MAX_NUMA_NODES);

	// initialize pktmbuf
	for (i = 0; i < num_numa_nodes; i++)
	{
		printf("allocating pktmbuf on node %zu... \n", i);
		char pool_name[64];
		snprintf(pool_name, sizeof(pool_name), "pktmbuf_pool%zu", i);
		// if this is not big enough, RX/TX performance may not be consistent, e.g., between CREW and CRCW experiments
		// the maximum cache size can be adjusted in DPDK's .config file: CONFIG_RTE_MEMPOOL_CACHE_MAX_SIZE
		const unsigned int cache_size = 512;
		mehcached_pktmbuf_pool[i] = rte_mempool_create(pool_name, MEHCACHED_MBUF_SIZE, MEHCACHED_MBUF_ENTRY_SIZE, cache_size, sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL, (int)i, 0);
		if (mehcached_pktmbuf_pool[i] == NULL)
		{
			fprintf(stderr, "failed to allocate mbuf for numa node %zu\n", i);
			return false;
		}
	}

	uint8_t port_id;

	// initialize driver
#ifdef RTE_LIBRTE_IXGBE_PMD
	printf("initializing PMD\n");
	// if (rte_ixgbe_pmd_init() < 0)
	// if (rte{
	// if (rte	fprintf(stderr, "failed to initialize ixgbe pmd\n");
	// if (rte	return false;
	// if (rte}
#endif

	printf("probing PCI\n");
	// if (rte_eal_pci_probe() < 0)
	// {
	// 	fprintf(stderr, "failed to probe PCI\n");
	// 	return false;
	// }

	// TODO: initialize and set up timer for forced TX

	// check port and queue limits
	uint8_t num_ports = rte_eth_dev_count_avail();
	// assert(num_ports <= MEHCACHED_MAX_PORTS);
	*out_num_ports = num_ports;

	printf("checking queue limits\n");
	RTE_ETH_FOREACH_DEV(port_id)
	{
		if ((port_mask & ((uint64_t)1 << port_id)) == 0)
			continue;

		struct rte_eth_dev_info dev_info;
		rte_eth_dev_info_get((uint8_t)port_id, &dev_info);

		if (num_queues > dev_info.max_tx_queues || num_queues > dev_info.max_rx_queues)
		{
			fprintf(stderr, "device supports too few queues\n");
			return false;
		}
	}

	// map queues to lcores
	uint32_t lcore = 0;
	uint16_t queue = 0;
	memset(mehcached_lcore_to_queue, -1, MEHCACHED_MAX_LCORES * sizeof(mehcached_lcore_to_queue[0]));
	// for (lcore = 0; lcore < MEHCACHED_MAX_LCORES; lcore++)
	RTE_LCORE_FOREACH(lcore)
	{
		// if ((cpu_mask & ((uint64_t)1 << i)) == 0)
		// {
		// 	mehcached_lcore_to_queue[lcore] = (uint16_t)-1;
		// 	continue;
		// }

		mehcached_lcore_to_queue[lcore] = queue;
		printf("queue %hhu mapped to lcore %hu\n", queue, lcore);
		queue++;
	}

	// initialize ports
	RTE_ETH_FOREACH_DEV(port_id)
	{
		if ((port_mask & ((uint64_t)1 << port_id)) == 0)
			continue;

		printf("initializing port %hhu...\n", port_id);

		// get mac address
		//rte_eth_macaddr_get((uint8_t)port, &mehcached_eth_addr[port]);

		ret = rte_eth_dev_configure(port_id, num_queues, num_queues, &mehcached_port_conf);
		if (ret < 0)
		{
			fprintf(stderr, "failed to configure port %hhu (err=%d)\n", port_id, ret);
			return false;
		}

		uint32_t lcore;
		printf("initializing core queues....\n");
		// for (lcore = 0; lcore < rte_lcore_count(); lcore++)
		RTE_LCORE_FOREACH(lcore)
		{
			struct rte_eth_dev_info dev_info;
			struct rte_eth_txconf *memcached_tx_conf;
			struct rte_eth_txconf *memcached_rx_conf;
			queue = mehcached_lcore_to_queue[lcore];
			printf("lcore %d queue %d\n", lcore, queue);
			if (queue == (uint16_t)-1)
				continue;
			// uint16_t queue = (uint16_t)lcore;

			size_t numa_node = rte_lcore_to_socket_id((unsigned int)lcore);
			printf("setting core %d queue %d\n", lcore, queue);
			rte_eth_dev_info_get((uint8_t)port_id, &dev_info);
			memcached_tx_conf = &dev_info.default_txconf;
			memcached_tx_conf->offloads = mehcached_port_conf.txmode.offloads;
			memcached_rx_conf = &dev_info.default_rxconf;
			memcached_rx_conf->offloads = mehcached_port_conf.rxmode.offloads;

			// ret = rte_eth_rx_queue_setup(port_id, queue, (unsigned int)mehcached_num_rx_desc, (unsigned int)numa_node, &memcached_rx_conf, mehcached_pktmbuf_pool[numa_node]);
			ret = rte_eth_rx_queue_setup(port_id, queue,
						     (unsigned int)mehcached_num_rx_desc,
						     (unsigned int)numa_node, memcached_rx_conf,
						     mehcached_pktmbuf_pool[numa_node]);
			if (ret < 0)
			{
				fprintf(stderr, "failed to configure port %hhu rx_queue %hu (err=%d)\n", port_id, queue, ret);
				return false;
			}

			// ret = rte_eth_tx_queue_setup(port_id, queue, (unsigned int)mehcached_num_tx_desc, (unsigned int)numa_node, &memcached_tx_conf);
			ret = rte_eth_tx_queue_setup(port_id, queue,
						     (unsigned int)mehcached_num_tx_desc,
						     (unsigned int)numa_node,
						     memcached_tx_conf);
			if (ret < 0)
			{
				fprintf(stderr, "failed to configure port %hhu tx_queue %hu (err=%d)\n", port_id, queue, ret);
				return false;
			}
		}

		// start device
		ret = rte_eth_dev_start(port_id);
		if (ret < 0)
		{
			fprintf(stderr, "failed to start port %hhu (err=%d)\n", port_id, ret);
			return false;
		}

// 		// turn on promiscuous mode
// #ifndef NDEBUG
// 		printf("setting promiscuous mode on port %hhu...\n", port_id);
// #endif
// 		rte_eth_promiscuous_enable(port_id);
	}

	// the following takes some time, but this ensures the device ready for full speed RX/TX when the initialization is done
	// without this, the initial packet transmission may be blocked
	RTE_ETH_FOREACH_DEV(port_id)
	{
		if ((port_mask & ((uint64_t)1 << port_id)) == 0)
			continue;

		printf("querying port %hhu... ", port_id);
		fflush(stdout);

		struct rte_eth_link link;
		rte_eth_link_get(port_id, &link);
		if (!link.link_status)
		{
			printf("link down\n");
			return false;
		}

		printf("%hu Gbps (%s)\n", link.link_speed / 1000, (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex"));
	}

	memset(mehcached_queue_states, 0, sizeof(mehcached_queue_states));
	RTE_ETH_FOREACH_DEV(port_id)
	{
		// for (lcore = 0; lcore < rte_lcore_count(); lcore++)
		RTE_LCORE_FOREACH(lcore)
		{
			queue = mehcached_lcore_to_queue[lcore];
			if (queue == (uint16_t)-1)
				continue;
			// uint16_t queue = (uint16_t)lcore;
			mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id] = mehcached_eal_malloc_lcore(sizeof(struct mehcached_queue_state), lcore);
			memset(mehcached_queue_states[queue * MEHCACHED_MAX_PORTS + port_id], 0, sizeof(struct mehcached_queue_state));
		}
	}

	return true;
}

void
mehcached_free_network(uint64_t port_mask)
{
	uint8_t port_id;
	uint8_t num_ports = rte_eth_dev_count_avail();
	
	RTE_ETH_FOREACH_DEV(port_id)
	{
		if ((port_mask & ((uint64_t)1 << port_id)) == 0)
			continue;

		printf("stopping port %hhu...\n", port_id);
		rte_eth_dev_stop(port_id);
	}

	RTE_ETH_FOREACH_DEV(port_id)
	{
		if ((port_mask & ((uint64_t)1 << port_id)) == 0)
			continue;

		printf("closing port %hhu...\n", port_id);
		rte_eth_dev_close(port_id);
	}
}

bool
mehcached_set_dst_port_mask(uint8_t port_id, uint16_t l4_dst_port_mask)
{
	// struct rte_fdir_masks mask;
	// memset(&mask, 0, sizeof(mask));
	// mask.dst_port_mask = l4_dst_port_mask;	// this must be little-endian (host)

	// int ret = rte_eth_dev_fdir_set_masks(port_id, &mask);
	// if (ret < 0)
	// {
	// 	fprintf(stderr, "failed to set perfect filter mask on port %hhu (err=%d)\n", port_id, ret);
	// 	return false;
	// }

	return true;
}

bool
mehcached_set_dst_port_mapping(uint8_t port_id, uint16_t l4_dst_port, uint32_t lcore)
{
	uint16_t queue = mehcached_lcore_to_queue[lcore];
	if (queue == (uint16_t)-1)
	{
		fprintf(stderr, "no queue on port %hhu exists for lcore %u\n", port_id, lcore);
		return false;
	}
	// uint16_t queue = (uint16_t)lcore;
	printf("Mapping port %d to queue %d\n", l4_dst_port, lcore);

	struct rte_flow_attr attr;
	struct rte_flow_item pattern[4];
	struct rte_flow_action action[2];
	struct rte_flow *flow = NULL;
	struct rte_flow_action_queue act_queue = { .index = queue };
	struct rte_flow_item_udp udp_spec;
	struct rte_flow_item_udp udp_mask = {};
	struct rte_flow_error error;
	int res;

	memset(pattern, 0, sizeof(pattern));
	memset(action, 0, sizeof(action));
	memset(&attr, 0, sizeof(struct rte_flow_attr));
	attr.ingress = 1;

	/*
	 * create the action sequence.
	 * one action only,  move packet to queue
	 */
	action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
	action[0].conf = &act_queue;
	action[1].type = RTE_FLOW_ACTION_TYPE_END;

	/*
	 * set the first level of the pattern (ETH).
	 * since in this example we just want to get the
	 * ipv4 we set this level to allow all.
	 */
	pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
	pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
	pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
	pattern[2].spec = &udp_spec;
	pattern[2].mask = &udp_mask;

	memset(&udp_spec, 0, sizeof(struct rte_flow_item_udp));
	memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));
	udp_spec.hdr.dst_port = htons(l4_dst_port);
	udp_mask.hdr.dst_port = 0xffff;
	//udp_spec.hdr.src_port = 123123;
	//udp_mask.hdr.src_port = 0;

	pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

	res = rte_flow_validate(port_id, &attr, pattern, action, &error);
	if (!res)
		flow = rte_flow_create(port_id, &attr, pattern, action, &error);

	if (!flow) {
		printf("Flow can't be created %d message: %s\n",
		       error.type,
		       error.message ? error.message : "(no stated reason)");
		return 0;
	}
////////////////////////////////////////////////////
	// struct rte_fdir_filter filter;
	// memset(&filter, 0, sizeof(filter));
	// filter.iptype = RTE_FDIR_IPTYPE_IPV4;
	// filter.l4type = RTE_FDIR_L4TYPE_UDP;
	// filter.port_dst = rte_cpu_to_be_16((uint16_t)l4_dst_port);    // this must be big-endian
	// 16_t soft_id = (uint16_t)l4_dst_port;	// will be unique on each port (with perfect filter)

	// int ret = rte_eth_dev_fdir_add_perfect_filter(port_id, &filter, soft_id, (uint8_t)queue, 0);
	// if (ret < 0)
	// {
	// 	fprintf(stderr, "failed to add perfect filter entry on port %hhu (err=%d)\n", port_id, ret);
	// 	return false;
	// }

	return 1;
}
