#include <iostream>
#include "cpf.h"

using namespace std;

CPF::CPF(uint16_t id, struct rte_ring **rx_rings, struct rte_ring **tx_rings,
         int cpf_port, struct CTAConfig *config, 
         struct cpf_actions *actions, vector<float> *cpu_loads) {
  this->id = id;
  this->rx_rings = rx_rings;
  this->tx_rings = tx_rings;
  this->cpf_port = cpf_port;
  this->config = config;
  this->rcvd_count = 0;
  this->droped_count = 0;
  this->cpf_actions = actions;
  this->cpu_loads = cpu_loads;
}

void CPF::setID(uint8_t id) {
  this->id = id;
}

uint8_t CPF::getID() {
  return this->id;
}

void CPF::enqueueRequest(uint8_t *req, int index) {

  if (unlikely(rte_ring_mp_enqueue(*(this->rx_rings + index), (void *)req) != 0)) {
    this->droped_count++;
    return;
  }

  this->rcvd_count++;
}

int CPF::getCPFPort() { return this->cpf_port; }

void CPF::setHash(int hash) { this->hash = hash; }

int CPF::getHash() { return this->hash; }

CPF::~CPF() {}