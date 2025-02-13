#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#include <rte_io.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_rawdev.h>
#include <rte_rawdev_pmd.h>
#include <rte_memcpy.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_dev.h>
#include <rte_kvargs.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_lcore.h>
#include <rte_bus.h>
#include <rte_bus_vdev.h>
#include <rte_memzone.h>
#include <rte_mempool.h>
#include <rte_rwlock.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#include <pthread.h>
#include <time.h>
#include <sched.h>

#include "ntb_mw.h"
#include "ntp_func.h"
#include "ntb.h"
#include "ntb_hw_intel.h"
#include "ntm_msg.h"
#include "ntp2nts_shm.h"
#include "ntm_ntp_shm.h"
#include "ntp_ntm_shm.h"
#include "config.h"
#include "nt_log.h"
DEBUG_SET_LEVEL(DEBUG_LEVEL_DEBUG);

#define NTP_CONFIG_FILE "/etc/ntp.cfg"

#define XEON_LINK_STATUS_OFFSET 0x01a2
#define RTE_RAWDEV_MAX_DEVS 64
#define NTB_DRV_NAME_LEN 7
#define EXIT_FAILURE 1

#define CREATE_CONN 1
#define DEFAULT_SLEEP_US 100
#define SPIN_THRESHOLD 100

static uint16_t dev_id;

struct ntb_link_custom *ntb_link;

// define the CPU cores set for ntb_partitions as an array
// 1. the array index indicates the ntb_partition id
// 2. the array value indicates the assigned CPU cores lcore_id;
static int cpu_cores[MAX_NUM_PARTITION * 2] = 
	{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 32};

static struct ntp_lcore_conf lcore_conf[RTE_MAX_LCORE];

static int
ntb_send_thread(__attribute__((unused)) void *arg)
{
	assert(ntb_link);
	assert(arg);

	struct lcore_ctx * core_ctx;
	core_ctx = (struct lcore_ctx *) arg;

	ntb_partition_t partition;
	partition = (ntb_partition_t)core_ctx->partition;
	// DEBUG("enter partition %d", partition->id);
	// printf("[SEND] enter partition %d\n", partition->id);

	struct ntp_lcore_conf * conf;
	conf = core_ctx->conf;

	ntp_send_list_node *pre_node = partition->send_list.ring_head;
	ntp_send_list_node *curr_node = NULL;
	uint64_t counter = 0;
	uint64_t loop_cnt = 0;

	memset(partition->data_link->remote_ring->start_addr, 0, NTP_CONFIG.data_ringbuffer_size);

	while (!ntb_link->is_stop)
	{
		curr_node = pre_node->next_node;

		loop_cnt++;
		if (loop_cnt % 100000 == 0)
        {
            if (conf->stopped)
                break;

            loop_cnt = 0;
        }

		// indicate non-existing ntb connection
		//	when head node in ntb-conn list is EMPTY.
		if (curr_node == partition->send_list.ring_head)
		{
			pre_node = curr_node;
			continue;
		}
		// when the ntb connection state is `PASSIVE CLOSE` or `ACTIVE CLOSE`,
		//	remove/clear current ntb connection
		if (curr_node->conn->state == PASSIVE_CLOSE ||
			curr_node->conn->state == ACTIVE_CLOSE)
		{
			DEBUG("conn close,remove and free node");

			// remove ntb_partition
			curr_node->conn->partition_id = -1;
			curr_node->conn->partition = NULL;

			// conn->state 不为READY，队列均已Close，移除map、list并free就可
			//	remove ntb conn from hash map
			Remove(ntb_link->port2conn, &curr_node->conn->conn_id);

			// remove ntb conn from traseval list
			pre_node->next_node = curr_node->next_node;
			if (curr_node == partition->send_list.ring_tail)
			{
				partition->send_list.ring_tail = pre_node;
			}

			ntp_shm_nts_close(curr_node->conn->nts_recv_ring);
			ntp_shm_destroy(curr_node->conn->nts_recv_ring);
			ntp_shm_nts_close(curr_node->conn->nts_send_ring);
			ntp_shm_destroy(curr_node->conn->nts_send_ring);

			free(curr_node->conn);
			free(curr_node);

			continue;
		}

		ntp_send_buff_data(partition->data_link, partition,
						   curr_node->conn->nts_send_ring, curr_node->conn);
		pre_node = curr_node; // move the current point to next ntb conn
	}

	printf("\n[Partition-%d] SEND Thread EXIT\n", partition->id);
	free(core_ctx);
	core_ctx = NULL;

	return 0;
}

static int
ntb_receive_thread(__attribute__((unused)) void *arg)
{
	assert(ntb_link);
	assert(arg);

	struct lcore_ctx * core_ctx;
	core_ctx = (struct lcore_ctx *) arg;

	ntb_partition_t partition;
	partition = (ntb_partition_t)core_ctx->partition;
	// DEBUG("enter partition %d", partition->id);
	// printf("[RECV] enter partition %d\n", partition->id);

	ntp_recv_data_to_buf(
		partition->data_link, ntb_link, partition, core_ctx->conf);

	printf("\n[Partition-%d] RECV Thread EXIT\n", partition->id);
	free(core_ctx);
	core_ctx = NULL;

	return 0;
}

static void *
ntb_ctrl_receive_thread(__attribute__((unused)) void *arg)
{
	ntp_ctrl_msg_receive(ntb_link);

	return NULL;
}

static void *
ntm_ntp_receive_thread(__attribute__((unused)) void *arg)
{
	ntm_ntp_shm_context_t recv_shm = ntb_link->ntm_ntp;
	ntm_ntp_msg recv_msg;
	uint64_t loop_spin_cnt = 0; // default sleep, when loop_spin_cnt > 10

	while (!ntb_link->is_stop)
	{
		if (UNLIKELY(ntm_ntp_shm_recv(recv_shm, &recv_msg) == -1))
		{
			sched_yield();
			loop_spin_cnt++;
			if (loop_spin_cnt == SPIN_THRESHOLD)
			{
				loop_spin_cnt = 0;
				usleep(DEFAULT_SLEEP_US);
			}

			continue;
		}
		else
		{
			DEBUG("receive ntm_ntp_msg, create ntb_conn");
			loop_spin_cnt = 0;
			if (recv_msg.msg_type == CREATE_CONN)
			{
				ntp_create_conn_handler(ntb_link, &recv_msg);
			}
		}
	}

	return NULL;
}

// for quit signal
static volatile bool s_signal_quit = false;
static volatile int s_signum = -1;

static void before_exit(void)
{
	INFO("destroy all ntm resources when on exit.");
	if (!s_signal_quit) 
	{
		s_signal_quit = true;
		ntb_destroy(ntb_link, lcore_conf);

		if (s_signum != -1)
		{
			kill(getpid(), s_signum);
		}
	}
}

static void crash_handler(int signum)
{
	printf("\n[Crash]: Signal %d received, preparing to exit...\n", signum);
	s_signum = signum;
	exit(-1);
}

static void signal_exit_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) 
	{
		printf("\nSignal %d received, preparing to exit...\n", signum);
		s_signum = signum;
	}
	exit(0);
}


static void *
ntp_epoll_listen_thread(__attribute__((unused)) void *arg)
{
	assert(ntb_link);

	epoll_sem_shm_ctx_t ep_recv_ctx = ntb_link->ntp_ep_recv_ctx;
	epoll_sem_shm_ctx_t ep_send_ctx = ntb_link->ntp_ep_send_ctx;

	epoll_msg req_msg;
	int rc;

	while (!ntb_link->is_stop)
	{
		rc = epoll_sem_shm_recv(ep_recv_ctx, &req_msg);
		if (UNLIKELY(rc != 0 || ntb_link->is_stop))
		{
			break;
		}
		
		DEBUG("receive one epoll_msg from ntm");
		rc = ntp_handle_epoll_msg(ntb_link, &req_msg);
		if (rc != 0)
		{
			break;
		}
	}

	INFO("ntp_epoll_listen_thread exit!");
	return NULL;
}


/* for NTP usage and cmd params parser */

#define OPT_MTU_SIZE "mtu-size"
#define OPT_NUM_PART "num-partitions"
#define OPT_BULK_SIZE "bulk-size"
#define OPT_CONF_FILE "cfg-path"

enum
{
	/* long options mapped to a short option */
	OPT_MTU_SIZE_NUM = 1,
	OPT_NUM_PART_NUM = 2,
	OPT_BULK_SIZE_NUM = 3,
	OPT_CONF_FILE_NUM = 4,
};

static const char short_options[] = "h";

static const struct option lgopts[] = {
	{OPT_MTU_SIZE, 2, NULL, OPT_MTU_SIZE_NUM},
	{OPT_NUM_PART, 2, NULL, OPT_NUM_PART_NUM},
	{OPT_BULK_SIZE, 2, NULL, OPT_BULK_SIZE_NUM},
	{OPT_CONF_FILE, 2, NULL, OPT_CONF_FILE_NUM},
	{0, 0, NULL, 0}
};

static void
ntp_usage(const char *prgname)
{
	printf("\nUsage: %s [EAL options] -- [options]\n"
		   "Startup NTP daemon for NTB transport.\n\n"
		   "\t-h: print usage.\n"
		   "\t--mtu-size=N: set MTU size as N (8 < N < 65536, default: %d).\n"
		   "\t--num-partitions=N: set the number of parllel partitions as N"
		   " (1 <= N <= %d, default: %d).\n"
		   "\t--bulk-size=N: set bulk size of batching packet forwarding as N"
		   " (1 <= N <= 65536, default: %d).\n"
		   "\t--cfg-path=[file path]: set file path of ntp config 'ntp.cfg',"
		   " (default: '%s').\n",
		   prgname, DEFAULT_MTU_SIZE, MAX_NUM_PARTITION,
		   DEFAULT_NUM_PARTITION, DEFAULT_BULK_SIZE, DEFAULT_CFG_PATH);
}

static void
ntp_parse_args(int argc, char **argv)
{
	char *prgname = argv[0], **argvopt = argv;
	int opt, opt_idx, n, i;

	while ((opt = getopt_long(argc, argvopt, short_options,
							  lgopts, &opt_idx)) != EOF)
	{
		switch (opt)
		{
		case 'h':
			ntp_usage(prgname);
			rte_exit(EXIT_SUCCESS, "\nprint NTP usage tips.\n");
			break;
		case OPT_MTU_SIZE_NUM:
			n = atoi(optarg);
			if (n > 8 && n < 65536)
			{
				NTP_CONFIG.data_packet_size = n;
				NTP_CONFIG.ntb_packetbits_size = math_log2(NTP_CONFIG.data_packet_size);
			}
			else
			{
				rte_exit(EXIT_FAILURE, "mtu size must be > 8 and < 65536.\n");
			}
			break;
		case OPT_NUM_PART_NUM:
			n = atoi(optarg);
			if (n >= 1 && n <= MAX_NUM_PARTITION)
			{
				NTP_CONFIG.num_partition = n;
			}
			else
			{
				rte_exit(EXIT_FAILURE, "number of parallel partitions "
						"must be >= 1 and n <= %d.\n",
						MAX_NUM_PARTITION);
			}
			break;
		case OPT_BULK_SIZE_NUM:
			n = atoi(optarg);
			if (n >= 1 && n <= 65536)
			{
				NTP_CONFIG.bulk_size = n;
			}
			else 
			{
				rte_exit(EXIT_FAILURE, "bulk size of batching packet forwarding "
						"must be >= 1 and <= 65536.\n");
			}
			break;
		case OPT_CONF_FILE_NUM:
			if (optarg && strlen(optarg) > 0)
			{
				if ((access(optarg, F_OK)) != -1)
				{
					memcpy(ntp_cfg_path, optarg, strlen(optarg));
					int rc = load_conf(ntp_cfg_path);
					if (rc != 0)
					{
						rte_exit(EXIT_FAILURE, "Cannot load config file '%s', "
								"please double check.\n", ntp_cfg_path);
					}
				}
			}
			else 
			{
				rte_exit(EXIT_FAILURE, "Invalid ntp config file path.\n");
			}
			break;

		default:
			ntp_usage(prgname);
			rte_exit(EXIT_FAILURE, 
					"Command line is incomplete or incorrect.\n");
			break;
		}
	}
}


int main(int argc, char **argv)
{
	uint64_t ntb_link_status;
	int ret, i;

	// register exit event processing
	s_signal_quit = false;
	atexit(before_exit);
	signal(SIGTERM, signal_exit_handler);
	signal(SIGINT, signal_exit_handler);

	signal(SIGBUS, crash_handler);  // 总线错误
	signal(SIGSEGV, crash_handler); // SIGSEGV，非法内存访问
	signal(SIGFPE, crash_handler);  // SIGFPE，数学相关的异常，如被0除，浮点溢出，等等
	signal(SIGABRT, crash_handler); // SIGABRT，由调用abort函数产生，进程非正常退出

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
	{
		rte_exit(EXIT_FAILURE, "Error with EAL initialization.\n");
	}

	if (rte_lcore_count() < 2) 
	{
		rte_exit(EXIT_FAILURE, "Need at least 2 cores\n");
	}

	/* Find 1st ntb rawdev. */
	for (i = 0; i < RTE_RAWDEV_MAX_DEVS; i++)
	{
		if (rte_rawdevs[i].driver_name &&
			(strncmp(rte_rawdevs[i].driver_name, "raw_ntb", NTB_DRV_NAME_LEN) == 0) &&
			(rte_rawdevs[i].attached == 1))
		{
			break;
		}
	}

	if (i == RTE_RAWDEV_MAX_DEVS)
	{
		rte_exit(EXIT_FAILURE, "Cannot find any ntb device.\n");
	}
	dev_id = i;

	if (load_conf(DEFAULT_CFG_PATH) == -1)
	{
		rte_exit(EXIT_FAILURE, "Cannot load default config file '%s', "
								"please double check.\n", ntp_cfg_path);
	}

	argc -= ret;
	argv += ret;

	ntp_parse_args(argc, argv);

	/** Load default NTP config file if not set. */
	if (strlen(ntp_cfg_path) <= 0) 
	{
		memcpy(ntp_cfg_path, DEFAULT_CFG_PATH, strlen(DEFAULT_CFG_PATH));
	}
	else if (load_conf(ntp_cfg_path) == -1)
	{
		rte_exit(EXIT_FAILURE, "Cannot load config file '%s', "
								"please double check.\n", ntp_cfg_path);
	}
	NTP_CONFIG.datapacket_payload_size =
		NTP_CONFIG.data_packet_size - NTPACKET_HEADER_LEN;
	print_conf();


	/* Waiting for peer dev up at most 100s.*/
	printf("Checking ntb link status...\n");
	for (i = 0; i < 1000; i++) {
		rte_rawdev_get_attr(dev_id, NTB_LINK_STATUS_NAME,
				    &ntb_link_status);
		if (ntb_link_status) {
			printf("Peer dev ready, ntb link up.\n");
			break;
		}
		rte_delay_ms(1000);
	}
	rte_rawdev_get_attr(dev_id, NTB_LINK_STATUS_NAME, &ntb_link_status);
	if (ntb_link_status == 0)
		printf("Expire 100s. Link is not up. Please restart app.\n");


	ntb_link = ntb_start(dev_id);
	if (!ntb_link) 
	{
		fprintf(stderr, "\n*********** NTB Link start failed ***********\n");
		rte_exit(EXIT_FAILURE, "NTB Link start failed.\n");
	}

	if (!ntb_link->hw)
	{
		ERR("Invalid device.");
		rte_exit(EXIT_FAILURE, "Invalid NTB device.\n");
	}

	uint16_t reg_val;
	ret = rte_pci_read_config(ntb_link->hw->pci_dev, &reg_val,
							  sizeof(reg_val), XEON_LINK_STATUS_OFFSET);
	if (ret < 0)
	{
		ERR("Unable to get NTB link status.");
        rte_exit(EXIT_FAILURE, "Unable to get NTB link status.\n");
	}

	ntb_link->hw->link_status = NTB_LNK_STA_ACTIVE(reg_val);
	if (ntb_link->hw->link_status)
	{
		ntb_link->hw->link_speed = NTB_LNK_STA_SPEED(reg_val);
		ntb_link->hw->link_width = NTB_LNK_STA_WIDTH(reg_val);
	}

	struct ntp_lcore_conf *conf;
	struct lcore_ctx *core_ctx;
	uint32_t lcore_id;
	printf("allocate cpu start\n");
	RTE_LCORE_FOREACH_SLAVE(lcore_id)
	{
		conf = &lcore_conf[lcore_id];
		conf->lcore_id = lcore_id;
		conf->is_enabled = 0;
		conf->stopped = 1;

		for (int i = 0; i < ntb_link->num_partition; i++)
		{
			int j = i * 2;
			if (lcore_id == cpu_cores[j])
			{
				conf->is_enabled = 1;
				conf->stopped = 0;

				core_ctx = (struct lcore_ctx *)calloc(1, sizeof(struct lcore_ctx));
				assert(core_ctx);

				core_ctx->conf = conf;
				core_ctx->partition = &ntb_link->partitions[i];

				rte_eal_remote_launch(ntb_receive_thread,
									  (void *)core_ctx, lcore_id);
			}
			if (lcore_id == cpu_cores[j + 1])
			{
				conf->is_enabled = 1;
				conf->stopped = 0;

				core_ctx = (struct lcore_ctx *)calloc(1, sizeof(struct lcore_ctx));
				assert(core_ctx);

				core_ctx->conf = conf;
				core_ctx->partition = &ntb_link->partitions[i];

				rte_eal_remote_launch(ntb_send_thread,
									  (void *)core_ctx, lcore_id);
			}
		}
	}
	printf("allocate cpu end\n");
	pthread_create(&ntb_link->ntm_ntp_listener,
				   NULL, ntm_ntp_receive_thread, NULL);
	pthread_create(&ntb_link->ctrl_recv_thr,
				   NULL, ntb_ctrl_receive_thread, NULL);
	pthread_create(&ntb_link->epoll_listen_thr,
				   NULL, ntp_epoll_listen_thread, NULL);

	printf("start ctrl thread end\n");			   
	rte_eal_mp_wait_lcore();

	printf("**************** ntp exit... ****************\n");

	return 0;
}
