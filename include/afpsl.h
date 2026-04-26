#ifndef __AFPSL_H_
#define __AFPSL_H_

#include <utime.h>

#include "afp.h"
#include "errno.h"

/* Socket path for stateless daemon */
#define SERVER_SL_SOCKET_PATH "/tmp/afp_sl"

/* Basic file information structure for stateless API
 * This is the wire protocol format sent between afpsld and clients.
 * Contains essential file metadata without the full afp_file_info details.
 */
struct afp_file_info_basic {
    char name[AFP_MAX_PATH];
    unsigned int creation_date;
    unsigned int modification_date;
    struct afp_unixprivs unixprivs;
    unsigned long long size;
};

struct afpfsd_connect {
    int fd;
    unsigned int len;
    char data[MAX_CLIENT_RESPONSE + 200];
    void (*print)(const char * text);
    char *shmem;
};

struct afp_volume_summary {
    char volume_name_printable[AFP_VOLUME_NAME_UTF8_LEN];
    char flags;
};

typedef void   *serverid_t;
typedef void   *volumeid_t;

void afp_sl_conn_setup(void);

int afp_sl_exit(void);
int afp_sl_status(const char * volumename, const char * servername,
                  char *text, unsigned int *remaining);
int afp_sl_connect(struct afp_url * url, unsigned int uam_mask,
                   serverid_t *id, char *loginmesg, int *error, int dsi_timeout);
int afp_sl_disconnect(serverid_t *id);
int afp_sl_getvolid(struct afp_url * url, volumeid_t *volid);
int afp_sl_attach(struct afp_url * url, unsigned int volume_options,
                  volumeid_t *volumeid);
int afp_sl_detach(volumeid_t * volumeid,
                  struct afp_url * url);
int afp_sl_readdir(volumeid_t * volid, const char * path, struct afp_url * url,
                   int start, int count, unsigned int *numfiles,
                   struct afp_file_info_basic **fpb,
                   int *eod);
int afp_sl_getvols(struct afp_url * url, unsigned int start,
                   unsigned int count, unsigned int *numvols,
                   struct afp_volume_summary * vols);
int afp_sl_stat(volumeid_t * volid, const char * path,
                struct afp_url * url, struct stat * stat);
int afp_sl_open(volumeid_t * volid, const char * path,
                struct afp_url * url, unsigned int *fileid,
                unsigned int mode);
int afp_sl_read(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                unsigned long long start,
                unsigned int length, unsigned int *received,
                unsigned int *eof, char *data);
int afp_sl_write(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                 unsigned long long offset, unsigned int size,
                 unsigned int *written, const char *data);
int afp_sl_creat(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_chmod(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_rename(volumeid_t * volid, const char * path_from,
                  const char *path_to, struct afp_url * url);
int afp_sl_unlink(volumeid_t * volid, const char * path,
                  struct afp_url * url);
int afp_sl_truncate(volumeid_t * volid, const char * path,
                    struct afp_url * url, unsigned long long offset);
int afp_sl_utime(volumeid_t * volid, const char * path,
                 struct afp_url * url, struct utimbuf * times);
int afp_sl_mkdir(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_rmdir(volumeid_t * volid, const char * path,
                 struct afp_url * url);
int afp_sl_statfs(volumeid_t * volid, const char * path,
                  struct afp_url * url, struct statvfs * stat);
int afp_sl_close(volumeid_t * volid, unsigned int fileid);
int afp_sl_serverinfo(struct afp_url * url, struct afp_server_basic * basic);
int afp_sl_changepw(struct afp_url * url,
                    const char *old_password,
                    const char *new_password);
int afp_sl_setup(void);

#endif
