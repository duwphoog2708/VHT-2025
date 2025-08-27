// amf_process.c
// Compile: gcc -o amf_process amf_process.c -pthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>

#define AMF_BASE_PORT 9101
#define GNB_PAGING_PORT 9200
#define NUM_UE 200

#define MSG_RRC_NGAP_REQ 0x12
#define MSG_NGAP_RESP     0x13
#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02

typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

int amf_index_global;
int capacity_global;

void send_paging_to_gnb(Message *paging) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(GNB_PAGING_PORT);
    serv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { close(sock); return; }
    send(sock, paging, sizeof(*paging), 0);
    close(sock);
}

void *handle_connection(void *arg) {
    int fd = *(int*)arg;
    free(arg);
    Message req;
    int r = recv(fd, &req, sizeof(req), 0);
    if (r <= 0) { close(fd); return NULL; }

    if (req.msgid == MSG_RRC_NGAP_REQ) {
        if (req.bitmask & BM_RANDOM_VALUE) {
            // Registration: generate S-TMSI and send response
            uint64_t s = (((uint64_t)(amf_index_global & 0xFF)) << 56) |
                         (((uint64_t)(amf_index_global & 0x3FF)) << 46) |
                         (((uint64_t)(amf_index_global & 0x3F)) << 40) |
                         (req.tmsi & 0xFFFFFFFFFF);
            Message resp = {0};
            resp.msgid = MSG_NGAP_RESP;
            resp.bitmask = BM_RANDOM_VALUE;
            resp.ue_id = req.ue_id;
            resp.tmsi = req.tmsi;
            resp.s_tmsi = s;
            send(fd, &resp, sizeof(resp), 0);

            // choose y and schedule paging in separate thread (detached)
            int rnds[6] = {500,1000,1500,2000,2500,3000};
            int y = rnds[rand() % 6];
            // prepare paging msg
            Message paging = {0};
            paging.msgid = MSG_NGAP_RESP; // gNB expects this id for paging in our gNB code
            paging.bitmask = BM_5G_STMSI;
            paging.ue_id = req.ue_id;
            paging.s_tmsi = s;
            // detach thread to sleep y and send paging
            pthread_t t; Message *mp = malloc(sizeof(Message)); *mp = paging;
            pthread_create(&t, NULL, (void*(*)(void*)) ( ^(void *arg)->void* {
                Message *p = (Message*)arg;
                usleep(y * 1000);
                send_paging_to_gnb(p);
                free(p);
                return NULL;
            }), mp);
            pthread_detach(t);
        } else if (req.bitmask & BM_5G_STMSI) {
            // Service request: reply ack
            Message resp = {0};
            resp.msgid = MSG_NGAP_RESP;
            resp.bitmask = BM_5G_STMSI;
            resp.ue_id = req.ue_id;
            send(fd, &resp, sizeof(resp), 0);
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("Usage: %s <amf_index 1..5> <capacity>\n", argv[0]); return 1; }
    amf_index_global = atoi(argv[1]) - 1;
    capacity_global = atoi(argv[2]);
    srand(time(NULL) ^ amf_index_global);

    int port = AMF_BASE_PORT + amf_index_global;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = INADDR_ANY;
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 50);
    printf("AMF%d listening on %d (cap=%d)\n", amf_index_global+1, port, capacity_global);

    while (1) {
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }
        // handle connection in thread
        pthread_t t; pthread_create(&t, NULL, handle_connection, cfd); pthread_detach(t);
    }
    close(srv);
    return 0;
}

