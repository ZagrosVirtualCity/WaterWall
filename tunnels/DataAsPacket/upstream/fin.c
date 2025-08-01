#include "structure.h"

#include "loggers/network_logger.h"

void dataaspacketTunnelUpStreamFinish(tunnel_t *t, line_t *l)
{

    dataaspacket_lstate_t *ls = lineGetState(tunnelchainGetWorkerPacketLine(tunnelGetChain(t), lineGetWID(l)), t);
    ls->line = NULL; // Clear the line state, as we are finishing the upstream
    ls->paused = false; // Reset the paused state
    bufferstreamDestroy(&ls->read_stream); // Clean up the read stream
}
