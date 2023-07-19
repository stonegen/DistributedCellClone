#ifndef __cpf_h__
#define __cpf_h__


#include "../Common/globals.h"
#include "map/map.h"
#include "tools/state.h"

void runDataWorker(void *);

class CPF {
 private:
  struct rte_ring *rx_ring;
  struct rte_ring *rep_ring;
  struct rte_mempool *rep_mempool;
  struct CTAConfig *config;
  struct cpf_actions *cpf_actions;
  map<unsigned long int, double> *pcts;
  vector<float> *cpu_loads;
  int hash;

  // Used only for debugging.
  unsigned int rcvd_count;
  unsigned int droped_count;
  CircularMap<unsigned long int, State *>
      state_map;  // Map to keep states of GUTI.

 public:
  CPF(uint16_t id, struct rte_ring *rx_ring,
      int cpf_port, struct CTAConfig *config, struct cpf_actions *actions,
      map<unsigned long int, double> *pcts, vector<float> *cpu_loads);
  ~CPF();

  void enqueueRequest(uint8_t *req, int index);
  void start();

  uint16_t id;
};

#endif
