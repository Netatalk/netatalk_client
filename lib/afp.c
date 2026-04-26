/*
 *  afp.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "afp.h"
#include "afp_protocol.h"
#include "libafpclient.h"
#include "server.h"
#include "dsi.h"
#include "dsi_protocol.h"
#include "utils.h"
#include "afp_replies.h"
#include "afp_internal.h"
#include "did.h"
#include "forklist.h"
#include "codepage.h"

struct afp_versions      afp_versions[] = {
    { "AFPVersion 1.1", 11 },
    { "AFPVersion 2.0", 20 },
    { "AFPVersion 2.1", 21 },
    { "AFP2.2", 22 },
    { "AFPX03", 30 },
    { "AFP3.1", 31 },
    { "AFP3.2", 32 },
    { "AFP3.3", 33 },
    { "AFP3.4", 34 },
    { NULL, 0 }
};

static int afp_blank_reply(struct afp_server *server, char * buf,
                           unsigned int size, void *ignored);

int (*afp_replies[])(struct afp_server * server, char * buf, unsigned int len,
                     void *other) = {
    NULL, afp_byterangelock_reply, afp_blank_reply, NULL,
    afp_blank_reply, NULL, afp_createdir_reply, afp_blank_reply, /* 0 - 7 */
    afp_blank_reply, afp_enumerate_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, NULL, NULL,                       /* 8 - 15 */
    afp_getsrvrparms_reply, afp_getvolparms_reply, afp_login_reply, afp_login_reply,
    afp_blank_reply, afp_mapid_reply, afp_mapname_reply, afp_blank_reply,  /*16 - 23 */
    afp_volopen_reply, NULL, afp_openfork_reply, afp_read_reply,
    afp_blank_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,    /*24 - 31 */
    NULL, afp_write_reply, afp_getfiledirparms_reply, afp_blank_reply,
    afp_changepassword_reply, afp_getuserinfo_reply, afp_getsrvrmsg_reply, NULL,      /*32 - 39 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,                       /*40 - 47 */
    afp_opendt_reply, afp_blank_reply, NULL, afp_geticon_reply,
    NULL, NULL, NULL, NULL,                       /*48 - 55 */
    afp_blank_reply, NULL, afp_getcomment_reply, afp_byterangelockext_reply,
    afp_readext_reply, afp_writeext_reply,
    NULL, afp_login_reply,            /*56 - 63 */
    afp_getsessiontoken_reply, afp_blank_reply, afp_enumerateext_reply, NULL,
    afp_enumerateext2_reply, afp_getextattr_reply, afp_blank_reply, afp_blank_reply, /*64 - 71 */
    afp_listextattrs_reply, afp_blank_reply, afp_blank_reply, afp_blank_reply,
    NULL, NULL, afp_blank_reply, afp_blank_reply,                       /*72 - 79 */

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,

};

/* This is the simplest afp reply */
static int afp_blank_reply(
    __attribute__((unused)) struct afp_server *server,
    char *buf,
    __attribute__((unused)) unsigned int size,
    __attribute__((unused)) void * ignored
)
{
    struct {
        struct dsi_header header __attribute__((__packed__));
    } * reply = (void *) buf;
    return reply->header.return_code.error_code;
}

/* Handle a reply packet */
int afp_reply(unsigned short subcommand, struct afp_server * server,
              void *other)
{
    int ret = 0;

    /* No AFP packet is valid if it is smaller than a DSI header. */

    if ((unsigned long) server->data_read < sizeof(struct dsi_header)) {
        return -1;
    }

    if (afp_replies[subcommand]) {
        ret = (*afp_replies[subcommand])(server,
                                         server->incoming_buffer,
                                         server->data_read, other);
    } else {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "AFP subcommand %d not supported", subcommand);
    }

    return ret;
}


static struct afp_server *server_base = NULL;
static pthread_mutex_t server_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static int auto_shutdown_on_unmount = 0;
static int auto_disconnect_on_unmount = 1;

void afp_set_auto_shutdown_on_unmount(int enabled)
{
    auto_shutdown_on_unmount = enabled;
}

int afp_get_auto_shutdown_on_unmount(void)
{
    return auto_shutdown_on_unmount;
}

void afp_set_auto_disconnect_on_unmount(int enabled)
{
    auto_disconnect_on_unmount = enabled;
}

int server_still_valid(struct afp_server * server)
{
    struct afp_server * s = server_base;

    for (; s; s = s->next)
        if (s == server) {
            return 1;
        }

    return 0;
}

static void add_server(struct afp_server *newserver)
{
    pthread_mutex_lock(&server_list_mutex);
    newserver->next = server_base;
    server_base = newserver;
    pthread_mutex_unlock(&server_list_mutex);
}

struct afp_server *get_server_base(void)
{
    return server_base;
}

struct afp_server *find_server_by_signature(char * signature)
{
    struct afp_server * s;

    for (s = get_server_base(); s; s = s->next) {
        if (memcmp(s->signature, signature, AFP_SIGNATURE_LEN) == 0) {
            return s;
        }
    }

    return NULL;
}

struct afp_server *find_server_by_name(char * name)
{
    for (struct afp_server * s = get_server_base(); s; s = s->next) {
        if (strcmp(s->server_name_utf8, name) == 0) {
            return s;
        }

        if (strcmp(s->server_name, name) == 0) {
            return s;
        }

        if (strcmp(s->server_name_printable, name) == 0) {
            return s;
        }
    }

    return NULL;
}

struct afp_server *find_server_by_pointer(struct afp_server *target)
{
    pthread_mutex_lock(&server_list_mutex);

    for (struct afp_server *s = server_base; s; s = s->next) {
        if (s == target) {
            __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&server_list_mutex);
            return s;
        }
    }

    pthread_mutex_unlock(&server_list_mutex);
    return NULL;
}

struct afp_server *find_server_by_address(struct addrinfo *address)
{
    struct afp_server *s;

    for (s = server_base; s; s = s->next) {
        for (struct addrinfo *p = address; p; p = p->ai_next) {
            if (s->used_address != NULL && s->used_address->ai_addr != NULL &&
                    p != NULL && p->ai_addr != NULL &&
                    s->used_address->ai_addrlen == p->ai_addrlen &&
                    memcmp(s->used_address->ai_addr, p->ai_addr,
                           p->ai_addrlen) == 0) {
                return s;
            }
        }
    }

    return NULL;
}

void afp_server_hold(struct afp_server *s)
{
    __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
}

void afp_server_release(struct afp_server *s)
{
    if (__atomic_sub_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        afp_free_server(&s);
    }
}

struct afp_server *afp_server_find_by_name_hold(char * name)
{
    struct afp_server * s;
    pthread_mutex_lock(&server_list_mutex);
    s = find_server_by_name(name);

    if (s) {
        __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_unlock(&server_list_mutex);
    return s;
}

struct afp_server *afp_server_find_by_address_hold(struct addrinfo * address)
{
    struct afp_server * s;
    pthread_mutex_lock(&server_list_mutex);
    s = find_server_by_address(address);

    if (s) {
        __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_unlock(&server_list_mutex);
    return s;
}

struct afp_volume *afp_volume_find_by_pointer_hold(void * id)
{
    struct afp_volume * v;
    pthread_mutex_lock(&server_list_mutex);

    for (struct afp_server * s = server_base; s; s = s->next) {
        for (int j = 0; j < s->num_volumes; j++) {
            v = &s->volumes[j];

            if ((void *) v == id) {
                __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&server_list_mutex);
                return v;
            }
        }
    }

    pthread_mutex_unlock(&server_list_mutex);
    return NULL;
}

void afp_lock_server_list(void)
{
    pthread_mutex_lock(&server_list_mutex);
}

void afp_unlock_server_list(void)
{
    pthread_mutex_unlock(&server_list_mutex);
}

int something_is_mounted(struct afp_server * server)
{
    int i;

    for (i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].mounted != AFP_VOLUME_UNMOUNTED) {
            return 1;
        }
    }

    return 0;
}

int something_is_attached(struct afp_server * server)
{
    for (int i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].attached != AFP_VOLUME_DETACHED) {
            return 1;
        }
    }

    return 0;
}

int afp_detach_volume(struct afp_volume * volume)
{
    struct afp_server * server;

    if (volume == NULL) {
        return -1;
    }

    server = volume->server;
    volume->attached = AFP_VOLUME_DETACHING;
    afp_flush(volume);
    free_entire_did_cache(volume);
    remove_fork_list(volume);

    if (volume->dtrefnum) {
        afp_closedt(server, volume->dtrefnum);
    }

    volume->dtrefnum = 0;
    volume->attached = AFP_VOLUME_DETACHED;

    if (something_is_attached(server)) {
        return 0;
    }

    if (auto_disconnect_on_unmount) {
        afp_logout(server, DSI_DONT_WAIT /* don't wait */);
        afp_server_remove(server);
    }

    return 0;
}

int afp_unmount_all_volumes(struct afp_server * server)
{
    int i;

    for (i = 0; i < server->num_volumes; i++) {
        if (server->volumes[i].attached == AFP_VOLUME_ATTACHED
                && afp_detach_volume(&server->volumes[i])) {
            return 1;
        }
    }

    return 0;
}

int afp_unmount_volume(struct afp_volume * volume)
{
    if (volume == NULL) {
        return -1;
    }

    if (volume->mounted != AFP_VOLUME_MOUNTED) {
        return -1;
    }

    volume->mounted = AFP_VOLUME_UNMOUNTING;

    if (libafpclient->unmount_volume && libafpclient->unmount_volume(volume) != 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "FUSE unmount not supported - volume remains mounted on %s",
                       volume->mountpoint);
        volume->mounted = AFP_VOLUME_MOUNTED;
        return -1;
    }

    afp_detach_volume(volume);
    volume->mounted = AFP_VOLUME_UNMOUNTED;
    return 0;
}


void afp_free_server(struct afp_server ** sp)
{
    struct dsi_request * p, *next;
    struct afp_volume * volumes;
    struct afp_server * server;

    if (sp == NULL) {
        return;
    }

    server = *sp;

    if (!server) {
        return;
    }

    for (p = server->command_requests; p;) {
        log_for_client(NULL, AFPFSD, LOG_NOTICE,
                       "FSLeft in queue: %p, id: %d command: %d",
                       p, p->requestid, p->subcommand);
        next = p->next;
        free(p);
        p = next;
    }

    volumes = server->volumes;

    if (server->incoming_buffer) {
        free(server->incoming_buffer);
    }

    if (server->attention_buffer) {
        free(server->attention_buffer);
    }

    if (volumes) {
        free(volumes);
    }

    free(server);
    *sp = NULL;
}

int afp_server_remove(struct afp_server *s)
{
    struct afp_server *s2;
    int found = 0;

    if (s == NULL) {
        return 0;
    }

    /* Remove from the server list first so no new lookups find it */
    pthread_mutex_lock(&server_list_mutex);

    if (s == server_base) {
        server_base = s->next;
        found = 1;
    } else {
        for (s2 = server_base; s2; s2 = s2->next) {
            if (s == s2->next) {
                s2->next = s->next;
                found = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&server_list_mutex);

    if (!found) {
        return -1;
    }

    /* Wake all waiting DSI requests so blocked threads can unblock */
    pthread_mutex_lock(&s->request_queue_mutex);

    for (struct dsi_request *p = s->command_requests; p;) {
        struct dsi_request *next_request = p->next;
        pthread_mutex_lock(&p->waiting_mutex);
        p->done_waiting = 1;
        pthread_cond_signal(&p->waiting_cond);
        pthread_mutex_unlock(&p->waiting_mutex);
        p = next_request;
    }

    pthread_mutex_unlock(&s->request_queue_mutex);
    /* Close the connection */
    loop_disconnect(s);
    /* Release the initial reference from creation.
     * The server will be freed when all holders release. */
    afp_server_release(s);
    return 0;
}

struct afp_server *afp_server_init(struct addrinfo * address)
{
    struct afp_server * s;
    struct passwd *pw;

    if ((s = malloc(sizeof(*s))) == NULL) {
        return NULL;
    }

    memset((void *) s, 0, sizeof(*s));
    s->exit_flag = 0;
    s->path_encoding = kFPUTF8Name; /* This is a default */
    s->next = NULL;
    s->bufsize = 128 * 1024;
    s->incoming_buffer = malloc(s->bufsize);
    s->attention_quantum = AFP_DEFAULT_ATTENTION_QUANTUM;
    s->attention_buffer = malloc(s->attention_quantum);
    s->attention_len = 0;
    s->dsi_default_timeout = DSI_DEFAULT_TIMEOUT;
    s->connect_state = SERVER_STATE_DISCONNECTED;
    s->address = address;
    /* Initialize mutexes */
    pthread_mutex_init(&s->requestid_mutex, NULL);
    pthread_mutex_init(&s->request_queue_mutex, NULL);
    pthread_mutex_init(&s->send_mutex, NULL);
    s->connection_generation = 0;
    s->refcount = 1;
    /* AFP 3.3+ replay cache - initialized to disabled */
    s->replay_cache_size = 0;
    s->supports_replay_cache = 0;
    /* FIXME this shouldn't be set here */
    pw = getpwuid(geteuid());
    memcpy(&s->passwd, pw, sizeof(struct passwd));
    return s;
}

static void setup_default_outgoing_token(struct afp_token * token)
{
    char foo[] = {(char)0x54, (char)0xc0, (char)0x75, (char)0xb0, (char)0x15, (char)0xe6, (char)0x1c, (char)0x13,
                  (char)0x86, (char)0x75, (char)0xd2, (char)0xc2, (char)0xfd, (char)0x03, (char)0x4e, (char)0x3b
                 };
    token->length = 16;
    memcpy(token->data, foo, 16);
}

static int resume_token(struct afp_server * server)
{
    struct afp_token outgoing_token;
    time_t now;
    int ret;
    /* Get the time */
    time(&now);
    /* Setup the outgoing token */
    setup_default_outgoing_token(&outgoing_token);
    ret = afp_getsessiontoken(server, kReconnWithTimeAndID,
                              (unsigned int) now,
                              &outgoing_token, &server->token);
    return ret;
}

static int setup_token(struct afp_server * server)
{
    time_t now;
    int ret;
    struct afp_token outgoing_token;
    /* Get the time */
    time(&now);
    /* Setup the outgoing token */
    setup_default_outgoing_token(&outgoing_token);
    ret = afp_getsessiontoken(server, kLoginWithTimeAndID,
                              (unsigned int) now,
                              &outgoing_token, &server->token);
    return ret;
}

int afp_server_login(struct afp_server *server,
                     char *mesg, unsigned int *l, unsigned int max)
{
    int rc;
    rc = afp_dologin(server, server->using_uam,
                     server->username, server->password);

    switch (rc) {
    case -1:
        *l += snprintf(mesg, max - *l,
                       "Could not find a valid UAM\n");
        goto error;

    case kFPAuthContinue:
        *l += snprintf(mesg, max - *l,
                       "Authentication method unsupported by AFPFS\n");
        goto error;

    case kFPBadUAM:
        *l += snprintf(mesg, max - *l,
                       "Specified UAM is unknown\n");
        goto error;

    case kFPBadVersNum:
        *l += snprintf(mesg, max - *l,
                       "Server does not support this AFP version\n");
        goto error;

    case kFPCallNotSupported:
    case kFPMiscErr:
        *l += snprintf(mesg, max - *l,
                       "Already logged in\n");
        break;

    case kFPNoServer:
        *l += snprintf(mesg, max - *l,
                       "Authentication server not responding\n");
        goto error;

    case kFPPwdExpiredErr:
    case kFPPwdNeedsChangeErr:
        *l += snprintf(mesg, max - *l,
                       "Warning: password needs changing\n");
        goto error;

    case kFPServerGoingDown:
        *l += snprintf(mesg, max - *l,
                       "Server going down, so I can't log you in.\n");
        goto error;

    case kFPUserNotAuth:
        *l += snprintf(mesg, max - *l,
                       "Authentication failed\n");
        goto error;

    case 0:
        break;

    default:
        *l += snprintf(mesg, max - *l,
                       "Unknown error, maybe username/passwd needed?\n");
        goto error;
    }

    if (server->flags & kSupportsReconnect) {
        /* Get the session */
        if (server->need_resume) {
            resume_token(server);
            server->need_resume = 0;
        } else {
            setup_token(server);
        }
    }

    return 0;
error:
    return 1;
}


struct afp_volume *find_volume_by_name(struct afp_server * server,
                                       char *volname)
{
    int i;
    struct afp_volume * using_volume = NULL;
    char converted_volname[AFP_VOLUME_NAME_UTF8_LEN];
    memset(converted_volname, 0, AFP_VOLUME_NAME_UTF8_LEN);
    convert_utf8dec_to_utf8pre(volname, strlen(volname),
                               converted_volname, AFP_VOLUME_NAME_UTF8_LEN);
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "find_volume_by_name: looking for '%s' (converted: '%s')",
                   volname, converted_volname);

    for (i = 0; i < server->num_volumes; i++)  {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "find_volume_by_name: comparing with volume[%d] '%s'",
                       i, server->volumes[i].volume_name_printable);

        if (strcmp(converted_volname,
                   server->volumes[i].volume_name_printable) == 0) {
            using_volume = &server->volumes[i];
            goto out;
        }
    }

out:
    return using_volume;
}

int afp_connect_volume(struct afp_volume * volume, struct afp_server * server,
                       char *mesg, unsigned int *l, unsigned int max)
{
    unsigned short bitmap =
        kFPVolAttributeBit | kFPVolSignatureBit |
        kFPVolCreateDateBit | kFPVolIDBit |
        kFPVolNameBit;
    char new_encoding;
    int ret;

    if (server->using_version->av_number >= 30) {
        bitmap |= kFPVolNameBit | kFPVolBlockSizeBit;
    }

    ret = afp_volopen(volume, bitmap,
                      (strlen(volume->volpassword) > 0) ? volume->volpassword : NULL);

    switch (ret) {
    case kFPAccessDenied:
        *l += snprintf(mesg, max - *l,
                       "Incorrect volume password\n");
        goto error;

    case kFPNoErr:
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPObjectNotFound:
    case kFPParamErr:
        *l += snprintf(mesg, max - *l,
                       "Could not open volume\n");
        goto error;

    case ETIMEDOUT:
        *l += snprintf(mesg, max - *l,
                       "Timed out waiting to open volume\n");
        goto error;
    }

    /* It is said that if a volume's encoding will be the same
     * the server's. */
    if (volume->attributes & kSupportsUTF8Names) {
        new_encoding = kFPUTF8Name;
    } else {
        new_encoding = kFPLongName;
    }

    if (new_encoding != server->path_encoding) {
        *l += snprintf(mesg, max - *l,
                       "Volume %s changes the server's encoding\n",
                       volume->volume_name_printable);
    }

    server->path_encoding = new_encoding;

    if (volume->signature != AFP_VOL_FIXED) {
        *l += snprintf(mesg, max - *l,
                       "Volume %s does not support fixed directories\n",
                       volume->volume_name_printable);
        afp_detach_volume(volume);
        goto error;
    }

    if (server->using_version->av_number >= 30) {
        if ((volume->server->server_type == AFPFS_SERVER_TYPE_NETATALK) &&
                (~ volume->attributes & kSupportsUnixPrivs)) {
            volume->extra_flags &= ~VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
        } else {
            volume->extra_flags |= VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX;
        }
    } else {
        /* This is very odd, but AFP 2.x doesn't give timestamps for directories */
    }

    volume->mounted = AFP_VOLUME_MOUNTED;
    volume->attached = AFP_VOLUME_ATTACHED;
    return 0;
error:
    return 1;
}

int afp_server_reconnect(struct afp_server * s, char * mesg,
                         unsigned int *l, unsigned int max)
{
    int i;
    struct afp_volume * v;
    /* Flush pending requests before reconnecting to avoid late reply confusion */
    dsi_flush_request_queue(s);

    if (afp_server_connect(s, 0))  {
        *l += snprintf(mesg, max - *l, "Error resuming connection to %s\n",
                       s->server_name_printable);
        return 1;
    }

    dsi_opensession(s);

    if (afp_server_login(s, mesg, l, max)) {
        return 1;
    }

    for (i = 0; i < s->num_volumes; i++) {
        v = &s->volumes[i];

        if (strlen(v->mountpoint)) {
            if (afp_connect_volume(v, v->server, mesg, l, max))
                *l += snprintf(mesg, max - *l,
                               "Could not mount %s\n",
                               v->volume_name_printable);
        }
    }

    s->connect_state = SERVER_STATE_CONNECTED;
    return 0;
}


int afp_server_connect(struct afp_server *server, int full)
{
    int 	error = 0;
    struct 	timeval t1, t2;
    struct 	addrinfo *address;
#define LOG_MSG_SIZE 64
    static char log_msg[LOG_MSG_SIZE];
    char	ip_addr[INET6_ADDRSTRLEN];
    address = server->address;

    if (server->fd > 0) {
        log_for_client(NULL, AFPFSD, LOG_INFO,
                       "Closing old socket fd=%d before reconnection", server->fd);
        close(server->fd);
        server->fd = -1;
    }

    /* Increment connection generation to detect stale replies */
    server->connection_generation++;
    log_for_client(NULL, AFPFSD, LOG_INFO,
                   "Connection generation now %u", server->connection_generation);

    while (address) {
        switch (address->ai_family) {
        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)address->ai_addr)->sin6_addr),
                      ip_addr, INET6_ADDRSTRLEN);
            break;

        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)address->ai_addr)->sin_addr),
                      ip_addr, INET6_ADDRSTRLEN);
            break;

        default:
            snprintf(ip_addr, 23, "unknown address family");
            break;
        }

        int written = snprintf(log_msg, LOG_MSG_SIZE, "Attempting connection to %s ...",
                               ip_addr);

        if (written >= LOG_MSG_SIZE) {
            log_msg[LOG_MSG_SIZE - 1] = '\0';
        }

        log_for_client(NULL, AFPFSD, LOG_NOTICE, log_msg);
        server->fd = socket(address->ai_family,
                            address->ai_socktype, address->ai_protocol);

        if (server->fd >= 0) {
            if (connect(server->fd, address->ai_addr, address->ai_addrlen) == 0) {
                break;
            }

            close(server->fd);
            server->fd	= -1;
        }

        address	= address->ai_next;
    }

    if (server->fd < 0) {
        error = errno;
        goto error;
    }

    server->exit_flag		= 0;

    /* AFP 3.3+: Only reset request ID if replay cache is not supported.
     * With replay cache, request IDs persist across reconnections. */
    if (!server->supports_replay_cache) {
        server->lastrequestid	= 0;
    }

    server->connect_state	= SERVER_STATE_CONNECTING;
    server->used_address	= address;
    /* Add server to the list if it's not already there.
     * On reconnect, the server is already in the list, so we skip this. */
    int found = 0;

    for (struct afp_server *s = get_server_base(); s; s = s->next) {
        if (s == server) {
            found = 1;
            break;
        }
    }

    if (!found) {
        add_server(server);
    }

    add_fd_and_signal(server->fd);

    if (!full) {
        return 0;
    }

    /* Get the status, and calculate the transmit time.  We use this to
    * calculate our rx quantum. */
    gettimeofday(&t1, NULL);

    if ((error = dsi_getstatus(server)) != 0) {
        goto error;
    }

    gettimeofday(&t2, NULL);
    afp_server_identify(server);

    if ((t2.tv_sec - t1.tv_sec) > 0) {
        server->tx_delay = (t2.tv_sec - t1.tv_sec) * 1000;
    } else {
        server->tx_delay = (t2.tv_usec - t1.tv_usec) / 1000;
    }

    /* Calculate the quantum based on our tx_delay and a threshold */
    /* For now, we'll just set a default */
    /* This is the default in 10.4.x where x > 7 */
    server->rx_quantum = 128 * 1024;
    return 0;
error:
    return -error;
}

struct afp_versions *pick_version(unsigned char *versions,
                                  unsigned char requested)
{
    /* Pick the right version number.  This means either the
       one requested or the last one. Set both the number and the
       string. */
    int i;
    char version_num = 0;
    char found_version = 0;
    struct afp_versions * p;
    char highest = 0;

    if (requested) {
        requested = min(requested, AFP_MAX_SUPPORTED_VERSION);
    }

    for (i = 0; versions[i] && (i < SERVER_MAX_VERSIONS); i++) {
        version_num = versions[i];
        highest = max(highest, version_num);

        if (versions[i] == requested) {
            found_version = 1;
            break;
        }
    };

    if (!found_version) {
        version_num = highest;
    }

    for (p = afp_versions; p->av_name; p++) {
        if (p->av_number == version_num) {
            return p;
        }
    }

    return NULL;
}

int pick_uam(unsigned int uam2, unsigned int uam1)
{
    int i;

    for (i = 15; i >= 0; i--) {
        if (((1 << i)) & (uam2 & uam1)) {
            return (1 << i);
        }
    }

    return -1;
}

int afp_list_volnames(struct afp_server * server, char * names, int max)
{
    int len = 0;
    int i;

    for (i = 0; i < server->num_volumes; i++) {
        if (i < server->num_volumes - 1)
            len += snprintf(names + len, max - len, "%s, ",
                            server->volumes[i].volume_name_printable);
        else
            len += snprintf(names + len, max - len, "%s",
                            server->volumes[i].volume_name_printable);
    }

    return len;
}
