
#ifndef __DSI_H_
#define __DSI_H_

#include "afp.h"

struct dsi_request {
    unsigned short requestid;
    unsigned char subcommand;
    void *other;
    int wait;
    int done_waiting;
    pthread_cond_t  waiting_cond;
    pthread_mutex_t waiting_mutex;
    struct dsi_request *next;
    int return_code;
    unsigned int connection_generation;
};

int dsi_receive(struct afp_server * server, void * data, int size);
int dsi_getstatus(struct afp_server * server);
int dsi_sendtickle(struct afp_server *server);
void dsi_flush_request_queue(struct afp_server *server);

int dsi_opensession(struct afp_server *server);

int dsi_send(struct afp_server *server, char * msg, int size, int wait,
             unsigned char subcommand, void **other);
struct dsi_session *dsi_create(struct afp_server *server);
int dsi_restart(struct afp_server *server);
int dsi_recv(struct afp_server * server);

#define DSI_BLOCK_TIMEOUT -1
#define DSI_DONT_WAIT 0
#define DSI_DEFAULT_TIMEOUT 5
/* A spun down time capsule can take up to 20 secs to
 * wake up and reply to a mount request */
#define DSI_OPENVOLUME_TIMEOUT 20
#define DSI_LOGIN_TIMEOUT 20
#define DSI_TIMECAPSULE_DEFAULT_TIMEOUT 30

#define GETSTATUS_BUF_SIZE 1024

#endif
