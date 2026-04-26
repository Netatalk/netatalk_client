/*
 *  proto_fork.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>
 *
*/

#include <stdlib.h>
#include <string.h>

#include "dsi.h"
#include "afp.h"
#include "utils.h"
#include "dsi_protocol.h"
#include "afp_protocol.h"

int afp_setforkparms(struct afp_volume * volume,
                     unsigned short forkid, unsigned short bitmap, unsigned long len)
{
    /* The implementation here deserves some explanation.
     * If the function is called with an extended size, use 64 bits.
     * otherwise, 32.
     */
    struct dsi_header hdr;
    dsi_setup_header(volume->server, &hdr, DSI_DSICommand);

    if (bitmap & (kFPExtDataForkLenBit | kFPExtRsrcForkLenBit)) {
        /* Extended 64-bit version */
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t forkid;
            uint16_t bitmap;
            uint16_t padding;
            uint64_t newlen64;
        }  __attribute__((__packed__)) request64;
        memcpy(&request64.dsi_header, &hdr, sizeof(struct dsi_header));
        request64.command = afpSetForkParms;
        request64.pad = 0;
        request64.forkid = htons(forkid);
        request64.bitmap = htons(bitmap);
        request64.padding = 0;
        request64.newlen64 = hton64(len);
        return dsi_send(volume->server, (char *) &request64,
                        sizeof(request64), volume->server->dsi_default_timeout, afpSetForkParms, NULL);
    } else {
        /* Legacy 32-bit version */
        struct {
            struct dsi_header dsi_header __attribute__((__packed__));
            uint8_t command;
            uint8_t pad;
            uint16_t forkid;
            uint16_t bitmap;
            uint32_t newlen;
        }  __attribute__((__packed__)) request32;
        memcpy(&request32.dsi_header, &hdr, sizeof(struct dsi_header));
        request32.command = afpSetForkParms;
        request32.pad = 0;
        request32.forkid = htons(forkid);
        request32.bitmap = htons(bitmap);
        request32.newlen = htonl(len);
        return dsi_send(volume->server, (char *) &request32,
                        sizeof(request32), volume->server->dsi_default_timeout, afpSetForkParms, NULL);
    }
}

int afp_closefork(struct afp_volume * volume,
                  unsigned short forkid)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t forkid;
    }  __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = afpCloseFork;
    request_packet.pad = 0;
    request_packet.forkid = htons(forkid);
    return dsi_send(volume->server, (char *) &request_packet,
                    sizeof(request_packet), volume->server->dsi_default_timeout, afpFlushFork,
                    NULL);
}


int afp_flushfork(struct afp_volume * volume,
                  unsigned short forkid)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t pad;
        uint16_t forkid;
    }  __attribute__((__packed__)) request_packet;
    struct dsi_header hdr;
    dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request_packet.dsi_header, &hdr, sizeof(struct dsi_header));
    request_packet.command = afpFlushFork;
    request_packet.pad = 0;
    request_packet.forkid = htons(forkid);
    return dsi_send(volume->server, (char *) &request_packet,
                    sizeof(request_packet), volume->server->dsi_default_timeout, afpFlushFork,
                    NULL);
}

int afp_openfork_reply(__attribute__((unused)) struct afp_server *server,
                       char *buf, unsigned int size, void *x)
{
    struct afp_file_info *fp = (struct afp_file_info *)x;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint16_t forkid;
    } __attribute__((__packed__)) reply;
    /* Copy the buffer into our properly structured reply */
    memcpy(&reply, buf, sizeof(reply));

    if ((reply.header.return_code.error_code == kFPNoErr) ||
            (reply.header.return_code.error_code == kFPDenyConflict)) {
        if (size < sizeof(reply)) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "openfork response is too short (%u bytes)", size);
            return -1;
        }

        fp->forkid = ntohs(reply.forkid);
    }

    /* We end up ignoring the reply bitmap */
    return 0;
}

int afp_openfork(struct afp_volume * volume,
                 unsigned char forktype,
                 unsigned int dirid,
                 unsigned short accessmode,
                 char *filename,
                 struct afp_file_info * fp)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t forktype;
        uint16_t volid;
        uint32_t dirid ;
        uint16_t bitmap ;
        uint16_t accessmode;
    }  __attribute__((__packed__)) * afp_openfork_request;
    char *msg;
    char *pathptr;
    struct afp_server * server = volume->server;
    unsigned int len = sizeof(*afp_openfork_request) +
                       sizeof_path_header(server) + strlen(filename);
    int ret;

    if ((msg = malloc(len)) == NULL) {
        return -1;
    }

    pathptr = msg + sizeof(*afp_openfork_request);
    afp_openfork_request = (void *) msg;
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&afp_openfork_request->dsi_header, &hdr, sizeof(struct dsi_header));
    afp_openfork_request->command = afpOpenFork;
    afp_openfork_request->forktype = forktype ? AFP_FORKTYPE_RESOURCE :
                                     AFP_FORKTYPE_DATA;
    afp_openfork_request->bitmap = 0;
    afp_openfork_request->volid = htons(volume->volid);
    afp_openfork_request->dirid = htonl(dirid);
    afp_openfork_request->accessmode = htons(accessmode);
    copy_path(server, pathptr, filename, strlen(filename));
    unixpath_to_afppath(server, pathptr);
    ret = dsi_send(server, (char *) msg, len, server->dsi_default_timeout,
                   afpOpenFork, (void *) fp);
    free(msg);
    return ret;
}

int afp_byterangelock(struct afp_volume * volume,
                      unsigned char flag,
                      unsigned short forkid,
                      uint32_t offset,
                      uint32_t len, uint32_t *generated_offset)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t flag;
        uint16_t forkid;
        uint32_t offset;
        uint32_t len;
    }  __attribute__((__packed__)) request;
    int rc;
    struct dsi_header hdr;
    dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpByteRangeLock;
    request.flag = flag;
    request.forkid = htons(forkid);
    request.offset = htonl(offset);
    request.len = htonl(len);
    rc = dsi_send(volume->server, (char *) &request,
                  sizeof(request), volume->server->dsi_default_timeout,
                  afpByteRangeLock, (void *) generated_offset);
    return rc;
}

int afp_byterangelock_reply(__attribute__((unused)) struct afp_server *server,
                            char *buf, unsigned int size, void *x)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint64_t offset;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    uint32_t *offset = x;
    *offset = 0;

    if (size >= sizeof(*reply)) {
        *offset = ntohl(reply->offset);
    }

    return reply->header.return_code.error_code;
}

int afp_byterangelockext(struct afp_volume * volume,
                         unsigned char flag,
                         unsigned short forkid,
                         uint64_t offset,
                         uint64_t len, uint64_t *generated_offset)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t flag;
        uint16_t forkid;
        uint64_t offset;
        uint64_t len;
    }  __attribute__((__packed__)) request;
    int rc;
    struct dsi_header hdr;
    dsi_setup_header(volume->server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpByteRangeLockExt;
    request.flag = flag;
    request.forkid = htons(forkid);
    request.offset = hton64(offset);
    request.len = hton64(len);
    rc = dsi_send(volume->server, (char *) &request,
                  sizeof(request), volume->server->dsi_default_timeout,
                  afpByteRangeLockExt, (void *) generated_offset);
    return rc;
}

int afp_byterangelockext_reply(__attribute__((unused)) struct afp_server
                               *server, char *buf, unsigned int size, void *x)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint64_t offset;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    uint64_t *offset = x;
    *offset = 0;

    if (size >= sizeof(*reply)) {
        *offset = ntoh64(reply->offset);
    }

    return reply->header.return_code.error_code;
}
