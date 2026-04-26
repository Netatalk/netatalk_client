/*
 *  proto_map.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *
 */

#include <stdlib.h>
#include <string.h>

#include "dsi.h"
#include "afp.h"
#include "utils.h"
#include "dsi_protocol.h"
#include "afp_protocol.h"

/* This is used to pass the return values back from afp_getuserinfo_reply() */
struct uidgid {
    unsigned int uid;
    unsigned int gid;
};

int afp_getuserinfo(struct afp_server * server, int thisuser,
                    unsigned int userid, unsigned short bitmap,
                    unsigned int *newuid, unsigned int *newgid)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t thisuser;
        uint32_t userid __attribute__((__packed__));
        uint16_t bitmap __attribute__((__packed__));
    }  __attribute__((__packed__)) request;
    struct uidgid uidgid;
    int ret;
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpGetUserInfo;
    request.thisuser = (thisuser != 0);
    request.userid = htonl(userid);
    request.bitmap = htons(bitmap);
    ret = dsi_send(server, (char *) &request, sizeof(request),
                   server->dsi_default_timeout, afpGetUserInfo, (void *) &uidgid);

    if (bitmap & kFPGetUserInfo_USER_ID) {
        *newuid = uidgid.uid;
    }

    if (bitmap & kFPGetUserInfo_PRI_GROUPID) {
        *newgid = uidgid.gid;
    }

    return ret;
}


int afp_getuserinfo_reply(__attribute__((unused)) struct afp_server *server,
                          char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint32_t id1;
        uint32_t id2;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    struct uidgid *uidgid = other;
    unsigned short bitmap;
    uidgid->uid = 0;
    uidgid->gid = 0;

    /* RJVB: there should be at least 2 bytes after the dsi_header */
    if (size < (sizeof(unsigned short) + 2 * sizeof(uint32_t) + sizeof(
                    struct dsi_header))) {
        return -1;
    }

    bitmap = ntohs(reply->bitmap);

    if (reply->header.return_code.error_code != kFPNoErr) {
        return -1;
    }

    if (bitmap & kFPGetUserInfo_USER_ID) {
        uidgid->uid = ntohl(reply->id1);

        if (bitmap & kFPGetUserInfo_PRI_GROUPID) {
            uidgid->gid = ntohl(reply->id2);
        }

        goto out;
    }

    if (bitmap & kFPGetUserInfo_PRI_GROUPID) {
        uidgid->gid = ntohl(reply->id1);
    }

out:
    return 0;
}


int afp_mapid(struct afp_server * server, unsigned char subfunction,
              unsigned int id, char *name)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t subfunction;
        uint32_t id;
    }  __attribute__((__packed__)) request;
    int ret;
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request.dsi_header, &hdr, sizeof(struct dsi_header));
    request.command = afpMapID;
    request.subfunction = subfunction;
    request.id = htonl(id);
    ret = dsi_send(server, (char *) &request, sizeof(request),
                   server->dsi_default_timeout, afpMapID, (void *) name);
    return ret;
}

int afp_mapid_reply(__attribute__((unused)) struct afp_server *server,
                    char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        char *name ;
    }  __attribute__((__packed__)) * reply = (void *) buf;
    static char name[AFP_MAX_PATH];
    char *name_ptr = other;

    /* RJVB: there should be at least 2 bytes after the dsi_header */
    if (size < (sizeof(unsigned short) + sizeof(struct dsi_header))) {
        return -1;
    }

    if (reply->header.return_code.error_code != kFPNoErr) {
        return -1;
    }

    memset(name, 0, AFP_MAX_PATH);
    copy_from_pascal_two(name, reply->name, sizeof(name));

    if (name_ptr != NULL) {
        memcpy(name_ptr, name, AFP_MAX_PATH);
    }

    return 0;
}

int afp_mapname(struct afp_server * server, unsigned char subfunction,
                char *name, unsigned int *id)
{
    struct {
        struct dsi_header dsi_header __attribute__((__packed__));
        uint8_t command;
        uint8_t subfunction;
    }  __attribute__((__packed__)) * request;
    unsigned int len = sizeof(*request) +
                       1 + strlen(name);
    char *msg, *nameptr;
    int ret;

    if ((msg = malloc(len)) == NULL) {
        return -1;
    }

    memset(msg, 0x4b, len);
    nameptr = msg + sizeof(*request);
    request = (void *) msg;
    copy_to_pascal(nameptr, name);
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSICommand);
    memcpy(&request->dsi_header, &hdr, sizeof(struct dsi_header));
    request->command = afpMapName;
    request->subfunction = subfunction;
    ret = dsi_send(server, (char *) request, len,
                   server->dsi_default_timeout, afpMapName, (void *) id);
    free(msg);
    return ret;
}


int afp_mapname_reply(__attribute__((unused)) struct afp_server *server,
                      char *buf, unsigned int size, void *other)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint32_t id ;
    }  __attribute__((__packed__))* reply = (void *) buf;
    unsigned int *id = (void *) other;

    /* RJVB: there should be at least 2 bytes after the dsi_header */
    if (size < (sizeof(unsigned short) + sizeof(struct dsi_header))) {
        return -1;
    }

    *id = ntohl(reply->id);
    return 0;
}

