#ifndef workers_h_
#define workers_h_

#include "../../Common/custom_header/CustomHeaderParser.h"
#include "../../Common/request/request.h"
#include "../map/map.h"
#include "../tools/state.h"


class DataWorker {
 private:
  CircularMap <guti_t, State *> &state_map;
  CustomHeaderParser headerParser;
  struct rte_ring *rx_ring;
  struct CTAConfig *config;
  struct cpf_actions *actions;
  map<unsigned long int, double> *pcts;
  vector<float> *cpu_loads;
  uint16_t id;

 public:
  DataWorker(uint16_t id, struct rte_ring *rx_ring, CircularMap<guti_t, State *> &state_map,
             struct CTAConfig *config, struct cpf_actions *actions, map<unsigned long int, double> *pcts,
             vector<float> *cpu_loads);
  ~DataWorker();

  void run();
};

#endif
