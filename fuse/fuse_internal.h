#ifndef __FUSE_INTERNAL_H_
#define __FUSE_INTERNAL_H_

#define AFP_CLIENT_INCOMING_BUF 2048+256

struct fuse_client {
    char incoming_string[AFP_CLIENT_INCOMING_BUF];
    int incoming_size;
    char client_string[MAX_CLIENT_RESPONSE];
    int fd;
    struct fuse_client *next;
};

#endif
