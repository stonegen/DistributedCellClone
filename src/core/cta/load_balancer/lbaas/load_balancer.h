#ifndef __lb_h__
#define __lb_h__

#include "../map/map.h"
#include "../../cpf/cpf.h"
#include <set>

class LoadBalancer
{
private:
	CircularMap<size_t, CPF *> balancing_ring;

public:
	LoadBalancer();
	~LoadBalancer();
	void forward(struct rte_mbuf **rsp, size_t key, CTAConfig *conf, int ring_index);
	CPF* nextSuitableNode(size_t key, int replicas, int number_of_cpf, set<CPF *> cpf_hashes);
	CPF* nextPhysicalNode(size_t key, set<CPF *> cpf_hashes);
	void addNode(CPF *new_cpf);
	void removeNode(size_t key);
	size_t hash(string key);
	void showStatus();
};

#endif
