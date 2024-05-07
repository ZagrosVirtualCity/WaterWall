#include "reality_server.h"
#include "buffer_pool.h"
#include "buffer_stream.h"
#include "frand.h"
#include "hloop.h"
#include "loggers/network_logger.h"
#include "managers/node_manager.h"
#include "shiftbuffer.h"
#include "tunnel.h"
#include "utils/jsonutils.h"
#include <openssl/evp.h>
#include <stdint.h>

enum reality_consts
{
    kEncryptionBlockSize  = 16,
    kSignHeaderLen        = kEncryptionBlockSize * 2,
    kSignPasswordLen      = kEncryptionBlockSize,
    kSignIVlen            = 16, // IV size for *most* modes is the same as the block size. For AES this is 128 bits
    kTLSVersion12         = 0x0303,
    kTLS12ApplicationData = 0x17,
};

enum connection_auth_state
{
    kConAuthPending,
    kConUnAuthorized,
    kConAuthorized
};

typedef struct reality_server_state_s
{

    tunnel_t *dest;
    // settings
    bool          anti_tit; // solve tls in tls using paddings
    uint32_t      max_delta_time;
    uint32_t      counter_threshould;
    char         *password;
    unsigned int  password_length;
    uint64_t      hash1;
    uint64_t      hash2;
    uint64_t      hash3;
    char          context_password[kSignPasswordLen];
    unsigned char context_base_iv[kSignIVlen];

} reality_server_state_t;

typedef struct reality_server_con_state_s
{
    unsigned char              calculated_iv[kSignIVlen];
    EVP_CIPHER_CTX            *encryption_context;
    EVP_CIPHER_CTX            *decryption_context;
    uint32_t                   sniff_counter;
    uint32_t                   epoch;
    enum connection_auth_state auth_state;
    bool                       first_sent;
    bool                       init_sent;

} reality_server_con_state_t;

static size_t paddingDecisionCb(SSL *ssl, int type, size_t len, void *arg)
{
    (void) ssl;
    (void) type;
    (void) len;
    reality_server_con_state_t *cstate = arg;

    if (cstate->reply_sent_tit >= 1 && cstate->reply_sent_tit < 6)
    {
        return (16 * (160 + (0x7F & (size_t) fastRand())));
    }

    return 0;
}

static void cleanup(tunnel_t *self, context_t *c)
{
    reality_server_con_state_t *cstate = CSTATE(c);
    if (cstate != NULL)
    {
        EVP_CIPHER_CTX_free(cstate->encryption_context);
        EVP_CIPHER_CTX_free(cstate->decryption_context);
        free(cstate);
        CSTATE_MUT(c) = NULL;
    }
}

static void fallbackWrite(tunnel_t *self, context_t *c)
{
    if (! isAlive(c->line))
    {
        destroyContext(c);
        return;
    }
    assert(c->payload == NULL); // payload must be consumed
    reality_server_state_t     *state  = STATE(self);
    reality_server_con_state_t *cstate = CSTATE(c);

    if (! cstate->fallback_init_sent)
    {
        cstate->fallback_init_sent = true;

        context_t *init_ctx = newInitContext(c->line);
        init_ctx->src_io    = c->src_io;
        cstate->init_sent   = true;
        state->fallback->upStream(state->fallback, init_ctx);
        if (! isAlive(c->line))
        {
            destroyContext(c);
            return;
        }
    }
    size_t record_len = bufferStreamLen(cstate->fallback_buf);
    if (record_len == 0)
    {
        destroyContext(c);
        return;
    }
    if (! cstate->fallback_first_sent)
    {
        c->first                    = true;
        cstate->fallback_first_sent = true;
    }

    c->payload = bufferStreamRead(cstate->fallback_buf, record_len);
    state->fallback->upStream(state->fallback, c);
}
static void onFallbackTimer(htimer_t *timer)
{
    struct timer_eventdata *data = hevent_userdata(timer);
    fallbackWrite(data->self, data->c);
    htimer_del(timer);
    free(data);
}
static bool isValid(shift_buffer_t *in_buf, reality_server_state_t *state, reality_server_con_state_t *cstate)
{
    if (bufLen(in_buf) < kSignHeaderLen + 1 + 2 + 2)
    {
        return false;
    }
    uint8_t  *header_p8  = ((uint8_t *) rawBufMut(in_buf)) + 1 + 2 + 2;
    uint64_t *header_p64 = ((uint64_t *) rawBufMut(in_buf)) + 1 + 2 + 2;

    for (int i = 0; i < kSignHeaderLen / sizeof(uint64_t); i++)
    {
        header_p64[i] = header_p64[i] ^ (state->hash2);
    }

    const uint32_t max_start_pos = 1 + kSignHeaderLen - sizeof(uint32_t);
    uint32_t       start_pos     = (uint32_t) ((state->hash1) % max_start_pos);

    uint32_t *header_epoch = (uint32_t *)(header_p8 + start_pos);

    retutrn(cstate->epoch - state->max_delta_time >= *header_epoch);
}

static void upStream(tunnel_t *self, context_t *c)
{
    reality_server_state_t     *state  = STATE(self);
    reality_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {

        switch (cstate->auth_state)
        {
        case kConAuthPending:
            if (isValid(c->payload, state->hash1, state->hash2, cstate->epoch, state->max_delta_time))
            {
                cstate->auth_state = kConAuthorized;
                EVP_EncryptInit_ex(cstate->encryption_context, EVP_aes_128_cbc(), NULL,
                                   (const uint8_t *) state->context_password, (const uint8_t *) iv);

                EVP_EncryptInit_ex(cstate->decryption_context, EVP_aes_128_cbc(), NULL,
                                   (const uint8_t *) state->context_password, (const uint8_t *) iv);
                goto authorized;
            }
            else
            {
                cstate->sniff_counter -= 0;
                if (cstate->sniff_counter == 0)
                {
                    cstate->auth_state = kConUnAuthorized;
                }
            }
        case kConUnAuthorized:
            state->dest->upStream(state->dest, c);

            break;
        authorized:;
        case kConAuthorized:
            self->up->upStream(self->up, c);
            break;
        }
    }
    else
    {

        if (c->init)
        {
            CSTATE_MUT(c) = malloc(sizeof(reality_server_con_state_t));
            memset(CSTATE(c), 0, sizeof(reality_server_con_state_t));
            cstate->auth_state         = kConAuthPending;
            cstate->sniff_counter      = state->counter_threshould;
            cstate->epoch              = (uint32_t) hloop_now(c->line->loop);
            cstate->encryption_context = EVP_CIPHER_CTX_new();
            cstate->decryption_context = EVP_CIPHER_CTX_new();

            state->dest->upStream(state->dest, c);
        }
        else if (c->fin)
        {

            if (cstate->fallback_mode)
            {
                if (cstate->fallback_init_sent)
                {
                    cleanup(self, c);
                    state->fallback->upStream(state->fallback, c);
                }
                else
                {
                    cleanup(self, c);
                    destroyContext(c);
                }
            }
            else if (cstate->init_sent)
            {
                cleanup(self, c);
                self->up->upStream(self->up, c);
            }
            else
            {
                cleanup(self, c);
                destroyContext(c);
            }
        }
    }

    return;

disconnect:;
    context_t *fail_context_up = newFinContextFrom(c);
    fail_context_up->src_io    = c->src_io;
    self->up->upStream(self->up, fail_context_up);

    context_t *fail_context = newFinContextFrom(c);
    cleanup(self, c);
    destroyContext(c);
    self->dw->downStream(self->dw, fail_context);
}

static void downStream(tunnel_t *self, context_t *c)
{
    reality_server_state_t     *state  = STATE(self);
    reality_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {
        if (state->anti_tit && isAuthenticated(c->line))
        {
            cstate->reply_sent_tit += 1;
        }

        enum sslstatus status;

        if (! cstate->handshake_completed)
        {
            if (cstate->fallback_mode)
            {
                self->dw->downStream(self->dw, c);
                return; // not gona encrypt fall back data
            }

            LOGF("How it is possible to receive data before sending init to upstream?");
            exit(1);
        }

        reuseContextBuffer(c);
        destroyContext(c);

        return;
    }

    if (c->est)
    {
        self->dw->downStream(self->dw, c);
        return;
    }
    if (c->fin)
    {
        cleanup(self, c);
        self->dw->downStream(self->dw, c);
    }

    return;

failed_after_establishment:;
    context_t *fail_context_up = newFinContextFrom(c);
    self->up->upStream(self->up, fail_context_up);

    context_t *fail_context = newFinContextFrom(c);
    cleanup(self, c);
    destroyContext(c);
    self->dw->downStream(self->dw, fail_context);
}

tunnel_t *newRealityServer(node_instance_context_t *instance_info)
{
    reality_server_state_t *state = malloc(sizeof(reality_server_state_t));
    memset(state, 0, sizeof(reality_server_state_t));

    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: RealityServer->settings (object field) : The object was empty or invalid");
        return NULL;
    }

    if (! getStringFromJsonObject(&(state->password), settings, "password"))
    {
        LOGF("JSON Error: RealityClient->settings->password (string field) : The data was empty or invalid");
        return NULL;
    }
    getIntFromJsonObjectOrDefault(&(state->counter_threshould), settings, "sniffing-counter", 5);
    getIntFromJsonObjectOrDefault(&(state->max_delta_time), settings, "max-delta-time", 10);

    state->password_length = (int) strlen(state->password);
    if (state->password_length < 3)
    {
        LOGF("JSON Error: RealityClient->settings->password (string field) : password is too short");
    }
    // memset already made buff 0
    memcpy(state->context_password, state->password, state->password_length);

    state->hash1 = CALC_HASH_BYTES(state->password, strlen(state->password));
    state->hash2 = CALC_HASH_PRIMITIVE(state->hash1);
    state->hash3 = CALC_HASH_PRIMITIVE(state->hash2);
    // the iv must be unpredictable, so initializing it from password
    for (int i = 0; i < kSignIVlen; i++)
    {
        const uint8_t seed        = (uint8_t) (state->hash3 * (i + 7));
        state->context_base_iv[i] = (uint8_t) (CALC_HASH_PRIMITIVE(seed));
    }

    char *dest_node_name = NULL;
    if (! getStringFromJsonObject(&dest_node_name, settings, "destination"))
    {
        LOGW("RealityServer: no destination node provided in json");
        return NULL;
    }
    LOGD("RealityServer: accessing destination node");

    hash_t  hash_next = CALC_HASH_BYTES(dest_node_name, strlen(dest_node_name));
    node_t *next_node = getNode(hash_next);
    if (next_node == NULL)
    {
        LOGF("RealityServer: destination node not found");
        exit(1);
    }

    if (next_node->instance == NULL)
    {
        runNode(next_node, instance_info->chain_index + 1);
    }

    state->dest = next_node->instance;
    free(dest_node_name);
    getBoolFromJsonObjectOrDefault(&(state->anti_tit), settings, "anti-tls-in-tls", false);

    tunnel_t *t = newTunnel();
    chainDown(t, state->dest);
    t->state      = state;
    t->upStream   = &upStream;
    t->downStream = &downStream;
    atomic_thread_fence(memory_order_release);
    return t;
}

api_result_t apiRealityServer(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t){0};
}

tunnel_t *destroyRealityServer(tunnel_t *self)
{
    (void) (self);
    return NULL;
}

tunnel_metadata_t getMetadataRealityServer()
{
    return (tunnel_metadata_t){.version = 0001, .flags = 0x0};
}
