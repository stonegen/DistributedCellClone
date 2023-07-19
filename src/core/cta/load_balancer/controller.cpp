#include "controller.h"

Controller::Controller()
{
	this->lb = new LoadBalancer();
}

Controller::~Controller() {}

void Controller::addNode(CPF * cpf) {
	this->lb->addNode(cpf);
}