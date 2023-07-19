#include <signal.h>
#include <libconfig.h++>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>

#include "Common/globals.h"
#include "Common/logger/Logger.h"
#include "src/cpf.h"

using namespace libconfig;
using namespace std;

#define CPU_SOCKETS 2
#define LOCK_FILE "core.lck"

#define NB_MBUF ((1 << 23) - 1)
const unsigned long MAX_RING_SIZE = (1 << 25);

unsigned int curr_cores[CPU_SOCKETS];

#define MEMPOOL_CACHE_SIZE 256
static uint16_t nb_rxd = (1 << 12);
static uint16_t nb_txd = (1 << 12);


volatile bool force_quit = false;
struct rte_mempool *global_mempool = NULL;

const size_t PAYLOAD_OFFSET =
    sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);

const size_t CPF_ID_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t); 

const size_t CPF_TYPE_OFFSET =
    sizeof(char);

void setCPFActions(Setting &action, struct cpf_actions *cpf_actions, int cpf_id);
void *setup_ports(int nb_workers, struct rte_eth_conf port_conf);

struct cpf_args
{
  int id;
  struct cpf_actions *actions;
  struct rte_ring *rx_ring;
  struct CTAConfig *cta_config;
  map<unsigned long int, double> *pcts;
  vector<float> *cpu_loads;
};

static void lock_cpf()
{
  FILE *fptr;
  fptr = fopen(LOCK_FILE, "w");
  fclose(fptr);
}

static void unlock_cpf() { remove(LOCK_FILE); }

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
      char cpf[32];
      sprintf(cpf, "cpf_%d", id);
      *cpf_action = (struct cpf_actions *) rte_malloc(cpf, sizeof(struct cpf_actions), 0);
      action.lookupValue("action", (*cpf_action)->action);

      Setting &config = action.lookup("config");
      for (int j = 0; j < config.getLength(); j++)
      {
        (*cpf_action)->config.push_back(config[j]);
      }
    }
  }
}

void check_port_status(uint8_t portid) {
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 50  /* 5s (50 * 100ms) in total */
  uint8_t count, port_up, print_flag = 0;
  struct rte_eth_link link;

  printf("\nChecking link status");
  fflush(stdout);
  port_up = 1;
  for (count = 0; count <= MAX_CHECK_TIME; count++) {
    if (force_quit) return;
    memset(&link, 0, sizeof(link));
    rte_eth_link_get_nowait(portid, &link);
    /* print link status if flag set */
    if (print_flag == 1) {
      if (link.link_status)
        printf(
            "Port %d Link Up - speed %u "
            "Mbps - %s\n",
            (uint8_t)portid, (unsigned)link.link_speed,
            (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex")
                                                       : ("half-duplex\n"));
      else
        printf("Port %d Link Down\n", (uint8_t)portid);
    }
    /* clear port_up flag if link down */
    if (link.link_status == ETH_LINK_DOWN) {
      port_up = 0;
    }

    /* after finally printing all link status, get out */
    if (print_flag == 1) break;

    if (port_up == 0) {
      printf(".");
      fflush(stdout);
      rte_delay_ms(CHECK_INTERVAL);
    }

    /* set the print_flag if all ports up or timeout */
    if (port_up == 1 || count == (MAX_CHECK_TIME - 1)) {
      print_flag = 1;
      printf("done\n");
    }
  }
}

static unsigned int get_next_lcore(int sock_id)
{
  curr_cores[sock_id] += CPU_SOCKETS;
  return curr_cores[sock_id];
}

static void launch_CPF(struct cpf_args *args)
{

  CPF *new_cpf = new CPF((uint16_t)(args->id), args->rx_ring,
                         args->id + 9091, args->cta_config,
                         args->actions, args->pcts, args->cpu_loads);

  rte_eal_remote_launch(runDataWorker, (void *)new_cpf,
                        get_next_lcore(rte_socket_id()));
}

/* 
  This thread is responsible for the following tasks,
  1. Consuming RX from the CTA server.
  2. Finding which message belongs to which CPF.
  3. Sending messages to the associated CPFs through ring buffers.
 */

static void do_rx(void *arg) {
    struct rte_ring **rx_rings = (struct rte_ring **) arg;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    int ret, nb_rx;
    int count = 0;

    while (!force_quit){

      // Consuming RX from CTA
      nb_rx = rte_eth_rx_burst((uint8_t)PORT, 0, pkts_burst, MAX_PKT_BURST);

      if (nb_rx == 0) {
        continue;
      }

      for (int i = 0; i < nb_rx; i++){
        uint8_t *payload = rte_pktmbuf_mtod_offset(pkts_burst[i], uint8_t *, PAYLOAD_OFFSET);

        // Finding for each message the selected CPF. This bit was set on CTA.
        uint8_t cpf_id = *(payload + CPF_ID_OFFSET);
      
        // Sending the message to the approriate CPF.
        ret = rte_ring_enqueue(rx_rings[cpf_id], pkts_burst[i]);
      }
      count += nb_rx;

    }

    printf("CPF main RX thread recv count: %d\n", count);
}


static void setup_ports(unsigned int nb_workers) {
  uint16_t nb_ports = rte_eth_dev_count();
  if (nb_ports < 2)
    rte_exit(EXIT_FAILURE, "We need atleast 2 ports to work.\n");

  static struct rte_eth_conf port_conf;
  port_conf.rxmode.split_hdr_size = 0;
  port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;

  setup_ports(nb_workers, port_conf);
  check_port_status(PORT);

  return;
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		printf("\n\nSignal %d received, preparing to exit...\n",
			   signum);
		force_quit = true;
	}
}

void *setup_ports(int nb_workers, struct rte_eth_conf port_conf) {

  uint16_t portids[TOTAL_PORTS] = {PORT};
  unsigned int nb_mbufs;
  int ret;
  struct rte_eth_rxconf rxq_conf;
  struct rte_eth_txconf txq_conf;

  struct rte_eth_conf local_port_conf = port_conf;
  struct rte_eth_dev_info dev_info;
  
  nb_mbufs = (1 << 23) - 1;
  struct rte_mempool *rx_mempool =
      rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs, MEMPOOL_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (rx_mempool == NULL) rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

  for (int i = 0; i < TOTAL_PORTS; i++){
    uint16_t portid = portids[i];

    printf("Initializing RX port %u...\n", portid);
    fflush(stdout);

    // Support for multi segment packets.
    rte_eth_dev_info_get(portid, &dev_info);
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
      local_port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    // Setup port for rx and tx queues
    ret = rte_eth_dev_configure(portid, 1, nb_workers, &local_port_conf);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n", ret,
              portid);

    /* init a single RX queue */
    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = local_port_conf.rxmode.offloads;

    ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd, rte_eth_dev_socket_id(portid),
                                &rxq_conf, rx_mempool);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n", ret,
              portid);

    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;

    for (int i = 0; i < nb_workers; i++) {
        ret = rte_eth_tx_queue_setup(portid, i, nb_txd,
                                    rte_eth_dev_socket_id(portid), &txq_conf);
        if (ret < 0)
          rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n", ret,
                  portid);
    }

    /* Start device */
    ret = rte_eth_dev_start(portid);
    if (ret < 0)
      rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n", ret, portid);

    rte_eth_promiscuous_enable(portid);

    printf("RX port %d is up!\n", portid);
    printf("TX port %d is up!\n", portid);
    fflush(stdout);

  }  
}

int main(int argc, char **argv){

  int ret;
  ret = rte_eal_init(argc, argv);
  if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

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

  setup_ports(number_of_cpf);
  global_mempool = rte_pktmbuf_pool_create("Global-mempool", NB_MBUF,
											 MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
											 rte_socket_id());
	if (global_mempool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

  signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
  InitLogger();


  struct cpf_args cpf_args;
  map<unsigned long int, double> pcts;
  vector<float> cpu_loads;
  vector<float> lb_cpu_loads;


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

  cpf_args.rx_ring = NULL;
  cpf_args.cta_config = conf;
  cpf_args.pcts = &pcts;
  cpf_args.cpu_loads = &cpu_loads;


  // RX rings for each cpf.
  unsigned ring_size = MAX_RING_SIZE / 4;
  struct rte_ring *rx_rings[number_of_cpf];

  for (int i = 0; i < number_of_cpf; i++)
  {
    char ring_name[32];
    sprintf(ring_name, "Dequeue_%d", i);

    cpf_args.id = i;

    rx_rings[i] = rte_ring_create(ring_name, ring_size, rte_socket_id(),
                               RING_F_SP_ENQ | RING_F_SC_DEQ);

    struct cpf_actions *action = nullptr;
    setCPFActions(actions, &action, i);
    cpf_args.actions = action;
    cpf_args.rx_ring = rx_rings[i];

    launch_CPF(&cpf_args);
  }

  rte_eal_remote_launch(do_rx, (void *)rx_rings,
                        get_next_lcore(rte_socket_id()));  


  lock_cpf();

  uint16_t i;
  RTE_LCORE_FOREACH_SLAVE(i) {
    if (rte_eal_wait_lcore(i) < 0) {
      ret = -1;
    }
  }

  unlock_cpf();

  for (auto const &x : pcts)
  {
    WriteNumber((uint32_t)x.second);
  }


  return 0;

}