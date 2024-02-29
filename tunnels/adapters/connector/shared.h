#pragma once
#include "api.h"
#include "loggers/dns_logger.h"
#include "loggers/network_logger.h"

#define STATE(x) ((connector_state_t *)((x)->state))
#define CSTATE(x) ((connector_con_state_t *)((((x)->line->chains_state)[self->chain_index])))
#define CSTATE_MUT(x) ((x)->line->chains_state)[self->chain_index]


// enable profile to see how much it takes to connect and downstream write
// #define PROFILE 1



typedef struct connector_state_s
{
    // settings
    bool tcp_no_delay;
    bool tcp_fast_open; 
    bool reuse_addr;
    enum domain_strategy domain_strategy;

    dynamic_value_t dest_addr;
    dynamic_value_t dest_port;


} connector_state_t;

typedef struct connector_con_state_s
{
#ifdef PROFILE
    struct timeval __profile_conenct;
#endif

    tunnel_t *tunnel;
    line_t *line;
    hio_t *io;
    hio_t *io_back;

    buffer_pool_t *buffer_pool;
    context_queue_t *queue;
    context_t *current_w;

    bool write_paused;
    bool established;
} connector_con_state_t;



bool connectorResolvedomain(socket_context_t *dest);


void connectorUpStream(tunnel_t *self, context_t *c);
void connectorDownStream(tunnel_t *self, context_t *c);

void connectorPacketUpStream(tunnel_t *self, context_t *c);
void connectorPacketDownStream(tunnel_t *self, context_t *c);




