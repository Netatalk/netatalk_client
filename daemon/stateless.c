/*
 *  stateless.c - Stateless AFP client library
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <sys/un.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/shm.h>
#include <time.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp.h"
#include "afp_server.h"
#include "uams_def.h"
#include "map_def.h"
#include "libafpclient.h"
#include "afpsl.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define default_uam "Cleartxt Passwrd"
#define AFPSLD_FILENAME "afpsld"

static struct afpfsd_connect connection;

static int start_afpsld(void)
{
    char *argv[2];
    argv[0] = AFPSLD_FILENAME;
    argv[1] = NULL;

    if (fork() == 0) {
        char filename[PATH_MAX];
        snprintf(filename, PATH_MAX, "%s/%s", BINDIR, AFPSLD_FILENAME);

        if (access(filename, X_OK)) {
            fprintf(stderr, "Could not find afpsld daemon (%s)\n",
                    filename);
            _exit(1);
        }

        execvp(filename, argv);
        /* exec failed */
        perror("execvp afpsld");
        _exit(1);
    }

    return 0;
}

int daemon_connect(unsigned int daemon_uid)
{
    int sock;
    struct sockaddr_un servaddr;
    int retries = 50;
    int ret;

    /* Check if we already have a valid connection */
    if (connection.fd > 0) {
        /* Reuse existing connection */
        return 0;
    }

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("Could not create socket\n");
        return -1;
    }

#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;
    snprintf(servaddr.sun_path, sizeof(servaddr.sun_path), "%s-%d",
             SERVER_SL_SOCKET_PATH, daemon_uid);
    /* Try to connect first */
    ret = connect(sock, (struct sockaddr*) &servaddr, sizeof(servaddr));

    if (ret >= 0) {
        goto done;
    }

    if (start_afpsld() != 0) {
        printf("Error in starting up afpsld daemon\n");
        close(sock);
        return -1;
    }

    /* Wait for daemon to start */
    while (retries > 0) {
        struct timespec ts = {0, 100000000}; /* 100ms */
        nanosleep(&ts, NULL);
        ret = connect(sock, (struct sockaddr*) &servaddr, sizeof(servaddr));

        if (ret >= 0) {
            goto done;
        }

        retries--;
    }

    perror("Could not connect to afpsld daemon");
    close(sock);
    return -1;
done:
    connection.fd = sock;
    return 0;
}

/* read_answer()
 *
 * Reads the answer from afpsld.
 * Returns:
 * -1: timeout or select error
 * >0: afpfsd header error
 */

static int read_answer(void)
{
    unsigned int expected_len = 0, packetlen;
    struct timeval tv;
    fd_set rds, ords;
    int ret;
    memset(connection.data, 0, MAX_CLIENT_RESPONSE);
    connection.len = 0;
    FD_ZERO(&rds);
    FD_SET(connection.fd, &rds);

    while (1) {
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ords = rds;
        ret = select(connection.fd + 1, &ords, NULL, NULL, &tv);

        if (ret == 0) {
            printf("No response from server, timed out.\n");
            close(connection.fd);
            connection.fd = 0;
            return -1;
        }

        if (FD_ISSET(connection.fd, &ords)) {
            packetlen = read(connection.fd,
                             connection.data + connection.len,
                             MAX_CLIENT_RESPONSE - connection.len);

            if (packetlen <= 0) {
                printf("Dropped connection\n");
                close(connection.fd);
                connection.fd = 0;
                goto done;
            }

            if (connection.len == 0) { /* This is our first read */
                expected_len =
                    ((struct afp_server_response_header *)
                     connection.data)->len;
            }

            connection.len += packetlen;

            if (connection.len == expected_len) {
                goto done;
            }

            if (ret < 0) {
                goto error;
            }
        }
    }

done:
    return ((struct afp_server_response_header *) connection.data)->result;
error:
    return -1;
}

/* Helper to write exactly len bytes */
static int write_bytes(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    ssize_t ret;

    while (total < len) {
        ret = send(fd, buf + total, len - total, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        total += ret;
    }

    return 0;
}

/* Helper to read exactly len bytes with timeout */
static int read_bytes_with_timeout(int fd, char *buf, size_t len)
{
    size_t total = 0;
    fd_set rds;
    struct timeval tv;
    int ret;

    while (total < len) {
        FD_ZERO(&rds);
        FD_SET(fd, &rds);
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ret = select(fd + 1, &rds, NULL, NULL, &tv);

        if (ret <= 0) {
            return -1;
        }

        ssize_t r = read(fd, buf + total, len - total);

        if (r < 0 && errno == EINTR) {
            continue;
        }

        if (r <= 0) {
            return -1;
        }

        total += r;
    }

    return 0;
}

static int send_command(unsigned int len, char * data, unsigned int num)
{
    int ret = write_bytes(connection.fd, data, len);

    if (ret < 0) {
        fprintf(stderr, "send_command: write failed for command %u: %s\n",
                num, strerror(errno));
        close(connection.fd);
        connection.fd = 0;
    }

    return  ret;
}

static int send_to_daemon(char * request, int req_len, char * reply,
                          int reply_len)
{
    int ret;
    unsigned int command = 0;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    if (req_len >= (int)sizeof(struct afp_server_request_header)) {
        command = ((struct afp_server_request_header *)request)->command;
    }

    if (send_command(req_len, request, command) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_bytes_with_timeout(connection.fd, reply, reply_len);

    if (ret < 0) {
        printf("Error reading response\n");
        close(connection.fd);
        connection.fd = 0;
        return -1;
    }

    return 0;
}

static void conn_print(const char * text)
{
    printf("%s", text);
}

void afp_sl_conn_setup(void)
{
    connection.print = conn_print;
}

int afp_sl_exit(void)
{
    struct afp_server_exit_request req;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    req.header.command = AFP_SERVER_COMMAND_EXIT;
    req.header.close = 1;
    req.header.len = sizeof(req);

    if (send_command(sizeof(req), (char *) &req, AFP_SERVER_COMMAND_EXIT) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    return read_answer();
}

/* afp_sl_status()
 *
 * Returns:
 * AFP_SERVER_RESULT_DAEMON_ERROR: could not connect to afpsld
 */

int afp_sl_status(const char * volumename, const char * servername,
                  char *text, unsigned int *remaining)
{
    struct afp_server_status_request req;
    int ret;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    memset(&req, 0, sizeof(req));
    req.header.command = AFP_SERVER_COMMAND_STATUS;
    req.header.close = 0;
    req.header.len = sizeof(req);

    if (volumename) {
        snprintf(req.volumename, AFP_VOLUME_NAME_UTF8_LEN, "%s", volumename);
    }

    if (servername) {
        snprintf(req.servername, AFP_SERVER_NAME_LEN, "%s", servername);
    }

    if (send_command(sizeof(req), (char *)&req, AFP_SERVER_COMMAND_STATUS) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_answer();

    if (ret < 0) {
        return ret;
    }

    strlcpy(text, connection.data +
            sizeof(struct afp_server_status_response), *remaining);
    return ret;
}

/* afp_sl_getvolid()
 *
 * Result header returns:
 * AFP_SERVER_RESULT_DAEMON_ERROR
 * AFP_SERVER_RESULT_OKAY
 *
 */

int afp_sl_getvolid(struct afp_url * url, volumeid_t *volid)
{
    struct afp_server_getvolid_request req;
    struct afp_server_getvolid_response response;
    int ret;
    int retries = 10;
    memset(&req, 0, sizeof(req));
    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_getvolid_request);
    req.header.command = AFP_SERVER_COMMAND_GETVOLID;
    memcpy(&req.url, url, sizeof(*url));

    while (1) {
        ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                             sizeof(response));

        if (ret != 0) {
            return ret;
        }

        /* Volume exists but attach is still in progress from another client;
         * wait briefly and retry so callers don't see a spurious error. */
        if (response.header.result == AFP_SERVER_RESULT_NOTATTACHED
                && --retries > 0) {
            struct timespec ts = {0, 200000000}; /* 200ms */
            nanosleep(&ts, NULL);
            continue;
        }

        break;
    }

    if (response.header.result == AFP_SERVER_RESULT_OKAY) {
        memcpy(volid, &response.volumeid, sizeof(volumeid_t));
    }

    return response.header.result;
}

int afp_sl_stat(volumeid_t * volid, const char * path,
                struct afp_url * url, struct stat * stat)
{
    struct afp_server_stat_request request;
    struct afp_server_stat_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_stat_request);
    request.header.command = AFP_SERVER_COMMAND_STAT;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    memcpy(stat, &response.stat, sizeof(struct stat));
    return response.header.result;
}

int afp_sl_open(volumeid_t * volid, const char * path,
                struct afp_url * url, unsigned int *fileid,
                unsigned int mode)
{
    struct afp_server_open_request request;
    struct afp_server_open_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_open_request);
    request.header.command = AFP_SERVER_COMMAND_OPEN;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    *fileid = response.fileid;
    return response.header.result;
}


int afp_sl_read(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                unsigned long long start,
                unsigned int length, unsigned int *received,
                unsigned int *eof, char *data)
{
    struct afp_server_read_request request;
    struct afp_server_read_response response;
    int ret;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_read_request);
    request.header.command = AFP_SERVER_COMMAND_READ;
    memcpy(&request.volumeid, volid, sizeof(volumeid_t));
    request.fileid = fileid;
    request.start = start;
    request.length = length;
    request.resource = resource;

    if (send_command(sizeof(request), (char *)&request,
                     AFP_SERVER_COMMAND_READ) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    /* Read just the response header first */
    ret = read_bytes_with_timeout(connection.fd, (char *)&response,
                                  sizeof(response));

    if (ret < 0) {
        printf("Error reading read response header\n");
        close(connection.fd);
        connection.fd = 0;
        return AFP_SERVER_RESULT_ERROR;
    }

    if (response.header.result != AFP_SERVER_RESULT_OKAY) {
        *received = 0;
        *eof = 0;
        return response.header.result;
    }

    *received = response.received;
    *eof = response.eof;
    /* Read the data directly into the user buffer */
    ret = read_bytes_with_timeout(connection.fd, data, *received);

    if (ret < 0) {
        printf("Error reading read data payload\n");
        close(connection.fd);
        connection.fd = 0;
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_write(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                 unsigned long long offset, unsigned int size,
                 unsigned int *written, const char *data)
{
    struct afp_server_write_request request;
    struct afp_server_write_response response;
    int ret;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_write_request);
    request.header.command = AFP_SERVER_COMMAND_WRITE;
    memcpy(&request.volumeid, volid, sizeof(volumeid_t));
    request.fileid = fileid;
    request.offset = offset;
    request.size = size;
    request.resource = resource;

    /* Send request header */
    if (send_command(sizeof(request), (char *)&request,
                     AFP_SERVER_COMMAND_WRITE) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    /* Send data payload */
    ret = write_bytes(connection.fd, data, size);

    if (ret < 0) {
        printf("Error writing data payload\n");
        close(connection.fd);
        connection.fd = 0;
        return AFP_SERVER_RESULT_ERROR;
    }

    /* Read response */
    ret = read_bytes_with_timeout(connection.fd, (char *)&response,
                                  sizeof(response));

    if (ret < 0) {
        printf("Error reading write response\n");
        close(connection.fd);
        connection.fd = 0;
        return AFP_SERVER_RESULT_ERROR;
    }

    if (response.header.result != AFP_SERVER_RESULT_OKAY) {
        *written = 0;
        return response.header.result;
    }

    *written = response.written;
    return response.header.result;
}

int afp_sl_creat(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode)
{
    struct afp_server_creat_request request;
    struct afp_server_creat_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_creat_request);
    request.header.command = AFP_SERVER_COMMAND_CREAT;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_chmod(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode)
{
    struct afp_server_chmod_request request;
    struct afp_server_chmod_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_chmod_request);
    request.header.command = AFP_SERVER_COMMAND_CHMOD;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_rename(volumeid_t * volid, const char * path_from,
                  const char *path_to, struct afp_url * url)
{
    struct afp_server_rename_request request;
    struct afp_server_rename_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_rename_request);
    request.header.command = AFP_SERVER_COMMAND_RENAME;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path_from, path_from, AFP_MAX_PATH);
    strlcpy(request.path_to, path_to, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_unlink(volumeid_t * volid, const char * path,
                  struct afp_url * url)
{
    struct afp_server_unlink_request request;
    struct afp_server_unlink_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_unlink_request);
    request.header.command = AFP_SERVER_COMMAND_UNLINK;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_truncate(volumeid_t * volid, const char * path,
                    struct afp_url * url, unsigned long long offset)
{
    struct afp_server_truncate_request request;
    struct afp_server_truncate_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_truncate_request);
    request.header.command = AFP_SERVER_COMMAND_TRUNCATE;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.offset = offset;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_utime(volumeid_t * volid, const char * path,
                 struct afp_url * url, struct utimbuf * times)
{
    struct afp_server_utime_request request;
    struct afp_server_utime_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_utime_request);
    request.header.command = AFP_SERVER_COMMAND_UTIME;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    memcpy(&request.times, times, sizeof(struct utimbuf));
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_mkdir(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode)
{
    struct afp_server_mkdir_request request;
    struct afp_server_mkdir_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_mkdir_request);
    request.header.command = AFP_SERVER_COMMAND_MKDIR;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    request.mode = mode;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_rmdir(volumeid_t * volid, const char * path,
                 struct afp_url * url)
{
    struct afp_server_rmdir_request request;
    struct afp_server_rmdir_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_rmdir_request);
    request.header.command = AFP_SERVER_COMMAND_RMDIR;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    return response.header.result;
}

int afp_sl_statfs(volumeid_t * volid, const char * path,
                  struct afp_url * url, struct statvfs * stat)
{
    struct afp_server_statfs_request request;
    struct afp_server_statfs_response response;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volid;
    const char *tmppath = path;
    int ret;
    memset(&request, 0, sizeof(request));
    request.header.close = 0;
    request.header.len = sizeof(struct afp_server_statfs_request);
    request.header.command = AFP_SERVER_COMMAND_STATFS;

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    /* Use current directory if no path specified */
    if (tmppath == NULL || tmppath[0] == '\0') {
        tmppath = "/";
    }

    memcpy(&request.volumeid, volid_p, sizeof(volumeid_t));
    strlcpy(request.path, tmppath, AFP_MAX_PATH);
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    if (response.header.result == AFP_SERVER_RESULT_OKAY) {
        memcpy(stat, &response.stat, sizeof(struct statvfs));
    }

    return response.header.result;
}

int afp_sl_close(volumeid_t *volid, unsigned int fileid)
{
    struct afp_server_close_request request;
    struct afp_server_close_response response;
    int ret;
    request.header.close = 0; /* Keep connection open - just closing file */
    request.header.len = sizeof(struct afp_server_close_request);
    request.header.command = AFP_SERVER_COMMAND_CLOSE;
    memcpy(&request.volumeid, volid, sizeof(volumeid_t));
    request.fileid = fileid;
    ret = send_to_daemon((char *)&request, sizeof(request), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    return response.header.result;
}

int afp_sl_readdir(volumeid_t * volid, const char * path, struct afp_url * url,
                   int start, int count, unsigned int *numfiles,
                   struct afp_file_info_basic **data,
                   int *eod)
{
    struct afp_server_readdir_request req;
    struct afp_server_readdir_response * mainrep;
    int ret;
    const char *tmppath = path;
    unsigned int size;
    volumeid_t *volid_p = volid;
    volumeid_t tmpvolid;
    unsigned int total_files = 0;
    unsigned int requested_count;
    struct afp_file_info_basic *buffer = NULL;
    int current_start = start;
    int eod_flag = 0;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    if (volid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        tmppath = url->path;
        volid_p = &tmpvolid;
    }

    if (tmppath == NULL) {
        tmppath = "/";
    }

    /* Ensure we request at least 256 files if the user asks for fewer (but > 0) */
    requested_count = (count > 0 && count < 256) ? 256 : count;

    /* Loop to fetch all requested files or until EOD */
    while (total_files < requested_count && !eod_flag) {
        unsigned int batch_count = requested_count - total_files;
        memset(&req, 0, sizeof(req));
        req.header.close = 0;
        req.header.len = sizeof(struct afp_server_readdir_request);
        req.header.command = AFP_SERVER_COMMAND_READDIR;
        req.start = current_start;
        req.count = batch_count;
        memcpy(&req.volumeid, volid_p, sizeof(volumeid_t));
        strlcpy(req.path, tmppath, AFP_MAX_PATH);

        if (send_command(sizeof(req), (char *)&req, AFP_SERVER_COMMAND_READDIR) < 0) {
            if (buffer) {
                free(buffer);
            }

            return AFP_SERVER_RESULT_DAEMON_ERROR;
        }

        ret = read_answer();

        if (ret < 0) {
            if (buffer) {
                free(buffer);
            }

            return ret;
        }

        mainrep = (void *) connection.data;

        if (mainrep->header.result) {
            if (buffer) {
                free(buffer);
            }

            return mainrep->header.result;
        }

        unsigned int batch_received = mainrep->numfiles;

        if (batch_received > 0) {
            /* Resize buffer */
            size = (total_files + batch_received) * sizeof(struct afp_file_info_basic);
            struct afp_file_info_basic *new_buffer = realloc(buffer, size);

            if (!new_buffer) {
                if (buffer) {
                    free(buffer);
                }

                return -1;
            }

            buffer = new_buffer;
            /* Unpack variable length data */
            const char *p = ((char *) mainrep) + sizeof(struct afp_server_readdir_response);
            struct afp_file_info_basic *current_basic = buffer + total_files;

            for (unsigned int i = 0; i < batch_received; i++) {
                uint32_t name_len;
                memcpy(&name_len, p, sizeof(uint32_t));
                p += sizeof(uint32_t);

                if (name_len >= AFP_MAX_PATH) {
                    name_len = AFP_MAX_PATH - 1;
                }

                memcpy(current_basic->name, p, name_len);
                current_basic->name[name_len] = '\0';
                p += name_len;
                memcpy(&current_basic->creation_date, p, sizeof(uint32_t));
                p += sizeof(uint32_t);
                memcpy(&current_basic->modification_date, p, sizeof(uint32_t));
                p += sizeof(uint32_t);
                memcpy(&current_basic->unixprivs, p, sizeof(struct afp_unixprivs));
                p += sizeof(struct afp_unixprivs);
                memcpy(&current_basic->size, p, sizeof(uint64_t));
                p += sizeof(uint64_t);
                current_basic++;
            }

            total_files += batch_received;
            current_start += batch_received;
        }

        if (mainrep->eod) {
            eod_flag = 1;
        }

        /* Safety break if daemon returns 0 files but no EOD to prevent infinite loop */
        if (batch_received == 0 && !eod_flag) {
            break;
        }
    }

    *numfiles = total_files;
    *data = buffer;

    if (eod) {
        *eod = eod_flag;
    }

    return 0;
}

int afp_sl_getvols(struct afp_url * url, unsigned int start,
                   unsigned int count, unsigned int *numvols,
                   struct afp_volume_summary * vols)
{
    struct afp_server_getvols_request req;
    int ret;
    struct afp_server_getvols_response * response;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_getvols_request);
    req.header.command = AFP_SERVER_COMMAND_GETVOLS;
    req.start = start;
    req.count = count;
    memcpy(&req.url, url, sizeof(*url));

    if (send_command(sizeof(req), (char *) &req, AFP_SERVER_COMMAND_GETVOLS) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_answer();

    if (ret < 0) {
        return ret;
    }

    response = (void *) connection.data;
    memcpy(vols,
           ((char *)response) + sizeof(struct afp_server_getvols_response),
           response->num * sizeof(struct afp_volume_summary));
    *numvols = response->num;
    return response->header.result;
}

/*
 *
 * afp_sl_connect(,&error)
 *
 * Sets error to:
 * 0:
 *      No error
 * -ENONET:
 *      could not get the address of the server
 * -ENOMEM:
 *      could not allocate memory
 * -ETIMEDOUT:
 *      timed out waiting for connection
 * -ENETUNREACH:
 *      Server unreachable
 * -EISCONN:
 *      Connection already established
 * -ECONNREFUSED:
 *     Remote server has refused the connection
 * -EACCES, -EPERM, -EADDRINUSE, -EAFNOSUPPORT, -EAGAIN, -EALREADY, -EBADF,
 * -EFAULT, -EINPROGRESS, -EINTR, -ENOTSOCK, -EINVAL, -EMFILE, -ENFILE,
 * -ENOBUFS, -EPROTONOSUPPORT:
 *     Internal error
 *
 * Returns:
 * 0: No error
 * -1: An error occurred
 */

int afp_sl_connect(struct afp_url * url, unsigned int uam_mask,
                   serverid_t *id, char *loginmesg, int *error, int dsi_timeout)
{
    struct afp_server_connect_request req;
    const struct afp_server_connect_response *resp;
    const char *t;
    int ret;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_connect_request);
    req.header.command = AFP_SERVER_COMMAND_CONNECT;
    memcpy(&req.url, url, sizeof(struct afp_url));
    req.uam_mask = uam_mask;
    req.dsi_timeout = dsi_timeout;

    if (send_command(sizeof(req), (char *)&req, AFP_SERVER_COMMAND_CONNECT) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_answer();

    if (ret < 0) {
        return ret;
    }

    resp = (void *) connection.data;

    /* Extract response fields before checking for additional data.
     * These fields are part of the response struct and always present. */
    if (id) {
        memcpy(id, &resp->serverid, sizeof(serverid_t));
    }

    if (loginmesg) {
        memcpy(loginmesg, resp->loginmesg, AFP_LOGINMESG_LEN);
    }

    if (error) {
        *error = resp->connect_error;
    }

    /* If there's additional data (log messages), print it */
    if (connection.len > sizeof(struct afp_server_connect_response)) {
        t = connection.data + sizeof(struct afp_server_connect_response);

        if (connection.print) {
            connection.print(t);
        }
    }

    /* Treat "already connected" as success */
    if (resp->header.result == AFP_SERVER_RESULT_OKAY ||
            resp->header.result == AFP_SERVER_RESULT_ALREADY_CONNECTED) {
        return 0;
    } else {
        return resp->connect_error;
    }
}

int afp_sl_attach(struct afp_url * url, unsigned int volume_options,
                  volumeid_t *volumeid)
{
    struct afp_server_attach_request req;
    struct afp_server_attach_response * response;
    const char *t;
    int ret;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    response = (void *) connection.data;
    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_attach_request);
    req.header.command = AFP_SERVER_COMMAND_ATTACH;
    memcpy(&req.url, url, sizeof(struct afp_url));
    req.volume_options = volume_options;

    if (send_command(sizeof(req), (char *)&req, AFP_SERVER_COMMAND_ATTACH) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_answer();

    if (ret < 0) {
        return ret;
    }

    if (connection.len < sizeof(struct afp_server_attach_response)) {
        return 0;
    }

    t = connection.data + sizeof(struct afp_server_attach_response);

    if (connection.print) {
        connection.print(t);
    }

    if (volumeid) {
        memcpy(volumeid, &response->volumeid, sizeof(volumeid_t));
    }

    /* Treat "already attached" as success since we have a valid volumeid */
    if (ret == AFP_SERVER_RESULT_OKAY
            || ret == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
        return 0;
    }

    return ret;
}

int afp_sl_detach(volumeid_t *volumeid, struct afp_url * url)
{
    struct afp_server_detach_request req;
    const char *t;
    int ret;
    volumeid_t tmpvolid;
    volumeid_t *volid_p = volumeid;

    if (afp_sl_setup()) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    if (volumeid == NULL) {
        ret = afp_sl_getvolid(url, &tmpvolid);

        if (ret) {
            return ret;
        }

        volid_p = &tmpvolid;
    }

    req.header.close = 1;
    req.header.len = sizeof(struct afp_server_detach_request);
    req.header.command = AFP_SERVER_COMMAND_DETACH;
    memcpy(&req.volumeid, volid_p, sizeof(volumeid_t));

    if (send_command(sizeof(req), (char *)&req, AFP_SERVER_COMMAND_DETACH) < 0) {
        return AFP_SERVER_RESULT_DAEMON_ERROR;
    }

    ret = read_answer();

    if (ret < 0) {
        return ret;
    }

    /* DETACH uses header.close=1, so daemon closed its side.
     * Close our side too to avoid reusing a stale connection. */
    if (connection.fd > 0) {
        close(connection.fd);
        connection.fd = 0;
    }

    if (connection.len <= sizeof(struct afp_server_detach_response)) {
        return 0;
    }

    t = connection.data + sizeof(struct afp_server_detach_response);

    if (connection.print) {
        connection.print(t);
    }

    return ret;
}

int afp_sl_changepw(struct afp_url * url,
                    const char *old_password,
                    const char *new_password)
{
    struct afp_server_changepw_request req;
    struct afp_server_changepw_response response;
    int ret;

    if (!url || !old_password || !new_password) {
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));
    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_changepw_request);
    req.header.command = AFP_SERVER_COMMAND_CHANGEPW;
    memcpy(&req.url, url, sizeof(struct afp_url));
    strlcpy(req.oldpasswd, old_password, AFP_MAX_PASSWORD_LEN);
    strlcpy(req.newpasswd, new_password, AFP_MAX_PASSWORD_LEN);
    ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return AFP_SERVER_RESULT_ERROR;
    }

    if (response.header.result != AFP_SERVER_RESULT_OKAY) {
        return response.afp_error;
    }

    return 0;
}

int afp_sl_setup(void)
{
    afp_sl_conn_setup();
    return daemon_connect(geteuid());
}

int afp_sl_serverinfo(struct afp_url * url, struct afp_server_basic * basic)
{
    struct afp_server_serverinfo_request req;
    struct afp_server_serverinfo_response response;
    int ret;
    req.header.close = 0;
    req.header.len = sizeof(struct afp_server_serverinfo_request);
    req.header.command = AFP_SERVER_COMMAND_SERVERINFO;
    memcpy(&req.url, url, sizeof(struct afp_url));
    ret = send_to_daemon((char *)&req, sizeof(req), (char *)&response,
                         sizeof(response));

    if (ret != 0) {
        return ret;
    }

    memcpy(basic, &response.server_basic, sizeof(struct afp_server_basic));
    return response.header.result;
}

int afp_sl_disconnect(serverid_t *id)
{
    struct afp_server_disconnect_request req;
    struct afp_server_disconnect_response response;
    int ret;

    if (!id || !*id) {
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.header.command = AFP_SERVER_COMMAND_DISCONNECT;
    req.header.len = sizeof(req);
    /* Close socket after this command */
    req.header.close = 1;
    req.serverid = *id;
    ret = send_to_daemon((char *) &req, sizeof(req),
                         (char *) &response, sizeof(response));

    if (ret == 0 && response.header.result == AFP_SERVER_RESULT_OKAY) {
        *id = NULL;

        if (connection.fd > 0) {
            close(connection.fd);
            connection.fd = 0;
        }

        return 0;
    }

    return -1;
}
