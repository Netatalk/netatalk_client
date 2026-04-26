#ifndef _FUSE_IPC_H_
#define _FUSE_IPC_H_

#include <limits.h>
#include "afp.h"
#include "afp_ipc.h"

#define SERVER_FUSE_SOCKET_PATH "/tmp/afp_server"

/* Internal command for FUSE manager daemon */
#define AFP_SERVER_COMMAND_SPAWN_MOUNT 100

struct afp_server_resume_request {
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afp_server_suspend_request {
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afp_server_unmount_request {
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afp_server_mount_request {
    struct afp_url url;
    unsigned int uam_mask;
    char mountpoint[AFP_MOUNTPOINT_LEN];
    unsigned int volume_options;
    unsigned int map;
    int changeuid;
    char fuse_options[256];
    int dsi_timeout;
};

struct afp_server_status_request {
    char volumename[AFP_VOLUME_NAME_UTF8_LEN];
    char servername[AFP_SERVER_NAME_UTF8_LEN];
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afp_server_spawn_mount_request {
    char mountpoint[AFP_MOUNTPOINT_LEN];
    char socket_id[PATH_MAX];
    char volumename[AFP_VOLUME_NAME_UTF8_LEN];
};

struct afp_server_response {
    char result;
    unsigned int len;
};

#endif
