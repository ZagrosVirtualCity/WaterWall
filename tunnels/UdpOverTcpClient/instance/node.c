#include "interface.h"
#include "structure.h"

#include "loggers/network_logger.h"

node_t nodeUdpOverTcpClientGet(void)
{
    const char *type_name     = "UdpOverTcpClient";
    node_t      node_udpovertcpclient = {
             .name                  = NULL,
             .type                  = stringDuplicate(type_name),
             .next                  = NULL,
             .hash_name             = 0,
             .hash_type             = calcHashBytes(type_name, stringLength(type_name)),
             .hash_next             = 0,
             .version               = 0001,
             .createHandle          = udpovertcpclientTunnelCreate,
             .node_json             = NULL,
             .node_settings_json    = NULL,
             .node_manager_config   = NULL,
             .instance              = NULL,
             .flags                 = kNodeFlagNone,
             .required_padding_left = 2, // to append the length of the payload (should not be changed) (must same kHeaderSize)
             .layer_group           = kNodeLayerAnything,
             .layer_group_next_node = kNodeLayerAnything,
             .layer_group_prev_node = kNodeLayerAnything,
             .can_have_next         = true,
             .can_have_prev         = true,
    };
    return node_udpovertcpclient;
}
