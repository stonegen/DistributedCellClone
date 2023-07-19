#ifndef __CONTROLLER__
#define __CONTROLLER__

#include "lbaas/load_balancer.h"

class Controller
{
  public:
	LoadBalancer *lb;

	Controller();
	~Controller();
	void addNode(CPF *cpf);
};

#endif
