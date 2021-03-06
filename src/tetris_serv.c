#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <strings.h>
#include <string.h>

#ifdef __sun
#include "strtonum.h"
#include <mtmalloc.h>
#elif defined(__linux__)
#include <bsd/stdlib.h>
#include <bsd/string.h>
#endif

#include <limits.h>
#include <uv.h>
#include <pthread.h>
#include "packet.h"
#include "macros.h"

#define DEFAULT_PORT 48879

uv_udp_t g_recv_sock;
uv_udp_t g_send_sock;

static int vanillaSock;

/* This will be the source of randomness, in solaris /dev/urandom
 * is non-blocking but under the hood should use KCF for secure
 * random numbers.  This may not be the case for other platforms,
 * I may toy around with using the entropy values from the random
 * pools provided by these files as a seed to better generators.
 * Honestly this doesn't need to be cryptographically secure, just
 * varied enough and not ridiculously predictable */
FILE *randFile = NULL;

/* This function does necessitate a syscall, so perhaps we should
only call this to a faster PRNG after so many calls */
uint32_t getRand()
{
    uint32_t retVal = 0;
    fread(&retVal, sizeof(uint32_t), 1, randFile);
    return retVal;
}

typedef struct _request {
    ssize_t len;
    void *payload;
    struct sockaddr from;
} request;

void handle_msg(uv_work_t *req)
{
    /* This function dispatches a request */
    request *r = req->data;
    packet_t *pkt = r->payload;

    /* Create a blank errPkt, populate later with 
     * createErrPacket */
    MSG_TYPE packetType = (MSG_TYPE)pkt->type;
    msg_err errMsg;
    msg_register_client *rclient = NULL;
    char *clientName = NULL;
    msg_reg_ack m;

    /* Stack allocated buffer for the error message packet */
    uint8_t errPktBuf[ERRMSG_SIZE];
    uint8_t ackPackBuf[sizeof(packet_t) + sizeof(msg_reg_ack)];
    packet_t *errPkt = errPktBuf;
    packet_t *ackPack = ackPackBuf;
    ssize_t ePktSize = -1;

    /* Validate lengths */
    if(!validateLength((packet_t*)r->payload, r->len, packetType, &ePktSize)) {
        WARNING("Incorrect/unexpected packet length\n"
                "Expected %zd bytes, got %zd bytes\n", ePktSize, r->len);
        createErrPacket(errPkt, BAD_LEN);
        reply(errPkt, ERRMSG_SIZE, &r->from, vanillaSock);
        return;
    }

    switch(packetType) {
        case REGISTER_TETRAD:
        case ERR_PACKET:
        case KICK_CLIENT:
        case UPDATE_CLIENT_STATE:
        case REG_ACK:
            createErrPacket(errPkt, ILLEGAL_MSG);
            reply(errPkt, ERRMSG_SIZE, &r->from, vanillaSock);
            break;

        case REGISTER_CLIENT:
            rclient = (msg_register_client*)(pkt->data);

            /* This won't be NULL terminated */
            clientName = malloc(rclient->nameLength + 1);
            strlcpy(clientName, rclient->name, rclient->nameLength + 1);

            /* Do something with this information, haven't written this
             * function just yet */ 
            // m = register_client(clientName, r->from);
            m.curPlayerId = htonl(getRand());
            ackPack->type = REG_ACK;
            memcpy(ackPack->data, &m, sizeof(msg_reg_ack));
            printf("Registering client %s\n", clientName);
            reply(ackPack, sizeof(ackPackBuf), &r->from, vanillaSock);
            free(clientName);
            break;

        default:
            WARN("Unhandled packet type!!!!\n");
            createErrPacket(errPkt, UNSUPPORTED_MSG);
            reply(errPkt, ERRMSG_SIZE, &r->from, vanillaSock);
            break;
    }
}

void destroy_msg(uv_work_t *req, int status)
{
    request *r = req->data;
    free(r->payload);
    free(r);
    free(req);

    if(status) {
        WARNING("WARNING: %s\n", uv_err_name(status));
    }
}

void onrecv(uv_udp_t *req, ssize_t nread, const uv_buf_t *buf,
            const struct sockaddr *addr, unsigned flags)
{
    if (nread < 0) {
        WARNING("WARNING: %s\n", uv_err_name(nread));
        uv_close((uv_handle_t*)req, NULL);
        free(buf->base);
        return;
    }

    if (nread == 0) {
        if(flags & UV_UDP_PARTIAL) {
            WARN("Lost some of the buffer\n");
        }
        free(buf->base);
        return;
    }

    char senderIP[20] = { 0 };
    uv_ip4_name((const struct sockaddr_in*)addr, senderIP, 19);
    in_port_t port = ((const struct sockaddr_in*)addr)->sin_port;
    //fprintf(stderr, "Received msg over %s:%d\n", senderIP, port);

    uv_work_t *work = (uv_work_t*)malloc(sizeof(uv_work_t));

    request *theReq = malloc(sizeof(request));
    theReq->payload = buf->base;
    //memcpy(theReq->payload, buf->base, nread);
    //free(buf->base);
    theReq->len = nread;
    theReq->from = *addr;

    work->data = theReq;
    uv_queue_work(uv_default_loop(), work, handle_msg, destroy_msg);
}

static void malloc_cb(uv_handle_t *h, size_t s, uv_buf_t *b)
{
    b->base = (char*)malloc(s * sizeof(char));
    assert(b->base != NULL);
    b->len = s;
}

int main(int argc, char *argv[])
{
    int go_ret;
    int port = DEFAULT_PORT;
    const char *err_str = NULL;


    static struct option longopts[] = {
        {"port",      required_argument,     NULL,     'p'},
        {"random",      required_argument,     NULL,     'r'},
        {NULL,        0,                     NULL,     0}
    };

    while ((go_ret = getopt_long(argc, argv, "p:r:", longopts, NULL)) != -1) {
       switch (go_ret) {
            case 'p':
                port = strtonum(optarg, 1, UINT16_MAX, &err_str);
                if (err_str) {
                    ERR("Bad value for port");
                }
            case 'r':
                randFile = fopen(optarg, "r");
       }
    }

    if (!randFile) {
        randFile = fopen("/dev/urandom", "r");
    }

    /* There were many possible approaches to take to deal with the lack
     * of thread safety in libuv's uv_udp_send() functions.  These include
     * async_send, multiple outbound sockets on a per thread basis,
     * constructing a raw socket based send call, or constructing a send
     * queue of messages when the worker thread has completed.  For now
     * we'll try the raw socket approach */

    uv_loop_t *loop = uv_default_loop();
    uv_udp_init(loop, &g_recv_sock);
    //uv_udp_init(loop, &g_send_sock);
    struct sockaddr_in recaddr;

    uv_ip4_addr("0.0.0.0", port, &recaddr);
    uv_udp_bind(&g_recv_sock, (const struct sockaddr*)&recaddr, UV_UDP_REUSEADDR);
    uv_udp_recv_start(&g_recv_sock, malloc_cb, onrecv);

    vanillaSock = socket(AF_INET, SOCK_DGRAM, 0);

    return uv_run(loop, UV_RUN_DEFAULT);
}
