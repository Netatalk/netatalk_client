/*
    Copyright (C) 1987-2002 Free Software Foundation, Inc.
    Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>

    This is based on readline's fileman.c example, which is very useful.
    The original fileman.c carries the following notice:

    This file is part of the GNU Readline Library, a library for
    reading lines of text with interactive input and history editing.

    The GNU Readline Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2, or
    (at your option) any later version.

    The GNU Readline Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    The GNU General Public License is often shipped with GNU software, and
    is generally kept in a file called COPYING or LICENSE.  If you do not
    have a copy of the license, write to the Free Software Foundation,
    59 Temple Place, Suite 330, Boston, MA 02111 USA.
*/

#include "afp.h"
#include "afpsl.h"
#include "afp_server.h"
#include "map_def.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#elif defined(HAVE_LIBEDIT)
#include <editline/readline.h>
#endif

#include "compat.h"
#include "libafpclient.h"
#include "utils.h"
#include "cmdline_afp.h"
#include "cmdline_main.h"

static char curdir[AFP_MAX_PATH];
static struct afp_url url;
static int cmdline_log_min_rank = 2; /* Default rank: notice */
static int verbose_mode = 0;
static char connect_servername[AFP_SERVER_NAME_UTF8_LEN];
static int cmdline_dsi_timeout = 0;

int full_url = 0;

#define DEFAULT_DIRECTORY "/"

static volumeid_t vol_id = NULL;
static serverid_t server_id = NULL;
static int connected = 0;
static int recursive_mode = 0;

static int attach_volume_with_password_prompt(volumeid_t *vol_id_ptr,
        unsigned int volume_options);

static unsigned int get_uam_mask_for_url(void)
{
    unsigned int uam_mask;

    if (url.uamname[0] != '\0') {
        uam_mask = find_uam_by_name(url.uamname);
    } else {
        uam_mask = default_uams_mask();
    }

    return uam_mask;
}

static int is_recoverable_session_error(int ret)
{
    if (ret < 0) {
        return 1;
    }

    switch (ret) {
    case AFP_SERVER_RESULT_ERROR:
    case AFP_SERVER_RESULT_ERROR_UNKNOWN:
    case AFP_SERVER_RESULT_NOTCONNECTED:
    case AFP_SERVER_RESULT_NOTATTACHED:
    case AFP_SERVER_RESULT_DAEMON_ERROR:
    case AFP_SERVER_RESULT_NOSERVER:
    case AFP_SERVER_RESULT_TIMEDOUT:
        return 1;

    default:
        return 0;
    }
}

static int reconnect_session(int restore_volume, int restore_dir)
{
    char mesg[MAX_ERROR_LEN];
    int error = 0;
    unsigned int uam_mask;
    int ret;
    serverid_t new_server_id = NULL;
    volumeid_t new_vol_id = NULL;
    serverid_t old_server_id;
    struct afp_url reconnect_url;
    char saved_volume[AFP_VOLUME_NAME_LEN];
    char saved_dir[AFP_MAX_PATH];
    int had_volume;
    unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;

    if (!connected) {
        return -1;
    }

    memset(mesg, 0, sizeof(mesg));
    strlcpy(saved_volume, url.volumename, sizeof(saved_volume));
    strlcpy(saved_dir, curdir, sizeof(saved_dir));
    had_volume = (vol_id != NULL);
    old_server_id = server_id;

    /* Drop local cached handles; stale IDs are no longer trustworthy. */
    if (old_server_id && afp_sl_disconnect(&old_server_id) != 0) {
        afp_sl_exit();
    }

    uam_mask = get_uam_mask_for_url();

    if (uam_mask == 0) {
        return -1;
    }

    reconnect_url = url;

    if (connect_servername[0] != '\0') {
        strlcpy(reconnect_url.servername, connect_servername,
                sizeof(reconnect_url.servername));
    }

    if (afp_sl_connect(&reconnect_url, uam_mask, &new_server_id, mesg,
                       &error, cmdline_dsi_timeout) != 0) {
        return -1;
    }

    if ((restore_volume || had_volume) && saved_volume[0] != '\0') {
        strlcpy(url.volumename, saved_volume, sizeof(url.volumename));
        ret = attach_volume_with_password_prompt(&new_vol_id, volume_options);

        if (ret != 0) {
            return -1;
        }
    }

    server_id = new_server_id;
    vol_id = new_vol_id;
    connected = 1;

    if (restore_dir && saved_dir[0] != '\0') {
        strlcpy(curdir, saved_dir, sizeof(curdir));
    }

    return 0;
}

static int escape_paths(char * outgoing1, char * outgoing2, char * incoming)
{
    char *writeto = outgoing1;
    int inquote = 0, inescape = 0, donewith1 = 0;
    char *p = incoming;
    size_t incoming_len;

    if (outgoing1 == NULL || incoming == NULL) {
        goto error;
    }

    incoming_len = strnlen(incoming, AFP_MAX_PATH);

    if (incoming_len >= AFP_MAX_PATH) {
        goto error;
    }

    if (incoming_len == 0) {
        goto error;
    }

    memset(outgoing1, 0, AFP_MAX_PATH);

    if (outgoing2) {
        memset(outgoing2, 0, AFP_MAX_PATH);
    }

    for (p = incoming; p < incoming + incoming_len; p++) {
        if (*p == '"') {
            if (inescape) {
                inescape = 0;
                goto add;
            } else if (inquote) {
                inquote = 0;
                continue;
            } else {
                inquote = 1;
                continue;
            }
        }

        if (*p == ' ') {
            if (inescape) {
                inescape = 0;
                goto add;
            } else if (inquote) {
                goto add;
            } else if ((donewith1 == 1) || (outgoing2 == NULL)) {
                goto out;
            }

            writeto = outgoing2;
            donewith1 = 1;
            continue;
        }

        if (*p == '\\' && inescape == 0) {
            inescape = 1;
            continue;
        } else if (inescape) {
            inescape = 0;
            goto add;
        }

add:
        *writeto = *p;
        writeto++;
    }

out:

    if ((outgoing2 != NULL) && (donewith1 == 0)) {
        goto error;
    }

    return 0;
error:
    return -1;
}

static unsigned int tvdiff(struct timeval * starttv, struct timeval * endtv)
{
    unsigned int d;
    d = (endtv->tv_sec - starttv->tv_sec) * 1000;
    d += (endtv->tv_usec - starttv->tv_usec) / 1000;
    return d;
}

static void printdiff(struct timeval * starttv, struct timeval *endtv,
                      unsigned long long *amount_written)
{
    unsigned int diff;
    unsigned long long kb_written;
    diff = tvdiff(starttv, endtv);
    float frac = ((float) diff) / 1000.0; /* Now in seconds */
    printf("    Transferred %lld bytes in ", *amount_written);
    printf("%.3f seconds. ", frac);
    /* Now calculate the transfer rate */
    kb_written = (*amount_written / 1000);
    float rate = (kb_written) / frac;
    printf("(%.0f kB/s)\n", rate);
}

static int cmdline_getpass(void)
{
    char *passwd;

    /* Prompt for password if:
     * - password is "-" (explicit prompt request), or
     * - a username was given but password is empty
     *   (without username, we fall back to guest auth as "nobody") */
    if (strcmp(url.password, "-") == 0 ||
            (url.username[0] != '\0'
             && strcmp(url.username, "nobody") != 0
             && url.password[0] == '\0')) {
        passwd = getpass("Password:");
        strlcpy(url.password, passwd, AFP_MAX_PASSWORD_LEN);
    }

    return 0;
}

static int cmdline_get_volpass(void)
{
    char *volpass;
    volpass = getpass("Volume password:");

    if (volpass == NULL) {
        return -1;  /* Ctrl+C or error */
    }

    strlcpy(url.volpassword, volpass, sizeof(url.volpassword));
    /* Clear the getpass() static buffer up to the max we could have used */
    explicit_bzero(volpass, sizeof(url.volpassword) - 1);
    return 0;
}

static int attach_volume_with_password_prompt(volumeid_t *vol_id_ptr,
        unsigned int volume_options)
{
    int ret;
    /* Clear any previous password to force a fresh prompt if needed */
    explicit_bzero(url.volpassword, sizeof(url.volpassword));
    /* First attempt */
    ret = afp_sl_attach(&url, volume_options, vol_id_ptr);

    if (ret == 0) {
        return 0;
    }

    if (ret == AFP_SERVER_RESULT_VOLPASS_NEEDED) {
        if (cmdline_get_volpass() != 0) {
            printf("Password prompt cancelled.\n");
            return AFP_SERVER_RESULT_VOLPASS_NEEDED;
        }

        /* Second attempt with password */
        ret = afp_sl_attach(&url, volume_options, vol_id_ptr);
    }

    return ret;
}


static int get_server_path(char * filename, char * server_fullname)
{
    int result;

    if (filename[0] != '/') {
        if (strcmp(curdir, "/") == 0) {
            result = snprintf(server_fullname, AFP_MAX_PATH, "/%s", filename);
        } else {
            result = snprintf(server_fullname, AFP_MAX_PATH, "%s/%s", curdir, filename);
        }

        if (result >= AFP_MAX_PATH || result < 0) {
            fprintf(stderr,
                    "Error: Path exceeds maximum length or other error occurred.\n");
            return -1;
        }
    } else {
        result = snprintf(server_fullname, AFP_MAX_PATH, "%s", filename);
    }

    if (result >= AFP_MAX_PATH || result < 0) {
        return -1;
    }

    return 0;
}

/**
 * Appends a basename to a directory path, adding a separator if needed.
 * Uses snprintf for efficient single-operation append.
 * Returns 0 on success, -1 if the result would exceed max_len.
 */
static int append_basename_to_path(char *path, const char *base, size_t max_len)
{
    size_t path_len, base_len;
    int need_slash;
    size_t space_needed;

    if (!path || !base) {
        return -1;
    }

    /* Validate that strings are null-terminated within max_len */
    path_len = strnlen(path, max_len);

    if (path_len >= max_len) {
        return -1;
    }

    base_len = strnlen(base, max_len);

    if (base_len >= max_len) {
        return -1;
    }

    need_slash = (path_len > 0 && path[path_len - 1] != '/') ? 1 : 0;
    space_needed = path_len + need_slash + base_len + 1;

    if (space_needed > max_len) {
        return -1;
    }

    if (need_slash) {
        snprintf(path + path_len, max_len - path_len, "/%s", base);
    } else {
        snprintf(path + path_len, max_len - path_len, "%s", base);
    }

    return 0;
}

static void print_file_details_basic(struct afp_file_info_basic * p,
                                     int size_width)
{
    struct tm * mtime;
    time_t t;
#define DATE_LEN 32
    char datestr[DATE_LEN];
    char mode_str[11];
    uint32_t mode = p->unixprivs.permissions;
    snprintf(mode_str, sizeof(mode_str), "----------");
    t = p->modification_date;
    mtime = localtime(&t);

    if (S_ISDIR(mode)) {
        mode_str[0] = 'd';
    }

    if (mode & S_IRUSR) {
        mode_str[1] = 'r';
    }

    if (mode & S_IWUSR) {
        mode_str[2] = 'w';
    }

    if (mode & S_IXUSR) {
        mode_str[3] = 'x';
    }

    if (mode & S_IRGRP) {
        mode_str[4] = 'r';
    }

    if (mode & S_IWGRP) {
        mode_str[5] = 'w';
    }

    if (mode & S_IXGRP) {
        mode_str[6] = 'x';
    }

    if (mode & S_IROTH) {
        mode_str[7] = 'r';
    }

    if (mode & S_IWOTH) {
        mode_str[8] = 'w';
    }

    if (mode & S_IXOTH) {
        mode_str[9] = 'x';
    }

    strftime(datestr, DATE_LEN, "%F %H:%M", mtime);
    printf("%s %*lld %s %s\n", mode_str, size_width, p->size, datestr, p->name);
}

static void list_volumes(void)
{
    unsigned int numvols = 0;
    struct afp_volume_summary *vols;
    unsigned int count = 100; /* Reasonable limit for CLI listing */
    int ret;
    vols = malloc(sizeof(struct afp_volume_summary) * count);

    if (!vols) {
        printf("Out of memory\n");
        return;
    }

    ret = afp_sl_getvols(&url, 0, count, &numvols, vols);

    if (ret != AFP_SERVER_RESULT_OKAY && is_recoverable_session_error(ret)
            && reconnect_session(0, 0) == 0) {
        numvols = 0;
        ret = afp_sl_getvols(&url, 0, count, &numvols, vols);
    }

    if (ret == AFP_SERVER_RESULT_OKAY) {
        printf("Available volumes on %s:\n", url.servername);

        for (unsigned int i = 0; i < numvols; i++) {
            printf("  %s\n", vols[i].volume_name_printable);
        }
    } else {
        printf("Could not list volumes\n");
    }

    free(vols);
}

int com_pass(__attribute__((unused)) char *unused)
{
    const char *old_password;
    const char *new_password;
    const char *new_password_confirm;
    int ret;

    if (!connected) {
        printf("You're not connected to a server.\n");
        return -1;
    }

    old_password = getpass("Old password: ");

    if (old_password == NULL || old_password[0] == '\0') {
        printf("Password change cancelled.\n");
        return -1;
    }

    /* stash old_password since getpass uses a static buffer */
    char old_pw_buf[AFP_MAX_PASSWORD_LEN];
    strlcpy(old_pw_buf, old_password, sizeof(old_pw_buf));
    new_password = getpass("New password: ");

    if (new_password == NULL || new_password[0] == '\0') {
        printf("Password change cancelled.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        return -1;
    }

    /* stash new_password since getpass uses a static buffer */
    char new_pw_buf[AFP_MAX_PASSWORD_LEN];
    strlcpy(new_pw_buf, new_password, sizeof(new_pw_buf));
    new_password_confirm = getpass("Confirm new password: ");

    if (new_password_confirm == NULL) {
        printf("Password change cancelled.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        explicit_bzero(new_pw_buf, sizeof(new_pw_buf));
        return -1;
    }

    if (strcmp(new_pw_buf, new_password_confirm) != 0) {
        printf("Passwords do not match.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        explicit_bzero(new_pw_buf, sizeof(new_pw_buf));
        return -1;
    }

    ret = afp_sl_changepw(&url, old_pw_buf, new_pw_buf);
    explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
    explicit_bzero(new_pw_buf, sizeof(new_pw_buf));

    if (ret != 0) {
        switch (ret) {
        case kFPAccessDenied:
            printf("Password change failed: access denied.\n");
            break;

        case kFPUserNotAuth:
            printf("Password change failed: incorrect old password.\n");
            break;

        case kFPBadUAM:
            printf("Password change failed: the server's authentication method "
                   "does not support password changing.\n");
            break;

        case kFPCallNotSupported:
            printf("Password change failed: not supported by the current "
                   "authentication method.\n");
            break;

        case kFPPwdSameErr:
            printf("Password change failed: new password must be different "
                   "from old password.\n");
            break;

        case kFPPwdTooShortErr:
            printf("Password change failed: new password is too short.\n");
            break;

        case kFPPwdExpiredErr:
            printf("Password change failed: password has expired.\n");
            break;

        case kFPPwdPolicyErr:
            printf("Password change failed: new password does not meet "
                   "the server's password policy.\n");
            break;

        case kFPParamErr:
            printf("Password change failed: invalid parameter.\n");
            break;

        default:
            printf("Password change failed (error code: %d).\n", ret);
            break;
        }

        return -1;
    }

    printf("Password changed successfully.\n");
    return 0;
}

int com_dir(char * arg)
{
    if (!arg) {
        arg = "";
    }

    struct afp_file_info_basic *filebase = NULL;

    unsigned int numfiles = 0;
    int eod = 0;
    int ret = -1;
    char path[AFP_MAX_PATH];
    char dir_path[AFP_MAX_PATH];

    if (!vol_id) {
        if (connected) {
            list_volumes();
            return 0;
        }

        printf("You're not connected to a server\n");
        goto out;
    }

    /* If an argument is provided, use it; otherwise use current directory */
    if (arg[0] != '\0') {
        if (escape_paths(path, NULL, arg)) {
            printf("Invalid path\n");
            goto out;
        }

        /* Handle "." as the current directory */
        if (strcmp(path, ".") == 0) {
            strlcpy(dir_path, curdir, AFP_MAX_PATH);
        } else {
            get_server_path(path, dir_path);
        }
    } else {
        strlcpy(dir_path, curdir, AFP_MAX_PATH);
    }

    ret = afp_sl_readdir(&vol_id, dir_path, NULL, 0, 100, &numfiles, &filebase,
                         &eod);

    if (ret != AFP_SERVER_RESULT_OKAY && is_recoverable_session_error(ret)
            && reconnect_session(1, 1) == 0) {
        if (filebase) {
            free(filebase);
            filebase = NULL;
        }

        numfiles = 0;
        eod = 0;
        ret = afp_sl_readdir(&vol_id, dir_path, NULL, 0, 100, &numfiles,
                             &filebase, &eod);
    }

    if (ret != AFP_SERVER_RESULT_OKAY) {
        printf("Could not read directory\n");
        goto out;
    }

    if (numfiles == 0) {
        ret = 0;
        goto out;
    }

    /* Calculate max width for file size column */
    int max_width = 0;

    for (unsigned int i = 0; i < numfiles; i++) {
        char size_str[32];
        int width = snprintf(size_str, sizeof(size_str), "%lld", filebase[i].size);

        if (width > max_width) {
            max_width = width;
        }
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        print_file_details_basic(&filebase[i], max_width);
    }

    ret = 0;
out:

    if (filebase) {
        free(filebase);
    }

    return ret;
}

int com_touch(char * arg)
{
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct utimbuf times;
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: touch <filename>\n");
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    ret = afp_sl_creat(&vol_id, server_fullname, NULL, 0644);

    if (ret == AFP_SERVER_RESULT_OKAY) {
        return 0;
    } else if (ret == AFP_SERVER_RESULT_EXIST) {
        time_t now = time(NULL);
        times.actime = now;
        times.modtime = now;
        ret = afp_sl_utime(&vol_id, server_fullname, NULL, &times);

        if (ret != AFP_SERVER_RESULT_OKAY) {
            printf("Could not update timestamp for %s (result=%d)\n", filename, ret);
            return -1;
        }

        return 0;
    }

    printf("Could not create file %s (result=%d)\n", filename, ret);
    return -1;
}

int com_chmod(char * arg)
{
    char mode_str[AFP_MAX_PATH];
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    mode_t mode;
    char *endptr;
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(mode_str, filename, arg)) {
        printf("expecting format: chmod <mode> <filename>\n");
        return -1;
    }

    mode = (mode_t)strtol(mode_str, &endptr, 8);

    if (*endptr != '\0') {
        printf("Invalid mode: %s\n", mode_str);
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    ret = afp_sl_chmod(&vol_id, server_fullname, NULL, mode);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied changing mode for %s\n", filename);
        } else if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("File not found: %s\n", filename);
        } else {
            printf("Could not chmod %s (result=%d)\n", filename, ret);
        }

        return -1;
    }

    return 0;
}

static int upload_file(char *local_filename, char *server_fullname,
                       unsigned long long *bytes_transferred)
{
    int localfd = -1;
    unsigned int fileid = 0;
    struct stat localstat;
    unsigned long long offset = 0;
    unsigned long long total_written = 0;
#define PUT_BUFSIZE 102400
    char buf[PUT_BUFSIZE];
    ssize_t amount_read;
    unsigned int written;
    int ret = -1;
    struct timeval starttv, endtv;
    int file_opened = 0;
    localfd = open(local_filename, O_RDONLY);

    if (localfd < 0) {
        printf("Could not open local file \"%s\"\n", local_filename);
        perror("open");
        goto out;
    }

    if (fstat(localfd, &localstat) != 0) {
        printf("Could not get attributes for local file \"%s\"\n", local_filename);
        perror("fstat");
        goto out;
    }

    if (!S_ISREG(localstat.st_mode)) {
        printf("Not a regular file: %s\n", local_filename);
        goto out;
    }

    if (verbose_mode) {
        printf("    Uploading file %s to %s\n", local_filename, server_fullname);
    }

    gettimeofday(&starttv, NULL);
    /* Create remote file first (with permission bits only, not file type)
     * We mask with 0777 to ensure we only pass permission bits, not file type bits (like S_IFREG)
     * which could confuse the server or midlevel API */
    ret = afp_sl_creat(&vol_id, server_fullname, NULL, localstat.st_mode & 0777);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_EXIST) {
            if (verbose_mode) {
                printf("    File exists, truncating before overwriting...\n");
            }

            ret = afp_sl_truncate(&vol_id, server_fullname, NULL, 0);

            if (ret != AFP_SERVER_RESULT_OKAY) {
                printf("Could not truncate existing file \"%s\" (result=%d)\n", server_fullname,
                       ret);
                goto out;
            }
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            /* Sometimes ACCESS is returned if file exists but is read-only.
               Try to truncate/overwrite anyway. */
            ret = afp_sl_truncate(&vol_id, server_fullname, NULL, 0);

            if (ret != AFP_SERVER_RESULT_OKAY) {
                printf("Permission denied creating file \"%s\"\n", server_fullname);
                goto out;
            }
        } else {
            printf("Could not create remote file \"%s\" (result=%d)\n", server_fullname,
                   ret);
            goto out;
        }
    }

    ret = afp_sl_open(&vol_id, server_fullname, NULL, &fileid, O_RDWR);

    if (ret) {
        if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied opening file \"%s\"\n", server_fullname);
        } else if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("Permission denied creating file \"%s\"\n", server_fullname);
        } else {
            printf("Could not open remote file for writing (result=%d)\n", ret);
        }

        /* If we created the file but failed to open it, try to remove it to avoid leaving
           a partial/inaccessible file and potentially confusing the server state */
        afp_sl_unlink(&vol_id, server_fullname, NULL);
        goto out;
    }

    file_opened = 1;

    /* Upload loop: read from local, write to remote */
    while ((amount_read = read(localfd, buf, PUT_BUFSIZE)) > 0) {
        int api_ret = afp_sl_write(&vol_id, fileid, 0, offset, amount_read, &written,
                                   buf);

        if (api_ret || written != (unsigned int)amount_read) {
            printf("Write error at offset %llu (wrote %u of %zd bytes)\n",
                   offset, written, amount_read);
            ret = -1;
            goto out;
        }

        offset += written;
        total_written += written;
    }

    if (amount_read < 0) {
        printf("Error reading local file\n");
        perror("read");
        goto out;
    }

    /* Set permissions on remote file (mask to get only permission bits) */
    ret = afp_sl_chmod(&vol_id, server_fullname, NULL, localstat.st_mode & 0777);

    if (ret) {
        printf("Warning: Could not set permissions on remote file\n");
        /* Non-fatal, continue */
    }

    gettimeofday(&endtv, NULL);

    if (verbose_mode) {
        unsigned long long elapsed_usec =
            (endtv.tv_sec - starttv.tv_sec) * 1000000ULL +
            (endtv.tv_usec - starttv.tv_usec);
        double elapsed_sec = elapsed_usec / 1000000.0;
        double rate_mbps = 0.0;

        if (elapsed_sec > 0.0) {
            rate_mbps = (total_written * 8.0) / (elapsed_sec * 1000000.0);
        }

        printf("    Transferred %llu bytes in %.2f seconds (%.2f Mbps)\n",
               total_written, elapsed_sec, rate_mbps);
    }

    if (bytes_transferred) {
        *bytes_transferred = total_written;
    }

    ret = 0;
out:

    if (file_opened && fileid) {
        afp_sl_close(&vol_id, fileid);
    }

    if (localfd >= 0) {
        close(localfd);
    }

    /* Note: We don't delete the partially created remote file on error */
    return ret;
}

static int upload_directory(char *local_dirname, char *server_parent_path,
                            unsigned long long *total_bytes)
{
    DIR *dir;
    struct dirent *entry;
    char local_path[PATH_MAX];
    char server_path[AFP_MAX_PATH];
    struct stat st;
    int ret = 0;
    unsigned long long bytes = 0;
    ret = afp_sl_mkdir(&vol_id, server_parent_path, NULL, 0755);

    if (ret != AFP_SERVER_RESULT_OKAY && ret != AFP_SERVER_RESULT_EXIST) {
        printf("Failed to create remote directory %s (error: %d)\n", server_parent_path,
               ret);

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    dir = opendir(local_dirname);

    if (!dir) {
        perror("opendir");

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(local_path, sizeof(local_path), "%s/%s", local_dirname, entry->d_name);
        snprintf(server_path, sizeof(server_path), "%s/%s", server_parent_path,
                 entry->d_name);

        if (stat(local_path, &st) != 0) {
            perror("stat");
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            unsigned long long subdir_bytes = 0;

            if (upload_directory(local_path, server_path, &subdir_bytes) < 0) {
                ret = -1;
            } else {
                bytes += subdir_bytes;
            }
        } else if (S_ISREG(st.st_mode)) {
            unsigned long long file_bytes = 0;

            if (upload_file(local_path, server_path, &file_bytes) < 0) {
                ret = -1;
            } else {
                bytes += file_bytes;
            }
        }
    }

    closedir(dir);

    if (total_bytes) {
        *total_bytes = bytes;
    }

    return ret;
}

int com_put(char *arg)
{
    char local_filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    char *basename_ptr;
    struct stat st;
    int recursive = recursive_mode;
    unsigned long long bytes_transferred = 0;
    int ret = -1;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((arg[0] == '-') && (arg[1] == 'r') && (arg[2] == ' ')) {
        recursive = 1;
        arg += 3;

        while (arg && isspace((unsigned char)arg[0])) {
            arg++;
        }
    }

    if (escape_paths(local_filename, NULL, arg)) {
        printf("expecting format: put [-r] <filename>\n");
        goto error;
    }

    if (stat(local_filename, &st) != 0) {
        perror("stat");
        goto error;
    }

    basename_ptr = basename(local_filename);
    get_server_path(basename_ptr, server_fullname);

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            ret = upload_directory(local_filename, server_fullname, &bytes_transferred);
            goto out;
        } else {
            printf("%s is a directory (start afpcmd with -r to upload recursively)\n",
                   local_filename);
            goto error;
        }
    }

    ret = upload_file(local_filename, server_fullname, &bytes_transferred);
out:

    if (bytes_transferred > 0) {
        printf("Transfer complete. %llu bytes sent.\n", bytes_transferred);
    }

error:
    return ret;
}

static int retrieve_file(char * arg, int fd, struct stat *stat,
                         unsigned long long *amount_written)
{
    unsigned int fileid = 0;
    int file_opened = 0;
    char path[PATH_MAX];
    unsigned long long offset = 0;
#define BUF_SIZE 102400
    unsigned int size = BUF_SIZE;
    char buf[BUF_SIZE];
    unsigned int received, eof = 0;
    unsigned long long total = 0;
    struct timeval starttv, endtv;
    int ret = -1;
    *amount_written = 0;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto out;
    }

    get_server_path(arg, path);
    gettimeofday(&starttv, NULL);

    if (afp_sl_stat(&vol_id, path, NULL, stat)) {
        printf("Could not get file attributes for file %s\n", path);
        goto out;
    }

    if (afp_sl_open(&vol_id, path, NULL, &fileid, O_RDONLY)) {
        printf("Could not open %s on server\n", arg);
        goto out;
    }

    file_opened = 1;

    /* Read file in chunks */
    while (!eof) {
        memset(buf, 0, BUF_SIZE);
        int api_ret = afp_sl_read(&vol_id, fileid, 0, offset, size, &received, &eof,
                                  buf);

        if (api_ret) {
            printf("Error reading file\n");
            ret = -1;
            goto out;
        }

        if (received == 0) {
            break;
        }

        total += write(fd, buf, received);
        offset += received;
    }

    if (verbose_mode) {
        gettimeofday(&endtv, NULL);
        printdiff(&starttv, &endtv, &total);
    }

    *amount_written = total;
    ret = 0;
out:

    /* Do not close fd here, caller owns it */
    if (file_opened && fileid) {
        afp_sl_close(&vol_id, fileid);
    }

    return ret;
}

static int download_directory(char *server_path, char *local_path,
                              unsigned long long *total_bytes)
{
    struct afp_file_info_basic *filebase = NULL;
    unsigned int numfiles = 0;
    int eod = 0;
    int ret = 0;
    char new_server_path[AFP_MAX_PATH];
    char new_local_path[PATH_MAX];
    struct stat st;
    unsigned long long bytes = 0;

    if (mkdir(local_path, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    if (afp_sl_readdir(&vol_id, server_path, NULL, 0, 1000, &numfiles, &filebase,
                       &eod)) {
        printf("Could not read directory %s\n", server_path);

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        struct afp_file_info_basic *p = &filebase[i];

        if (strcmp(p->name, ".") == 0 || strcmp(p->name, "..") == 0) {
            continue;
        }

        int path_len = snprintf(new_server_path, sizeof(new_server_path), "%s/%s",
                                server_path,
                                p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_server_path)) {
            printf("Path too long: %s/%s\n", server_path, p->name);
            continue;
        }

        path_len = snprintf(new_local_path, sizeof(new_local_path), "%s/%s", local_path,
                            p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_local_path)) {
            printf("Local path too long: %s/%s\n", local_path, p->name);
            continue;
        }

        if (S_ISDIR(p->unixprivs.permissions)) {
            unsigned long long subdir_bytes = 0;

            if (download_directory(new_server_path, new_local_path, &subdir_bytes) < 0) {
                ret = -1;
            } else {
                bytes += subdir_bytes;
            }
        } else {
            int fd = open(new_local_path, O_CREAT | O_TRUNC | O_RDWR, 0644);

            if (fd < 0) {
                perror("open");
                ret = -1;
                continue;
            }

            /* Construct a stat struct for retrieve_file */
            memset(&st, 0, sizeof(st));
            st.st_mode = p->unixprivs.permissions;
            st.st_size = p->size;
            st.st_uid = p->unixprivs.uid;
            st.st_gid = p->unixprivs.gid;
            st.st_mtime = p->modification_date;
            unsigned long long amount = 0;

            if (verbose_mode) {
                printf("    Downloading file %s\n", p->name);
            }

            if (retrieve_file(new_server_path, fd, &st, &amount) < 0) {
                ret = -1;
            } else {
                bytes += amount;
            }

            close(fd);
        }
    }

    if (filebase) {
        free(filebase);
    }

    if (total_bytes) {
        *total_bytes = bytes;
    }

    return ret;
}

static int com_get_file(char * arg, unsigned long long *total)
{
    int fd;
    struct stat stat;
    char *localfilename;
    char filename[AFP_MAX_PATH];
    char getattr_path[AFP_MAX_PATH];

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((escape_paths(filename, NULL, arg))) {
        printf("expecting format: get <filename>\n");
        goto error;
    }

    localfilename = basename(filename);

    if (verbose_mode) {
        printf("    Downloading file %s\n", filename);
    }

    get_server_path(filename, getattr_path);

    if (afp_sl_stat(&vol_id, getattr_path, NULL, &stat)) {
        printf("Could not get attributes for file \"%s\"\n", filename);
        goto error;
    }

    fd = open(localfilename, O_CREAT | O_TRUNC | O_RDWR, stat.st_mode);

    if (fd < 0) {
        printf("Failed to open \"%s\" for writing\n", localfilename);
        perror("Opening local file");
        goto error;
    }

    if (fchmod(fd, stat.st_mode) < 0) {
        perror("Setting file mode");
        /* Non-fatal error, continue */
    }

    if (fchown(fd, stat.st_uid, stat.st_gid) < 0) {
        perror("Setting file ownership");
        /* Non-fatal error, continue */
    }

    retrieve_file(filename, fd, &stat, total);
    close(fd);
    return 0;
error:
    return -1;
}

int com_get(char *arg)
{
    unsigned long long amount_written = 0;
    char filename[AFP_MAX_PATH];
    char server_path[AFP_MAX_PATH];
    struct stat st;
    int recursive = recursive_mode;
    int ret = -1;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((arg[0] == '-') && (arg[1] == 'r') && (arg[2] == ' ')) {
        recursive = 1;
        arg += 3;

        while (arg && isspace((unsigned char)arg[0])) {
            arg++;
        }
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: get [-r] <filename>\n");
        goto error;
    }

    get_server_path(filename, server_path);

    if (afp_sl_stat(&vol_id, server_path, NULL, &st) != 0) {
        printf("File not found: %s\n", filename);
        goto error;
    }

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            char *local_name = basename(filename);
            ret = download_directory(server_path, local_name, &amount_written);
            goto out;
        } else {
            printf("%s is a directory (start afpcmd with -r to download recursively)\n",
                   filename);
            goto error;
        }
    }

    ret = com_get_file(arg, &amount_written);
out:

    if (amount_written > 0) {
        printf("Transfer complete. %llu bytes received.\n", amount_written);
    }

error:
    return ret;
}


int com_view(char * arg)
{
    unsigned long long amount_written;
    char filename[AFP_MAX_PATH];
    struct stat stat;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((escape_paths(filename, NULL, arg))) {
        printf("expecting format: cat <filename>\n");
        goto error;
    }

    retrieve_file(filename, fileno(stdout), &stat, &amount_written);
    printf("\n");
    return 0;
error:
    return -1;
}

int com_rename(char * arg)
{
    char oldpath[AFP_MAX_PATH];
    char newpath[AFP_MAX_PATH];
    char server_oldpath[AFP_MAX_PATH];
    char server_newpath[AFP_MAX_PATH];
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(oldpath, newpath, arg)) {
        printf("expecting format: mv <oldname> <newname>\n");
        return -1;
    }

    if (get_server_path(oldpath, server_oldpath) < 0) {
        printf("Invalid old path\n");
        return -1;
    }

    if (get_server_path(newpath, server_newpath) < 0) {
        printf("Invalid new path\n");
        return -1;
    }

    ret = afp_sl_rename(&vol_id, server_oldpath, server_newpath, NULL);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        printf("Failed to move %s to %s (error: %d)\n",
               server_oldpath, server_newpath, ret);
        return -1;
    }

    /* Check if target is a directory for display purposes */
    struct stat st;

    if (afp_sl_stat(&vol_id, server_newpath, NULL, &st) == 0
            && S_ISDIR(st.st_mode)) {
        char oldpath_copy[AFP_MAX_PATH];
        strlcpy(oldpath_copy, oldpath, AFP_MAX_PATH);
        const char *base = basename(oldpath_copy);
        append_basename_to_path(newpath, base, AFP_MAX_PATH);
        /* Silent failure is OK here for display purposes */
    }

    return 0;
}

int com_copy(char * arg)
{
    char source_path[AFP_MAX_PATH];
    char target_path[AFP_MAX_PATH];
    char server_source[AFP_MAX_PATH];
    char server_target[AFP_MAX_PATH];
    struct stat source_stat;
    struct stat target_stat;
    unsigned int source_fid = 0, target_fid = 0;
    int ret = -1;
    unsigned long long offset = 0;
    unsigned int received, written;
    unsigned int eof = 0;
#define COPY_BUFSIZE 102400
    char buf[COPY_BUFSIZE];

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto out;
    }

    if (escape_paths(source_path, target_path, arg)) {
        printf("expecting format: cp <source> <target>\n");
        goto out;
    }

    if (get_server_path(source_path, server_source) < 0) {
        printf("Invalid source path\n");
        goto out;
    }

    if (get_server_path(target_path, server_target) < 0) {
        printf("Invalid target path\n");
        goto out;
    }

    if (afp_sl_stat(&vol_id, server_source, NULL, &source_stat)) {
        printf("Could not stat source file: %s\n", source_path);
        goto out;
    }

    if (S_ISDIR(source_stat.st_mode)) {
        printf("Source is a directory (recursive copy not supported)\n");
        goto out;
    }

    if (afp_sl_stat(&vol_id, server_target, NULL,
                    &target_stat) == AFP_SERVER_RESULT_OKAY && S_ISDIR(target_stat.st_mode)) {
        const char *base = basename(source_path);

        if (append_basename_to_path(server_target, base, AFP_MAX_PATH) < 0) {
            printf("Target path too long\n");
            goto out;
        }
    }

    if (afp_sl_open(&vol_id, server_source, NULL, &source_fid, O_RDONLY)) {
        printf("Could not open source file: %s\n", source_path);
        goto out;
    }

    ret = afp_sl_creat(&vol_id, server_target, NULL, source_stat.st_mode & 0777);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_EXIST) {
            if (afp_sl_truncate(&vol_id, server_target, NULL, 0)) {
                printf("Could not truncate target file\n");
                goto out;
            }
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied creating target file\n");
            goto out;
        } else {
            printf("Could not create target file: %d\n", ret);
            goto out;
        }
    }

    if (afp_sl_open(&vol_id, server_target, NULL, &target_fid, O_RDWR)) {
        printf("Could not open target file for writing\n");
        goto out;
    }

    while (!eof) {
        int api_ret = afp_sl_read(&vol_id, source_fid, 0, offset, COPY_BUFSIZE,
                                  &received, &eof, buf);

        if (api_ret != AFP_SERVER_RESULT_OKAY) {
            printf("Error reading from source\n");
            ret = -1;
            goto out;
        }

        if (received > 0) {
            api_ret = afp_sl_write(&vol_id, target_fid, 0, offset, received, &written, buf);

            if (api_ret != AFP_SERVER_RESULT_OKAY || written != received) {
                printf("Error writing to target\n");
                ret = -1;
                goto out;
            }

            offset += received;
        }
    }

    ret = 0;
    printf("Copied %llu bytes\n", offset);
out:

    if (source_fid) {
        afp_sl_close(&vol_id, source_fid);
    }

    if (target_fid) {
        afp_sl_close(&vol_id, target_fid);
    }

    return ret;
}

static int delete_directory(char *server_path)
{
    struct afp_file_info_basic *filebase = NULL;
    unsigned int numfiles = 0;
    int eod = 0;
    int ret = 0;
    char new_server_path[AFP_MAX_PATH];

    if (afp_sl_readdir(&vol_id, server_path, NULL, 0, 1000, &numfiles, &filebase,
                       &eod)) {
        printf("Could not read directory %s\n", server_path);
        return -1;
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        struct afp_file_info_basic *p = &filebase[i];

        if (strcmp(p->name, ".") == 0 || strcmp(p->name, "..") == 0) {
            continue;
        }

        int path_len = snprintf(new_server_path, sizeof(new_server_path), "%s/%s",
                                server_path,
                                p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_server_path)) {
            printf("Path too long: %s/%s\n", server_path, p->name);
            continue;
        }

        if (S_ISDIR(p->unixprivs.permissions)) {
            if (delete_directory(new_server_path) < 0) {
                ret = -1;
            }
        } else {
            int del_ret = afp_sl_unlink(&vol_id, new_server_path, NULL);

            if (del_ret != AFP_SERVER_RESULT_OKAY) {
                printf("Failed to delete file %s (error: %d)\n", new_server_path, del_ret);
                ret = -1;
            }
        }
    }

    if (filebase) {
        free(filebase);
    }

    if (ret == 0) {
        int rmdir_ret = afp_sl_rmdir(&vol_id, server_path, NULL);

        if (rmdir_ret != AFP_SERVER_RESULT_OKAY) {
            printf("Failed to remove directory %s (error: %d)\n", server_path, rmdir_ret);
            return -1;
        }
    }

    return ret;
}

int com_delete(char *arg)
{
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct stat st;
    int ret;
    int recursive = recursive_mode;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if ((arg[0] == '-') && (arg[1] == 'r') && (arg[2] == ' ')) {
        recursive = 1;
        arg += 3;

        while (arg && isspace((unsigned char)arg[0])) {
            arg++;
        }
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: rm [-r] <filename>\n");
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    if (afp_sl_stat(&vol_id, server_fullname, NULL, &st) != 0) {
        printf("File not found: %s\n", filename);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            ret = delete_directory(server_fullname);

            if (ret == 0) {
                printf("Deleted directory: %s\n", filename);
            }

            return ret;
        } else {
            printf("%s is a directory (start afpcmd with -r to delete recursively)\n",
                   filename);
            return -1;
        }
    }

    ret = afp_sl_unlink(&vol_id, server_fullname, NULL);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("File not found: %s\n", filename);
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied: %s\n", filename);
        } else {
            printf("Failed to delete %s (error: %d)\n", filename, ret);
        }

        return -1;
    }

    return 0;
}

int com_mkdir(char *arg)
{
    char dirname[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(dirname, NULL, arg)) {
        printf("expecting format: mkdir <dirname>\n");
        return -1;
    }

    if (get_server_path(dirname, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    /* Call the stateless library mkdir function with default directory permissions */
    ret = afp_sl_mkdir(&vol_id, server_fullname, NULL, 0755);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_EXIST) {
            printf("Directory already exists: %s\n", dirname);
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied: %s\n", dirname);
        } else if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("Parent directory not found: %s\n", dirname);
        } else {
            printf("Failed to create directory %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    return 0;
}

int com_rmdir(char *arg)
{
    char dirname[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct stat st;
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(dirname, NULL, arg)) {
        printf("expecting format: rmdir <dirname>\n");
        return -1;
    }

    if (get_server_path(dirname, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    ret = afp_sl_stat(&vol_id, server_fullname, NULL, &st);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("Directory not found: %s\n", dirname);
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied: %s\n", dirname);
        } else {
            printf("Failed to stat %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        printf("Not a directory: %s\n", dirname);
        return -1;
    }

    ret = afp_sl_rmdir(&vol_id, server_fullname, NULL);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("Directory not found: %s\n", dirname);
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied: %s\n", dirname);
        } else if (ret == AFP_SERVER_RESULT_ENOTEMPTY) {
            printf("Directory not empty: %s\n", dirname);
        } else {
            printf("Failed to remove directory %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    return 0;
}

int com_status(__attribute__((unused)) char *unused)
{
    char text[40960];
    unsigned int len = sizeof(text);
    int ret;
    ret = afp_sl_status(NULL, NULL, text, &len);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        printf("Could not get status (result=%d)\n", ret);
        return -1;
    }

    printf("%s", text);
    return 0;
}

int com_statvfs(__attribute__((unused)) char *unused)
{
    struct statvfs stat;
    char server_path[AFP_MAX_PATH];
    int ret;
    unsigned long long total_bytes, free_bytes;
    unsigned long long total_mb, free_mb;
    int percent_used;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    get_server_path(".", server_path);
    ret = afp_sl_statfs(&vol_id, server_path, NULL, &stat);

    if (ret != AFP_SERVER_RESULT_OKAY) {
        if (ret == AFP_SERVER_RESULT_ENOENT) {
            printf("Path not found\n");
        } else if (ret == AFP_SERVER_RESULT_ACCESS) {
            printf("Permission denied\n");
        } else {
            printf("Failed to get filesystem statistics (error: %d)\n", ret);
        }

        return -1;
    }

    total_bytes = (unsigned long long)stat.f_blocks * stat.f_frsize;
    free_bytes = (unsigned long long)stat.f_bfree * stat.f_frsize;
    total_mb = total_bytes / (1024 * 1024);
    free_mb = free_bytes / (1024 * 1024);

    if (total_bytes > 0) {
        percent_used = (int)(((total_bytes - free_bytes) * 100) / total_bytes);
    } else {
        percent_used = 0;
    }

    printf("Filesystem statistics for volume:\n");
    printf("  Total space:     %10llu MB\n", total_mb);
    printf("  Free space:      %10llu MB\n", free_mb);
    printf("  Used:            %10d%%\n", percent_used);
    return 0;
}


int com_lcd(char * path)
{
    int ret;
    char curpath[PATH_MAX];
    ret = chdir(path);

    if (ret != 0) {
        perror("Changing directories");
    } else {
        if (getcwd(curpath, PATH_MAX) == NULL) {
            perror("Getting current directory");
        } else {
            printf("Now in local directory %s\n", curpath);
        }
    }

    return ret;
}

/* Change to the directory ARG, or attach to volume if not attached. */
int com_cd(char *arg)
{
    char path[AFP_MAX_PATH];
    char dir_path[AFP_MAX_PATH];
    struct stat statbuf;
    size_t arg_len;
    int ret = -1;
    int show_dir = 0;

    if (!connected) {
        printf("You're not connected to a server\n");
        goto error;
    }

    if (!arg) {
        if (vol_id) {
            snprintf(curdir, AFP_MAX_PATH, "/");
            show_dir = 1;
        } else {
            list_volumes();
        }

        goto out;
    }

    arg_len = strnlen(arg, AFP_MAX_PATH);

    if (arg_len >= AFP_MAX_PATH) {
        printf("Path too long\n");
        goto error;
    }

    if (arg_len == 0) {
        if (vol_id) {
            snprintf(curdir, AFP_MAX_PATH, "/");
            show_dir = 1;
        } else {
            list_volumes();
        }

        goto out;
    }

    if (escape_paths(path, NULL, arg)) {
        printf("Invalid path\n");
        goto error;
    }

    if (vol_id == NULL) {
        /* Not attached to a volume, treat arg as volume name */
        strlcpy(url.volumename, path, AFP_VOLUME_NAME_LEN);
        unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        ret = attach_volume_with_password_prompt(&vol_id, volume_options);

        if (ret != AFP_SERVER_RESULT_OKAY
                && is_recoverable_session_error(ret) && reconnect_session(0, 0) == 0) {
            ret = attach_volume_with_password_prompt(&vol_id, volume_options);
        }

        if (ret != 0) {
            if (ret == AFP_SERVER_RESULT_VOLPASS_NEEDED) {
                printf("Could not attach to volume %s: authentication failed\n",
                       url.volumename);
            } else if (ret == AFP_SERVER_RESULT_NOVOLUME) {
                printf("Volume %s does not exist on this server\n", path);
            } else {
                printf("Could not attach to volume %s (error code: %d)\n",
                       url.volumename, ret);
            }

            url.volumename[0] = '\0';
            ret = -1;
            goto error;
        }

        printf("Attached to volume %s\n", url.volumename);
        snprintf(curdir, AFP_MAX_PATH, "/");
        goto out;
    }

    /* Attached to volume, treat arg as directory */

    if (strcmp(path, "..") == 0) {
        char *p = strrchr(curdir, '/');

        if (p && p != curdir) {
            *p = '\0';
        } else {
            snprintf(curdir, AFP_MAX_PATH, "/");
        }

        show_dir = 1;
        goto out;
    }

    if (strcmp(path, ".") == 0) {
        goto out;
    }

    if (path[0] == '/') {
        strlcpy(dir_path, path, AFP_MAX_PATH);
    } else {
        int path_len;

        if (strcmp(curdir, "/") == 0) {
            path_len = snprintf(dir_path, AFP_MAX_PATH, "/%s", path);
        } else {
            path_len = snprintf(dir_path, AFP_MAX_PATH, "%s/%s", curdir, path);
        }

        if (path_len < 0 || (size_t)path_len >= AFP_MAX_PATH) {
            printf("Path too long\n");
            goto error;
        }
    }

    ret = afp_sl_stat(&vol_id, dir_path, NULL, &statbuf);

    if (ret != AFP_SERVER_RESULT_OKAY && is_recoverable_session_error(ret)
            && reconnect_session(1, 1) == 0) {
        ret = afp_sl_stat(&vol_id, dir_path, NULL, &statbuf);
    }

    if (ret != AFP_SERVER_RESULT_OKAY) {
        printf("Directory not found: %s\n", dir_path);
        goto error;
    }

    if (!S_ISDIR(statbuf.st_mode)) {
        printf("Not a directory: %s\n", dir_path);
        goto error;
    }

    strlcpy(curdir, dir_path, AFP_MAX_PATH);
    show_dir = 1;
out:
    ret = 0;

    if (show_dir) {
        printf("Now in directory %s\n", curdir);
    }

    return ret;
error:
    return ret;
}

/* Disconnect command - explicitly detach volume and terminate server connection */
int com_disconnect(__attribute__((unused)) char *unused)
{
    if (!connected) {
        printf("You're not connected to a server\n");
        return -1;
    }

    if (vol_id) {
        afp_sl_detach(&vol_id, NULL);
        vol_id = NULL;
        explicit_bzero(url.volpassword, sizeof(url.volpassword));
    }

    if (server_id) {
        afp_sl_disconnect(&server_id);
        server_id = NULL;
    }

    afp_sl_exit();
    printf("Disconnected from %s\n", url.servername);
    connected = 0;
    return 0;
}

/* Exit command - detach from volume but remain connected */
int com_exit(__attribute__((unused)) char *unused)
{
    if (!connected) {
        printf("You're not connected to a server\n");
        return -1;
    }

    if (vol_id) {
        afp_sl_detach(&vol_id, NULL);
        vol_id = NULL;
        explicit_bzero(url.volpassword, sizeof(url.volpassword));
        printf("Detached from volume\n");
    }

    snprintf(curdir, AFP_MAX_PATH, "/");
    return 0;
}

/* Print out the current working directory locally. */
int com_lpwd(__attribute__((unused)) char *unused)
{
    char dir[PATH_MAX];

    if (getcwd(dir, PATH_MAX) == NULL) {
        perror("Getting current directory");
        return -1;
    }

    printf("Now in local directory %s\n", dir);
    return 0;
}

/* Print out the current working directory. */
int com_pwd(__attribute__((unused)) char *unused)
{
    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    printf("Now in directory %s\n", curdir);
    return 0;
}

void cmdline_set_log_level(int loglevel)
{
    cmdline_log_min_rank = loglevel_to_rank(loglevel);
}

void cmdline_set_verbose(int verbose)
{
    verbose_mode = verbose;
}

void cmdline_set_dsi_timeout(int timeout)
{
    cmdline_dsi_timeout = timeout;
}

static void cmdline_log_for_client(__attribute__((unused)) void * priv,
                                   __attribute__((unused)) enum logtypes logtype,
                                   int loglevel, const char *message)
{
    int type_rank = loglevel_to_rank(loglevel);

    if (type_rank < cmdline_log_min_rank) {
        return; /* Filter out less-verbose messages */
    }

    /* Log to syslog - priv is always NULL for cmdline */
    syslog(loglevel, "%s", message);
}

static struct libafpclient afpclient = {
    .unmount_volume = NULL,
    .log_for_client = cmdline_log_for_client,
    .forced_ending_hook = cmdline_forced_ending_hook,
    .scan_extra_fds = NULL,
    .loop_started = cmdline_loop_started,
};

static int cmdline_server_startup(int batch_mode)
{
    char mesg[MAX_ERROR_LEN];
    memset(mesg, 0, sizeof(mesg));
    int error = 0;
    unsigned int uam_mask;
    struct afp_server_basic basic;
    uam_mask = get_uam_mask_for_url();

    if (uam_mask == 0) {
        printf("I don't know about UAM %s\n", url.uamname);
        exit(1);
    }

    if (connect_servername[0] == '\0') {
        strlcpy(connect_servername, url.servername, sizeof(connect_servername));
    }

    if (afp_sl_connect(&url, uam_mask, &server_id, mesg, &error,
                       cmdline_dsi_timeout)) {
        printf("Could not connect to server\n");
        return -1;
    }

    connected = 1;

    if (afp_sl_serverinfo(&url, &basic) == 0
            && basic.server_name_printable[0] != '\0') {
        snprintf(url.servername, sizeof(url.servername), "%s",
                 basic.server_name_printable);
    }

    printf("Connected to server %s\n", url.servername);

    if (url.volumename[0] != '\0') {
        unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        int ret;
        ret = attach_volume_with_password_prompt(&vol_id, volume_options);

        if (ret != 0) {
            if (ret == AFP_SERVER_RESULT_VOLPASS_NEEDED) {
                printf("Could not attach to volume %s: authentication failed\n",
                       url.volumename);
            } else if (ret == AFP_SERVER_RESULT_NOVOLUME) {
                printf("Volume %s does not exist on this server\n", url.volumename);
            } else {
                printf("Could not attach to volume %s (error code: %d)\n",
                       url.volumename, ret);
            }

            return -1;
        }

        if (url.path[0] != '\0') {
            snprintf(curdir, AFP_MAX_PATH, "%s", url.path);
        } else {
            snprintf(curdir, AFP_MAX_PATH, "/");
        }

        /* In non-batch mode, validate that the path (if provided) is a directory */
        if (!batch_mode && url.path[0] != '\0') {
            struct stat st;

            if (afp_sl_stat(&vol_id, url.path, NULL, &st) != 0) {
                printf("Error: Cannot access path: %s\n", url.path);
                return -1;
            }

            if (!S_ISDIR(st.st_mode)) {
                printf("Error: Path points to a file, not a directory. Interactive mode requires a directory path.\n");
                return -1;
            }
        }
    } else {
        printf("Use 'ls' to list available volumes, 'cd' to attach to a volume\n");
    }

    return 0;
}

int cmdline_batch_transfer(char * local_path, int direction, int recursive)
{
    size_t local_path_len;
    unsigned long long bytes_transferred = 0;
    int ret = -1;

    if (!connected) {
        printf("Not connected to server.\n");
        goto error;
    }

    if (!vol_id) {
        printf("Not connected to a volume. URL must include volume name.\n");
        goto error;
    }

    /* Validate local_path parameter */
    if (!local_path) {
        printf("Invalid local path.\n");
        goto error;
    }

    local_path_len = strnlen(local_path, PATH_MAX);

    if (local_path_len >= PATH_MAX) {
        printf("Local path too long.\n");
        goto error;
    }

    /* direction: 0 = GET (remote->local), 1 = PUT (local->remote) */
    if (direction == 0) {
        struct stat st;
        char remote_path[AFP_MAX_PATH];

        if (url.path[0] == '\0' || url.path[1] == '\0') { /* Empty or just "/" */
            strlcpy(remote_path, "/", sizeof(remote_path));
        } else {
            if (strlcpy(remote_path, url.path,
                        sizeof(remote_path)) >= sizeof(remote_path)) {
                printf("Warning: remote path truncated\n");
            }
        }

        if (afp_sl_stat(&vol_id, remote_path, NULL, &st) != 0) {
            printf("Remote path not found: %s\n", remote_path);
            goto error;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                printf("Remote path is a directory (start afpcmd with -r to download recursively)\n");
                goto error;
            }

            ret = download_directory(remote_path, local_path, &bytes_transferred);
            goto out;
        } else {
            /* It's a file. If local_path is a directory, append filename. */
            struct stat local_st;
            char dest_path[PATH_MAX];

            if (stat(local_path, &local_st) == 0 && S_ISDIR(local_st.st_mode)) {
                char *base = strrchr(remote_path, '/');

                if (base) {
                    base++;
                } else {
                    base = remote_path;
                }

                /* Use validated local_path_len from function entry */
                int path_len;

                if (local_path_len > 0 && local_path[local_path_len - 1] == '/') {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s%s", local_path, base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", local_path, base);
                }

                if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                    printf("Destination path too long\n");
                    goto error;
                }
            } else {
                snprintf(dest_path, sizeof(dest_path), "%s", local_path);
            }

            int fd = open(dest_path, O_CREAT | O_TRUNC | O_RDWR, 0644);

            if (fd < 0) {
                perror("open");
                goto error;
            }

            ret = retrieve_file(remote_path, fd, &st, &bytes_transferred);
            close(fd);
            goto out;
        }
    } else {
        /* PUT: url.path is the destination directory */
        struct stat st;

        if (stat(local_path, &st) != 0) {
            perror("stat");
            goto error;
        }

        char remote_base[AFP_MAX_PATH];

        if (url.path[0] == '\0') {
            strlcpy(remote_base, "/", AFP_MAX_PATH);
        } else {
            strlcpy(remote_base, url.path, AFP_MAX_PATH);
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                printf("Local path is a directory (start afpcmd with -r to upload recursively)\n");
                goto error;
            }

            char *base = basename(local_path);
            char dest_path[AFP_MAX_PATH];

            /* If local path is "." or equivalent, upload contents directly to remote_base */
            if (strcmp(base, ".") == 0) {
                strlcpy(dest_path, remote_base, AFP_MAX_PATH);
            } else {
                int path_len;

                if (strcmp(remote_base, "/") == 0) {
                    path_len = snprintf(dest_path, sizeof(dest_path), "/%s", base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", remote_base, base);
                }

                if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                    printf("Destination path too long\n");
                    goto error;
                }
            }

            ret = upload_directory(local_path, dest_path, &bytes_transferred);
            goto out;
        } else {
            char *base = basename(local_path);
            char dest_path[AFP_MAX_PATH];
            struct stat remote_st;
            int is_dir = 0;
            int path_len;

            if (afp_sl_stat(&vol_id, remote_base, NULL, &remote_st) == 0
                    && S_ISDIR(remote_st.st_mode)) {
                is_dir = 1;
            }

            if (is_dir) {
                if (strcmp(remote_base, "/") == 0) {
                    path_len = snprintf(dest_path, sizeof(dest_path), "/%s", base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", remote_base, base);
                }
            } else {
                path_len = snprintf(dest_path, sizeof(dest_path), "%s", remote_base);
            }

            if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                printf("Destination path too long\n");
                goto error;
            }

            ret = upload_file(local_path, dest_path, &bytes_transferred);
            goto out;
        }
    }

out:

    if (bytes_transferred > 0) {
        if (direction == 0) {
            printf("Transfer complete. %llu bytes received.\n", bytes_transferred);
        } else {
            printf("Transfer complete. %llu bytes sent.\n", bytes_transferred);
        }
    }

error:
    return ret;
}

void cmdline_afp_exit(void)
{
    /* Drop our socket and let the daemon clean up the client slot
     * while keeping connections alive. Other clients may be using
     * the same daemon with the same server connection. */
    vol_id = NULL;
    server_id = NULL;
    connected = 0;
}

void cmdline_afp_setup_client(void)
{
    openlog("afpcmd", LOG_PID | LOG_CONS, LOG_USER);
    libafpclient_register(&afpclient);
}


int cmdline_afp_setup(int recursive, int batch_mode, char * url_string)
{
    recursive_mode = recursive;
    snprintf(curdir, AFP_MAX_PATH, "%s", DEFAULT_DIRECTORY);
    memset(connect_servername, 0, sizeof(connect_servername));

    if (init_uams() < 0) {
        return -1;
    }

    afp_default_url(&url);

    if (url_string) {
        size_t url_len = strnlen(url_string, MAX_INPUT_LEN);

        if (url_len >= MAX_INPUT_LEN) {
            printf("URL too long.\n");
            return -1;
        }

        if (url_len > 1) {
            if (afp_parse_url(&url, url_string)) {
                printf("Could not parse url.\n");
                return -1;
            }

            /* If no username was specified in URL, use AFP guest user */
            if (url.username[0] == '\0') {
                strlcpy(url.username, "nobody", AFP_MAX_USERNAME_LEN);
            }

            strlcpy(connect_servername, url.servername, sizeof(connect_servername));
            cmdline_getpass();
            trigger_connected();

            if (cmdline_server_startup(batch_mode) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static char *escape_spaces(const char *str)
{
    size_t len, spaces = 0;

    if (!str) {
        return NULL;
    }

    len = strnlen(str, AFP_MAX_PATH);

    if (len >= AFP_MAX_PATH) {
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        if (str[i] == ' ') {
            spaces++;
        }
    }

    if (!spaces) {
        return strdup(str);
    }

    if (len > ((size_t) -1) - spaces - 1) {
        return NULL;
    }

    char *ret = malloc(len + spaces + 1);

    if (!ret) {
        return NULL;
    }

    char *dst = ret;
    const char *end = ret + len + spaces;
    const char *src = str;

    while (*src) {
        if (*src == ' ') {
            if (dst >= end) {
                break;
            }

            *dst++ = '\\';
        }

        if (dst >= end) {
            break;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
    return ret;
}

/* Helper to unescape backslash-escaped spaces in completion text */
static char *unescape_spaces(const char *str)
{
    if (!str) {
        return NULL;
    }

    size_t len = strnlen(str, AFP_MAX_PATH);

    if (len >= AFP_MAX_PATH) {
        return NULL;
    }

    /* Quick check: if no backslashes, just duplicate */
    int has_escape = 0;

    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\\' && i + 1 < len && str[i + 1] == ' ') {
            has_escape = 1;
            break;
        }
    }

    if (!has_escape) {
        return strdup(str);
    }

    char *ret = malloc(len + 1);

    if (!ret) {
        return NULL;
    }

    char *dst = ret;
    const char *src = str;

    while (*src) {
        if (*src == '\\' && *(src + 1) == ' ') {
            *dst++ = ' ';
            src += 2;  /* skip both \ and space */
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return ret;
}

char *afp_remote_file_generator(const char *text, int state)
{
    static struct afp_file_info_basic *filebase = NULL;
    static struct afp_volume_summary *volbase = NULL;
    static unsigned int count = 0;
    static unsigned int list_index = 0;
    static int len = 0;
    static int is_volume_list = 0;
    static char *basename_unescaped = NULL;
    /* Number of unescaped chars already in the buffer before readline's word
       start; non-zero when the library split the word at a backslash-escaped
       space (e.g. libedit gives text="q" for input "cd asdf\ q"). */
    static int return_offset = 0;
    const char *name;
    char *ret_str = NULL;

    if (!state) {
        /* Clean up from previous completion */
        if (basename_unescaped) {
            free(basename_unescaped);
            basename_unescaped = NULL;
        }

        if (filebase) {
            free(filebase);
            filebase = NULL;
        }

        if (volbase) {
            free(volbase);
            volbase = NULL;
        }

        count = 0;
        list_index = 0;
        return_offset = 0;

        if (!text) {
            return NULL;
        }

        size_t text_len = strnlen(text, MAX_INPUT_LEN);

        if (text_len >= MAX_INPUT_LEN) {
            return NULL;
        }

        is_volume_list = 0;

        if (!connected) {
            return NULL;
        }

        if (!vol_id) {
            unsigned int numvols = 0;
            volbase = malloc(sizeof(struct afp_volume_summary) * 100);

            if (!volbase) {
                return NULL;
            }

            /* Set up basename matching so the loop doesn't use stale values
               from a previous file-list completion. */
            basename_unescaped = unescape_spaces(text);

            if (!basename_unescaped) {
                free(volbase);
                return NULL;
            }

            len = strlen(basename_unescaped);

            if (afp_sl_getvols(&url, 0, 100, &numvols, volbase) == 0) {
                count = numvols;
                is_volume_list = 1;
            } else {
                free(volbase);
                volbase = NULL;
                return NULL;
            }
        } else {
            char dir_path[AFP_MAX_PATH];
            const char *last_slash = strrchr(text, '/');
            char prefix[AFP_MAX_PATH] = {0};

            if (last_slash) {
                int dir_len = last_slash - text;
                int path_len;
                memcpy(prefix, text, dir_len);
                prefix[dir_len] = '\0';

                if (text[0] == '/') {
                    if (dir_len == 0) {
                        strlcpy(dir_path, "/", sizeof(dir_path));
                    } else {
                        path_len = snprintf(dir_path, sizeof(dir_path), "%s", prefix);

                        if (path_len < 0 || (size_t)path_len >= sizeof(dir_path)) {
                            return NULL;
                        }
                    }
                } else {
                    if (strcmp(curdir, "/") == 0) {
                        path_len = snprintf(dir_path, sizeof(dir_path), "/%s", prefix);
                    } else {
                        path_len = snprintf(dir_path, sizeof(dir_path), "%s/%s", curdir, prefix);
                    }

                    if (path_len < 0 || (size_t)path_len >= sizeof(dir_path)) {
                        return NULL;
                    }
                }

                /* Extract and unescape just the basename for matching */
                const char *basename_part = last_slash + 1;
                basename_unescaped = unescape_spaces(basename_part);

                if (!basename_unescaped) {
                    return NULL;
                }

                len = strnlen(basename_unescaped, text_len - (last_slash - text + 1));
            } else {
                strlcpy(dir_path, curdir, sizeof(dir_path));
                /* Some readline-compatible libraries (e.g. libedit) split
                   the word at backslash-escaped spaces, so for input like
                   "cd asdf\ q<TAB>" they pass text="q" instead of the full
                   "asdf\ q".  Detect this by scanning rl_line_buffer backward
                   from where the library put the word start; if there are
                   one or more "\ " sequences immediately before it, reconstruct
                   the full unescaped prefix for matching and remember how many
                   unescaped chars are already in the buffer (return_offset) so
                   we return only the suffix readline needs to insert. */
                int readline_start = rl_point - (int)text_len;
                int true_start = readline_start;

                while (true_start >= 2 &&
                        rl_line_buffer[true_start - 1] == ' ' &&
                        rl_line_buffer[true_start - 2] == '\\') {
                    true_start -= 2;

                    while (true_start > 0 &&
                            rl_line_buffer[true_start - 1] != ' ') {
                        true_start--;
                    }
                }

                if (true_start < readline_start) {
                    int raw_prefix_len = readline_start - true_start;
                    char escaped_prefix[AFP_MAX_PATH];

                    if (raw_prefix_len >= AFP_MAX_PATH) {
                        return NULL;
                    }

                    memcpy(escaped_prefix, rl_line_buffer + true_start, raw_prefix_len);
                    escaped_prefix[raw_prefix_len] = '\0';
                    char *unescaped_prefix = unescape_spaces(escaped_prefix);

                    if (!unescaped_prefix) {
                        return NULL;
                    }

                    char *unescaped_text = unescape_spaces(text);

                    if (!unescaped_text) {
                        free(unescaped_prefix);
                        return NULL;
                    }

                    return_offset = (int)strlen(unescaped_prefix);
                    size_t combined_len = return_offset + strlen(unescaped_text);
                    basename_unescaped = malloc(combined_len + 1);

                    if (!basename_unescaped) {
                        free(unescaped_prefix);
                        free(unescaped_text);
                        return NULL;
                    }

                    snprintf(basename_unescaped, combined_len + 1, "%s%s",
                             unescaped_prefix, unescaped_text);
                    free(unescaped_prefix);
                    free(unescaped_text);
                } else {
                    /* Normal case: unescape the whole text for matching */
                    basename_unescaped = unescape_spaces(text);

                    if (!basename_unescaped) {
                        return NULL;
                    }
                }

                len = strlen(basename_unescaped);
            }

            int eod = 0;

            if (afp_sl_readdir(&vol_id, dir_path, NULL, 0, 1000, &count, &filebase,
                               &eod) != 0) {
                return NULL;
            }
        }
    }

    while (list_index < count) {
        if (is_volume_list) {
            name = volbase[list_index].volume_name_printable;
        } else {
            name = filebase[list_index].name;
        }

        list_index++;

        if (strncmp(name, basename_unescaped, len) == 0) {
            /* Return only the suffix after the prefix already in the buffer.
               In the normal case return_offset is 0 and the full escaped name
               is returned.  When the library split at an escaped space,
               return_offset > 0 and we skip the part already typed. */
            char *escaped_name = escape_spaces(name + return_offset);

            if (!escaped_name) {
                return NULL;
            }

            if (!is_volume_list) {
                const char *last_slash = strrchr(text, '/');

                if (last_slash) {
                    int dir_len = last_slash - text + 1;
                    size_t total_len = dir_len + strlen(escaped_name) + 1;
                    ret_str = malloc(total_len);

                    if (ret_str) {
                        snprintf(ret_str, total_len, "%.*s%s", dir_len, text, escaped_name);
                    }

                    free(escaped_name);
                    return ret_str;
                }
            }

            return escaped_name;
        }
    }

    return NULL;
}
