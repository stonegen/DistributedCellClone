#ifndef GLOBALS_H_
#define GLOBALS_H_

#define MAX_PKT_SIZE 1024
#define MAX_PKT_BURST 32

// Ports configuration
#define PORT 3
#define TOTAL_PORTS 1

#include <rte_mempool.h>
#include <vector>

extern volatile bool force_quit;

struct cpf_actions {
  int action;
  std::vector<int> config;
};

struct CTAConfig {
  int serializer;
  int number_of_cpfs;
  int number_of_remote_cpfs;
  int replicas;
  int remote_replicas;
  int tx_arg;
  int delay;
  int procedure;
  std::vector<float> *cpu_loads;
};

extern struct rte_mempool *global_mempool;

#endif
