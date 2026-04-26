/*
 *  proto_attr.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *
 */

#include <string.h>
#include <stdlib.h>
#include "dsi.h"
#include "afp.h"
#include "utils.h"
#include "afp_protocol.h"
#include "dsi_protocol.h"

/* RJVB 20140707: someone forgot one ought to check for volume before doing volume->server ...
 * and every time! */

int afp_listextattr(struct afp_volume * volume,
                    unsigned int dirid, unsigned short bitmap,
                    char *pathname, struct afp_extattr_info * info)
{
    int ret = -1;

    if (volume) {
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t volid ;
            uint32_t dirid ;
            uint16_t bitmap;
            uint16_t reqcount;
            uint32_t startindex;
            uint32_t maxreplysize;
        } __attribute__((__packed__)) *request_packet;
        struct afp_server * server = volume->server;
        unsigned int len = sizeof(*request_packet) + sizeof_path_header(
                               server) + strlen(pathname);
        char *pathptr;
        char *msg = malloc(len);

        if (!msg) {
            log_for_client(NULL, AFPFSD, LOG_WARNING, "Out of memory in afp_listextattr");
            return -1;
        };

        pathptr = msg + (sizeof(*request_packet));

        request_packet = (void *) msg;

        struct dsi_header hdr;

        dsi_setup_header(server, &hdr, DSI_DSICommand);
        memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
        request_packet->command = afpListExtAttrs;
        request_packet->pad = 0;
        request_packet->volid = htons(volume->volid);
        request_packet->dirid = htonl(dirid);
        request_packet->reqcount = 0;
        request_packet->startindex = 0;
        request_packet->bitmap = htons(bitmap);
        request_packet->maxreplysize = htonl(info->maxsize);
        copy_path(server, pathptr, pathname, strlen(pathname));
        unixpath_to_afppath(server, pathptr);
        ret = dsi_send(server, (char *) request_packet, len,
                       server->dsi_default_timeout,
                       afpListExtAttrs, (void *) info);
        free(msg);
    }

    return ret;
}

int afp_listextattrs_reply(__attribute__((unused)) struct afp_server * server,
                           char *buf,
                           __attribute__((unused)) unsigned int size, void * x)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t reserved ;
        uint32_t datalength ;
        char data[] ;
    } __attribute__((__packed__)) * reply = (void *) buf;
    struct afp_extattr_info * i = x;

    /* Check for error before parsing reply data */
    if (reply->header.return_code.error_code != 0) {
        return 0;  /* DSI extracts error_code from header automatically */
    }

    unsigned int len = min(i->maxsize, ntohl(reply->datalength));
    i->size = len;

    if (len > 0) {
        memcpy(i->data, reply->data, len);
    }

    return 0;
}

int afp_getextattr_reply(__attribute__((unused)) struct afp_server * server,
                         char *buf,
                         __attribute__((unused)) unsigned int size, void * x)
{
    const struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint32_t datalength;
        char data[];
    } __attribute__((__packed__)) * reply = (void *) buf;
    struct afp_extattr_info * i = x;

    /* Check for error before parsing reply data */
    if (reply->header.return_code.error_code != 0) {
        return 0;  /* DSI extracts error_code from header automatically */
    }

    if (i) {
        unsigned int len = min(i->maxsize, ntohl(reply->datalength));
        i->size = len;

        if (len > 0) {
            memcpy(&i->data, reply->data, len);
        }
    }

    return 0;
}


int afp_getextattr(struct afp_volume * volume, unsigned int dirid,
                   unsigned short bitmap, unsigned int replysize,
                   const char *pathname, unsigned short namelen, const char *name,
                   struct afp_extattr_info * i)
{
    int ret = -1;

    if (volume) {
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t volid ;
            uint32_t dirid ;
            uint16_t bitmap ;
            uint64_t offset ;
            uint64_t reqcount;
            uint32_t maxreplysize;
        } __attribute__((__packed__)) *request_packet;
        struct afp_server * server = volume->server;
        unsigned int pathlen = sizeof_path_header(server) + strlen(pathname);
        /* Calculate padding: 1 if pathlen is odd, 0 if even */
        unsigned int padding = pathlen & 1;
        unsigned int len = sizeof(*request_packet) + pathlen + padding + 2 + namelen;
        char *p, *p2;
        char *msg = malloc(len);

        if (!msg) {
            log_for_client(NULL, AFPFSD, LOG_WARNING, "Out of memory in afp_getextattr");
            return -1;
        };

        p = msg + sizeof(*request_packet);

        request_packet = (void *) msg;

        struct dsi_header hdr;

        dsi_setup_header(server, &hdr, DSI_DSICommand);
        memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
        request_packet->command = afpGetExtAttr;
        request_packet->pad = 0;
        request_packet->volid = htons(volume->volid);
        request_packet->dirid = htonl(dirid);
        request_packet->bitmap = htons(bitmap);
        request_packet->offset = hton64(0);
        request_packet->reqcount = hton64(replysize);
        request_packet->maxreplysize = htonl(replysize);
        /* Copy path */
        copy_path(server, p, pathname, strlen(pathname));
        unixpath_to_afppath(server, p);
        p2 = p + pathlen;

        /* Pad to even boundary if needed (AFP protocol requirement) */
        if ((unsigned long)p2 & 1) {
            p2++;
        }

        /* EA name: length-prefixed (2 bytes length + name) */
        *((uint16_t *)p2) = htons(namelen);
        memcpy(p2 + 2, name, namelen);
        ret = dsi_send(server, (char *) request_packet, len,
                       server->dsi_default_timeout,
                       afpGetExtAttr, (void *) i);
        free(msg);
    }

    return ret;
}

int afp_setextattr(struct afp_volume * volume, unsigned int dirid,
                   unsigned short bitmap, uint64_t offset, const char *pathname,
                   unsigned short namelen, const char *name,
                   unsigned int attribdatalen, const char *attribdata)
{
    int ret = -1;

    if (volume) {
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t volid ;
            uint32_t dirid ;
            uint16_t bitmap ;
            uint64_t offset ;
        } __attribute__((__packed__)) *request_packet;
        struct afp_server * server = volume->server;
        unsigned int pathlen = sizeof_path_header(server) + strlen(pathname);
        /* Calculate padding: 1 if pathlen is odd, 0 if even */
        unsigned int padding = pathlen & 1;
        unsigned int len = sizeof(*request_packet) + pathlen + padding + 2 + namelen +
                           4 + attribdatalen;
        char *p, *p2;
        char *msg = malloc(len);

        if (!msg) {
            log_for_client(NULL, AFPFSD, LOG_WARNING, "Out of memory in afp_setextattr");
            return -1;
        };

        p = msg + sizeof(*request_packet);

        request_packet = (void *) msg;

        struct dsi_header hdr;

        dsi_setup_header(server, &hdr, DSI_DSICommand);
        memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
        request_packet->command = afpSetExtAttr;
        request_packet->pad = 0;
        request_packet->volid = htons(volume->volid);
        request_packet->dirid = htonl(dirid);
        request_packet->bitmap = htons(bitmap);
        request_packet->offset = hton64(offset);
        /* Copy path */
        copy_path(server, p, pathname, strlen(pathname));
        unixpath_to_afppath(server, p);
        p2 = p + pathlen;

        /* Pad to even boundary if needed (AFP protocol requirement) */
        if ((unsigned long)p2 & 1) {
            p2++;
        }

        /* EA name: length-prefixed (2 bytes length + name) */
        *((uint16_t *)p2) = htons(namelen);
        memcpy(p2 + 2, name, namelen);
        p2 += 2 + namelen;
        /* EA data size (4 bytes) */
        *((uint32_t *)p2) = htonl(attribdatalen);
        p2 += 4;

        /* EA data payload */
        if (attribdatalen > 0 && attribdata) {
            memcpy(p2, attribdata, attribdatalen);
        }

        ret = dsi_send(server, (char *) request_packet, len,
                       server->dsi_default_timeout,
                       afpSetExtAttr, NULL);
        free(msg);
    }

    return ret;
}

int afp_removeextattr(struct afp_volume * volume, unsigned int dirid,
                      unsigned short bitmap, const char *pathname,
                      unsigned short namelen, const char *name)
{
    int ret = -1;

    if (volume) {
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t volid;
            uint32_t dirid;
            uint16_t bitmap;
        } __attribute__((__packed__)) *request_packet;
        struct afp_server * server = volume->server;
        unsigned int pathlen = sizeof_path_header(server) + strlen(pathname);
        /* Calculate padding: 1 if pathlen is odd, 0 if even */
        unsigned int padding = pathlen & 1;
        unsigned int len = sizeof(*request_packet) + pathlen + padding + 2 + namelen;
        char *p, *p2;
        char *msg = malloc(len);

        if (!msg) {
            log_for_client(NULL, AFPFSD, LOG_WARNING, "Out of memory in afp_removeextattr");
            return -1;
        }

        p = msg + sizeof(*request_packet);
        request_packet = (void *) msg;
        struct dsi_header hdr;
        dsi_setup_header(server, &hdr, DSI_DSICommand);
        memcpy(&request_packet->dsi_header, &hdr, sizeof(struct dsi_header));
        request_packet->command = afpRemoveExtAttr;
        request_packet->pad = 0;
        request_packet->volid = htons(volume->volid);
        request_packet->dirid = htonl(dirid);
        request_packet->bitmap = htons(bitmap);
        /* Copy path */
        copy_path(server, p, pathname, strlen(pathname));
        unixpath_to_afppath(server, p);
        p2 = p + pathlen;

        /* Pad to even boundary if needed (AFP protocol requirement) */
        if ((unsigned long)p2 & 1) {
            p2++;
        }

        /* EA name: length-prefixed (2 bytes length + name) */
        *((uint16_t *)p2) = htons(namelen);
        memcpy(p2 + 2, name, namelen);
        ret = dsi_send(server, (char *) request_packet, len,
                       server->dsi_default_timeout,
                       afpRemoveExtAttr, NULL);
        free(msg);
    }

    return ret;
}
