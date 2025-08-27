// gnb_process.c
// Compile: gcc -o gnb_process gnb_process.c -pthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

#define NUM_UE 200
#define NUM_AMF 5
#define SHM_NAME "/5g_sim_shm"
#define SHM_SIZE (sizeof(SharedMemory))
#define AMF_BASE_PORT 9101
#define GNB_PAGING_PORT 9200

// message ids and masks same as UE file
#define MSG_UE_RRC_CONNECTION_REQUEST 0x10
#define MSG_RRC_UE_CONNECTION_RESPONSE 0x11
#define MSG_RRC_UE_PAGING             0x14
#define MSG_RRC_NGAP_REQ              0x12
#define MSG_NGAP_RESP                 0x13
#define BM_RANDOM_VALUE 0x01
#define BM_5G_STMSI     0x02

typedef struct {
    uint8_t msgid;
    uint8_t bitmask;
    uint16_t ue_id;
    uint64_t tmsi;
    uint64_t s_tmsi;
} Message;

typedef struct {
    pthread_mutex_t mutex;
    Message ul[NUM_UE];
    int ul_ready[NUM_UE];
    Message dl[NUM_UE];
    int dl_ready[NUM_UE];
    int ue_states[NUM_UE];
} SharedMemory;

SharedMemory *shm = NULL;

int ue_to_amf[NUM_UE];
int amf_counts[NUM_AMF];
int amf_capacity[NUM_AMF] = {40,20,30,70,40};

// WRR - smooth simplified implementation
int amf_current_weight[NUM_AMF];
int amf_weight[NUM_AMF];

void init_shm() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) { perror("gNB shm_open"); exit(1); }
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

int pick_amf_wrr() {
    // smooth weighted RR
    int total = 0;
    int best = 0;
    for (int i=0;i<NUM_AMF;i++) total += amf_weight[i];
    int best_i = -1;
    int best_val = -2147483648;
    for (int i=0;i<NUM_AMF;i++) {
        amf_current_weight[i] += amf_weight[i];
        if (amf_current_weight[i] > best_val) { best_val = amf_current_weight[i]; best_i = i; }
    }
    amf_current_weight[best_i] -= total;
    return best_i;
}

// paging listener (AMF will connect to this port to send paging)
void *paging_server(void *arg) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(GNB_PAGING_PORT); addr.sin_addr.s_addr = INADDR_ANY;
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 50);
    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        Message m; int r = recv(c, &m, sizeof(m), 0);
        close(c);
        if (r <= 0) continue;
        if (m.msgid == MSG_NGAP_RESP /* we reuse id for paging here */) {
            // find UE slot by ue_id and write DL paging
            int uid = m.ue_id;
            pthread_mutex_lock(&shm->mutex);
            shm->dl[uid].msgid = MSG_RRC_UE_PAGING;
            shm->dl[uid].bitmask = BM_5G_STMSI;
            shm->dl[uid].s_tmsi = m.s_tmsi;
            shm->dl_ready[uid] = 1;
            pthread_mutex_unlock(&shm->mutex);
        }
    }
    return NULL;
}

int send_ngap_to_amf(int amf_idx, Message *req, Message *resp) {
    // connect to AMF server (tcp)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(AMF_BASE_PORT + amf_idx);
    serv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { close(sock); return -1; }
    send(sock, req, sizeof(*req), 0);
    int r = recv(sock, resp, sizeof(*resp), 0);
    close(sock);
    return r > 0 ? 0 : -1;
}

int main() {
    // open shared memory created by UE process
    init_shm();

    // init WRR
    for (int i=0;i<NUM_AMF;i++) { amf_weight[i] = amf_capacity[i]; amf_current_weight[i]=0; amf_counts[i]=0; }
    for (int i=0;i<NUM_UE;i++) ue_to_amf[i] = -1;

    // start paging server
    pthread_t t; pthread_create(&t, NULL, paging_server, NULL);

    // main loop: poll UL slots
    int handled_initial = 0;
    while (1) {
        // scan UL
        for (int i=0;i<NUM_UE;i++) {
            int do_process = 0;
            pthread_mutex_lock(&shm->mutex);
            if (shm->ul_ready[i]) { do_process = 1; }
            pthread_mutex_unlock(&shm->mutex);
            if (!do_process) continue;

            // grab message
            Message m;
            pthread_mutex_lock(&shm->mutex);
            m = shm->ul[i];
            shm->ul_ready[i] = 0;
            pthread_mutex_unlock(&shm->mutex);

            if (m.msgid == MSG_UE_RRC_CONNECTION_REQUEST) {
                // Registration?
                if (m.bitmask & BM_RANDOM_VALUE) {
                    int amf = pick_amf_wrr();
                    ue_to_amf[i] = amf;
                    amf_counts[amf]++;
                    // build NGAP req
                    Message ngap = {0};
                    ngap.msgid = MSG_RRC_NGAP_REQ;
                    ngap.bitmask = BM_RANDOM_VALUE;
                    ngap.ue_id = i;
                    ngap.tmsi = m.tmsi;
                    Message ngap_resp = {0};
                    if (send_ngap_to_amf(amf, &ngap, &ngap_resp) == 0) {
                        // forward response to UE DL
                        pthread_mutex_lock(&shm->mutex);
                        shm->dl[i].msgid = MSG_RRC_UE_CONNECTION_RESPONSE;
                        shm->dl[i].bitmask = ngap_resp.bitmask;
                        shm->dl[i].s_tmsi = ngap_resp.s_tmsi;
                        shm->dl_ready[i] = 1;
                        shm->ue_states[i] = 1; // REGISTERED
                        pthread_mutex_unlock(&shm->mutex);
                        handled_initial++;
                    } else {
                        // AMF not reachable (ignore)
                    }
                } else if (m.bitmask & BM_5G_STMSI) {
                    // Service request: forward to mapped AMF
                    int amf = ue_to_amf[i];
                    if (amf < 0) {
                        amf = pick_amf_wrr();
                        ue_to_amf[i] = amf;
                        amf_counts[amf]++;
                    }
                    Message ngap = {0};
                    ngap.msgid = MSG_RRC_NGAP_REQ;
                    ngap.bitmask = BM_5G_STMSI;
                    ngap.ue_id = i;
                    ngap.tmsi = m.tmsi;
                    ngap.s_tmsi = m.s_tmsi;
                    Message ngap_resp = {0};
                    send_ngap_to_amf(amf, &ngap, &ngap_resp);
                    // forward ack to UE
                    pthread_mutex_lock(&shm->mutex);
                    shm->dl[i].msgid = MSG_RRC_UE_CONNECTION_RESPONSE;
                    shm->dl_ready[i] = 1;
                    shm->ue_states[i] = (shm->ue_states[i]==2)?2:1;
                    pthread_mutex_unlock(&shm->mutex);
                }
            }
        }

        // check termination: all connected?
        int connected = 0;
        pthread_mutex_lock(&shm->mutex);
        for (int i=0;i<NUM_UE;i++) if (shm->ue_states[i]==2) connected++;
        pthread_mutex_unlock(&shm->mutex);
        if (connected == NUM_UE && handled_initial >= NUM_UE) {
            printf("gNB: all UE CONNECTED. distribution:\n");
            for (int k=0;k<NUM_AMF;k++) {
                printf("AMF%d: %d UEs (%.2f%%)\n", k+1, amf_counts[k], (float)amf_counts[k]/NUM_UE*100.0f);
            }
            break;
        }
        usleep(2000);
    }

    return 0;
}

