#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <unistd.h>

#include <mutex>

#include "workers.h"

#include <queue>

#include <stdio.h>
#include <ctime>

#include "../../Common/globals.h"
// #include "../../Common/messages/messages.hpp"
#include "../../Common/time/time.h"

int maxProcedures = 20000000;
std::mutex print_mtx;

const size_t PAYLOAD_OFFSET =
    sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);

const size_t CPF_ID_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t); 

const size_t CPF_TYPE_OFFSET =
    sizeof(char);

const size_t CPF_TIME_OFFSET =
    sizeof(time_t);

const size_t FLAG_OFFSET =
    sizeof(double) + sizeof(lgclock_t) + sizeof(size_t) + sizeof(guti_t) + sizeof(uint8_t);

static struct ether_addr ether_dst = {{0x68, 0x05, 0xca, 0x00, 0x00, 0x01}};
static struct ether_addr zero_mac = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

DataWorker::DataWorker(uint16_t id, struct rte_ring *rx_ring,
                       CircularMap<unsigned long int, State *> &state_map,
                       struct CTAConfig *config,
                       struct cpf_actions *actions, map<unsigned long int, double> *pcts,
                       vector<float> *cpu_loads)
    : state_map(state_map)
{
  this->rx_ring = rx_ring;
  this->config = config;
  this->actions = actions;
  this->pcts = pcts;
  this->cpu_loads = cpu_loads;
  this->id = id;
}

DataWorker::~DataWorker() {}

static inline uint16_t generate_packet(struct rte_mbuf *buf, uint8_t *response,
                                       uint32_t size)
{
  buf->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
  buf->l2_len = sizeof(struct ether_hdr);
  buf->l3_len = sizeof(struct ipv4_hdr);
  buf->l4_len = sizeof(struct udp_hdr);

  struct ether_hdr *eth_hdr;
  struct ipv4_hdr *ip_hdr;
  struct udp_hdr *udp_hdr;
  struct tcp_hdr *tcp_hdr;
  uint16_t pkt_size = MAX_PKT_SIZE;

  buf->pkt_len = pkt_size;
  buf->data_len = pkt_size;

  eth_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);
  if (is_zero_ether_addr(&zero_mac))
  {
    ether_addr_copy(&ether_dst, &eth_hdr->d_addr);
  }
  else
  {
    ether_addr_copy(&zero_mac, &eth_hdr->d_addr);
  }

  if (is_zero_ether_addr(&zero_mac))
  {
    ether_addr_copy(&zero_mac, &eth_hdr->s_addr);
  }
  else
  {
    ether_addr_copy(&zero_mac, &eth_hdr->s_addr);
  }
  eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

  ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
  ip_hdr->type_of_service = 0;
  ip_hdr->fragment_offset = 0;
  ip_hdr->time_to_live = 64;
  ip_hdr->next_proto_id = IPPROTO_UDP;
  ip_hdr->packet_id = 0;
  ip_hdr->version_ihl = (1 << 6) + 5;
  ip_hdr->total_length = rte_cpu_to_be_16(pkt_size - sizeof(struct ether_hdr));
  ip_hdr->src_addr = 0xAABB;
  ip_hdr->dst_addr = 0xCCDD;
  ip_hdr->hdr_checksum = 0;

  udp_hdr = (struct udp_hdr *)(ip_hdr + 1);
  udp_hdr->src_port = 0xAABB;
  udp_hdr->dst_port = 0xCCDD;
  udp_hdr->dgram_cksum = 0;
  udp_hdr->dgram_len =
      rte_cpu_to_be_16(pkt_size - sizeof(struct ether_hdr) - sizeof(*ip_hdr));
  return pkt_size;
}

/* 
  These workers acts as a CPFs. A single CPF's responsibilites are given
  below,

  1. Receiving messages from the do_rx worker defined in 'main.c'
  2. Implementing the stragger logic.
  3. Processing and generating response for the messages. 
  4. Sending the responses back to CTA. 
 */

void DataWorker::run()
{

  uint32_t pkt_count = 0;
  uint32_t response_count = 0;

  double start_time = 0, end_time, complete_end_time;

  vector<pair<unsigned long int, double>> time_v;
  time_v.reserve(maxProcedures);


  double interval = 90 * 1e6;
  double duration = 20 * 1e6;
  // double duration = 1 * 1e6;
  double interval_duration = 0;

  double max_guti = 0;
  int new_msgs = 0;
  int res_msgs = 0;

  const uint64_t hz = rte_get_timer_hz();
  double processing_cycles = 0, total_cycles = -1;

  double current_cycles = rte_get_timer_cycles();
  uint16_t nb_rx;
  struct rte_mbuf *pkt_burst[MAX_PKT_BURST];
  struct rte_mbuf *rsp[MAX_PKT_BURST];

  int len;
  struct rte_eth_xstat *xstats;
  struct rte_eth_xstat_name *xstats_names;
  bool isRemote = false;
  while (!force_quit)
  {

    // Receiving messages from the 'do_rx' thread. 
    isRemote = false;
    nb_rx =
        rte_ring_dequeue_burst(rx_ring, (void **)pkt_burst, MAX_PKT_BURST, NULL);
    if (nb_rx == 0)
      continue;

    for (int pkt = 0; pkt < nb_rx; pkt++) {
      isRemote = false;
      uint8_t *data = rte_pktmbuf_mtod_offset(pkt_burst[pkt], uint8_t*, PAYLOAD_OFFSET);

      rsp[pkt] = rte_pktmbuf_alloc(global_mempool);
      if (rsp[pkt] == NULL){
        printf("Failed to allocate from mempool\n");
        continue;
      }
      
      char type = *(data + CPF_ID_OFFSET + CPF_TYPE_OFFSET);
      if (type == 'R')
      {
        // printf("ISTRUE\n");
        isRemote = true;
      } else {
        isRemote = false;
      }
      

      if (total_cycles != -1)
      {
        total_cycles += (double)(rte_get_timer_cycles() - current_cycles) / hz;
        current_cycles = rte_get_timer_cycles();
      }

      if (unlikely(pkt_count == 0))
      {
        interval_duration = TimeStampMicro();
        start_time = TimeStampMicro();
        total_cycles = (double)(rte_get_timer_cycles() - current_cycles) / hz;
        current_cycles = rte_get_timer_cycles();
      }

      pkt_count++;

      uint8_t *req = (uint8_t *)data;

      double time = TimeStampMicro();

      // Parse message header to get access to the individual fields.
      headerParser.Parse(req);

      if (this->actions != nullptr)
      {

        // Killing CPF after a certain time (for failur experiment)
        if (this->actions->action == 0)
        {
          if (start_time + (this->actions->config[1] * 1e6) < time)
          {
            return;
          }
        }

        if (this->actions->action == 1)
        {

          // Temporary Straggler (T-Straggler implementation)
          // 10% probability to slow down the CPF upon arrival of each message
          if (this->actions->config[1] == 0)
          {
            if (rand() % 10 == 0)
            {
              if (config->serializer == 0)
                usleep(8);
              else
                usleep(2.7);
            }
              // Time series code
              // if (time > (interval_duration + interval))
              // {
              //   if (time < (interval_duration + interval + duration))
              //   {
              //     if (rand() % 10 == 0)
              //     {
              //       if (config->serializer == 0)
              //         usleep(8);
              //       else
              //         usleep(2.7);
              //     }
              //   }
              //   else
              //   {
              //     interval_duration = TimeStampMicro();
              //   }
              // }
          }

          // Permanent Straggler (P-Straggler implementation)
          // Slow down the CPF upon arrival of each message.
          if (this->actions->config[1] == 1)
          {
            if (config->serializer == 0)
              usleep(8);
            else
              usleep(2.7);

            // Time series plus data-plane experiments code.
            // if (time > (interval_duration + interval))
            // {
            // //   if (time < (interval_duration + interval + duration))
            //   // {

            // // if (likely(headerParser.controlMessage.header.GUTI > max_guti))
            // // {
            // //   max_guti = headerParser.controlMessage.header.GUTI;
            // // }
            // // if (max_guti >= 200000)
            // // {
            // //   if (unlikely(interval_duration == 0))
            // //   {
            // //     interval_duration = TimeStampMicro();
            // //   }
            //     if (time < (interval_duration + interval + duration))
            //     {
            //       if (config->serializer == 0)
            //         usleep(8);
            //       else
            //         usleep(2.7);
            //     }
            //     else
            //     {
            //       interval_duration = TimeStampMicro();
            //     }
            //   // }
            // }
          }
        }

        // }
      }

      Request *request = new Request(
          req, time, headerParser.controlMessage.header.payloadLength);


      /* In case of non deterministic operations, we send a special
       synchronization message to CPFs to copy the non-deterministic 
      state to each CPF.
      */ 

      if (data[FLAG_OFFSET] == 77) {
        data[FLAG_OFFSET] = 1;
        rsp[response_count] = pkt_burst[pkt];
        response_count++;
        continue;
      }

      guti_t hash = headerParser.controlMessage.header.GUTI;

      // Per-user state at each CPF.
      State *state;
      if (state_map.count(hash))
      {
        state = state_map.get(hash);
      }
      else
      {

        if (config->serializer == 0)
        {
          state = new Asn1State(headerParser.controlMessage.header.GUTI, hash,
                                headerParser.controlMessage.header.payloadLength);
          state->startTime = request->getArrivalTime();
          state_map.insert(hash, state);
        }
        else if (config->serializer == 1)
        {
          state = new FlatbuffersState(
              headerParser.controlMessage.header.GUTI, hash,
              headerParser.controlMessage.header.payloadLength);
          state->startTime = request->getArrivalTime();
          state_map.insert(hash, state);
        }
        else
        {
          cout << "Wrong serializer mentioned in config file..." << endl;
        }
        new_msgs++;
        state->setHash(hash);
        state->setGuti(headerParser.controlMessage.header.GUTI);
      }

      // Process the incoming procedure message and identify the response message(s)
      // by executing the relevant procedure state machine. 
      std::vector<int> next_msgs =
          state->UpdateState((void *)headerParser.controlMessage.actualMessage,
                            headerParser.controlMessage.header.payloadLength);


      if (next_msgs.size() > 0)
      {
        for (auto it = next_msgs.begin(); it != next_msgs.end(); ++it)
        {
          // If is the last message of procedure, record its completion time
          // in an array.
          if (*it == 99)
          {
            state->completed = true;

            state->completed_logicalClock =
                headerParser.controlMessage.header.logicalClock;
            state->endTime = request->getArrivalTime();

            pair<unsigned long int, double> pair;
            pair.first = state->getGuti();
            pair.second = (state->endTime - state->startTime);
            if (this->config->tx_arg > (this->config->replicas - this->config->remote_replicas))
            {
              switch (this->config->procedure)
              {
              case 1:
                pair.second = pair.second + (this->config->delay)*5;
                break;
              case 2:
                pair.second = pair.second + (this->config->delay)*11;
                break;
              case 3:
                pair.second = pair.second + (this->config->delay)*3;
                break;
              default:
                pair.second = pair.second;
                break;
              }
            }
            
            time_v.push_back(pair);

            complete_end_time = TimeStampMicro();
            break;
          }
          uint8_t buff[1024] = {0};

          // Generate the identified procedure message(s).
          size_t bytes_to_send = state_map.get(hash)->PrepareMsg(*it, buff);
          
          if (bytes_to_send == 0)
          {
            break;
          }

          uint8_t *payload_ptr = rte_pktmbuf_mtod_offset(rsp[response_count], 
                                                         uint8_t *, PAYLOAD_OFFSET);
          if (isRemote == true)
          {
            // time_t currentTime = std::time(NULL);
            *(payload_ptr + CPF_ID_OFFSET + CPF_TYPE_OFFSET) = 'R';
            // *(payload_ptr + CPF_ID_OFFSET + CPF_TYPE_OFFSET + CPF_TIME_OFFSET) = currentTime;
          } else {
            *(payload_ptr + CPF_ID_OFFSET + CPF_TYPE_OFFSET) = 'L';
          }
          // Add application header to the message. 
          bytes_to_send =
              headerParser.PrependCustomHeader(buff, payload_ptr, bytes_to_send);

          // Attach protocol headers to the message
          generate_packet(rsp[response_count], NULL, bytes_to_send);
          response_count++;
        }
      }
      delete request;
      end_time = TimeStampMicro();
      processing_cycles += (double)(rte_get_timer_cycles() - current_cycles) / hz;
    }

    // Return back the responses to the CTA.
    // if (isRemote)
    // {
    //   usleep(50);
    // }
    
    uint16_t to_send = response_count;
    int sent;

    while (to_send != 0) {
      sent = rte_eth_tx_burst((uint8_t)PORT, this->id, rsp + (response_count - to_send), 1);
      res_msgs += sent;
      to_send--;
    }

    for (int k = 0; k < nb_rx; k++){
      rte_pktmbuf_free(pkt_burst[k]);
    }
    response_count = 0;
  }

  len = rte_eth_xstats_get(PORT, NULL, 0);
  if (len < 0)
      rte_exit(EXIT_FAILURE,
              "rte_eth_xstats_get(%u) failed: %d", PORT,
              len);

  xstats = calloc(len, sizeof(*xstats));
  if (xstats == NULL)
      rte_exit(EXIT_FAILURE,
              "Failed to calloc memory for xstats");


  int ret = rte_eth_xstats_get(PORT, xstats, len);
  if (ret < 0 || ret > len) {
      free(xstats);
      rte_exit(EXIT_FAILURE,
              "rte_eth_xstats_get(%u) len%i failed: %d",
              PORT, len, ret);
  }

  xstats_names = calloc(len, sizeof(*xstats_names));
  if (xstats_names == NULL) {
      free(xstats);
      rte_exit(EXIT_FAILURE,
              "Failed to calloc memory for xstats_names");
  }

  ret = rte_eth_xstats_get_names(PORT, xstats_names, len);
  if (ret < 0 || ret > len) {
      free(xstats);
      free(xstats_names);
      rte_exit(EXIT_FAILURE,
              "rte_eth_xstats_get_names(%u) len%i failed: %d",
              PORT, len, ret);
  }

  printf("Len: %d\n", len);
  for (int i = 0; i < len; i++) {
      if (xstats[i].value > 0)
          printf("Port %u: %s:\t\t%"PRIu64"\n",
                  PORT,
                  xstats_names[i].name,
                  xstats[i].value);
  }

  double elapsed_complete = (complete_end_time - start_time) * 1e-6;
  double elapsed = (end_time - start_time) * 1e-6;
  double proc_rate = pkt_count / elapsed;
  printf("New messages: %d | Res messages: %d\n", new_msgs, res_msgs);
  printf("Response count %d\n", response_count);
  print_mtx.lock();

  printf("Time_v len: %d\n", time_v.size());
  for (auto it = time_v.begin(); it != time_v.end(); ++it)
  {

    if (this->pcts->find((*it).first) == this->pcts->end())
    {
        (*this->pcts)[(*it).first] = (*it).second;
    }
    else if ((*it).second < (*this->pcts)[(*it).first])
    {
        (*this->pcts)[(*it).first] = (*it).second;
    }
  }

  (this->cpu_loads)->push_back((processing_cycles / total_cycles) * 100);
  printf("Processed %u packets\n", pkt_count);
  print_mtx.unlock();
  return;
}

