#include "socket_manager.h"
#include "basic_types.h"
#include "buffer_pool.h"
#include "frand.h"
#include "generic_pool.h"
#include "hloop.h"
#include "hmutex.h"
#include "idle_table.h"
#include "loggers/network_logger.h"
#include "stc/common.h"
#include "tunnel.h"
#include "utils/hashutils.h"
#include "utils/procutils.h"
#include "utils/sockutils.h"
#include "ww.h"

#define i_type balancegroup_registry_t // NOLINT
#define i_key  hash_t                  // NOLINT
#define i_val  idle_table_t *          // NOLINT

#include "stc/hmap.h"

typedef struct socket_filter_s
{
    union {
        hio_t  *listen_io;
        hio_t **listen_ios;
    };
    socket_filter_option_t option;
    tunnel_t              *tunnel;
    onAccept               cb;

} socket_filter_t;

#define i_key     socket_filter_t * // NOLINT
#define i_type    filters_t         // NOLINT
#define i_use_cmp                   // NOLINT
#include "stc/vec.h"

#define SUPPORT_V6 false
enum
{
    kSoOriginalDest          = 80,
    kFilterLevels            = 4,
    kMaxBalanceSelections    = 64,
    kAcceptThreadIdOffset    = 0,
    kDefalultBalanceInterval = 60 * 1000
};

typedef struct socket_manager_s
{
    filters_t filters[kFilterLevels];

    struct
    {
        generic_pool_t *pool; /* holds udp_payload_t */
        hhybridmutex_t  mutex;

    } *udp_pools;

    struct
    {
        generic_pool_t *pool; /* holds socket_accept_result_t */
        hhybridmutex_t  mutex;

    } *tcp_pools;

    hhybridmutex_t          mutex;
    balancegroup_registry_t balance_groups;
    hthread_t               accept_thread;
    hloop_t                *loop;

    uint16_t last_round_tid;
    bool     iptables_installed;
    bool     ip6tables_installed;
    bool     lsof_installed;
    bool     iptable_cleaned;
    bool     iptables_used;
    bool     started;

} socket_manager_state_t;

static socket_manager_state_t *state = NULL;

static pool_item_t *allocTcpResultObjectPoolHandle(struct generic_pool_s *pool)
{
    (void) pool;
    return malloc(sizeof(socket_accept_result_t));
}

static void destroyTcpResultObjectPoolHandle(struct generic_pool_s *pool, pool_item_t *item)
{
    (void) pool;
    free(item);
}

static pool_item_t *allocUdpPayloadPoolHandle(struct generic_pool_s *pool)
{
    (void) pool;
    return malloc(sizeof(udp_payload_t));
}

static void destroyUdpPayloadPoolHandle(struct generic_pool_s *pool, pool_item_t *item)
{
    (void) pool;
    free(item);
}

void destroySocketAcceptResult(socket_accept_result_t *sar)
{
    const uint8_t tid = sar->tid;

    hhybridmutex_lock(&(state->tcp_pools[tid].mutex));
    reusePoolItem(state->tcp_pools[tid].pool, sar);
    hhybridmutex_unlock(&(state->tcp_pools[tid].mutex));
}

static udp_payload_t *newUpdPayload(uint8_t tid)
{
    hhybridmutex_lock(&(state->udp_pools[tid].mutex));
    udp_payload_t *item = popPoolItem(state->udp_pools[tid].pool);
    hhybridmutex_unlock(&(state->udp_pools[tid].mutex));
    return item;
}

void destroyUdpPayload(udp_payload_t *upl)
{
    const uint8_t tid = upl->tid;

    hhybridmutex_lock(&(state->udp_pools[tid].mutex));
    reusePoolItem(state->udp_pools[tid].pool, upl);
    hhybridmutex_unlock(&(state->udp_pools[tid].mutex));
}

static bool redirectPortRangeTcp(unsigned int pmin, unsigned int pmax, unsigned int to)
{
    char b[300];
#if SUPPORT_V6
    sprintf(b, "ip6tables -t nat -A PREROUTING -p TCP --dport %u:%u -j REDIRECT --to-port %u", pmin, pmax, to);
    execCmd(b);
#endif

    sprintf(b, "iptables -t nat -A PREROUTING -p TCP --dport %u:%u -j REDIRECT --to-port %u", pmin, pmax, to);
    return execCmd(b).exit_code == 0;
}

static bool redirectPortRangeUdp(unsigned int pmin, unsigned int pmax, unsigned int to)
{
    char b[300];
#if SUPPORT_V6
    sprintf(b, "ip6tables -t nat -A PREROUTING -p UDP --dport %u:%u -j REDIRECT --to-port %u", pmin, pmax, to);
    execCmd(b);
#endif
    sprintf(b, "iptables -t nat -A PREROUTING -p UDP --dport %u:%u -j REDIRECT --to-port %u", pmin, pmax, to);
    return execCmd(b).exit_code == 0;
}

static bool redirectPortTcp(unsigned int port, unsigned int to)
{
    char b[300];
#if SUPPORT_V6
    sprintf(b, "ip6tables -t nat -A PREROUTING -p TCP --dport %u -j REDIRECT --to-port %u", port, to);
    execCmd(b);
#endif
    sprintf(b, "iptables -t nat -A PREROUTING -p TCP --dport %u -j REDIRECT --to-port %u", port, to);
    return execCmd(b).exit_code == 0;
}

static bool redirectPortUdp(unsigned int port, unsigned int to)
{
    char b[300];
#if SUPPORT_V6
    sprintf(b, "ip6tables -t nat -A PREROUTING -p UDP --dport %u -j REDIRECT --to-port %u", port, to);
    execCmd(b);
#endif
    sprintf(b, "iptables -t nat -A PREROUTING -p UDP --dport %u -j REDIRECT --to-port %u", port, to);
    return execCmd(b).exit_code == 0;
}

static bool resetIptables(bool safe_mode)
{

    if (safe_mode)
    {
        char    msg[] = "SocketManager: clearing iptables nat rules\n";
        ssize_t _     = write(STDOUT_FILENO, msg, sizeof(msg));
        (void) _;
    }
    else
    {
        char msg[] = "SocketManager: clearing iptables nat rules";
        LOGD(msg);
    }

#if SUPPORT_V6
    execCmd("ip6tables -t nat -F");
    execCmd("ip6tables -t nat -X");
#endif
    // todo (async unsafe) this works but should be replaced with a asyncsafe execcmd function
    return execCmd("iptables -t nat -F").exit_code == 0 && execCmd("iptables -t nat -X").exit_code == 0;
}
static void exitHook(void)
{
    if (state->iptables_used)
    {
        resetIptables(true);
    }
}
static void signalHandler(int signum)
{
    signal(signum, SIG_DFL);
    if (signum == SIGTERM || signum == SIGINT)
    {
        exitHook();
    }
    raise(signum);
}

static inline int checkIPRange4(const struct in_addr test_addr, const struct in_addr base_addr,
                                const struct in_addr subnet_mask)
{
    if ((test_addr.s_addr & subnet_mask.s_addr) == (base_addr.s_addr & subnet_mask.s_addr))
    {
        return 1;
    }
    return 0;
}
static inline int checkIPRange6(const struct in6_addr test_addr, const struct in6_addr base_addr,
                                const struct in6_addr subnet_mask)
{

    uint64_t *test_addr_p   = (uint64_t *) &(test_addr.s6_addr[0]);
    uint64_t *base_addr_p   = (uint64_t *) &(base_addr.s6_addr[0]);
    uint64_t *subnet_mask_p = (uint64_t *) &(subnet_mask.s6_addr[0]);

    if ((base_addr_p[0] & subnet_mask_p[0]) != test_addr_p[0] || (base_addr_p[1] & subnet_mask_p[1]) != test_addr_p[1])
    {
        return 0;
    }
    return 1;

    // // IPv6 addresses
    // for (int i = 0; i < 16; i++)
    // {
    //     result_addr.s6_addr[i] = base_addr.s6_addr[i] & subnet_mask.s6_addr[i];
    // }
    // if (memcmp(&test_addr, &result_addr, sizeof(struct in6_addr)) == 0)
    // {
    //     return 1;
    // }
    // return 0;
}

int parseIPWithSubnetMask(struct in6_addr *base_addr, const char *input, struct in6_addr *subnet_mask)
{
    char *slash;
    char *ip_part;
    char *subnet_part;
    char  input_copy[strlen(input) + 1];
    strcpy(input_copy, input);

    slash = strchr(input_copy, '/');
    if (slash == NULL)
    {
        fprintf(stderr, "Invalid input format.\n");
        return -1;
    }

    *slash      = '\0';
    ip_part     = input_copy;
    subnet_part = slash + 1;

    if (inet_pton(AF_INET, ip_part, base_addr) == 1)
    {
        // IPv4 address
        int prefix_length = atoi(subnet_part);
        if (prefix_length < 0 || prefix_length > 32)
        {
            fprintf(stderr, "Invalid subnet mask length.\n");
            return -1;
        }
        uint32_t mask;
        if (sizeof(unsigned long long) == 8)
        {
            mask = htonl(0xFFFFFFFF &
                         ((unsigned long long) 0x00000000FFFFFFFF << (unsigned long long) (32 - prefix_length)));
        }
        else
        {
            if (prefix_length > 0)
            {
                mask = htonl(0xFFFFFFFF & (0xFFFFFFFF << (32 - prefix_length)));
            }
            else
            {
                mask = 0;
            }
        }
        struct in_addr mask_addr = {.s_addr = mask};
        memcpy(subnet_mask, &mask_addr, 4);
        // inet_ntop(AF_INET, &mask_addr, subnet_mask, INET_ADDRSTRLEN);
    }
    else if (inet_pton(AF_INET6, ip_part, base_addr) == 1)
    {
        // IPv6 address
        int prefix_length = atoi(subnet_part);
        if (prefix_length < 0 || prefix_length > 128)
        {
            fprintf(stderr, "Invalid subnet mask length.\n");
            return -1;
        }
        int i;
        for (i = 0; i < 16; i++)
        {
            int bits                     = prefix_length >= 8 ? 8 : prefix_length;
            ((uint8_t *) subnet_mask)[i] = bits == 0 ? 0 : (0xFF << (8 - bits));
            prefix_length -= bits;
        }
        // struct in6_addr mask_addr;
        // memcpy(&(subnet_mask->s6_addr), subnet_mask, 16);
        // inet_ntop(AF_INET6, &mask_addr, subnet_mask, INET6_ADDRSTRLEN);
    }
    else
    {
        fprintf(stderr, "Invalid IP address.\n");
        return -1;
    }

    return 0;
}

void parseWhiteListOption(socket_filter_option_t *option)
{
    assert(option->white_list_raddr != NULL);

    int   len = 0;
    char *cur = NULL;

    while ((cur = option->white_list_raddr[len]))
    {
        len++;
    }

    option->white_list_parsed_length = len;
    option->white_list_parsed        = malloc(sizeof(option->white_list_parsed[0]) * len);
    for (int i = 0; i < len; i++)
    {
        cur              = option->white_list_raddr[i];
        int parse_result = parseIPWithSubnetMask(&(option->white_list_parsed[i].ip_bytes_buf), cur,
                                                 &(option->white_list_parsed[i].mask_bytes_buf));

        if (parse_result == -1)
        {
            LOGF("SocketManager: stopping due to whitelist address [%d] \"%s\" parse failure", i, cur);
            exit(1);
        }
    }

    if (option->balance_group_name)
    {
        hash_t        name_hash = CALC_HASH_BYTES(option->balance_group_name, strlen(option->balance_group_name));
        idle_table_t *b_table   = NULL;
        hhybridmutex_lock(&(state->mutex));

        balancegroup_registry_t_iter find_result = balancegroup_registry_t_find(&(state->balance_groups), name_hash);

        if (find_result.ref == balancegroup_registry_t_end(&(state->balance_groups)).ref)
        {
            b_table = newIdleTable(state->loop);
            balancegroup_registry_t_insert(&(state->balance_groups), name_hash, b_table);
        }
        else
        {
            b_table = (find_result.ref->second);
        }

        hhybridmutex_unlock(&(state->mutex));

        option->shared_balance_table = b_table;
    }
}

void registerSocketAcceptor(tunnel_t *tunnel, socket_filter_option_t option, onAccept cb)
{
    if (state->started)
    {
        LOGF("SocketManager: cannot register after accept thread starts");
        exit(1);
    }
    socket_filter_t *filter   = malloc(sizeof(socket_filter_t));
    unsigned int     pirority = 0;
    if (option.multiport_backend == kMultiportBackendNothing)
    {
        pirority++;
    }
    if (option.white_list_raddr != NULL)
    {
        pirority++;
        parseWhiteListOption(&option);
    }
    if (option.black_list_raddr != NULL)
    {
        pirority++;
    }

    *filter = (socket_filter_t){.tunnel = tunnel, .option = option, .cb = cb, .listen_io = NULL};

    hhybridmutex_lock(&(state->mutex));
    filters_t_push(&(state->filters[pirority]), filter);
    hhybridmutex_unlock(&(state->mutex));
}

static inline uint16_t getCurrentDistributeTid(void)
{
    return state->last_round_tid;
}

static inline void incrementDistributeTid(void)
{
    state->last_round_tid++;
    if (state->last_round_tid >= workers_count)
    {
        state->last_round_tid = 0;
    }
}
static void distributeSocket(void *io, socket_filter_t *filter, uint16_t local_port)
{

    uint8_t tid = (uint8_t) getCurrentDistributeTid();

    hhybridmutex_lock(&(state->tcp_pools[tid].mutex));
    socket_accept_result_t *result = popPoolItem(state->tcp_pools[tid].pool);
    hhybridmutex_unlock(&(state->tcp_pools[tid].mutex));

    result->real_localport = local_port;

    hloop_t *worker_loop = loops[tid];
    hevent_t ev          = (hevent_t){.loop = worker_loop, .cb = filter->cb};
    result->tid          = tid;
    result->io           = io;
    result->tunnel       = filter->tunnel;
    ev.userdata          = result;
    incrementDistributeTid();

    hloop_post_event(worker_loop, &ev);
}

static void noTcpSocketConsumerFound(hio_t *io)
{
    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN]  = {0};

    LOGE("SocketManager: could not find consumer for Tcp socket FD:%x [%s] <= [%s]", hio_fd(io),
         SOCKADDR_STR(hio_localaddr(io), localaddrstr), SOCKADDR_STR(hio_peeraddr(io), peeraddrstr));
    hio_close(io);
}

static bool checkIpIsWhiteList(sockaddr_u *addr, const socket_filter_option_t option)
{
    const bool is_v4 = addr->sa.sa_family == AF_INET;
    if (is_v4)
    {
        for (unsigned int i = 0; i < option.white_list_parsed_length; i++)
        {

            if (checkIPRange4(addr->sin.sin_addr, *(struct in_addr *) &(option.white_list_parsed[i].ip_bytes_buf),
                              *(struct in_addr *) &(option.white_list_parsed[i].mask_bytes_buf)))
            {
                return true;
            }
        }
    }
    else
    {
        for (unsigned int i = 0; i < option.white_list_parsed_length; i++)
        {

            if (checkIPRange6(addr->sin6.sin6_addr, option.white_list_parsed[i].ip_bytes_buf,
                              option.white_list_parsed[i].mask_bytes_buf))
            {
                return true;
            }
        }
    }

    return false;
}

static void distributeTcpSocket(hio_t *io, uint16_t local_port)
{
    sockaddr_u *paddr = (sockaddr_u *) hio_peeraddr(io);

    static socket_filter_t *balance_selection_filters[kMaxBalanceSelections];
    uint8_t                 balance_selection_filters_length = 0;
    idle_table_t           *selected_balance_table           = NULL;
    hash_t                  src_hash;
    bool                    src_hashed = false;
    const uint8_t           this_tid   = workers_count + kAcceptThreadIdOffset;

    for (int ri = (kFilterLevels - 1); ri >= 0; ri--)
    {
        c_foreach(k, filters_t, state->filters[ri])
        {
            socket_filter_t       *filter   = *(k.ref);
            socket_filter_option_t option   = filter->option;
            uint16_t               port_min = option.port_min;
            uint16_t               port_max = option.port_max;

            if (selected_balance_table != NULL && option.shared_balance_table != selected_balance_table)
            {
                continue;
            }
            if (option.protocol != kSapTcp || port_min > local_port || port_max < local_port)
            {
                continue;
            }
            if (option.white_list_raddr != NULL)
            {
                if (! checkIpIsWhiteList(paddr, option))
                {
                    continue;
                }
            }

            if (option.shared_balance_table)
            {
                if (! src_hashed)
                {
                    src_hash = sockAddrCalcHash((sockaddr_u *) hio_peeraddr(io));
                }
                idle_item_t *idle_item = getIdleItemByHash(this_tid, option.shared_balance_table, src_hash);

                if (idle_item)
                {
                    socket_filter_t *target_filter = idle_item->userdata;
                    keepIdleItemForAtleast(option.shared_balance_table, idle_item,
                                           option.balance_group_interval == 0 ? kDefalultBalanceInterval
                                                                              : option.balance_group_interval);
                    if (option.no_delay)
                    {
                        tcp_nodelay(hio_fd(io), 1);
                    }
                    hio_detach(io);
                    distributeSocket(io, target_filter, local_port);
                    return;
                }

                if (WW_UNLIKELY(balance_selection_filters_length >= kMaxBalanceSelections))
                {
                    // probably never but the limit can be simply increased
                    LOGW("SocketManager: balance between more than %d tunnels is not supported", kMaxBalanceSelections);
                }
                balance_selection_filters[balance_selection_filters_length++] = filter;
                selected_balance_table                                        = option.shared_balance_table;
                continue;
            }

            if (option.no_delay)
            {
                tcp_nodelay(hio_fd(io), 1);
            }
            hio_detach(io);
            distributeSocket(io, filter, local_port);
            return;
        }
    }

    if (balance_selection_filters_length > 0)
    {
        socket_filter_t *filter = balance_selection_filters[fastRand() % balance_selection_filters_length];
        newIdleItem(filter->option.shared_balance_table, src_hash, filter, NULL, this_tid,
                    filter->option.balance_group_interval == 0 ? kDefalultBalanceInterval
                                                               : filter->option.balance_group_interval);
        distributeSocket(io, filter, local_port);
    }
    else
    {
        noTcpSocketConsumerFound(io);
    }
}

static void onAcceptTcpSinglePort(hio_t *io)
{
    distributeTcpSocket(io, sockaddr_port((sockaddr_u *) hio_localaddr(io)));
}

static void onAcceptTcpMultiPort(hio_t *io)
{
#ifdef OS_UNIX

    unsigned char pbuf[28] = {0};
    socklen_t     size     = 16; // todo (ipv6) value is 28 but requires dual stack
    if (getsockopt(hio_fd(io), IPPROTO_IP, kSoOriginalDest, &(pbuf[0]), &size) < 0)
    {
        char localaddrstr[SOCKADDR_STRLEN] = {0};
        char peeraddrstr[SOCKADDR_STRLEN]  = {0};

        LOGE("SocketManger: multiport failure getting origin port FD:%x [%s] <= [%s]", hio_fd(io),
             SOCKADDR_STR(hio_localaddr(io), localaddrstr), SOCKADDR_STR(hio_peeraddr(io), peeraddrstr));
        hio_close(io);
        return;
    }

    distributeTcpSocket(io, (pbuf[2] << 8) | pbuf[3]);
#else
    onAcceptTcpSinglePort(io);
#endif
}

static multiport_backend_t getDefaultMultiPortBackend(void)
{
    if (state->iptables_installed)
    {
        return kMultiportBackendIptables;
    }
    return kMultiportBackendSockets;
}

static void listenTcpMultiPortIptables(hloop_t *loop, socket_filter_t *filter, char *host, uint16_t port_min,
                                       uint8_t *ports_overlapped, uint16_t port_max)
{
    if (! state->iptables_installed)
    {
        LOGF("SocketManager: multi port backend \"iptables\" colud not start, error: not installed");
        exit(1);
    }
    state->iptables_used = true;
    if (! state->iptable_cleaned)
    {
        if (! resetIptables(false))
        {
            LOGF("SocketManager: could not clear iptables rules");
            exit(1);
        }
        state->iptable_cleaned = true;
    }
    uint16_t main_port = port_max;
    // select main port
    {
        do
        {
            if (ports_overlapped[main_port] != 1)
            {
                filter->listen_io           = hloop_create_tcp_server(loop, host, main_port, onAcceptTcpMultiPort);
                ports_overlapped[main_port] = 1;
                if (filter->listen_io != NULL)
                {
                    break;
                }

                main_port--;
            }
        } while (main_port >= port_min);

        if (filter->listen_io == NULL)
        {
            LOGF("SocketManager: stopping due to null socket handle");
            exit(1);
        }
    }
    redirectPortRangeTcp(port_min, port_max, main_port);
    LOGI("SocketManager: listening on %s:[%u - %u] >> %d (%s)", host, port_min, port_max, main_port, "TCP");
}

static void listenTcpMultiPortSockets(hloop_t *loop, socket_filter_t *filter, char *host, uint16_t port_min,
                                      uint8_t *ports_overlapped, uint16_t port_max)
{
    const int length           = (port_max - port_min);
    filter->listen_ios         = (hio_t **) malloc(sizeof(hio_t *) * (length + 1));
    filter->listen_ios[length] = 0x0;
    int i                      = 0;
    for (uint16_t p = port_min; p < port_max; p++)
    {
        if (ports_overlapped[p] == 1)
        {
            LOGW("SocketManager: could not listen on %s:[%u] , skipped...", host, p, "TCP");
            continue;
        }
        ports_overlapped[p] = 1;

        filter->listen_ios[i] = hloop_create_tcp_server(loop, host, p, onAcceptTcpSinglePort);

        if (filter->listen_ios[i] == NULL)
        {
            LOGW("SocketManager: could not listen on %s:[%u] , skipped...", host, p, "TCP");
            continue;
        }
        i++;
        LOGI("SocketManager: listening on %s:[%u] (%s)", host, p, "TCP");
    }
}

static void listenTcpSinglePort(hloop_t *loop, socket_filter_t *filter, char *host, uint16_t port,
                                uint8_t *ports_overlapped)
{
    if (ports_overlapped[port] == 1)
    {
        return;
    }
    ports_overlapped[port] = 1;
    LOGI("SocketManager: listening on %s:[%u] (%s)", host, port, "TCP");
    filter->listen_io = hloop_create_tcp_server(loop, host, port, onAcceptTcpSinglePort);

    if (filter->listen_io == NULL)
    {
        LOGF("SocketManager: stopping due to null socket handle");
        exit(1);
    }
}
static void listenTcp(hloop_t *loop, uint8_t *ports_overlapped)
{
    for (int ri = (kFilterLevels - 1); ri >= 0; ri--)
    {
        c_foreach(k, filters_t, state->filters[ri])
        {
            socket_filter_t *filter = *(k.ref);
            if (filter->option.multiport_backend == kMultiportBackendDefault)
            {
                filter->option.multiport_backend = getDefaultMultiPortBackend();
            }

            socket_filter_option_t option   = filter->option;
            uint16_t               port_min = option.port_min;
            uint16_t               port_max = option.port_max;
            if (port_min > port_max)
            {
                LOGF("SocketManager: port min must be lower than port max");
                exit(1);
            }
            else if (port_min == port_max)
            {
                option.multiport_backend = kMultiportBackendNothing;
            }
            if (option.protocol == kSapTcp)
            {
                if (option.multiport_backend == kMultiportBackendIptables)
                {
                    listenTcpMultiPortIptables(loop, filter, option.host, port_min, ports_overlapped, port_max);
                }
                else if (option.multiport_backend == kMultiportBackendSockets)
                {
                    listenTcpMultiPortSockets(loop, filter, option.host, port_min, ports_overlapped, port_max);
                }
                else
                {
                    listenTcpSinglePort(loop, filter, option.host, port_min, ports_overlapped);
                }
            }
        }
    }
}

static void noUdpSocketConsumerFound(const udp_payload_t upl)
{
    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN]  = {0};
    LOGE("SocketManager: could not find consumer for Udp socket  [%s] <= [%s]",
         SOCKADDR_STR(hio_localaddr(upl.sock->io), localaddrstr),
         SOCKADDR_STR(hio_peeraddr(upl.sock->io), peeraddrstr));
}

static void postPayload(udp_payload_t post_pl, socket_filter_t *filter)
{

    hhybridmutex_lock(&(state->udp_pools[post_pl.tid].mutex));
    udp_payload_t *pl = popPoolItem(state->udp_pools[post_pl.tid].pool);
    hhybridmutex_unlock(&(state->udp_pools[post_pl.tid].mutex));
    *pl = post_pl;

    pl->tunnel           = filter->tunnel;
    hloop_t *worker_loop = loops[pl->tid];
    hevent_t ev          = (hevent_t){.loop = worker_loop, .cb = filter->cb};
    ev.userdata          = (void *) pl;

    hloop_post_event(worker_loop, &ev);
}

static void distributeUdpPayload(const udp_payload_t pl)
{
    // hhybridmutex_lock(&(state->mutex)); new socket manager will not lock here
    sockaddr_u *paddr      = (sockaddr_u *) hio_peeraddr(pl.sock->io);
    uint16_t    local_port = pl.real_localport;

    static socket_filter_t *balance_selection_filters[kMaxBalanceSelections];
    uint8_t                 balance_selection_filters_length = 0;
    idle_table_t           *selected_balance_table           = NULL;
    hash_t                  src_hash;
    bool                    src_hashed = false;
    const uint8_t           this_tid   = workers_count + kAcceptThreadIdOffset;

    for (int ri = (kFilterLevels - 1); ri >= 0; ri--)
    {
        c_foreach(k, filters_t, state->filters[ri])
        {

            socket_filter_t       *filter   = *(k.ref);
            socket_filter_option_t option   = filter->option;
            uint16_t               port_min = option.port_min;
            uint16_t               port_max = option.port_max;

            if (selected_balance_table != NULL && option.shared_balance_table != selected_balance_table)
            {
                continue;
            }

            if (option.protocol != kSapUdp || port_min > local_port || port_max < local_port)
            {
                continue;
            }
            if (option.white_list_raddr != NULL)
            {
                if (! checkIpIsWhiteList(paddr, option))
                {
                    continue;
                }
            }
            if (option.shared_balance_table)
            {
                if (! src_hashed)
                {
                    src_hash = sockAddrCalcHash((sockaddr_u *) hio_peeraddr(pl.sock->io));
                }
                idle_item_t *idle_item = getIdleItemByHash(this_tid, option.shared_balance_table, src_hash);

                if (idle_item)
                {
                    socket_filter_t *target_filter = idle_item->userdata;
                    keepIdleItemForAtleast(option.shared_balance_table, idle_item,
                                           option.balance_group_interval == 0 ? kDefalultBalanceInterval
                                                                              : option.balance_group_interval);
                    postPayload(pl, target_filter);
                    return;
                }

                if (WW_UNLIKELY(balance_selection_filters_length >= kMaxBalanceSelections))
                {
                    // probably never but the limit can be simply increased
                    LOGW("SocketManager: balance between more than %d tunnels is not supported", kMaxBalanceSelections);
                }
                balance_selection_filters[balance_selection_filters_length++] = filter;
                selected_balance_table                                        = option.shared_balance_table;
                continue;
            }

            postPayload(pl, filter);
            return;
        }
    }
    if (balance_selection_filters_length > 0)
    {
        socket_filter_t *filter = balance_selection_filters[fastRand() % balance_selection_filters_length];
        newIdleItem(filter->option.shared_balance_table, src_hash, filter, NULL, this_tid,
                    filter->option.balance_group_interval == 0 ? kDefalultBalanceInterval
                                                               : filter->option.balance_group_interval);
        postPayload(pl, filter);
    }
    else
    {
        noUdpSocketConsumerFound(pl);
    }
}

static void onRecvFrom(hio_t *io, shift_buffer_t *buf)
{
    udpsock_t *socket     = hevent_userdata(io);
    uint16_t   local_port = sockaddr_port((sockaddr_u *) hio_localaddr(io));
    // TODO (check)
    uint8_t target_tid = local_port % workers_count;

    udp_payload_t item = (udp_payload_t){.sock           = socket,
                                         .buf            = buf,
                                         .tid            = target_tid,
                                         .peer_addr      = *(sockaddr_u *) hio_peeraddr(io),
                                         .real_localport = local_port};

    distributeUdpPayload(item);
}

static void listenUdpSinglePort(hloop_t *loop, socket_filter_t *filter, char *host, uint16_t port,
                                uint8_t *ports_overlapped)
{
    if (ports_overlapped[port] == 1)
    {
        return;
    }
    ports_overlapped[port] = 1;
    LOGI("SocketManager: listening on %s:[%u] (%s)", host, port, "UDP");
    filter->listen_io = hloop_create_udp_server(loop, host, port);

    if (filter->listen_io == NULL)
    {
        LOGF("SocketManager: stopping due to null socket handle");
        exit(1);
    }
    udpsock_t *socket = malloc(sizeof(udpsock_t));
    *socket           = (udpsock_t){.io = filter->listen_io, .table = newIdleTable(loop)};
    hevent_set_userdata(filter->listen_io, socket);
    hio_setcb_read(filter->listen_io, onRecvFrom);
    hio_read(filter->listen_io);
}

// todo (udp manager)
static void listenUdp(hloop_t *loop, uint8_t *ports_overlapped)
{
    for (int ri = (kFilterLevels - 1); ri >= 0; ri--)
    {
        c_foreach(k, filters_t, state->filters[ri])
        {
            socket_filter_t *filter = *(k.ref);
            if (filter->option.multiport_backend == kMultiportBackendDefault)
            {
                filter->option.multiport_backend = getDefaultMultiPortBackend();
            }

            socket_filter_option_t option   = filter->option;
            uint16_t               port_min = option.port_min;
            uint16_t               port_max = option.port_max;
            if (port_min > port_max)
            {
                LOGF("SocketManager: port min must be lower than port max");
                exit(1);
            }
            else if (port_min == port_max)
            {
                option.multiport_backend = kMultiportBackendNothing;
            }
            if (option.protocol == kSapUdp)
            {
                if (option.multiport_backend == kMultiportBackendIptables)
                {
                    ;
                    // listenUdpMultiPortIptables(loop, filter, option.host, port_min, ports_overlapped, port_max);
                }
                else if (option.multiport_backend == kMultiportBackendSockets)
                {
                    // listenUdpMultiPortSockets(loop, filter, option.host, port_min, ports_overlapped, port_max);
                }
                else
                {
                    listenUdpSinglePort(loop, filter, option.host, port_min, ports_overlapped);
                }
            }
        }
    }
}

static void writeUdpThisLoop(hevent_t *ev)
{
    udp_payload_t *upl    = hevent_userdata(ev);
    size_t         nwrite = hio_write(upl->sock->io, upl->buf);
    (void) nwrite;
    destroyUdpPayload(upl);
}

void postUdpWrite(udpsock_t *socket_io, uint8_t tid_from, shift_buffer_t *buf)
{

    udp_payload_t *item = newUpdPayload(tid_from);

    *item = (udp_payload_t){.sock = socket_io, .buf = buf, .tid = tid_from};

    hevent_t ev = (hevent_t){.loop = hevent_loop(socket_io->io), .userdata = item, .cb = writeUdpThisLoop};

    hloop_post_event(hevent_loop(socket_io->io), &ev);
}

static HTHREAD_ROUTINE(accept_thread) // NOLINT
{
    (void) userdata;

    const uint8_t tid = workers_count + kAcceptThreadIdOffset;
    assert(tid >= 1);

    shift_buffer_pools[tid] =
        newGenericPoolWithCap((64) + ram_profile, allocShiftBufferPoolHandle, destroyShiftBufferPoolHandle);
    buffer_pools[tid] = createBufferPool(tid);
    state->loop       = hloop_new(HLOOP_FLAG_AUTO_FREE, buffer_pools[tid], tid);
    loops[tid]        = state->loop;

    hhybridmutex_lock(&(state->mutex));

    {
        uint8_t ports_overlapped[65536] = {0};
        listenTcp(state->loop, ports_overlapped);
    }
    {
        uint8_t ports_overlapped[65536] = {0};
        listenUdp(state->loop, ports_overlapped);
    }
    state->started = true;
    hhybridmutex_unlock(&(state->mutex));

    hloop_run(state->loop);
    LOGW("AcceptThread eventloop finished!");
    return 0;
}

struct socket_manager_s *getSocketManager(void)
{
    return state;
}

void setSocketManager(struct socket_manager_s *new_state)
{
    assert(state == NULL);
    state = new_state;
}

void startSocketManager(void)
{
    assert(state != NULL);
    // accept_thread(accept_thread_loop);
    const uint8_t tid = workers_count + kAcceptThreadIdOffset;

    state->accept_thread = hthread_create(accept_thread, NULL);
    workers[tid]         = state->accept_thread;
}

socket_manager_state_t *createSocketManager(void)
{
    assert(state == NULL);
    state = malloc(sizeof(socket_manager_state_t));
    memset(state, 0, sizeof(socket_manager_state_t));
    for (size_t i = 0; i < kFilterLevels; i++)
    {
        state->filters[i] = filters_t_init();
    }

    hhybridmutex_init(&state->mutex);

    state->udp_pools = malloc(sizeof(*state->udp_pools) * workers_count);
    memset(state->udp_pools, 0, sizeof(*state->udp_pools) * workers_count);

    state->tcp_pools = malloc(sizeof(*state->tcp_pools) * workers_count);
    memset(state->tcp_pools, 0, sizeof(*state->tcp_pools) * workers_count);

    for (unsigned int i = 0; i < workers_count; ++i)
    {
        state->udp_pools[i].pool =
            newGenericPoolWithCap((8) + ram_profile, allocUdpPayloadPoolHandle, destroyUdpPayloadPoolHandle);
        hhybridmutex_init(&(state->udp_pools[i].mutex));

        state->tcp_pools[i].pool =
            newGenericPoolWithCap((8) + ram_profile, allocTcpResultObjectPoolHandle, destroyTcpResultObjectPoolHandle);
        hhybridmutex_init(&(state->tcp_pools[i].mutex));
    }

    state->iptables_installed = checkCommandAvailable("iptables");
    state->lsof_installed     = checkCommandAvailable("lsof");
#if SUPPORT_V6
    state->ip6tables_installed = checkCommandAvailable("ip6tables");
#endif

    if (signal(SIGTERM, signalHandler) == SIG_ERR)
    {
        perror("Error setting SIGTERM signal handler");
        exit(1);
    }
    if (signal(SIGINT, signalHandler) == SIG_ERR)
    {
        perror("Error setting SIGINT signal handler");
        exit(1);
    }

    return state;
}
