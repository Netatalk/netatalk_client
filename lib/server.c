/*
 *  server.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "afp.h"
#include "dsi.h"
#include "utils.h"
#include "uams_def.h"
#include "codepage.h"
#include "users.h"
#include "libafpclient.h"
#include "afp_internal.h"
#include "dsi.h"


struct afp_server *afp_server_complete_connection(
    void *priv,
    struct afp_server * server,
#if 0
    struct addrinfo * address,
#endif
    unsigned char *versions,
    unsigned int uams, char *username, char *password,
    unsigned int requested_version, unsigned int uam_mask)
{
    char loginmsg[AFP_LOGINMESG_LEN];
    int using_uam;
    char mesg[MAX_ERROR_LEN];
    unsigned int len = 0;
    memset(loginmsg, 0, AFP_LOGINMESG_LEN);
    server->requested_version = requested_version;
    memcpy(server->username, username, sizeof(server->username));
    memcpy(server->password, password, sizeof(server->password));
    add_fd_and_signal(server->fd);
    dsi_opensession(server);
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "afp_server_complete_connection -- DSI session: tx_quantum=%u (max write size)",
                   server->tx_quantum);

    /* Figure out what version we're using */
    if (((server->using_version =
                pick_version(versions, requested_version)) == NULL)) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Server cannot handle AFP version %d",
                       requested_version);
        goto error;
    }

    using_uam = pick_uam(uams, uam_mask);

    if (using_uam == -1) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not pick a matching UAM");
        goto error;
    }

    server->using_uam = using_uam;

    if (afp_server_login(server, mesg, &len, MAX_ERROR_LEN)) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Login error: %s", mesg);
        goto error;
    }

    if (afp_getsrvrparms(server)) {
        log_for_client(priv, AFPFSD, LOG_ERR,
                       "Could not get server parameters");
        goto error;
    }

    /* If we haven't gotten a proper date back, so set it to the connect time. */
    if (server->connect_time == AD_DATE_TO_UNIX(0)) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        server->connect_time = tv.tv_sec;
    }

    afp_getsrvrmsg(server, AFPMESG_LOGIN,
                   ((server->using_version && server->using_version->av_number >= 30) ? 1 : 0),
                   server->dsi_default_timeout, loginmsg); /* block */

    if (strlen(loginmsg) > 0)
        log_for_client(priv, AFPFSD, LOG_NOTICE,
                       "Login message: %s", loginmsg);

    memcpy(server->loginmesg, loginmsg, AFP_LOGINMESG_LEN);
    server->connect_state = SERVER_STATE_CONNECTED;
    return server;
error:
    afp_server_remove(server);
    return NULL;
}
