#include "cpf.h"
#include "workers/workers.h"

using namespace std;

void runDataWorker(void *data) {
  CPF *cpf = (CPF *)data;
  printf("Starting an CPF on lcore %d\n", rte_lcore_id());
  cpf->start();
}

CPF::CPF(uint16_t id, struct rte_ring *rx_ring,
         int cpf_port, struct CTAConfig *config, 
         struct cpf_actions *actions, map<unsigned long int, double> *pcts,
         vector<float> *cpu_loads) {
  this->id = id;
  this->rx_ring = rx_ring;
  this->config = config;
  this->cpf_actions = actions;
  this->pcts = pcts;
  this->cpu_loads = cpu_loads;
}

CPF::~CPF() {}

/* Main Routine of an CPF */
void CPF::start() {
  DataWorker *worker =
      new DataWorker(id, rx_ring, state_map, config, 
                     this->cpf_actions, this->pcts, this->cpu_loads);
  worker->run();
  printf("Dropped count: %u\n", this->droped_count);

}