#include "NicInterface.h"

static bytesToNumber converter;
static logicalClockExtractor ext;
/* 
   It is called from main.c when a packet is
   received from the pktgen to the CTA.
   It forwards the message to the proper CPF.
*/
void NicInterface::OnPayloadReceived(struct rte_mbuf **rsp, CustomAppHeader header, 
									 CTAConfig *conf, int ring_index)
{
	string guti_str = std::to_string(header.GUTI);
	controller->lb->forward(rsp, controller->lb->hash(guti_str), conf, ring_index);
}

// All initializations will go here.
void NicInterface::Init()
{
	srand(time(NULL));
}

void NicInterface::setRxRings(struct rte_ring **rx_rings)
{
	this->rx_rings = rx_rings;
}

NicInterface::NicInterface()
{
	controller = new Controller();
}

NicInterface::~NicInterface() { }
