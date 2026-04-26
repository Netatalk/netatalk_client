#ifndef _AFP_SERVER_H_
#define _AFP_SERVER_H_

#include <limits.h>
#include "afp_ipc.h"
#include "afpsl.h"

struct afp_server_response_header {
    char result;
    unsigned int len;
};

struct afp_server_request_header {
    char command;
    unsigned int len;
    unsigned int close;
};

struct afp_server_attach_request {
    struct afp_server_request_header header;
    struct afp_url url;
    unsigned int volume_options;
};

struct afp_server_attach_response {
    struct afp_server_response_header header;
    volumeid_t volumeid;
};

struct afp_server_detach_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
};

struct afp_server_detach_response {
    struct afp_server_response_header header;
    char detach_message[1024];
};

struct afp_server_connect_request {
    struct afp_server_request_header header;
    struct afp_url url;
    unsigned int uam_mask;
    int dsi_timeout;
};

struct afp_server_connect_response {
    struct afp_server_response_header header;
    serverid_t serverid;
    char loginmesg[AFP_LOGINMESG_LEN];
    int connect_error;
};

struct afp_server_disconnect_request {
    struct afp_server_request_header header;
    serverid_t serverid;
};

struct afp_server_disconnect_response {
    struct afp_server_response_header header;
};

struct afp_server_getvolid_request {
    struct afp_server_request_header header;
    struct afp_url url;
};

struct afp_server_getvolid_response {
    struct afp_server_response_header header;
    volumeid_t volumeid;
};

struct afp_server_readdir_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    int start;
    int count;
};

struct afp_server_readdir_response {
    struct afp_server_response_header header;
    unsigned int numfiles;
    char eod;
};

struct afp_server_getvols_request {
    struct afp_server_request_header header;
    struct afp_url url;
    int start;
    int count;
};

struct afp_server_getvols_response {
    struct afp_server_response_header header;
    unsigned int num;
    char endlist;
};

struct afp_server_stat_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afp_server_stat_response {
    struct afp_server_response_header header;
    struct stat stat;
};

struct afp_server_open_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    int mode;
};

struct afp_server_open_response {
    struct afp_server_response_header header;
    unsigned int fileid;
};

struct afp_server_read_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    unsigned int fileid;
    unsigned long long start;
    unsigned int length;
    unsigned int resource;
};

struct afp_server_read_response {
    struct afp_server_response_header header;
    unsigned int received;
    unsigned int eof;
};

struct afp_server_write_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    unsigned int fileid;
    unsigned long long offset;
    unsigned int size;
    unsigned int resource;
};

struct afp_server_write_response {
    struct afp_server_response_header header;
    unsigned int written;
};

struct afp_server_close_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    unsigned int fileid;
};

struct afp_server_close_response {
    struct afp_server_response_header header;
};

struct afp_server_creat_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afp_server_creat_response {
    struct afp_server_response_header header;
};

struct afp_server_chmod_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afp_server_chmod_response {
    struct afp_server_response_header header;
};

struct afp_server_rename_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path_from[AFP_MAX_PATH];
    char path_to[AFP_MAX_PATH];
};

struct afp_server_rename_response {
    struct afp_server_response_header header;
};

struct afp_server_unlink_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afp_server_unlink_response {
    struct afp_server_response_header header;
};

struct afp_server_truncate_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    unsigned long long offset;
};

struct afp_server_truncate_response {
    struct afp_server_response_header header;
};

struct afp_server_mkdir_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    mode_t mode;
};

struct afp_server_mkdir_response {
    struct afp_server_response_header header;
};

struct afp_server_rmdir_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afp_server_rmdir_response {
    struct afp_server_response_header header;
};

struct afp_server_statfs_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
};

struct afp_server_statfs_response {
    struct afp_server_response_header header;
    struct statvfs stat;
};

struct afp_server_utime_request {
    struct afp_server_request_header header;
    volumeid_t volumeid;
    char path[AFP_MAX_PATH];
    struct utimbuf times;
};

struct afp_server_utime_response {
    struct afp_server_response_header header;
};

struct afp_server_serverinfo_request {
    struct afp_server_request_header header;
    struct afp_url url;
};

struct afp_server_serverinfo_response {
    struct afp_server_response_header header;
    struct afp_server_basic server_basic;
};

struct afp_server_status_request {
    struct afp_server_request_header header;
    char volumename[AFP_VOLUME_NAME_UTF8_LEN];
    char servername[AFP_SERVER_NAME_LEN];
    char mountpoint[AFP_MOUNTPOINT_LEN];
};

struct afp_server_status_response {
    struct afp_server_response_header header;
};

struct afp_server_exit_request {
    struct afp_server_request_header header;
};

struct afp_server_changepw_request {
    struct afp_server_request_header header;
    struct afp_url url;
    char oldpasswd[AFP_MAX_PASSWORD_LEN];
    char newpasswd[AFP_MAX_PASSWORD_LEN];
};

struct afp_server_changepw_response {
    struct afp_server_response_header header;
    int afp_error;
};

#endif
