#include "load_balancer.h"
#include <iostream>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "../../Common/DataType.h"

const size_t PAYLOAD_OFFSET =
    sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);

const size_t CPF_ID_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t); 

const size_t CPF_TYPE_OFFSET =
    sizeof(char);

using namespace std;

LoadBalancer::LoadBalancer() {}

LoadBalancer::~LoadBalancer() {}

void LoadBalancer::addNode(CPF *new_cpf)
{
  int key = this->hash(to_string(new_cpf->getCPFPort()));
  new_cpf->setHash(key);
  balancing_ring.insert(key, new_cpf);

  for (int i = 1; i < 400; i++)
  {
    key = this->hash(to_string(new_cpf->getHash() + i * 1373));
    balancing_ring.insert(key, new_cpf);
  }
}

void LoadBalancer::removeNode(size_t key)
{
  size_t hash = this->hash(to_string(key));
  balancing_ring.remove(hash);
  this->showStatus();
}

size_t LoadBalancer::hash(string key)
{
  std::hash<string> str_hash;
  return str_hash(key) % 999999937;
}

CPF* LoadBalancer::nextPhysicalNode(size_t key, set<CPF* > cpf_hashes)
{

  size_t k = balancing_ring.upper_key(key);
  CPF *cpf = balancing_ring.get(k);

  while (cpf_hashes.find(cpf) != cpf_hashes.end())
  {
    k = balancing_ring.upper_key(k);
    cpf = balancing_ring.get(k);
  }

  return cpf;
}

CPF* LoadBalancer::nextSuitableNode(size_t key, int replicas, int number_of_cpf, set<CPF *> cpf_hashes)
{
  if (replicas > number_of_cpf)
  {
    return balancing_ring.upper(key);
  }
  else
  {
    return nextPhysicalNode(key, cpf_hashes);
  }
}

/*
  This function is responsible for searching suitable 
  CPFs in a consistent hash ring for each message. After it, each message is 
  sent to the CPF server.
*/
void LoadBalancer::forward(struct rte_mbuf **rsp, size_t key, CTAConfig *conf, int ring_index)
{

  if (balancing_ring.size() > 0)
  {
    int replicas = conf->replicas;
    int remote_replicas = 0;
    // Already extracted CPFs hashes.
    set<CPF *> cpf_hashes;

    for (int i = 0; i < replicas; i++)
    { 
      // Searching for next physical node in the consistent hash ring
      CPF *cpf = this->nextSuitableNode(key, i, conf->number_of_cpfs, cpf_hashes);

      uint8_t *payload = rte_pktmbuf_mtod_offset(rsp[i], uint8_t *, PAYLOAD_OFFSET);

      // Embedding the CPF id for later use.
      *(payload + CPF_ID_OFFSET) = cpf->getID();

      if (remote_replicas != conf->remote_replicas)
      {
        *(payload + CPF_ID_OFFSET + CPF_TYPE_OFFSET) = 'R';
        remote_replicas++;
      } else {
        *(payload + CPF_ID_OFFSET + CPF_TYPE_OFFSET) = 'L';
      }
      
      cpf_hashes.insert(cpf);
    }


    // Sending the duplicate messages to the CPF server through DPDK.
    uint16_t to_send, sent;
    to_send = replicas;
    while (to_send != 0) {
      sent = rte_eth_tx_burst(CPF_PORT, ring_index, rsp + (replicas - to_send), to_send);
      to_send -= sent;

    }
  }
}

void LoadBalancer::showStatus()
{
  cout << "Nodes on Ring = " << balancing_ring.size() << endl
       << std::flush;
  cout << "+++++=====+++++=====+++++\n"
       << std::flush;
}
