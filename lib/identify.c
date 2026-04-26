/*
 *  identify.c
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <string.h>
#include "afp.h"
#include "dsi.h"

/*
 * afp_server_identify()
 *
 * Identifies a server
 *
 * Right now, this only does identification using the machine_type
 * given in getsrvrinfo, but this could later use mDNS to get
 * more details.
 */

void afp_server_identify(struct afp_server * s)
{
    if (strncmp(s->machine_type, "Netatalk", 8) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Netatalk",
                       s->server_name_printable);
        s->server_type = AFPFS_SERVER_TYPE_NETATALK;
    } else if (strncmp(s->machine_type, "AirPort", 7) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as AirPort",
                       s->server_name_printable);
        s->server_type = AFPFS_SERVER_TYPE_AIRPORT;
    } else if (strncmp(s->machine_type, "Mac", 3) == 0
               || strncmp(s->machine_type, "iMac", 4) == 0
               || strncmp(s->machine_type, "Xserve", 6) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Macintosh",
                       s->server_name_printable);
        s->server_type = AFPFS_SERVER_TYPE_MACINTOSH;
    } else if (strncmp(s->machine_type, "TimeCapsule", 11) == 0) {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Identified server %s as Time Capsule",
                       s->server_name_printable);
        s->server_type = AFPFS_SERVER_TYPE_TIMECAPSULE;

        if (s->dsi_default_timeout == DSI_DEFAULT_TIMEOUT) {
            s->dsi_default_timeout = DSI_TIMECAPSULE_DEFAULT_TIMEOUT;
        }
    } else {
        log_for_client(NULL, AFPFSD, LOG_DEBUG,
                       "Could not identify server %s (machine type %s)",
                       s->server_name_printable,
                       s->machine_type);
        s->server_type = AFPFS_SERVER_TYPE_UNKNOWN;
    }
}
