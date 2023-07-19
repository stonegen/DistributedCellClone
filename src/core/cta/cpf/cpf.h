#ifndef __cpf_h__
#define __cpf_h__

#include <list>
#include <string>
#include <iostream>

#include <rte_ring.h>

#include "../Common/globals.h"

using namespace std;

class CPF {
 private:
  int cpf_port;  // Port number at which this CPF is listening to for other CPFs
  struct rte_ring **rx_rings;
  struct rte_ring **tx_rings;
  struct rte_ring *rep_ring;
  struct rte_mempool *rep_mempool;
  uint8_t id;
  struct CTAConfig *config;
  struct cpf_actions *cpf_actions;
  vector<float> *cpu_loads;
  int hash;

  // Used only for debugging.
  unsigned int rcvd_count;
  unsigned int droped_count;

 public:
  CPF(uint16_t id, struct rte_ring **rx_rings, struct rte_ring **tx_rings,
      int cpf_port, struct CTAConfig *config, struct cpf_actions *actions,
      vector<float> *cpu_loads);
  ~CPF();

  int getCPFPort();
  void setHash(int hash);
  int getHash();
  void enqueueRequest(uint8_t *req, int index);
  void setID(uint8_t id);
  uint8_t getID();
};

#endif
