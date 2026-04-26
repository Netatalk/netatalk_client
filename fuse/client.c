/*
 *  client.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <limits.h>

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp.h"
#include "fuse_ipc.h"
#include "uams_def.h"
#include "map_def.h"
#include "libafpclient.h"
#include "utils.h"

#define default_uam "Cleartxt Passwrd"

#define MAX_OUTGOING_LENGTH 8192

#define AFPFSD_FILENAME "afpfsd"
#define DEFAULT_MOUNT_FLAGS (VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE|\
	VOLUME_EXTRA_FLAGS_NO_LOCKING | VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS)

static char outgoing_buffer[MAX_OUTGOING_LENGTH];
static int outgoing_len = 0;
static unsigned int uid, gid = 0;
static int changeuid = 0;
static int changegid = 0;
static char *thisbin;
static int client_log_min_rank = 2; /* Default to LOG_NOTICE */

/* Log handler that filters by log level */
static void client_log_for_client(__attribute__((unused)) void *priv,
                                  __attribute__((unused)) enum logtypes logtype,
                                  int loglevel, const char *message)
{
    int type_rank = loglevel_to_rank(loglevel);

    if (type_rank < client_log_min_rank) {
        return; /* Filter out less-verbose messages */
    }

    printf("%s\n", message);
}

static struct libafpclient client = {
    .unmount_volume = NULL,
    .log_for_client = client_log_for_client,
    .forced_ending_hook = NULL,
    .scan_extra_fds = NULL,
    .loop_started = NULL,
};

/* Forward declaration for get_daemon_filename */
static void get_daemon_filename(char *filename, size_t size,
                                const char *mountpoint);

/* SIGCHLD handler to reap child processes and prevent zombies */
static void sigchld_handler(int sig)
{
    (void)sig;  /* Unused parameter */
    int status;

    /* Reap all available child processes */
    while (waitpid(-1, &status, WNOHANG) > 0) {
        /* Child has been reaped */
    }
}

static int start_manager_daemon(void)
{
    char *argv[4];
    int argc = 0;
    struct sigaction sa, old_sa;
    argv[argc++] = AFPFSD_FILENAME;
    argv[argc++] = "--manager";
    argv[argc] = NULL;
    /* Temporarily install SIGCHLD handler to reap any child process failures */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, &old_sa);

    if (fork() == 0) {
        char filename[PATH_MAX];

        if (changegid) {
            if (setegid(gid)) {
                perror("Changing gid");
                _exit(1);
            }
        }

        if (changeuid) {
            if (seteuid(uid)) {
                perror("Changing uid");
                _exit(1);
            }
        }

        snprintf(filename, PATH_MAX, AFPFSD_FILENAME);

        if (getenv("PATH") == NULL) {
            /* If we don't have an PATH set, it is probably
               becaue we are being called from mount,
               so go search for it */
            snprintf(filename, PATH_MAX,
                     "/usr/local/bin/%s", AFPFSD_FILENAME);

            if (access(filename, X_OK)) {
                snprintf(filename, sizeof(filename), "/usr/bin/%s",
                         AFPFSD_FILENAME);
                filename[sizeof(filename) - 1] = 0;

                if (access(filename, X_OK)) {
                    fprintf(stderr, "Could not find server (%s)\n",
                            filename);
                    _exit(1);
                }
            }
        }

        if (execvp(filename, argv)) {
            if (errno == ENOENT) {
                /* Try the path of afp_client */
                char newpath[PATH_MAX];
                snprintf(newpath, PATH_MAX, "%s/%s",
                         (char *)basename(thisbin), AFPFSD_FILENAME);

                if (execvp(newpath, argv)) {
                    perror("Starting up afpfsd manager");
                    _exit(1);
                }
            } else {
                perror("Starting up afpfsd manager");
                _exit(1);
            }
        }

        /* execvp never returns on success */
        _exit(1);
    }

    /* Restore old signal handler */
    sigaction(SIGCHLD, &old_sa, NULL);
    return 0;
}

static int start_afpfsd(const char *mountpoint, const char *volumename)
{
    int sock;
    struct sockaddr_un servaddr;
    char manager_socket[PATH_MAX];
    char mount_socket[PATH_MAX];
    struct afp_server_spawn_mount_request req;
    unsigned char command = AFP_SERVER_COMMAND_SPAWN_MOUNT;
    unsigned char result;
    /* Get manager socket name (NULL = shared socket) */
    get_daemon_filename(manager_socket, sizeof(manager_socket), NULL);
    /* Get mount-specific socket name */
    get_daemon_filename(mount_socket, sizeof(mount_socket), mountpoint);

    /* Try to connect to manager daemon */
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("Could not create socket");
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;

    if (strlcpy(servaddr.sun_path, manager_socket,
                sizeof(servaddr.sun_path)) >= sizeof(servaddr.sun_path)) {
        close(sock);
        fprintf(stderr, "Manager socket path too long\n");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&servaddr,
                sizeof(servaddr.sun_family) + sizeof(servaddr.sun_path)) < 0) {
        /* Manager not running, start it */
        close(sock);

        if (start_manager_daemon() != 0) {
            return -1;
        }

        /* Wait for manager to start */
        sleep(1);

        /* Reconnect to manager */
        if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
            return -1;
        }

        if (connect(sock, (struct sockaddr *)&servaddr,
                    sizeof(servaddr.sun_family) + sizeof(servaddr.sun_path)) < 0) {
            close(sock);
            perror("Could not connect to manager daemon");
            return -1;
        }
    }

    /* Send spawn mount request */
    memset(&req, 0, sizeof(req));
    snprintf(req.mountpoint, sizeof(req.mountpoint), "%s", mountpoint);
    snprintf(req.socket_id, sizeof(req.socket_id), "%s", mount_socket);
    snprintf(req.volumename, sizeof(req.volumename), "%s",
             volumename ? volumename : "");

    if (write(sock, &command, 1) != 1) {
        close(sock);
        return -1;
    }

    if (write(sock, &req, sizeof(req)) != sizeof(req)) {
        close(sock);
        return -1;
    }

    /* Wait for response */
    if (read(sock, &result, 1) != 1) {
        close(sock);
        return -1;
    }

    close(sock);

    if (result != AFP_SERVER_RESULT_OKAY) {
        return -1;
    }

    return 0;
}


/* Each mount gets a unique daemon process for fault isolation.
 * Management commands use NULL mountpoint to get shared management socket. */
static void get_daemon_filename(char *filename, size_t size,
                                const char *mountpoint)
{
    unsigned long hash = 5381;

    if (mountpoint) {
        /* Hash the mountpoint path to create unique socket per mount */
        for (const char *p = mountpoint; *p; p++) {
            hash = ((hash << 5) + hash) ^ (unsigned char) * p;
        }

        /* One daemon per mountpoint */
        snprintf(filename, size, "%s-%d-%lx", SERVER_FUSE_SOCKET_PATH, uid, hash);
    } else {
        /* Shared management socket for status/exit commands */
        snprintf(filename, size, "%s-%d", SERVER_FUSE_SOCKET_PATH, uid);
    }
}

static int daemon_connect(const char *mountpoint, const char *volumename)
{
    int sock;
    struct sockaddr_un servaddr;
    char filename[PATH_MAX];
    unsigned char trying = 2;

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("Could not create socket\n");
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;
    get_daemon_filename(filename, sizeof(filename), mountpoint);

    if (strlcpy(servaddr.sun_path, filename,
                sizeof(servaddr.sun_path)) >= sizeof(servaddr.sun_path)) {
        close(sock);
        fprintf(stderr, "Socket path too long\n");
        return -1;
    }

    while (trying) {
        if ((connect(sock, (struct sockaddr *) &servaddr,
                     sizeof(servaddr.sun_family) +
                     sizeof(servaddr.sun_path))) >= 0) {
            goto done;
        }

        if (start_afpfsd(mountpoint, volumename) != 0) {
            perror("Error in starting up afpfsd\n");
            goto error;
        }

        if ((connect(sock, (struct sockaddr *) &servaddr,
                     sizeof(servaddr.sun_family) +
                     sizeof(servaddr.sun_path))) >= 0) {
            goto done;
        }

        sleep(1);
        trying--;
    }

error:
    perror("Trying to startup afpfsd");
    return -1;
done:
    return sock;
}


static void usage(void)
{
    printf(
        "afp_client <command> [options]\n"
        "    mount [mountopts] <server>:<volume> <mountpoint>\n"
        "         mount options:\n"
        "         -u, --user <username>      : log in as user <username>\n"
        "         -p, --pass <password>      : use <password>\n"
        "                                      If password is '-', you get prompted for it\n"
        "         -P, --volpass <password>   : use this volume password\n"
        "                                      If password is '-', you get prompted for it\n"
        "         -o, --port <portnum>       : connect using <portnum> instead of 548\n"
        "         -v, --afpversion <version> : set the AFP version, eg. 3.1\n"
        "         -a, --uam <uam>            : use this authentication method, one of:\n"
        "                                      guest, clrtxt, randnum, randnum2, dhx, dhx2\n"
        "         -m, --map <mapname>        : use this uid/gid mapping method, one of:\n"
        "                                      common, loginids\n"
        "         -O, --options <flags>      : FUSE mount options; see the fuse man page\n"
        "         -t, --timeout <seconds>    : DSI request timeout (overrides server-type default)\n"
        "\n"
        "    unmount <mountpoint> : unmount the specified mountpoint\n"
        "    status [mountpoint]  : get status of the AFP daemon;\n"
        "                           If mountpoint is specified, show detailed\n"
        "                           status for that specific mount\n"
        "    suspend <mountpoint> : suspends the connection to the server, but\n"
        "                           maintains the mount.  For laptop suspend/resume\n"
        "    resume  <mountpoint> : resumes the server connection\n"
        "    exit                 : unmounts all volumes and exits afpfsd\n"
    );
}


static char *get_password(const char *prompt)
{
    if (isatty(fileno(stdin))) {
        return getpass(prompt);
    } else {
        char *askpass = NULL;
        static char pwd[AFP_MAX_PASSWORD_LEN + 1];
        FILE *fp;

        if (asprintf(&askpass, "ssh-askpass %s", prompt) < 0) {
            return NULL;
        }

        if ((fp = popen(askpass, "r"))) {
            size_t len = fread(pwd, 1, sizeof(pwd) - 1, fp);
            pwd[len] = '\0';
            pclose(fp);

            /* ssh-askpass always adds a newline: chop it. */
            if (len > 0 && pwd[len - 1] == '\n') {
                pwd[len - 1] = '\0';
            }
        } else {
            perror(askpass);
            memset(pwd, (int) sizeof(pwd), (0));
        }

        free(askpass);
        return pwd;
    }
}

static int send_command(int sock, char * msg, int len)
{
    return write(sock, msg, len);
}

static int do_exit(__attribute__((unused)) int argc,
                   __attribute__((unused)) char **argv)
{
    outgoing_len = 1;
    outgoing_buffer[0] = AFP_SERVER_COMMAND_EXIT;
    return 0;
}

/* Resolve mountpoint to absolute path */
static int resolve_mountpoint(const char *path, char *resolved, size_t size)
{
    char *result;

    /* If already absolute, just copy it */
    if (path[0] == '/') {
        snprintf(resolved, size, "%s", path);
        return 0;
    }

    /* Resolve relative path */
    result = realpath(path, NULL);

    if (result) {
        snprintf(resolved, size, "%s", result);
        free(result);
        return 0;
    }

    /* realpath failed - path might not exist yet, build absolute path manually */
    char cwd[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return -1;
    }

    /* Check if combined path fits in buffer */
    size_t needed = strlen(cwd) + 1 + strlen(path) + 1;

    if (needed > size) {
        fprintf(stderr, "Mountpoint path too long\n");
        return -1;
    }

    int ret = snprintf(resolved, size, "%s/%s", cwd, path);

    if (ret < 0 || (size_t)ret >= size) {
        fprintf(stderr, "Mountpoint path formatting error\n");
        return -1;
    }

    return 0;
}

static int do_status(int argc, char ** argv)
{
    int c;
    int option_index = 0;
    struct afp_server_status_request req = {0};
    struct option long_options[] = {
        {"volume", 1, 0, 'v'},
        {"server", 1, 0, 's'},
        {0, 0, 0, 0},
    };
    outgoing_buffer[0] = AFP_SERVER_COMMAND_STATUS;
    outgoing_len = sizeof(struct afp_server_status_request) + 1;

    while (1) {
        c = getopt_long(argc, argv, "v:s:", long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'v':
            snprintf(req.volumename, AFP_VOLUME_NAME_UTF8_LEN, "%s", optarg);
            break;
        }
    }

    /* Check for optional mountpoint argument */
    if (argc > 2
            && resolve_mountpoint(argv[2], req.mountpoint, sizeof(req.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &req, sizeof(req));
    return 0;
}

static int do_resume(int argc, char ** argv)
{
    struct afp_server_resume_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFP_SERVER_COMMAND_RESUME;
    outgoing_len = sizeof(struct afp_server_resume_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static int do_suspend(int argc, char ** argv)
{
    struct afp_server_suspend_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFP_SERVER_COMMAND_SUSPEND;
    outgoing_len = sizeof(struct afp_server_suspend_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static int do_unmount(int argc, char ** argv)
{
    struct afp_server_unmount_request request = {0};

    if (argc < 3) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFP_SERVER_COMMAND_UNMOUNT;
    outgoing_len = sizeof(struct afp_server_unmount_request) + 1;

    if (resolve_mountpoint(argv[2], request.mountpoint,
                           sizeof(request.mountpoint)) < 0) {
        fprintf(stderr, "Failed to resolve mountpoint\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static int do_mount(int argc, char ** argv)
{
    int c;
    int option_index = 0;
    struct afp_server_mount_request request = {0};
    int optnum = 0;
    unsigned int uam_mask = default_uams_mask();
    struct option long_options[] = {
        {"afpversion", 1, 0, 'v'},
        {"volpass", 1, 0, 'P'},
        {"user", 1, 0, 'u'},
        {"pass", 1, 0, 'p'},
        {"port", 1, 0, 'o'},
        {"uam", 1, 0, 'a'},
        {"map", 1, 0, 'm'},
        {"options", 1, 0, 'O'},
        {"timeout", 1, 0, 't'},
        {0, 0, 0, 0},
    };

    if (argc < 4) {
        usage();
        return -1;
    }

    outgoing_buffer[0] = AFP_SERVER_COMMAND_MOUNT;
    outgoing_len = sizeof(struct afp_server_mount_request) + 1;
    request.url.port = 548;
    request.map = AFP_MAPPING_UNKNOWN;
    request.fuse_options[0] = '\0';

    while (1) {
        optnum++;
        c = getopt_long(argc, argv, "a:m:O:o:P:p:t:u:v:", long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'a':
            uam_mask = uam_string_to_bitmap(optarg);
            break;

        case 'm':
            request.map = map_string_to_num(optarg);

            /* Validate: if user explicitly provided a value and we got UNKNOWN,
             * they provided an invalid mapping name */
            if (request.map == AFP_MAPPING_UNKNOWN &&
                    strcasecmp(optarg, "unknown") != 0) {
                printf("Invalid mapping method: %s\n", optarg);
                printf("Valid options: \"common\", \"loginids\", "
                       "\"Common user directory\", \"Login ids\"\n");
                return -1;
            }

            break;

        case 'O':
            snprintf(request.fuse_options, sizeof(request.fuse_options), "%s", optarg);
            break;

        case 'o':
            request.url.port = strtol(optarg, NULL, 10);
            break;

        case 'P':
            snprintf(request.url.volpassword, 9, "%s", optarg);
            break;

        case 'p':
            snprintf(request.url.password, AFP_MAX_PASSWORD_LEN, "%s", optarg);
            break;

        case 't':
            request.dsi_timeout = strtol(optarg, NULL, 10);
            break;

        case 'u':
            snprintf(request.url.username, AFP_MAX_USERNAME_LEN, "%s", optarg);
            break;

        case 'v': {
            /* Parse AFP version string like "3.1" or "22" to integer format.
             * AFP 2.2 = 22, AFP 3.1 = 31, AFP 3.2 = 32, etc. */
            int major, minor;

            if (strchr(optarg, '.')) {
                /* Format like "3.1" */
                if (sscanf(optarg, "%d.%d", &major, &minor) == 2) {
                    request.url.requested_version = major * 10 + minor;
                } else {
                    printf("Invalid AFP version format: %s\n", optarg);
                    return -1;
                }
            } else {
                /* Format like "31" or "22" */
                request.url.requested_version = strtol(optarg, NULL, 10);
            }

            break;
        }
        }
    }

    /* Handle password prompts - also prompt when username is given
     * but password is empty (without username, guest auth is used) */
    if (strcmp(request.url.password, "-") == 0 ||
            (request.url.username[0] != '\0'
             && request.url.password[0] == '\0')) {
        char *p = get_password("Password: ");

        if (p) {
            snprintf(request.url.password, AFP_MAX_PASSWORD_LEN, "%s", p);
        }
    }

    if (strcmp(request.url.volpassword, "-") == 0) {
        char *p = get_password("Volume password:");

        if (p) {
            snprintf(request.url.volpassword, 9, "%s", p);
        }
    }

    optnum = optind + 1;

    if (optnum >= argc) {
        printf("No volume or mount point specified\n");
        return -1;
    }

    if (sscanf(argv[optnum++], "%[^':']:%[^':']",
               request.url.servername, request.url.volumename) != 2) {
        printf("Incorrect server:volume specification\n");
        return -1;
    }

    if (uam_mask == 0) {
        printf("Unknown UAM\n");
        return -1;
    }

    request.uam_mask = uam_mask;
    request.volume_options = DEFAULT_MOUNT_FLAGS;

    if (optnum >= argc) {
        printf("No mount point specified\n");
        return -1;
    }

    if (resolve_mountpoint(argv[optnum], request.mountpoint, 255) < 0) {
        printf("Failed to resolve mount point\n");
        return -1;
    }

    memcpy(outgoing_buffer + 1, &request, sizeof(request));
    return 0;
}

static void mount_afpfs_usage(void)
{
    printf("afpfs-ng %s - mount an Apple Filing Protocol network filesystem with FUSE\n"
           "Usage:\n"
           "\tmount_afpfs [-o volpass=password] <afp url> <mountpoint>\n", AFPFS_VERSION);
}

static int handle_mount_afpfs(int argc, char * argv[])
{
    struct afp_server_mount_request * req = (struct afp_server_mount_request *)
                                            &outgoing_buffer[1];
    unsigned int uam_mask = default_uams_mask();
    char *urlstring, *mountpoint;
    char *volpass = NULL;
    int readonly = 0;
    char *temp_url = NULL;

    if (argc < 2) {
        mount_afpfs_usage();
        return -1;
    }

    if (strncmp(argv[1], "-o", 2) == 0) {
        char *p, *q;
        char command[256];
        struct passwd * passwd;
        struct group * group;

        if (argc < 3) {
            printf("Option -o requires an argument\n");
            return -1;
        }

        p = argv[2];

        do {
            memset(command, 0, 256);

            if ((q = strchr(p, ','))) {
                strlcpy(command, p, (q - p) + 1);
            } else {
                strlcpy(command, p, sizeof(command));
            }

            if (strncmp(command, "volpass=", 8) == 0) {
                volpass = p + 8;
            } else if (strncmp(command, "user=", 5) == 0) {
                p = command + 5;

                if ((passwd = getpwnam(p)) == NULL) {
                    printf("Unknown user %s\n", p);
                    return -1;
                }

                uid = passwd->pw_uid;

                if (geteuid() != uid) {
                    changeuid = 1;
                }
            } else if (strncmp(command, "group=", 6) == 0) {
                p = command + 6;

                if ((group = getgrnam(p)) == NULL) {
                    printf("Unknown group %s\n", p);
                    return -1;
                }

                gid = group->gr_gid;
                changegid = 1;
            } else if (strcmp(command, "rw") == 0) {
                /* Don't do anything */
            } else if (strcmp(command, "ro") == 0) {
                readonly = 1;
            } else {
                printf("Unknown option %s, skipping\n", command);
            }

            if (q) {
                p = q + 1;
            } else {
                p = NULL;
            }
        } while (p);

        urlstring = argv[3];
        mountpoint = argv[4];
    } else {
        urlstring = argv[1];
        mountpoint = argv[2];
    }

    outgoing_len = sizeof(struct afp_server_mount_request) +1;
    memset(outgoing_buffer, 0, outgoing_len);
    afp_default_url(&req->url);
    req->changeuid = changeuid;
    req->volume_options |= DEFAULT_MOUNT_FLAGS;

    if (readonly) {
        req->volume_options |= VOLUME_EXTRA_FLAGS_READONLY;
    }

    req->uam_mask = uam_mask;
    outgoing_buffer[0] = AFP_SERVER_COMMAND_MOUNT;
    req->map = AFP_MAPPING_UNKNOWN;

    if (mountpoint == NULL) {
        printf("No mount point specified\n");
        return -1;
    }

    if (resolve_mountpoint(mountpoint, req->mountpoint, 255) < 0) {
        printf("Failed to resolve mount point\n");
        return -1;
    }

    if (strncmp(urlstring, "afp://", 6) != 0) {
        if (asprintf(&temp_url, "afp://%s", urlstring) < 0) {
            printf("Out of memory constructing URL\n");
            return -1;
        }

        urlstring = temp_url;
    }

    if (afp_parse_url(&req->url, urlstring) != 0) {
        printf("Could not parse URL\n");

        if (temp_url) {
            free(temp_url);
        }

        return -1;
    }

    if (temp_url) {
        free(temp_url);
    }

    if (strcmp(req->url.password, "-") == 0 ||
            (req->url.username[0] != '\0'
             && req->url.password[0] == '\0')) {
        char *p = get_password("Password:");

        if (p) {
            snprintf(req->url.password, AFP_MAX_PASSWORD_LEN, "%s", p);
        }
    }

    if (volpass && (strcmp(volpass, "-") == 0)) {
        volpass  = get_password("Volume password:");
    }

    if (volpass) {
        snprintf(req->url.volpassword, 9, "%s", volpass);
    }

    return 0;
}

static int prepare_buffer(int argc, char * argv[])
{
    if (argc < 2) {
        usage();
        return -1;
    }

    if (strncmp(argv[1], "mount", 5) == 0) {
        return do_mount(argc, argv);
    } else if (strncmp(argv[1], "resume", 6) == 0) {
        return do_resume(argc, argv);
    } else if (strncmp(argv[1], "suspend", 7) == 0) {
        return do_suspend(argc, argv);
    } else if (strncmp(argv[1], "status", 6) == 0) {
        return do_status(argc, argv);
    } else if (strncmp(argv[1], "unmount", 7) == 0) {
        return do_unmount(argc, argv);
    } else if (strncmp(argv[1], "exit", 4) == 0) {
        return do_exit(argc, argv);
    } else {
        usage();
        return -1;
    }
}


int read_answer(int sock)
{
    int len = 0, expected_len = 0, packetlen;
    char *incoming_buffer = NULL;
    int buffer_size = MAX_CLIENT_RESPONSE;
    struct timeval tv;
    fd_set rds, ords;
    int ret;
    const struct afp_server_response * answer;
    incoming_buffer = malloc(buffer_size);

    if (!incoming_buffer) {
        printf("Out of memory\n");
        return -1;
    }

    answer = (void *) incoming_buffer;
    memset(incoming_buffer, 0, buffer_size);
    FD_ZERO(&rds);
    FD_SET(sock, &rds);

    while (1) {
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ords = rds;

        do {
            ret = select(sock + 1, &ords, NULL, NULL, &tv);
        } while (ret < 0 && errno == EINTR);

        if (ret == 0) {
            printf("No response from server\n");
            free(incoming_buffer);
            return -1;
        }

        if (FD_ISSET(sock, &ords)) {
            if (len == 0) {
                /* Read header first */
                do {
                    packetlen = read(sock, incoming_buffer + len,
                                     sizeof(struct afp_server_response));
                } while (packetlen < 0 && errno == EINTR);

                if (packetlen <= 0) {
                    /* If the outgoing command was UNMOUNT, treat EOF as success */
                    extern char outgoing_buffer[];

                    if (outgoing_buffer[0] == AFP_SERVER_COMMAND_UNMOUNT) {
                        free(incoming_buffer);
                        return 0;
                    }

                    printf("Failed to read response header\n");
                    free(incoming_buffer);
                    return -1;
                }

                len += packetlen;
                expected_len = answer->len;

                /* Grow buffer if needed */
                if (expected_len + sizeof(struct afp_server_response) > (size_t)buffer_size) {
                    buffer_size = expected_len + sizeof(struct afp_server_response) + 1;
                    char *new_buffer = realloc(incoming_buffer, buffer_size);

                    if (!new_buffer) {
                        printf("Out of memory\n");
                        free(incoming_buffer);
                        return -1;
                    }

                    incoming_buffer = new_buffer;
                    answer = (void *) incoming_buffer;
                }
            } else {
                do {
                    packetlen = read(sock, incoming_buffer + len, buffer_size - len);
                } while (packetlen < 0 && errno == EINTR);

                if (packetlen == 0) {
                    printf("Connection closed\n");
                    goto done;
                }

                if (packetlen < 0) {
                    goto error;
                }

                len += packetlen;
            }

            if ((unsigned long) len >= expected_len + sizeof(struct afp_server_response)) {
                goto done;
            }
        }
    }

done:
    /* Print the entire message (null-terminate first for safety) */
    incoming_buffer[len] = '\0';
    printf("%s", incoming_buffer + sizeof(*answer));
    ret = answer->result;
    free(incoming_buffer);
    return ret;
error:
    free(incoming_buffer);
    return -1;
}

int main(int argc, char *argv[])
{
    int sock;
    int ret;
    const char *mountpoint = NULL;
    const char *volumename = NULL;
    thisbin = argv[0];
    uid = ((unsigned int) geteuid());
    /* Register logging handler to filter debug messages */
    libafpclient_register(&client);

    if (strstr(argv[0], "mount_afpfs")) {
        if (handle_mount_afpfs(argc, argv) < 0) {
            return -1;
        }

        /* Extract mountpoint and volumename from mount request */
        if (outgoing_buffer[0] == AFP_SERVER_COMMAND_MOUNT && outgoing_len > 1) {
            const struct afp_server_mount_request *req =
                (const struct afp_server_mount_request *)(outgoing_buffer + 1);
            mountpoint = req->mountpoint;
            volumename = req->url.volumename;
        }
    } else if (prepare_buffer(argc, argv) < 0) {
        return -1;
    }

    /* Extract mountpoint and volumename for per-mount daemon routing;
       STATUS, SUSPEND, RESUME, EXIT go to manager, which forwards appropriately */
    if (mountpoint == NULL) {
        if (outgoing_buffer[0] == AFP_SERVER_COMMAND_MOUNT && outgoing_len > 1) {
            const struct afp_server_mount_request *req =
                (const struct afp_server_mount_request *)(outgoing_buffer + 1);
            mountpoint = req->mountpoint;
            volumename = req->url.volumename;
        } else if (outgoing_buffer[0] == AFP_SERVER_COMMAND_UNMOUNT
                   && outgoing_len > 1) {
            const struct afp_server_unmount_request *req =
                (const struct afp_server_unmount_request *)(outgoing_buffer + 1);
            mountpoint = req->mountpoint;
        }
    }

    if ((sock = daemon_connect(mountpoint, volumename)) < 0) {
        return -1;
    }

    send_command(sock, outgoing_buffer, outgoing_len);
    ret = read_answer(sock);
    return ret;
}
