/*
 *  proto_login.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <stdlib.h>
#include <string.h>
#include "dsi.h"
#include "dsi_protocol.h"
#include "afp.h"
#include "utils.h"
#include "afp_internal.h"


int afp_logout(struct afp_server *server, unsigned char wait)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
    }  __attribute__((__packed__)) request;
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpLogout;
    request.pad = 0;
    return dsi_send(server, (char *) &request, sizeof(request),
                    wait, afpLogout, NULL);
}

int afp_login_reply(__attribute__((unused)) struct afp_server *server,
                    char *buf, unsigned int size,
                    void *other)
{
    struct afp_rx_buffer * rx = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        char userauthinfo[];
    } * afp_login_reply_packet = (void *)buf;
    size -= sizeof(struct dsi_header);

    if (size > 0 && rx != NULL) {
        if (size > rx->maxsize) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "afp_login_reply: Response truncated from %u to %u bytes",
                           size, rx->maxsize);
            size = rx->maxsize;
        }

        memcpy(rx->data, afp_login_reply_packet->userauthinfo, size);
        rx->size = size;
    }

    return 0;
}

int afp_changepassword(struct afp_server *server, const char * ua_name,
                       char *userauthinfo, unsigned int userauthinfo_len,
                       struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
    }  __attribute__((__packed__)) * request;
    unsigned int ua_pascal_len = (unsigned char)strlen(ua_name) + 1;
    /* Pad to even boundary after UAM pascal string */
    unsigned int ua_pad = (ua_pascal_len & 1) ? 1 : 0;
    unsigned int len =
        sizeof(*request) /* DSI Header */
        + ua_pascal_len + ua_pad /* UAM + alignment */
        + userauthinfo_len;
    msg = malloc(len);

    if (!msg) {
        return -1;
    }

    request = (void *) msg;
    p = msg + (sizeof(*request));
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpChangePassword;
    request->pad = 0;
    p += copy_to_pascal(p, ua_name) + 1;

    if (ua_pad) {
        *p++ = 0;
    }

    memcpy(p, userauthinfo, userauthinfo_len);
    ret = dsi_send(server, (char *) msg, len, server->dsi_default_timeout,
                   afpChangePassword, (void *)rx);
    free(msg);
    return ret;
}

int afp_changepassword_reply(__attribute__((unused)) struct afp_server *server,
                             char *buf,
                             unsigned int size, void *other)
{
    struct afp_rx_buffer * rx = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        char userauthinfo[];
    } * afp_changepassword_reply_packet = (void *)buf;
    size -= sizeof(struct dsi_header);

    if (size > 0 && rx != NULL) {
        if (size > rx->maxsize) {
            size = rx->maxsize;
        }

        memcpy(rx->data, afp_changepassword_reply_packet->userauthinfo, size);
        rx->size = size;
    }

    return 0;
}

int afp_login(struct afp_server *server, const char * ua_name,
              char *userauthinfo, unsigned int userauthinfo_len,
              struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
    }  __attribute__((__packed__)) * request;
    unsigned int len =
        sizeof(*request) /* DSI Header */
        + (unsigned int)strnlen(server->using_version->av_name, UINT8_MAX) + 1
        + (unsigned int)strnlen(ua_name, UINT8_MAX) + 1
        + userauthinfo_len;
    msg = malloc(len);

    if (!msg) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "afp_login: Failed to allocate message buffer");
        return -1;
    }

    request = (void *) msg;
    p = msg + (sizeof(*request));
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpLogin;
    p += copy_to_pascal(p, server->using_version->av_name) +1;
    p += copy_to_pascal(p, ua_name) +1;
    memcpy(p, userauthinfo, userauthinfo_len);
    ret = dsi_send(server, (char *) msg, len, DSI_BLOCK_TIMEOUT,
                   afpLogin, (void *)rx);
    free(msg);
    return ret;
}


int afp_loginext(struct afp_server *server, const char *ua_name,
                 const char *username,
                 char *userauthinfo, unsigned int userauthinfo_len,
                 struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t flags;
    }  __attribute__((__packed__)) * request;
    unsigned int user_len = (unsigned int)strnlen(username, AFP_MAX_USERNAME_LEN);
    /* Compute offset from start of AFP payload to end of Pathname,
     * to determine if a pad byte is needed for even alignment. */
    unsigned int ver_pascal_len = (unsigned int)strnlen(
                                      server->using_version->av_name, UINT8_MAX) + 1;
    unsigned int uam_pascal_len = (unsigned int)strnlen(ua_name, UINT8_MAX) + 1;
    unsigned int pre_authinfo_len =
        1 + 1 + 2            /* command + pad + flags */
        + ver_pascal_len      /* Version (Pascal string) */
        + uam_pascal_len      /* UAM (Pascal string) */
        + 1 + 2 + user_len   /* Username: type(1) + len(2) + data */
        + 1 + 2;             /* Pathname: type(1) + len(2), empty */
    unsigned int path_pad = (pre_authinfo_len & 1) ? 1 : 0;
    unsigned int len =
        sizeof(*request)
        + ver_pascal_len
        + uam_pascal_len
        + 1 + 2 + user_len
        + 1 + 2
        + path_pad
        + userauthinfo_len;
    msg = malloc(len);

    if (!msg) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "afp_loginext: Failed to allocate message buffer");
        return -1;
    }

    memset(msg, 0, len);
    request = (void *)msg;
    p = msg + sizeof(*request);
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afp_LoginExt;
    request->pad = 0;
    request->flags = 0;
    p += copy_to_pascal(p, server->using_version->av_name) + 1;
    p += copy_to_pascal(p, ua_name) + 1;
    /* Username: Type-3 UTF-8 encoding */
    *p++ = 3; /* type = UTF-8 */
    *(uint16_t *)p = htons((uint16_t)user_len);
    p += 2;
    memcpy(p, username, user_len);
    p += user_len;
    /* Pathname: Type-3, empty */
    *p++ = 3;
    *(uint16_t *)p = 0;
    p += 2;

    /* Pad to even boundary if needed */
    if (path_pad) {
        *p++ = 0;
    }

    /* UserAuthInfo */
    memcpy(p, userauthinfo, userauthinfo_len);
    ret = dsi_send(server, msg, len, DSI_BLOCK_TIMEOUT,
                   afp_LoginExt, (void *)rx);
    free(msg);
    return ret;
}


int afp_logincont(struct afp_server *server, unsigned short id,
                  char *userauthinfo, unsigned int userauthinfo_len,
                  struct afp_rx_buffer *rx)
{
    char *msg;
    char *p;
    int ret;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t id;
    } __attribute__((__packed__)) * request;
    unsigned int len =
        sizeof(*request) /* DSI header */
        + userauthinfo_len;
    msg = malloc(len);

    if (msg == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "afp_logincont: Failed to allocate message buffer");
        return -1;
    }

    memset(msg, 0, len);
    request = (void *)msg;
    p = msg + sizeof(*request);
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->header, &hdr, sizeof(struct dsi_header));
    request->command = afpLoginCont;
    request->id = htons(id);
    memcpy(p, userauthinfo, userauthinfo_len);
    ret = dsi_send(server, (char *)msg, len, DSI_LOGIN_TIMEOUT,
                   afpLoginCont, (void *)rx);
    free(msg);
    return ret;
}
