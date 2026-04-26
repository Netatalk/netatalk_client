/*
 *  dsi.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"
#include "dsi.h"
#include "afp.h"
#include "uams_def.h"
#include "dsi_protocol.h"
#include "libafpclient.h"
#include "afp_internal.h"
#include "afp_protocol.h"
#include "afp_replies.h"
#include "codepage.h"

/* define this in order to get reams of DSI debugging information */
#ifdef DEBUG
#define DEBUG_DSI
#endif

static int dsi_remove_from_request_queue(struct afp_server *server,
        struct dsi_request *toremove);
int convert_utf8dec_to_utf8pre(char *src, int src_len,
                               char *dest, int dest_len);
int convert_utf8pre_to_utf8dec(char * src, int src_len,
                               char *dest, int dest_len);

/* This sets up a DSI header. */
void dsi_setup_header(struct afp_server * server, struct dsi_header * header,
                      char command)
{
    memset(header, 0, sizeof(struct dsi_header));
    pthread_mutex_lock(&server->requestid_mutex);

    if (server->lastrequestid == 65535) {
        /* AFP 3.3+: Don't reset to 0 if replay cache is supported.
         * Request IDs wrap around but remain persistent across reconnects. */
        if (!server->supports_replay_cache) {
            server->lastrequestid = 0;
        } else {
            server->lastrequestid = 1;  /* Wrap to 1, avoiding 0 */
        }
    } else {
        server->lastrequestid++;
    }

    server->expectedrequestid = server->lastrequestid;
    header->requestid = htons(server->lastrequestid);
    pthread_mutex_unlock(&server->requestid_mutex);
    header->command = command;
}

int dsi_getstatus(struct afp_server * server)
{
    struct dsi_header  header;
    struct afp_rx_buffer rx;
    int ret;
    rx.data = malloc(GETSTATUS_BUF_SIZE);

    if (rx.data == NULL) {
        return -1;
    }

    rx.maxsize = GETSTATUS_BUF_SIZE;
    rx.size = 0;
    dsi_setup_header(server, &header, DSI_DSIGetStatus);
    /* We're intentionally ignoring the results */
    ret = dsi_send(server, (char *) &header, sizeof(struct dsi_header), 60,
                   DSI_DSIGetStatus, (void *) &rx);
    free(rx.data);
    return ret;
}

int dsi_sendtickle(struct afp_server *server)
{
    struct dsi_header  header;
    dsi_setup_header(server, &header, DSI_DSITickle);
    dsi_send(server, (char *) &header, sizeof(struct dsi_header),
             DSI_DONT_WAIT, DSI_DSITickle, NULL);
    return 0;
}

int dsi_opensession(struct afp_server *server)
{
    struct {
        struct dsi_header dsi_header  __attribute__((__packed__));
        uint8_t flags;
        uint8_t length;
        uint32_t rx_quantum ;
    } __attribute__((__packed__)) dsi_opensession_header;
    struct dsi_header hdr;
    dsi_setup_header(server, &hdr, DSI_DSIOpenSession);
    memcpy(&dsi_opensession_header.dsi_header, &hdr, sizeof(struct dsi_header));
    /* Advertize our rx quantum */
    dsi_opensession_header.flags = 1;
    dsi_opensession_header.length = sizeof(unsigned int);
    dsi_opensession_header.rx_quantum = htonl(server->attention_quantum);
    dsi_send(server, (char *) &dsi_opensession_header,
             sizeof(dsi_opensession_header), DSI_BLOCK_TIMEOUT,
             DSI_DSIOpenSession, NULL);
    return 0;
}

static int dsi_remove_from_request_queue(struct afp_server *server,
        struct dsi_request *toremove)
{
    struct dsi_request *p, *prev = NULL;

    if (!server_still_valid(server)) {
        return -1;
    }

#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** Removing %d, %s", toremove->requestid,
                   afp_get_command_name(toremove->subcommand));
#endif
    pthread_mutex_lock(&server->request_queue_mutex);

    for (p = server->command_requests; p; p = p->next) {
        if (p == toremove) {
            if (prev == NULL) {
                server->command_requests = p->next;
            } else {
                prev->next = p->next;
            }

            server->stats.requests_pending--;
            /* SAFEGUARD: Wake any waiting thread before destroying */
            pthread_mutex_lock(&p->waiting_mutex);
            p->done_waiting = 1;
            pthread_cond_signal(&p->waiting_cond);
            pthread_mutex_unlock(&p->waiting_mutex);
            /* Now safe to destroy mutex/cond */
            pthread_cond_destroy(&p->waiting_cond);
            pthread_mutex_destroy(&p->waiting_mutex);
            free(p);
            pthread_mutex_unlock(&server->request_queue_mutex);
            return 0;
        }

        prev = p;
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "*** Never removed anything for %d, %s", toremove->requestid,
                   afp_get_command_name(toremove->subcommand));
#endif
    log_for_client(NULL, AFPFSD, LOG_WARNING,
                   "Got an unknown reply for requestid %i",
                   ntohs(toremove->requestid));
    return -1;
}

/* Flush all pending requests from the queue when reconnecting
 * to prevent late replies from arriving on a new connection */
void dsi_flush_request_queue(struct afp_server *server)
{
    struct dsi_request *p, *next;

    if (!server_still_valid(server)) {
        return;
    }

    log_for_client(NULL, AFPFSD, LOG_INFO,
                   "Flushing pending request queue before reconnection");
    pthread_mutex_lock(&server->request_queue_mutex);
    p = server->command_requests;

    while (p != NULL) {
        next = p->next;
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "*** Flushing request %d, %s", p->requestid,
                       afp_get_command_name(p->subcommand));
#endif
        /* Wake any waiting thread with error */
        pthread_mutex_lock(&p->waiting_mutex);
        p->return_code = -EIO;
        p->done_waiting = 1;
        pthread_cond_signal(&p->waiting_cond);
        pthread_mutex_unlock(&p->waiting_mutex);
        /* Destroy and free */
        pthread_cond_destroy(&p->waiting_cond);
        pthread_mutex_destroy(&p->waiting_mutex);
        free(p);
        server->stats.requests_pending--;
        p = next;
    }

    server->command_requests = NULL;
    pthread_mutex_unlock(&server->request_queue_mutex);
    log_for_client(NULL, AFPFSD, LOG_INFO,
                   "Request queue flushed");
}


int dsi_send(struct afp_server *server, char * msg, int size, int wait,
             unsigned char subcommand, void **other)
{
    /* For wait:
     * -1: wait forever
     *  0: don't wait
     * x>n: wait for N seconds */
    struct dsi_header  *header = (struct dsi_header *) msg;
    struct dsi_request * new_request, *p;
    int rc = 0;
    struct timespec ts;
    struct timeval tv;
    header->length = htonl(size - sizeof(struct dsi_header));

    if (!server || !server_still_valid(server) || server->fd == 0) {
        return -1;
    }

    afp_wait_for_started_loop();

    /* Add request to the queue */
    if ((new_request = malloc(sizeof(struct dsi_request))) == NULL) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Could not allocate for new request");
        return -1;
    }

    memset(new_request, 0, sizeof(struct dsi_request));
    new_request->requestid = ntohs(header->requestid);
    new_request->subcommand = subcommand;
    new_request->other = other;
    new_request->wait = wait;
    new_request->next = NULL;
    new_request->done_waiting = 0;
    new_request->connection_generation = server->connection_generation;
    /* SAFEGUARD: Initialize mutex/cond BEFORE adding to queue to prevent race condition */
    pthread_cond_init(&new_request->waiting_cond, NULL);
    pthread_mutex_init(&new_request->waiting_mutex, NULL);
    pthread_mutex_lock(&server->request_queue_mutex);

    if (server->command_requests == NULL) {
        server->command_requests = new_request;
    } else {
        for (p = server->command_requests; p->next; p = p->next);

        p->next = new_request;
    }

    server->stats.requests_pending++;
    pthread_mutex_unlock(&server->request_queue_mutex);

    if (server->connect_state == SERVER_STATE_DISCONNECTED) {
        char mesg[MAX_ERROR_LEN];
        unsigned int l = 0;
        /* Try and reconnect */
        afp_server_reconnect(server, mesg, &l, MAX_ERROR_LEN);
    }

    pthread_mutex_lock(&server->send_mutex);
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "*** Sending %d, %s",
                   ntohs(header->requestid),
                   afp_get_command_name(new_request->subcommand));
#endif
    /* Write with EINTR retry - write() can return partial data or be interrupted */
    size_t total_written = 0;
    ssize_t ret;

    while (total_written < (size_t)size) {
        ret = write(server->fd, msg + total_written, size - total_written);

        if (ret < 0) {
            if (errno == EINTR) {
                /* Interrupted by signal - retry */
                continue;
            }

            if ((errno == EPIPE) || (errno == EBADF)) {
                /* The server has closed the connection */
                server->connect_state = SERVER_STATE_DISCONNECTED;
                rc = -1;
                pthread_mutex_unlock(&server->send_mutex);
                /* SAFEGUARD: Set return_code so waiting thread gets the error */
                new_request->return_code = -EIO;
                goto out;  /* SAFEGUARD: Properly clean up instead of direct return */
            }

            perror("writing to server");
            rc = -1;
            pthread_mutex_unlock(&server->send_mutex);
            /* SAFEGUARD: Set return_code so waiting thread gets the error */
            new_request->return_code = -EIO;
            goto out;
        }

        total_written += ret;
    }

    server->stats.tx_bytes += size;
    pthread_mutex_unlock(&server->send_mutex);
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Waiting for response for %d %s",
                   new_request->requestid,
                   afp_get_command_name(new_request->subcommand));
#endif

    if (new_request->wait < 0) {
        /* Wait forever */
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Waiting forever for %d, %s",
                       new_request->requestid,
                       afp_get_command_name(new_request->subcommand));
#endif
        pthread_mutex_lock(&new_request->waiting_mutex);

        /* SAFEGUARD: Use while loop to handle spurious wakeups */
        while (new_request->done_waiting == 0) {
            rc = pthread_cond_wait(
                     &new_request->waiting_cond,
                     &new_request->waiting_mutex);

            if (rc != 0) {
                break;
            }
        }

        pthread_mutex_unlock(&new_request->waiting_mutex);
    } else if (new_request->wait > 0) {
        /* wait for new_request->wait seconds */
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Waiting for %d %s, for %ds",
                       new_request->requestid,
                       afp_get_command_name(new_request->subcommand),
                       new_request->wait);
#endif
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec;
        ts.tv_sec += new_request->wait;
        ts.tv_nsec = tv.tv_usec * 1000;

        if (new_request->wait == 0) {
#ifdef DEBUG_DSI
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "=== Changing my mind, no longer waiting for %d",
                           new_request->requestid);
#endif
            goto skip;
        }

        pthread_mutex_lock(&new_request->waiting_mutex);

        /* SAFEGUARD: Use while loop to handle spurious wakeups */
        while (new_request->done_waiting == 0) {
            rc = pthread_cond_timedwait(
                     &new_request->waiting_cond,
                     &new_request->waiting_mutex, &ts);

            if (rc == ETIMEDOUT) {
                break;
            }

            if (rc != 0 && rc != ETIMEDOUT) {
                break;
            }
        }

        pthread_mutex_unlock(&new_request->waiting_mutex);

        if (rc == ETIMEDOUT) {
            /* FIXME: should handle this case properly */
#ifdef DEBUG_DSI
            log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Timedout for %d",
                           new_request->requestid);
#endif
            goto out;
        }
    } else {
        /* Don't wait */
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== Skipping wait altogether for %d",
                       new_request->requestid);
#endif
    }

#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "=== Done waiting for %d %s, waiting for %ds,"
                   " return %d, DSI return %d",
                   new_request->requestid,
                   afp_get_command_name(new_request->subcommand),
                   new_request->wait,
                   rc, new_request->return_code);
#endif
skip:

    /* SAFEGUARD: Only use return_code if we didn't have an error */
    if (rc == 0) {
        rc = new_request->return_code;
    }

out:
    dsi_remove_from_request_queue(server, new_request);
    return rc;
}

int dsi_command_reply(struct afp_server* server, unsigned short subcommand,
                      void *other)
{
    int ret = 0;

    if ((unsigned long) server->data_read < sizeof(struct dsi_header)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Got a short reply command, I am just ignoring it. size: %d",
                       server->data_read);
        return -1;
    }

    if (subcommand == 0) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Broken subcommand: %d", subcommand);
        return -1;
    }

    if ((subcommand == afpRead) || (subcommand == afpReadExt)) {
        struct afp_rx_buffer * buf = other;
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "=== read() for afpRead, %d bytes",
                       buf->maxsize - buf->size);
#endif

        do {
            ret = read(server->fd, buf->data + buf->size,
                       buf->maxsize - buf->size);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            return -1;
        }

        server->stats.rx_bytes += ret;

        if (ret == 0) {
            return -1;
        }

        server->data_read += ret;
    }

    ret = afp_reply(subcommand, server, other);
    return ret;
}


/* Default tx_quantum if server doesn't provide one (64KB is conservative) */
#define DSI_DEFAULT_TX_QUANTUM 65536

void dsi_opensession_reply(struct afp_server * server)
{
    const char *p;
    int offset;
    uint8_t option_type, option_len;
    /* Parse DSI OpenSession reply options properly.
     * Format: repeated [option_type (1 byte), option_len (1 byte), data (option_len bytes)]
     * Option 0 = Server Request Quantum (tx_quantum) */
    server->tx_quantum = 0;  /* Will be set if server provides it */
    offset = sizeof(struct dsi_header);
    p = server->incoming_buffer + offset;

    while (offset + 2 <= server->data_read) {
        option_type = (uint8_t)p[0];
        option_len = (uint8_t)p[1];

        if (offset + 2 + option_len > server->data_read) {
            break;  /* Malformed option */
        }

        if (option_type == 0 && option_len == 4) {
            /* Option 0: Server Request Quantum (tx_quantum) */
            uint32_t quantum;
            memcpy(&quantum, p + 2, 4);
            server->tx_quantum = ntohl(quantum);
        } else if (option_type == kServerReplayCacheSize && option_len == 4) {
            /* Option for replay cache */
            uint32_t cache_size;
            memcpy(&cache_size, p + 2, 4);
            server->replay_cache_size = ntohl(cache_size);
            server->supports_replay_cache = 1;
            log_for_client(NULL, AFPFSD, LOG_INFO,
                           "Replay cache enabled (size: %u)",
                           server->replay_cache_size);
        }

        offset += 2 + option_len;
        p += 2 + option_len;
    }

    /* Ensure tx_quantum has a sensible default if server didn't provide one */
    if (server->tx_quantum == 0) {
        server->tx_quantum = DSI_DEFAULT_TX_QUANTUM;
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Server did not provide tx_quantum, using default: %u",
                       server->tx_quantum);
    }
}

static int dsi_parse_versions(struct afp_server * server, char * msg)
{
    unsigned char num_versions = msg[0];
    int i, j = 0;
    char *p;
    unsigned char len;
    char tmpversionname[33];
    struct afp_versions * tmpversion;
    memset(server->versions, 0, SERVER_MAX_VERSIONS);

    if (num_versions > SERVER_MAX_VERSIONS) {
        num_versions = SERVER_MAX_VERSIONS;
    }

    p = msg + 1;

    for (i = 0; i < num_versions; i++) {
        len = copy_from_pascal(tmpversionname, p, sizeof(tmpversionname)) + 1;

        for (tmpversion = afp_versions; tmpversion->av_name; tmpversion++) {
            if (strcmp(tmpversion->av_name, tmpversionname) == 0) {
                server->versions[j] = tmpversion->av_number;
                j++;
                break;
            }
        }

        p += len;
    }

    return 0;
}

static int dsi_parse_uams(struct afp_server * server, char * msg)
{
    unsigned char num_uams = msg[0];
    unsigned char len;
    int i;
    char *p;
    char ua_name[AFP_UAM_LENGTH + 1];
    server->supported_uams = 0;
    memset(ua_name, 0, AFP_UAM_LENGTH + 1);

    if (num_uams > SERVER_MAX_UAMS) {
        num_uams = SERVER_MAX_UAMS;
    }

    p = msg + 1;

    for (i = 0; i < num_uams; i++) {
        len = copy_from_pascal(ua_name, p, AFP_UAM_LENGTH) +1;
        server->supported_uams |= uam_string_to_bitmap(ua_name);
        p += len;
    }

    return 0;
}


/* The parsing of the return for DSI GetStatus is the same as for
 * AFP GetSrvrInfo (which we don't yet support) */

void dsi_getstatus_reply(struct afp_server * server)
{
    /* Todo: check for buffer overruns */
    char *data, *p, *p2;
    int len;
    uint16_t *offset;
    /* This is the fixed portion */
    struct dsi_getstatus_header {
        struct dsi_header dsi __attribute__((__packed__));
        uint16_t machine_offset;
        uint16_t version_offset;
        uint16_t uams_offset;
        uint16_t icon_offset;
        uint16_t flags ;
    } __attribute__((__packed__)) * reply1 = (void *) server->incoming_buffer;
    struct reply2 {
        uint16_t signature_offset;
        uint16_t networkaddress_offset;
        uint16_t directoryservices_offset;
        uint16_t utf8servername_offset;
    } __attribute__((__packed__)) * reply2;

    if ((unsigned long) server->data_read < (sizeof(*reply1) + sizeof(*reply2))) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Got incomplete data for getstatus");
        return ;
    }

    data = (char *) server->incoming_buffer + sizeof(struct dsi_header);
    /* First, get the fixed portion */
    p = data + ntohs(reply1->machine_offset);
    copy_from_pascal(server->machine_type, p, sizeof(server->machine_type));
    p = data + ntohs(reply1->version_offset);
    dsi_parse_versions(server, p);
    p = data + ntohs(reply1->uams_offset);
    dsi_parse_uams(server, p);

    if (ntohs(reply1->icon_offset) > 0) {
        /* The icon and mask are optional */
        p = data + ntohs(reply1->icon_offset);
        memcpy(server->icon, p, AFP_SERVER_ICON_LEN);
    }

    server->flags = ntohs(reply1->flags);
    p = (void *)((unsigned long) server->incoming_buffer + sizeof(*reply1));
    p += copy_from_pascal(server->server_name, p, sizeof(server->server_name)) + 1;

    /* Now work our way through the variable bits */

    /* First, make sure we're on an even boundary */
    if (((uintptr_t) p) & 0x1) {
        p++;
    }

    /* Get the signature */
    uint16_t signature_offset;
    offset = (uint16_t *) p;
    signature_offset = ntohs(*offset);
    memcpy(server->signature,
           data + signature_offset,
           AFP_SIGNATURE_LEN);
    p += 2;

    /* The network addresses */
    if (server->flags & kSupportsTCP) {
        offset = (uint16_t *) p;
        /* We don't actually do anything with the network addresses,
         * but if we did, it'd go here */
        p += 2;
    }

    /* The directory names */
    if (server->flags & kSupportsDirServices) {
        offset = (uint16_t *) p;
        /* We don't actually do anything with the directory names,
         * but if we did, it'd go here */
        p += 2;
    }

    if (server->flags & kSupportsUTF8SrvrName) {
        /* And now the UTF8 server name */
        offset = (uint16_t *) p;
        uint16_t utf8_name_offset = ntohs(*offset);
        p2 = data + utf8_name_offset;
        /* Skip the hint character */
        p2 += 1;
        len = copy_from_pascal(server->server_name_utf8, p2,
                               sizeof(server->server_name_utf8));

        /* Workaround for netatalk that in some circumstances
         * puts the UTF8 servername off by one character,
         * notably when server hostname is used as server name */
        if (len == 0) {
            p2++;
            copy_from_pascal(server->server_name_utf8, p2,
                             sizeof(server->server_name_utf8));
        }

        convert_utf8dec_to_utf8pre(server->server_name_utf8,
                                   strlen(server->server_name_utf8),
                                   server->server_name_printable, AFP_SERVER_NAME_UTF8_LEN);
    } else {
        /* We don't have a UTF8 servername, so let's make one */
        iconv_t cd;
        size_t inbytesleft = strlen(server->server_name);
        size_t outbytesleft = AFP_SERVER_NAME_UTF8_LEN;
        char *inbuf = server->server_name;
        char *outbuf = server->server_name_printable;

        if ((cd  = iconv_open("MACINTOSH", "UTF-8")) == (iconv_t) -1) {
            return;
        }

        iconv(cd, &inbuf, &inbytesleft,
              &outbuf, &outbytesleft);
        iconv_close(cd);
    }
}


void dsi_incoming_closesession(struct afp_server *server)
{
    afp_unmount_all_volumes(server);
    loop_disconnect(server);
}

void dsi_incoming_tickle(struct afp_server * server)
{
    struct dsi_header  header;
    dsi_setup_header(server, &header, DSI_DSITickle);
    dsi_send(server, (char *) &header, sizeof(struct dsi_header),
             DSI_DONT_WAIT, DSI_DSITickle, NULL);
}


void *dsi_incoming_attention(void * other)
{
    struct afp_server * server = other;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t flags ;
    } __attribute__((__packed__)) *packet = (void *) server->attention_buffer;
    unsigned short flags;
    char mesg[AFP_LOGINMESG_LEN];
    unsigned char shutdown = 0;
    unsigned char mins = 0;
    unsigned char checkmessage = 0;
    memset(mesg, 0, AFP_LOGINMESG_LEN);

    /* The logic here's undocumented.  If we get an attention packet and
       there's no flag, then go check the message.  Also, go check the
       the message if there is a flag and we have the AFPATTN_MESG flag.
       Checked on netatalk 2.0.3. */

    /* It's a bit tough to find docs on this, but I found it at:

    http://web.archive.org/web/20010806173437/developer.apple.com/techpubs/macosx/Networking/AFPClient/AFPClient-15.html

    */

    if (ntohl(packet->header.length) >= 2) {
        flags = ntohs(packet->flags);

        if (flags & AFPATTN_MESG) {
            checkmessage = 1;
        }

        if (flags & (AFPATTN_CRASH | AFPATTN_SHUTDOWN)) {
            shutdown = 1;
        }

        mins = flags & 0xff;
    } else {
        checkmessage = 1;
    }

    if (checkmessage) {
        afp_getsrvrmsg(server, AFPMESG_SERVER,
                       ((server->using_version && server->using_version->av_number >= 30) ? 1 : 0),
                       server->dsi_default_timeout, mesg);

        if (bcmp(mesg, "The server is going down for maintenance.", 41) == 0) {
            shutdown = 1;
        }
    }

    if (shutdown) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Got a shutdown notice in packet %d, going down in %d mins",
                       ntohs(packet->header.requestid), mins);
        loop_disconnect(server);
        server->connect_state = SERVER_STATE_DISCONNECTED;
        return NULL;
    }

    return NULL;
}


struct dsi_request *dsi_find_request(struct afp_server *server,
                                     unsigned short request_id)
{
    struct dsi_request *p;
    pthread_mutex_lock(&server->request_queue_mutex);

    for (p = server->command_requests; p; p = p->next) {
        if (request_id == p->requestid) {
            pthread_mutex_unlock(&server->request_queue_mutex);
            return p;
        }
    }

    pthread_mutex_unlock(&server->request_queue_mutex);
    return NULL;
}

int dsi_recv(struct afp_server * server)
{
    struct dsi_header * header = (void *) server->incoming_buffer;
    struct dsi_request * request = NULL;
#ifdef DEBUG_DSI
    int rc;
#endif
    int amount_to_read = 0;
    int ret;
    unsigned char runt_packet = 0;

    /* Make sure we have at least one  header */
    if ((amount_to_read = sizeof(struct dsi_header) - server->data_read) > 0) {
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< read() for dsi, %d bytes",
                       amount_to_read);
#endif

        do {
            ret = read(server->fd, server->incoming_buffer + server->data_read,
                       amount_to_read);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error on fd=%d: %s (errno=%d)",
                           server->fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed by server (EOF on fd=%d, data_read=%d/%zu)",
                           server->fd, server->data_read, sizeof(struct dsi_header));
            return -1;
        }

        server->stats.rx_bytes += ret;
        server->data_read += ret;

        if ((server->data_read == sizeof(struct dsi_header)) &&
                (ntohl(header->length) == 0)) {
            goto gotenough;
        }

        return 0;
        /* We'll get the rest of the packet next time */
    }

gotenough:
    /* At this point, we have just the header */
    /* Figure out what it is a reply to */
    request = dsi_find_request(server, ntohs(header->requestid));

    if (!request && (header->flags == DSI_REPLY)) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "I have no idea what this is a reply to, id %d",
                       ntohs(header->requestid));
        runt_packet = 1;
        server->stats.runt_packets++;
    }

    /* Check if this is a stale reply from an old connection */
    if (request
            && request->connection_generation != server->connection_generation) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Discarding stale reply id %d from connection generation %u (current: %u)",
                       ntohs(header->requestid), request->connection_generation,
                       server->connection_generation);
        runt_packet = 1;
        server->stats.runt_packets++;
        request = NULL;
    }

    if (request) {
        request->return_code = ntohl(header->return_code.error_code);
    }

    /* If it is a read, read in as much as we can */
    if ((request) &&
            ((request->subcommand == afpRead) ||
             (request->subcommand == afpReadExt))) {
        struct afp_rx_buffer * buf = request->other;
        unsigned int newmax = buf->maxsize - buf->size;

        if (((server->data_read == sizeof(struct dsi_header)) &&
                (ntohl(header->length) == 0))) {
            server->data_read = 0;
            goto out;
        }

        if ((!buf) || (!buf->maxsize)) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "No buffer allocated for incoming data");
            return -1;
        }

        if (newmax > ntohl(header->length) - buf->size) {
            newmax = ntohl(header->length) - buf->size;
        }

#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "<<< read() in response to a request, %d bytes", newmax);
#endif

        do {
            ret = read(server->fd, buf->data + buf->size,
                       newmax);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error in afpRead response on fd=%d: %s (errno=%d)",
                           server->fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed during afpRead response (EOF on fd=%d)",
                           server->fd);
            return -1;
        }

        server->stats.rx_bytes += ret;
        buf->size += ret;

        /* Check to see if we've read enough */
        if (ntohl(header->length) == buf->size) {
            char *tmpbuf;
            int size_to_copy = server->data_read
                               - sizeof(struct dsi_header);

            if (size_to_copy == 0) {
                server->data_read = 0;
                goto out;
            } else if (size_to_copy < 0) {
                goto error;
            }

            /* Okay, so there is a buffer we have to shift */
            if ((tmpbuf = malloc(size_to_copy)) == NULL) {
                log_for_client(NULL, AFPFSD, LOG_ERR,
                               "Problem allocating memory for dsi_recv of size %d", size_to_copy);
                goto error;
            }

            memcpy(tmpbuf,
                   server->incoming_buffer +
                   sizeof(struct dsi_header),
                   size_to_copy);
            memcpy(server->incoming_buffer, tmpbuf,
                   size_to_copy);
            server->data_read = size_to_copy;
            free(tmpbuf);
        } else {
            return 0;
        }
    } else {
        /* Okay, so it isn't a response to an afpRead or afpReadExt */
        if (((server->data_read == sizeof(struct dsi_header)) &&
                (ntohl(header->length) == 0))) {
            goto process_packet;
        }

        amount_to_read = min(ntohl(header->length), (unsigned long) server->bufsize);
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< read() of rest of AFP, %d bytes",
                       amount_to_read);
#endif

        do {
            ret = read(server->fd, (void *)
                       (((unsigned long) server->incoming_buffer) + server->data_read),
                       amount_to_read);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "dsi_recv: read() error reading AFP packet on fd=%d: %s (errno=%d)",
                           server->fd, strerror(errno), errno);
            return -1;
        }

        if (ret == 0) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "dsi_recv: connection closed while reading AFP packet (EOF on fd=%d)",
                           server->fd);
            return -1;
        }

        server->stats.rx_bytes += ret;
        server->data_read += ret;

        if ((unsigned long) server->data_read < (ntohl(header->length) + sizeof(
                    *header))) {
            return 0;
        }
    }

    if (runt_packet) {
        goto after_processing;
    }

process_packet:
    /* At this point, we have a full DSI packet
       (or, for an afpRead, just the header and the data's been
        dumped in the preallocated buffer */
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "<<< Handling DSI command %d, request id %d",
                   header->command, ntohs(header->requestid));
#endif

    switch (header->command) {
    case DSI_DSICloseSession:
        dsi_incoming_closesession(server);
        break;

    case DSI_DSIGetStatus:
        dsi_getstatus_reply(server);
        break;

    case DSI_DSIOpenSession:
        dsi_opensession_reply(server);
        break;

    case DSI_DSITickle:
        dsi_incoming_tickle(server);
        break;

    case DSI_DSIWrite:
    case DSI_DSICommand:
        if (!runt_packet) {
            dsi_command_reply(server, request->subcommand, request->other);
        }

        break;

    case DSI_DSIAttention: {
        pthread_t thread;
        memcpy(server->attention_buffer,
               server->incoming_buffer,
               server->data_read);
        server->attention_len = server->data_read;
        pthread_create(&thread, NULL,
                       dsi_incoming_attention, server);
    }
    break;

    default:
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "Unknown DSI command %i", header->command);
        goto error;
    }

after_processing:

    if ((unsigned long) server->data_read == ntohl(header->length) + sizeof(
                *header)) {
        /* The most common case */
        server->data_read = 0;
    } else {
        unsigned int size_to_copy =
            server->data_read -
            (ntohl(header->length) + sizeof(*header));
        char *tmpbuf;

        if (size_to_copy < ntohl(header->length)) {
            /* This could be replaced with memmove, as it handles
             * overlaps */
            memcpy(server->incoming_buffer,
                   server->incoming_buffer + ntohl(header->length),
                   size_to_copy);
        } else {
            /* This is more complicated, we need an tmp buf */
            if ((tmpbuf = malloc(size_to_copy)) == NULL) {
                log_for_client(NULL, AFPFSD, LOG_ERR,
                               "Problem allocating memory for dsi_recv of size %d", size_to_copy);
                goto error;
            }

            memcpy(tmpbuf,
                   server->incoming_buffer + ntohl(header->length),
                   size_to_copy);
            memcpy(server->incoming_buffer, tmpbuf, size_to_copy);
            free(tmpbuf);
        }

        server->data_read -= size_to_copy;
    }

out:
#ifdef DEBUG_DSI
    rc = ntohl(header->return_code.error_code);
#endif

    if (request) {
#ifdef DEBUG_DSI
        log_for_client(NULL, AFPFSD, LOG_DEBUG, "<<< Found request %d, %s",
                       request->requestid,
                       afp_get_command_name(request->subcommand));
#endif

        if (request->wait) {
#ifdef DEBUG_DSI
            log_for_client(NULL, AFPFSD, LOG_DEBUG,
                           "<<< Signalling %d, returning %d or %d", request->requestid,
                           request->return_code, rc);
#endif
            pthread_mutex_lock(&request->waiting_mutex);
            request->wait = 0;
            request->done_waiting = 1;
            pthread_cond_signal(&request->waiting_cond);
            pthread_mutex_unlock(&request->waiting_mutex);
        } else {
            dsi_remove_from_request_queue(server, request);
        }
    }

    return 0;
error:
#ifdef DEBUG_DSI
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Returning from dsi_recv with an error");
#endif
    return -1;
}
