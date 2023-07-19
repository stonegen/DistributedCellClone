#include <signal.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_udp.h>
#include <rte_hash_crc.h>

#include <mutex>
#include <unistd.h>
#include <libconfig.h++>

#include "Common/logger/Logger.h"
#include "NicInterface/NicInterface.h"
#include "init/initializer.hpp"
#include "cpf/cpf.h"

#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <ctime>

#define LOCK_FILE "core.lck"
#define CPU_SOCKETS 1
unsigned int curr_cores[CPU_SOCKETS];

typedef unsigned long long int nb_bytes_t;

// Number of threads which will dequeue the packets from
// general ring buffer and forward it to CPF.
using namespace libconfig;

volatile bool force_quit = false;

#define RTE_HASH_ENTRIES_MAX 1 << 25

// Not the actuall size of the ring, it is divided into equal
// parts to create rings for dequeue threads.
// Number of dequeue threads is controlled by DEQUEUE_THREADS.
const unsigned long MAX_RING_SIZE = (1 << 25);
const unsigned long CPF_TX_RING_SIZE = 1 << 22;

struct rte_ring *rings[RX_DEQUEUE_THREADS];
struct rte_mempool *global_mempool = NULL;

static struct ether_addr ether_dst = {{0x68, 0x05, 0xca, 0x00, 0x00, 0x01}};
static struct ether_addr zero_mac = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

static inline uint16_t generate_packet(struct rte_mbuf *, uint8_t *, uint32_t);
void setCPFActions(Setting &action, struct cpf_actions *cpf_actions, int cpf_id);
int is_duplicate(struct rte_hash *, struct rte_mbuf *, int, struct CTAConfig *);

NicInterface nicInterface;

const size_t FLAG_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t) + sizeof(uint8_t);

const size_t PAYLOAD_OFFSET =
    sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);

const size_t CPF_ID_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t); 

const size_t CPF_TYPE_OFFSET =
    sizeof(char);

const size_t CPF_TIME_OFFSET =
    sizeof(time_t);

int is_duplicate(struct rte_hash *logical_clocks, struct rte_mbuf *pkt, 
                  int write_quorum_size, struct CTAConfig *conf){

  static logicalClockExtractor ext;
  static bytesToNumber conv;

  uint8_t *data = rte_pktmbuf_mtod_offset(pkt, uint8_t *, PAYLOAD_OFFSET);

  ext.buff[0] = data[8];
	ext.buff[1] = data[9];
	ext.buff[2] = data[10];
	ext.buff[3] = data[11];
	ext.buff[4] = data[12];
	ext.buff[5] = data[13];
	ext.buff[6] = data[14];
	ext.buff[7] = data[15];

  conv.buff[0] = data[16];
  conv.buff[1] = data[17];


  if (data[FLAG_OFFSET] == 1){
    data[FLAG_OFFSET] = 0;
    return 0;
  }


	if (rte_hash_lookup(logical_clocks, &ext.logical_clock) != -ENOENT){

		int count;
		rte_hash_lookup_data(logical_clocks, &ext.logical_clock, (void **) &count);
		rte_hash_add_key_data(logical_clocks, &ext.logical_clock, (void *) ++count);

		if (count == conf->replicas){
			rte_hash_del_key(logical_clocks, &ext.logical_clock);
		}

    if (count == write_quorum_size && (conv.value == 91 || conv.value == 234)) {
      *(data + FLAG_OFFSET) = 77;

      for (int j = 0; j < 0; j++){
      }

      return 2;
    }
    if (count == write_quorum_size) {
      return 0;
    } else {
      return 1;
    }

	}
	else
	{
		int *num = 1;
		rte_hash_add_key_data(logical_clocks, &ext.logical_clock, (void *) num);

    if (conf->replicas == 1){
			rte_hash_del_key(logical_clocks, &ext.logical_clock);
		}

		if (write_quorum_size == 1) {
      return 0;
    } else {
      return 1;
    }

	}
}

/* 
  This thread is responsible for following tasks.
  1. Receive the responses coming from CPFs.
  2. Filter duplicates and handling the write quorum operations.
  3. Forwarding the messages to the transmission workers.
 */


void *listen_on_tx(void *args) {
  
  struct LBRtnInfo *lb_rtn_info = (struct LBRtnInfo*) args;
  struct rte_ring **cpf_tx_rings = lb_rtn_info->lb_rtn_rings;

  int count = 0;
  uint64_t start_time, end_time;
  nb_bytes_t tx_bytes = 0;
  const uint64_t hz = rte_get_timer_hz();
  uint64_t nb_rx;
  int ret;

  // This variable keeps track of the duplicate messages and their state
  // variables.
  struct rte_hash *logical_clocks;
  struct rte_hash_parameters hash_params = {0};

  char ring_name[32];
  hash_params.name = ring_name;
  hash_params.entries = RTE_HASH_ENTRIES_MAX;
  hash_params.key_len = sizeof(unsigned long int);
  hash_params.hash_func = rte_hash_crc;
  hash_params.hash_func_init_val = 0;
  hash_params.socket_id = rte_socket_id();

  logical_clocks = rte_hash_create(&hash_params);
  if (logical_clocks == NULL)
  {
    printf("Failed to create hash table, errno = %d\n", rte_errno);
    rte_panic("Failed to create hash table, errno = %d\n", rte_errno);
  }

  printf("Starting tx thread on lcore %d\n", rte_lcore_id());
  struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

  int ring_index = 0;
  int q = RTE_MAX(RX_DEQUEUE_THREADS, TX_DEQUEUE_THREADS);

  while (!force_quit)
  {
    // Receiving responses from the CPFs.
    nb_rx = rte_eth_rx_burst((uint8_t)CPF_PORT, 0, pkts_burst, MAX_PKT_BURST);

    // uint16_t consumed = 0;

    // time_t currentTime = time(NULL);
    // int returned;
    // for (uint16_t i = 0; i < nb_rx; ++i)
    // {
    //     struct rte_mbuf *pkt = pkts_burst[i];
    //     uint8_t *data = rte_pktmbuf_mtod_offset(pkt, uint8_t*, PAYLOAD_OFFSET);
    //     char type = *(data + CPF_ID_OFFSET + CPF_TYPE_OFFSET);

    //     if (type == 'L')
    //     {
    //         consumed++;
    //     }
    //     else
    //     {
    //         time_t packet_time = *(data + CPF_ID_OFFSET + CPF_TYPE_OFFSET + CPF_TIME_OFFSET);
    //         long time_diff = (currentTime - packet_time) * 1000000L;

    //         if (time_diff >= 50000)
    //         {
    //             consumed++;
    //         }
    //         else
    //         {
    //           returned = rte_eth_tx_burst((uint8_t)CPF_PORT, 0, pkts_burst, MAX_PKT_BURST);
    //         }
    //     }
    // }

    if (nb_rx == 0) {
      continue;
    }

    rte_mbuf *rsp[MAX_PKT_BURST];
    rte_mbuf *rsp_to_cpf[MAX_PKT_BURST];
    int j = 0, k = 0;

    for (int i = 0; i < nb_rx; i++) {
      // Check for the status of message.
      int ret = is_duplicate(logical_clocks, pkts_burst[i], lb_rtn_info->config->tx_arg, lb_rtn_info->config);

      // If the message is not duplicate, save it for further processing.
      if(ret == 0){
          rsp[j] = pkts_burst[i];
          j++; 
      // If the message is duplicate, discard it.
      } else if (ret == 1){
          rte_pktmbuf_free(pkts_burst[i]);
      // If the write quorum is 2 and it is a non-deterministic messge,
      // Return it to the CPF for asynchronous state copying.
      } else if (ret == 2){
          rsp_to_cpf[k] = pkts_burst[i];
          uint8_t *data = rte_pktmbuf_mtod_offset(rsp_to_cpf[k], uint8_t *, PAYLOAD_OFFSET);
          k++;
      } else {
        printf("Wrong return\n");
      }
    }

    // Forward the messages to transmission threads to send the to pktgen.
    if (j > 0){
      ret = rte_ring_enqueue_burst(cpf_tx_rings[ring_index], rsp, j, NULL);
      if (unlikely(ret < j)){
        printf("CTA response ring buffer full");
      }
      update_rx(CPF_PORT, j);
      ring_index = (ring_index + 1) % TX_DEQUEUE_THREADS;
      count += j;
      end_time = rte_get_timer_cycles();
    }

    // Return back the message to the CPFs for asynchronous state copying.
    if (k > 0) {
      uint16_t to_send, sent;
      to_send = k;
      while (to_send != 0) {
        sent = rte_eth_tx_burst((uint8_t)CPF_PORT, q, rsp_to_cpf + (k - to_send), to_send);
        to_send -= sent;
      }
    }
  }

  rte_hash_free(logical_clocks);

  double total_time = (double)(end_time - start_time) / hz;
  double tx_rate = (tx_bytes * 8 * 1e-9) / total_time;

  printf("Cycles: %0.4f\n", total_time);
  printf("Tx Rate %0.4f\n", tx_rate);
}

/* These workers are resposible for following tasks.
   1. Receiving the duplicate filtered messages from the 'listen_on_tx' 
   function.
   2. Sending these messages back to pktgen.
 */
void* flush_tx(void *args) {
  LBInfo *conf = (LBInfo *)args;
  uint64_t count;

  struct rte_mbuf *pkt_burst[MAX_PKT_BURST];
  unsigned int nb_rx;

  while(!force_quit) {


    // Receiving messages from the ring buffers filled by the 'listen_on_tx' 
    // thread.
    nb_rx =
          rte_ring_dequeue_burst(conf->ring, (void **)pkt_burst, MAX_PKT_BURST, NULL);

    if (nb_rx == 0){
      continue;
    }


    // Sending the message back to the pktgen.
    uint16_t to_send, sent;
    to_send = nb_rx;
    count += nb_rx;
    while (to_send != 0) {
      sent = rte_eth_tx_burst((uint8_t)TX_PORT, conf->id, pkt_burst + (nb_rx - to_send), to_send);
      to_send -= sent;
    }

    update_tx(TX_PORT, nb_rx);

  }

  printf("Count at CTA TX thread: %u\n", count);
}

static inline uint16_t generate_packet(struct rte_mbuf *buf, uint8_t *response,
                                       uint32_t size) {
  buf->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
  buf->l2_len = sizeof(struct ether_hdr);
  buf->l3_len = sizeof(struct ipv4_hdr);
  buf->l4_len = sizeof(struct udp_hdr);

  struct ether_hdr *eth_hdr;
  struct ipv4_hdr *ip_hdr;
  struct udp_hdr *udp_hdr;
  struct tcp_hdr *tcp_hdr;
  uint16_t pkt_size = MAX_PKT_SIZE;

  buf->pkt_len = pkt_size;
  buf->data_len = pkt_size;

  eth_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);
  if (is_zero_ether_addr(&zero_mac))
  {
    ether_addr_copy(&ether_dst, &eth_hdr->d_addr);
  }
  else
  {
    ether_addr_copy(&zero_mac, &eth_hdr->d_addr);
  }

  if (is_zero_ether_addr(&zero_mac))
  {
    ether_addr_copy(&zero_mac, &eth_hdr->s_addr);
  }
  else
  {
    ether_addr_copy(&zero_mac, &eth_hdr->s_addr);
  }

  eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

  ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
  ip_hdr->type_of_service = 0;
  ip_hdr->fragment_offset = 0;
  ip_hdr->time_to_live = 64;
  ip_hdr->next_proto_id = IPPROTO_UDP;
  ip_hdr->packet_id = 0;
  ip_hdr->version_ihl = (1 << 6) + 5;
  ip_hdr->total_length = rte_cpu_to_be_16(pkt_size - sizeof(struct ether_hdr));
  ip_hdr->src_addr = 0xAABB;
  ip_hdr->dst_addr = 0xCCDD;
  ip_hdr->hdr_checksum = 0;

  udp_hdr = (struct udp_hdr *)(ip_hdr + 1);
  udp_hdr->src_port = 0xAABB;
  udp_hdr->dst_port = 0xCCDD;
  udp_hdr->dgram_cksum = 0;
  udp_hdr->dgram_len =
      rte_cpu_to_be_16(pkt_size - sizeof(struct ether_hdr) - sizeof(*ip_hdr));

  uint8_t *ptr = rte_pktmbuf_mtod_offset(buf, uint8_t *, PAYLOAD_OFFSET);

  rte_memcpy((void *)ptr, response, size);
  return pkt_size;
}

std::mutex mtx;

static inline struct rte_mbuf *rte_pktmbuf_copy(struct rte_mbuf *md,
                                                struct rte_mempool *mp){
	struct rte_mbuf *mc = NULL;
	struct rte_mbuf **prev = &mc;

	do {
		struct rte_mbuf *mi;

		mi = rte_pktmbuf_alloc(mp);
		if (unlikely(mi == NULL)) {
			rte_pktmbuf_free(mc);
			return NULL;
		}

		mi->data_off = md->data_off;
		mi->data_len = md->data_len;
		mi->port = md->port;
		mi->vlan_tci = md->vlan_tci;
		mi->tx_offload = md->tx_offload;
		mi->hash = md->hash;

		mi->next = NULL;
		mi->pkt_len = md->pkt_len;
		mi->nb_segs = md->nb_segs;
		mi->ol_flags = md->ol_flags;
		mi->packet_type = md->packet_type;

		rte_memcpy(rte_pktmbuf_mtod(mi, char *),
			   rte_pktmbuf_mtod(md, char *),
			   md->data_len);

		*prev = mi;
		prev = &mi->next;
	} while ((md = md->next) != NULL);

	*prev = NULL;
	__rte_mbuf_sanity_check(mc, 1);
	return mc;
}

/* 
  processPkt workers are responsible for the following tasks,
  1. Receiving burst messages from the 'recvPkts' thread.
  2. Creating copies for each message equal to the 'replicas' number mentioned in 
  the config.json file.
  3. Searching the consistent hash ring to find the appropriate CPF for
  each message.
  4. Sending each message to the CPFs through DPDK apis.
 */

void *processPkt(void *arg)
{
  printf("Starting a separate thread for sending packets to CPF on core %d\n",
         rte_lcore_id());

  struct LBInfo *lb_args = (struct LBInfo *)arg;
  int ring_index = lb_args->id;
  struct rte_ring *ring = lb_args->ring;

  CustomHeaderParser header_parser;
  CustomAppHeader header;
  struct rte_mbuf *pkt_burst[MAX_PKT_BURST];
  unsigned int nb_rx, i;

  uint64_t rx_count = 0;
  uint64_t rx_start_time = 0;

  const uint64_t hz = rte_get_timer_hz();
  double processing_cycles = 0, total_cycles = -1;
  double current_cycles = rte_get_timer_cycles();

  while (!force_quit)
  {

    // Dequeueing messages coming from the 'recvPkts' thread.
    nb_rx =
        rte_ring_dequeue_burst(ring, (void **)pkt_burst, MAX_PKT_BURST, NULL);

    if (total_cycles != -1){
      total_cycles += (double)(rte_get_timer_cycles() - current_cycles) / hz;
      current_cycles = rte_get_timer_cycles();
    } 

    if (nb_rx == 0)
      continue;

    if (unlikely(rx_count == 0))
    {
      rx_start_time = rte_get_timer_cycles();
      total_cycles = (double) (rte_get_timer_cycles() - current_cycles) / hz;
      current_cycles = rte_get_timer_cycles();
    }

    for (i = 0; i < nb_rx; i++)
    {
      struct rte_mbuf *rsp[lb_args->config->replicas];
      rsp[0] = pkt_burst[i];

      uint8_t *to_copy = rte_pktmbuf_mtod_offset(rsp[0], uint8_t *, PAYLOAD_OFFSET);

      header_parser.Parse(to_copy);
      header = header_parser.controlMessage.header;

      // Creating copies of messages depending on total 'replicas' selected.
      for (int j = 1; j < lb_args->config->replicas; j++)
      {
        rsp[j] = rte_pktmbuf_copy(rsp[0], global_mempool);
      }
      nicInterface.OnPayloadReceived(rsp, header, lb_args->config, ring_index);
    }

    update_tx(CPF_PORT, nb_rx);
    rx_count += nb_rx;
    processing_cycles += (double) (rte_get_timer_cycles() - current_cycles) / hz;
  }

  mtx.lock();
  (lb_args->config->cpu_loads)->push_back((processing_cycles / total_cycles) * 100);
  mtx.unlock();
  return NULL;
}

// Given a packet, this fucntion adds an always increasing logical clock to the packet header.
static inline void add_logical_clock(struct rte_mbuf **pkts, uint16_t nb_rx,
                                     nb_bytes_t *rx_bytes) {
  static lgclock_t lg_clock = 9;
  const static size_t lg_clock_offset = sizeof(lgclock_t);
  uint8_t *ptr;

  for (int i = 0; i < nb_rx; i++)
  {
    // Jump to logical clock ....
    ptr = rte_pktmbuf_mtod_offset(pkts[i], uint8_t *, PAYLOAD_OFFSET);

    logicalClockExtractor ext;
    ext.logical_clock = lg_clock;

    ptr = ptr + lg_clock_offset;

    for (int i = 0; i < sizeof(lgclock_t); i++)
    {
      *(ptr + i) = ext.buff[i];
    }

    lg_clock++;
    *rx_bytes += pkts[i]->pkt_len;
  }
}

/* This is the main function for RX thrad.
   This fucntion reads the packets from the NIC using dpdk's internal function call,
   adds an always increasing logical clock and sends the complete bursts of packets
   to a dequeue-thread.
*/
static void recvPkts(void)
{
  uint64_t count = 0;
  uint64_t index = 0;
  unsigned drop_count = 0;
  uint64_t t_start = 0;
  uint64_t t_end = 0;
  unsigned lcore_id;
  uint16_t nb_rx;
  unsigned int ret;
  struct rte_mbuf *pkt_burst[MAX_PKT_BURST];
  nb_bytes_t rx_bytes = 0;

  const uint64_t hz = rte_get_timer_hz();
  lcore_id = rte_lcore_id();

  printf("Starting master thread for RX on lcore %u\n", lcore_id);
  while (!force_quit)
  {
    // Consumes RX
    nb_rx = rte_eth_rx_burst((uint8_t)RX_PORT, 0, pkt_burst, MAX_PKT_BURST);
    if (nb_rx == 0)
      continue;

    if (unlikely(count == 0))
      t_start = rte_get_timer_cycles();

    add_logical_clock(pkt_burst, nb_rx, &rx_bytes);

    update_rx(RX_PORT, nb_rx);
    count += nb_rx;

    // Forwarding message burst to workers through their ring buffers.
    ret = rte_ring_enqueue_burst(rings[index], pkt_burst, nb_rx, NULL);
    if (unlikely(ret < nb_rx))
    {
      drop_count += nb_rx - ret;
      while (ret != nb_rx)
      {
        rte_pktmbuf_free(pkt_burst[ret]);
        ret++;
      }
    }
    index = (index + 1) % RX_DEQUEUE_THREADS;
    t_end = rte_get_timer_cycles();
  }

  update_stats();

  // ===================================================
  printf("Received count at main RX: %u\n", count);
  printf("Pkts droped at RX: %u\n", drop_count);
  double total_time = (double)(t_end - t_start) / hz;
  double rx_rate = (rx_bytes * 8 * 1e-9) / total_time;
  printf("Rx Rate: %0.4f Gbps\n", rx_rate);
}

static void lock_cta()
{
  FILE *fptr;
  fptr = fopen(LOCK_FILE, "w");
  fclose(fptr);
}

static void unlock_cta() { remove(LOCK_FILE); }

static unsigned int get_next_lcore(int sock_id)
{
  curr_cores[sock_id] += CPU_SOCKETS;
  printf("Used cores: %d\n", curr_cores[sock_id]);
  return curr_cores[sock_id];
}

struct cpf_args
{
  int id;
  struct cpf_actions *actions;
  struct rte_ring **tx_rings;
  struct CTAConfig *cta_config;
  vector<float> *cpu_loads;
};

void setCPFActions(Setting &actions, struct cpf_actions **cpf_action, int cpf_id)
{
  cpf_id++;
  for (int i = 0; i < actions.getLength(); i++)
  {
    Setting &action = actions[i];
    int id;

    action.lookupValue("cpf", id);

    if (id == cpf_id)
    {
      *cpf_action = rte_malloc(NULL, sizeof(struct cpf_actions), 0);
      action.lookupValue("action", (*cpf_action)->action);

      Setting &config = action.lookup("config");
      for (int j = 0; j < config.getLength(); j++)
      {
        (*cpf_action)->config.push_back(config[j]);
      }
    }
  }
}

static void create_CPF(struct cpf_args *args)
{

  CPF *new_cpf = new CPF((uint16_t)(args->id), NULL, NULL,
                         args->id + 9091, args->cta_config,
                         args->actions, args->cpu_loads);
  new_cpf->setID((uint8_t) args->id);

  nicInterface.controller->addNode(new_cpf);
}

int main(int argc, char **argv)
{
  struct cpf_args cpf_args;

  InitLogger();
  vector<float> cpu_loads;
  vector<float> lb_cpu_loads;

  Config cfg;
  cfg.readFile("cta_config.cfg");
  int serializer = cfg.lookup("serializer");
  int number_of_cpf = cfg.lookup("number_of_cpf");
  int number_of_remote_cpfs = cfg.lookup("number_of_remote_cpfs");
  int replicas = cfg.lookup("replicas");
  int remote_replicas = cfg.lookup("remote_replicas");
  int tx_arg = cfg.lookup("tx_arg");
  int delay = cfg.lookup("delay");
  int procedure = cfg.lookup("procedure");
  Setting &actions = cfg.lookup("cpfs_action");
  

  struct CTAConfig *conf = malloc(sizeof(CTAConfig));
  conf->serializer = serializer;
  conf->number_of_cpfs = number_of_cpf;
  conf->number_of_remote_cpfs = number_of_remote_cpfs;
  conf->replicas = replicas;
  conf->remote_replicas = remote_replicas;
  conf->tx_arg = tx_arg;
  conf->delay = delay;
  conf->procedure = procedure;
  conf->cpu_loads = &lb_cpu_loads;
  setup_cta(argc, argv);
  printf("===================================================\n");
  nicInterface.Init();
  unsigned ring_size = MAX_RING_SIZE / 4;

  for (unsigned int i = 0; i < CPU_SOCKETS; i++)
  {
    curr_cores[i] = i;
  }

  // Spawns dequeue threads
  for (int i = 0; i < RX_DEQUEUE_THREADS; i++)
  {
    char ring_name[32];
    sprintf(ring_name, "Dequeue_%d", i);
    rings[i] = rte_ring_create(ring_name, ring_size, rte_socket_id(),
                               RING_F_SP_ENQ | RING_F_SC_DEQ);

    struct LBInfo *info = (struct LBInfo *)malloc(sizeof(LBInfo));
    info->id = i;
    info->ring = rings[i];
    info->config = conf;

    rte_eal_remote_launch(processPkt, info,
                          get_next_lcore(rte_socket_id()));
  }


  struct rte_ring **cpf_tx_rings = rte_malloc(NULL, sizeof(rte_ring*), 0);

  cpf_args.tx_rings = NULL;
  cpf_args.cta_config = conf;

  // Spawn multiple CPFs
  for (int i = 0; i < number_of_cpf; i++)
  {
    cpf_args.id = i;

    struct cpf_actions *action = nullptr;
    setCPFActions(actions, &action, i);
    cpf_args.actions = action;
    create_CPF(&cpf_args);
  }

  nicInterface.setRxRings(NULL);

  for (uint8_t i = 0; i < TX_DEQUEUE_THREADS; i++){

    char ring_name[32];
    sprintf(ring_name, "tx_ring_%d", i);
    cpf_tx_rings[i] = rte_ring_create(
        ring_name, CPF_TX_RING_SIZE, rte_socket_id(), RING_F_SC_DEQ);


    struct LBInfo *info = (struct LBInfo *) malloc(sizeof(LBInfo));
    info->id = i;
    info->config = conf;
    info->ring = cpf_tx_rings[i];

    rte_eal_remote_launch((lcore_function_t *) flush_tx, info, get_next_lcore(rte_socket_id()));
  }

  struct LBRtnInfo *rtn_info = (struct LBRtnInfo *) malloc(sizeof(LBRtnInfo));
  rtn_info->lb_rtn_rings = cpf_tx_rings;
  rtn_info->config = conf;

  rte_eal_remote_launch((lcore_function_t *) listen_on_tx, rtn_info, get_next_lcore(rte_socket_id()));

  check_ports();
  lock_cta();

  rte_eal_remote_launch((lcore_function_t *) recvPkts, NULL, get_next_lcore(rte_socket_id()));

  uint16_t i;
  RTE_LCORE_FOREACH_SLAVE(i) {
    if (rte_eal_wait_lcore(i) < 0) {
      break;
    }
  }

  exit_cta();
  unlock_cta();
  free(conf);

  float sum = 0;
  for (float st: cpu_loads)
  {
      sum += st;
  }

  printf("Average CPU Load: %f\n", sum / cpu_loads.size());

  sum = 0;
  for (float st: lb_cpu_loads)
  {
      sum += st;
  }

  printf("Average LB CPU Load: %f\n", sum / lb_cpu_loads.size());

  CloseLogger();
  return 0;
}