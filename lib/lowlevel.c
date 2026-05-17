/*
    lowlevel.c: some functions that abstract common operations; used
	so the same code can be used between meta and normal files

    Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include "afp.h"
#include "afp_protocol.h"
#include "codepage.h"
#include "utils.h"
#include "midlevel.h"
#include "did.h"
#include "users.h"
#include "midlevel.h"
#include "forklist.h"

static void set_nonunix_perms(unsigned int * mode, struct afp_file_info *fp)
{
    if (fp->isdir) {
        *mode = 0700 | S_IFDIR;
    } else {
        *mode = 0600 | S_IFREG;
    }
}

int ll_handle_unlocking(struct afp_volume * volume, unsigned short forkid,
                        uint64_t offset, uint64_t sizetorequest)
{
    uint64_t generated_offset;
    int rc;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_NO_LOCKING) {
        return 0;
    }

    if (volume->server->using_version->av_number < 30)
        rc = afp_byterangelock(volume, ByteRangeLock_Unlock,
                               forkid, offset, sizetorequest,
                               (uint32_t *) &generated_offset);
    else
        rc = afp_byterangelockext(volume, ByteRangeLock_Unlock,
                                  forkid, offset, sizetorequest, &generated_offset);

    switch (rc) {
    case kFPNoErr:
        break;

    case kFPMiscErr:
    case kFPParamErr:
    case kFPRangeNotLocked:
    default:
        return -1;
    }

    return 0;
}

int ll_handle_locking(struct afp_volume * volume, unsigned short forkid,
                      uint64_t offset, uint64_t sizetorequest)
{
#define MAX_LOCKTRYCOUNT 10
    int rc = 0;

    int try = 0;

    uint64_t generated_offset;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_NO_LOCKING) {
        return 0;
    }

    while (try < MAX_LOCKTRYCOUNT) {
            try++;

            if (volume->server->using_version->av_number < 30)
                rc = afp_byterangelock(volume, ByteRangeLock_Lock,
                                       forkid, offset, sizetorequest,
                                       (uint32_t *) &generated_offset);
            else
                rc = afp_byterangelockext(volume, ByteRangeLock_Lock,
                                          forkid, offset, sizetorequest, &generated_offset);

            switch (rc) {
            case kFPNoErr:
                goto done;

            case kFPNoMoreLocks: /* Max num of locks on server */
            case kFPLockErr:  /*Some or all of the requested range is locked
				    by another user. */
                sleep(1);
                break;

            default:
                return -1;
            }
        }

done:
    return 0;
}




/* zero_file()
 *
 * This function will truncate the fork given to zero bytes in length.
 * This has been abstracted because there is some differences in the
 * expectation of Ext or not Ext. */

int ll_zero_file(struct afp_volume * volume, unsigned short forkid,
                 unsigned int resource)
{
    unsigned int bitmap;
    int ret;

    /* The Airport Extreme 7.1.1 will crash if you send it
     * DataForkLenBit. */

    if (volume->server->using_version->av_number < 30) {
        bitmap = (resource ?
                  kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        bitmap = (resource ?
                  kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    ret = afp_setforkparms(volume, forkid, bitmap, 0);

    switch (ret) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPVolLocked:
    case kFPLockErr:
        ret = EBUSY;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
        ret = EIO;
        break;

    default:
        ret = 0;
    }

    return ret;
}

int ll_setfork_size(struct afp_volume * volume, unsigned short forkid,
                    unsigned int resource, uint64_t size)
{
    unsigned int bitmap;
    int ret;

    /* The Airport Extreme 7.1.1 will crash if you send it
     * DataForkLenBit. */

    if (volume->server->using_version->av_number < 30) {
        bitmap = (resource ?
                  kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        bitmap = (resource ?
                  kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    ret = afp_setforkparms(volume, forkid, bitmap, size);

    switch (ret) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPVolLocked:
    case kFPLockErr:
        ret = EBUSY;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
        ret = EIO;
        break;

    default:
        ret = 0;
    }

    return ret;
}


/* get_directory_entry is used to abstract afp_getfiledirparms
 *  * because in AFP<3.0 there is only afp_getfileparms and afp_getdirparms.
 *   */

int ll_get_directory_entry(struct afp_volume * volume,
                           char *basename,
                           unsigned int dirid,
                           unsigned int filebitmap, unsigned int dirbitmap,
                           struct afp_file_info *p)
{
    int ret;
    char tmpname[AFP_MAX_PATH];
    memcpy(tmpname, p->basename, AFP_MAX_PATH);
    ret = afp_getfiledirparms(volume, dirid,
                              filebitmap, dirbitmap, basename, p);
    memcpy(p->basename, tmpname, AFP_MAX_PATH);
    return ret;
}



int ll_open(struct afp_volume * volume,
            const char *path __attribute__((unused)), int flags,
            struct afp_file_info *fp)
{
    int ret;
    int dsi_ret;
    int rc;
    unsigned char aflags = 0;
    /* O_RDONLY is 0, so we need to check access mode bits properly */
    int access_mode = flags & O_ACCMODE;

    if (access_mode == O_RDONLY) {
        aflags |= AFP_OPENFORK_ALLOWREAD;
    } else if (access_mode == O_WRONLY) {
        aflags |= AFP_OPENFORK_ALLOWWRITE;
    } else if (access_mode == O_RDWR) {
        aflags |= (AFP_OPENFORK_ALLOWREAD | AFP_OPENFORK_ALLOWWRITE);
    }

    if ((aflags & AFP_OPENFORK_ALLOWWRITE) &&
            (volume_is_readonly(volume))) {
        ret = EPERM;
        goto error;
    }

    /*
       O_NOBLOCK - todo: it is unclear how to this in fuse.
    */
    /* The following flags don't need any attention here:
       O_ASYNC - not relevant for files
       O_APPEND
       O_NOATIME - we have no way to handle this anyway
    */
    /*this will be used later for caching*/
    fp->sync = (unsigned char)(flags & (O_SYNC));

    /* Handle file creation properly when O_CREAT is set */
    if ((flags & O_CREAT) && (aflags & AFP_OPENFORK_ALLOWWRITE)) {
        if (flags & O_EXCL) {
            /* With O_EXCL, file must not exist. Use kFPHardCreate
             * (hard create - fails if file exists) instead of kFPSoftCreate */
            rc = afp_createfile(volume, kFPHardCreate, fp->did, fp->basename);

            if (rc == kFPObjectExists) {
                ret = EEXIST;
                goto error;
            } else if (rc != 0) {
                ret = EIO;
                goto error;
            }
        } else {
            /* Without O_EXCL, use kFPSoftCreate (soft create - succeeds if exists) */
            rc = afp_createfile(volume, kFPSoftCreate, fp->did, fp->basename);

            if (rc && rc != kFPObjectExists) {
                ret = EIO;
                goto error;
            }

            /* If rc == kFPObjectExists, file already exists, which is fine */
        }
    }

    if (volume->server->using_version->av_number < 30) {
        switch (ll_get_directory_entry(volume, fp->basename, fp->did,
                                       kFPParentDirIDBit | kFPNodeIDBit |
                                       (fp->resource ? kFPRsrcForkLenBit : kFPDataForkLenBit),
                                       0, fp)) {
        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPObjectNotFound:
            ret = ENOENT;
            goto error;

        case kFPNoErr:
            break;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPParamErr:
        default:
            ret = EIO;
            goto error;
        }

        if ((fp->resource ? (fp->resourcesize >= (AFP_MAX_AFP2_FILESIZE - 1)) :
                (fp->size >= AFP_MAX_AFP2_FILESIZE - 1))) {
            /* According to p.30, if the server doesn't support >4GB files
               and the file being opened is >4GB, then resourcesize or size
               will return 4GB.  How can it return 4GB in 32 bits?  I
               suspect it actually returns 4GB-1.
            */
            ret = EOVERFLOW;
            goto error;
        }
    }

    dsi_ret = afp_openfork(volume, fp->resource ? 1 : 0, fp->did,
                           aflags, fp->basename, fp);

    switch (dsi_ret) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPObjectNotFound:
        /* File not found and O_CREAT wasn't set (or wasn't honored above) */
        ret = ENOENT;
        goto error;

    case kFPObjectLocked:
        ret = EROFS;
        goto error;

    case kFPObjectTypeErr:
        ret = EISDIR;
        goto error;

    case kFPParamErr:
        ret = EACCES;
        goto error;

    case kFPTooManyFilesOpen:
        ret = EMFILE;
        goto error;

    case kFPVolLocked:
    case kFPDenyConflict:
    case kFPMiscErr:
    case kFPBitmapErr:
    case -1:
        ret = EFAULT;
        goto error;

    case 0:
        ret = 0;
        break;

    default:
        ret = EFAULT;
        goto error;
    }

    add_opened_fork(volume, fp);

    /* Handle O_TRUNC flag: truncate existing files (but not newly created ones) */
    /* Skip truncation if O_CREAT was set (file just created, already empty) */
    if ((flags & O_TRUNC) && !(flags & O_CREAT)) {
        ret = ll_zero_file(volume, fp->forkid, fp->resource);

        if (ret != 0) {
            goto error;
        }
    }

    return 0;
error:
    return -ret;
}


/* FIXME: chunked reads are not implemented. The original intent was to loop,
 * issuing rx_quantum-sized requests until all bytes were read (see the #if 0
 * bytesleft block below). Currently a single afp_read/afp_readext call is
 * issued for the full size, which means when size > rx_quantum the server is
 * asked for more data than buffer.maxsize can hold. Either restore the loop or
 * remove the rx_quantum cap on buffer.maxsize and document that chunking is
 * delegated to the AFP layer. */
int ll_read(struct afp_volume * volume,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof)
{
#if 0
    int bytesleft = size;
#endif
    int totalsize = 0;
    int ret = 0;
    int rc;
    unsigned int bufsize = min(volume->server->rx_quantum, size);
    struct afp_rx_buffer buffer;
    *eof = 0;
    buffer.data = buf;
    buffer.maxsize = bufsize;
    buffer.size = 0;

    /* Lock the range */
    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        /* There was an irrecoverable error when locking */
        ret = EBUSY;
        goto error;
    }

    if (volume->server->using_version->av_number < 30) {
        rc = afp_read(volume, fp->forkid, offset, size, &buffer);
    } else {
        rc = afp_readext(volume, fp->forkid, offset, size, &buffer);
    }

    if (ll_handle_unlocking(volume, fp->forkid, offset, size)) {
        /* Somehow, we couldn't unlock the range. */
        ret = EIO;
        goto error;
    }

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPLockErr:
        ret = EBUSY;
        goto error;

    case kFPMiscErr:
    case kFPParamErr:
        ret = EIO;
        goto error;

    case kFPEOFErr:
        *eof = 1;
        break;

    case kFPNoErr:
        break;
    }

#if 0
    bytesleft -= buffer.size;
#endif
    totalsize += buffer.size;
    return totalsize;
error:
    return -ret;
}




int ll_readdir(struct afp_volume * volume, const char *path,
               struct afp_file_info **fb, int resource)
{
    struct afp_file_info * p, *filebase = NULL, *base, *last = NULL;
    unsigned short reqcount = 256; /* Get them in batches of 256 */
    unsigned long startindex = 1;
    int rc = 0, ret = 0, exit = 0;
    unsigned int filebitmap, dirbitmap;
    char basename[AFP_MAX_PATH];
    char converted_name[AFP_MAX_PATH];
    unsigned int dirid;

    if (invalid_filename(volume->server, path)) {
        return -ENAMETOOLONG;
    }

    if (get_dirid(volume, path, basename, &dirid) < 0) {
        return -ENOENT;
    }

    /* We need to handle length bits differently for AFP < 3.0 */
    filebitmap = kFPAttributeBit | kFPParentDirIDBit |
                 kFPCreateDateBit | kFPModDateBit |
                 kFPBackupDateBit |
                 kFPNodeIDBit;
    dirbitmap = kFPAttributeBit | kFPParentDirIDBit |
                kFPCreateDateBit | kFPModDateBit |
                kFPBackupDateBit |
                kFPNodeIDBit | kFPOffspringCountBit |
                kFPOwnerIDBit | kFPGroupIDBit;

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        dirbitmap |= kFPUnixPrivsBit;
        filebitmap |= kFPUnixPrivsBit;
    }

    if (volume->attributes & kSupportsUTF8Names) {
        dirbitmap |= kFPUTF8NameBit;
        filebitmap |= kFPUTF8NameBit;
    } else {
        dirbitmap |= kFPLongNameBit | kFPShortNameBit;
        filebitmap |= kFPLongNameBit | kFPShortNameBit;
    }

    if (volume->server->using_version->av_number < 30) {
        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    while (!exit) {
        /* Select appropriate enumerate command based on AFP version:
         * AFP < 3.0: afp_enumerate (16-bit fields, 4GB volume limit)
         * AFP 3.0: afp_enumerateext (16-bit indexes, handles >4GB volumes)
         * AFP >= 3.1: afp_enumerateext2 (32-bit indexes for large directories) */
        if (volume->server->using_version->av_number < 30) {
            rc = afp_enumerate(volume, dirid,
                               filebitmap, dirbitmap, reqcount,
                               startindex, basename, &base);
        } else if (volume->server->using_version->av_number == 30) {
            rc = afp_enumerateext(volume, dirid,
                                  filebitmap, dirbitmap, reqcount,
                                  startindex, basename, &base);
        } else {
            rc = afp_enumerateext2(volume, dirid,
                                   filebitmap, dirbitmap, reqcount,
                                   startindex, basename, &base);
        }

        switch (rc) {
        case -1:
            ret = EIO;
            goto error;

        case 0:
        case kFPObjectNotFound:
            if (filebase == NULL) {
                filebase = base;
            } else {
                last->next = base;
            }

            for (p = base; p; p = p->next) {
                startindex++;
                last = p;
            }

            if (rc == kFPObjectNotFound) {
                exit = 1;
            }

            break;

        case kFPAccessDenied:
            ret = EACCES;
            goto error;

        case kFPDirNotFound:
            ret = ENOENT;
            exit++;
            break;

        case kFPBitmapErr:
        case kFPMiscErr:
        case kFPObjectTypeErr:
        case kFPParamErr:
        case kFPCallNotSupported:
            ret = EIO;
            goto error;
        }
    }

    for (p = filebase; p; p = p->next) {
        /* Convert all the names back to precomposed */
        convert_path_to_unix(
            volume->server->path_encoding,
            converted_name, p->name, AFP_MAX_PATH);
        snprintf(p->name, AFP_MAX_PATH, "%s", converted_name);
        p->name[AFP_MAX_PATH - 1] = '\0';
        startindex++;
    }

    if (volume->server->using_version->av_number < 30) {
        for (p = filebase; p; p = p->next) {
            unsigned int temp_permissions = p->unixprivs.permissions;
            set_nonunix_perms(&temp_permissions, p);
            p->unixprivs.permissions = temp_permissions;
        }
    }

    *fb = filebase;
    return 0;
error:
    return -ret;
}


int ll_getattr(struct afp_volume * volume, const char *path, struct stat *stbuf,
               int resource)
{
    struct afp_file_info fp;
    unsigned int dirid;
    int rc;
    unsigned int filebitmap, dirbitmap;
    char basename[AFP_MAX_PATH];
    unsigned int creation_date;
    unsigned int modification_date;
    memset(stbuf, 0, sizeof(struct stat));

    if ((volume->server) &&
            (invalid_filename(volume->server, path))) {
        return -ENAMETOOLONG;
    }

    if (get_dirid(volume, path, basename, &dirid) < 0) {
        return -ENOENT;
    }

    dirbitmap = kFPAttributeBit
                | kFPCreateDateBit | kFPModDateBit |
                kFPNodeIDBit |
                kFPParentDirIDBit | kFPOffspringCountBit;
    filebitmap = kFPAttributeBit |
                 kFPCreateDateBit | kFPModDateBit |
                 kFPNodeIDBit |
                 kFPFinderInfoBit |
                 kFPParentDirIDBit;

    if (volume->server->using_version->av_number < 30) {
        if (path[0] == '/' && path[1] == '\0') {
            /* This will sound odd, but when referring to /, AFP 2.x
               clients check on a 'file' with the volume name. */
            snprintf(basename, AFP_MAX_PATH, "%s",
                     volume->volume_name);
            dirid = 1;
        }

        filebitmap |= (resource ? kFPRsrcForkLenBit : kFPDataForkLenBit);
    } else {
        filebitmap |= (resource ? kFPExtRsrcForkLenBit : kFPExtDataForkLenBit);
    }

    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        dirbitmap |= kFPUnixPrivsBit;
        filebitmap |= kFPUnixPrivsBit;
    } else {
        dirbitmap |= kFPOwnerIDBit | kFPGroupIDBit;
    }

    rc = afp_getfiledirparms(volume, dirid, filebitmap, dirbitmap,
                             (char *) basename, &fp);

    switch (rc) {
    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPNoErr:
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPParamErr:
    default:
        return -EIO;
    }

    if (volume->server->using_version->av_number >= 30
            && fp.unixprivs.permissions != 0) {
        stbuf->st_mode |= fp.unixprivs.permissions;
    } else {
        set_nonunix_perms((unsigned int *)&stbuf->st_mode, &fp);
    }

    stbuf->st_uid = fp.unixprivs.uid;
    stbuf->st_gid = fp.unixprivs.gid;

    if (translate_uidgid_to_client(volume,
                                   &stbuf->st_uid, &stbuf->st_gid)) {
        return -EIO;
    }

    if (stbuf->st_mode & S_IFDIR) {
        stbuf->st_nlink = fp.offspring + 2;
        stbuf->st_size = (fp.offspring * 34) + 24;
        /* This slight voodoo was taken from Mac OS X 10.2 */
    } else {
        stbuf->st_nlink = 1;
        stbuf->st_size = (resource ? fp.resourcesize : fp.size);
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = (stbuf->st_size) / 4096;
    }

    if ((volume->server->using_version->av_number < 30) &&
            (stbuf->st_mode & S_IFDIR)) {
        /* AFP 2.x doesn't give ctime and mtime for directories*/
        creation_date = volume->server->connect_time;
        modification_date = volume->server->connect_time;
    } else if ((path[0] == '/' && path[1] == '\0') && (stbuf->st_mode & S_IFDIR)) {
        /* For the root directory, use the mount time as creation/modification date */
        creation_date = volume->mount_time;
        modification_date = volume->mount_time;
    } else {
        creation_date = fp.creation_date;
        modification_date = fp.modification_date;
    }

#ifdef __linux__
    stbuf->st_ctim.tv_sec = creation_date;
    stbuf->st_mtim.tv_sec = modification_date;
#else
    stbuf->st_ctime = creation_date;
    stbuf->st_mtime = modification_date;
#endif
#ifdef __APPLE__
    /* On macOS, set birthtimespec for Finder display (handled by stat_to_darwin_attr) */
    stbuf->st_birthtimespec.tv_sec = creation_date;
    stbuf->st_birthtimespec.tv_nsec = 0;
#endif
    return 0;
}


int ll_write(struct afp_volume * volume,
             const char *data, size_t size, off_t offset,
             struct afp_file_info * fp, size_t *totalwritten)
{
    int ret, err = 0;
    uint64_t sizetowrite, ignored;
    uint32_t ignored32;
    unsigned int max_packet_size = volume->server->tx_quantum;
    off_t o = 0;
    *totalwritten = 0;

    if (!fp) {
        return -EBADF;
    }

    /* Sanity check: tx_quantum must be non-zero */
    if (max_packet_size == 0) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "ll_write: tx_quantum is 0, cannot write");
        return -EIO;
    }

    /* Get a lock */
    if (ll_handle_locking(volume, fp->forkid, offset, size)) {
        /* There was an irrecoverable error when locking */
        ret = EBUSY;
        goto error;
    }

    ret = 0;

    while (*totalwritten < size) {
        sizetowrite = max_packet_size;

        if ((size - *totalwritten) < max_packet_size) {
            sizetowrite = size - *totalwritten;
        }

        /* Defensive check: never send a zero-byte write */
        if (sizetowrite == 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "ll_write: sizetowrite is 0, aborting");
            err = EIO;
            goto error;
        }

        if (volume->server->using_version->av_number < 30) {
            ret = afp_write(volume, fp->forkid,
                            offset + o, sizetowrite,
                            (char *) data + o, &ignored32);
        } else {
            ret = afp_writeext(volume, fp->forkid,
                               offset + o, sizetowrite,
                               (char *) data + o, &ignored);
        }

        switch (ret) {
        case kFPAccessDenied:
            err = EACCES;
            goto error;

        case kFPDiskFull:
            err = ENOSPC;
            goto error;

        case kFPLockErr:
        case kFPMiscErr:
        case kFPParamErr:
            err = EINVAL;
            goto error;
        }

        *totalwritten += sizetowrite;
        o += sizetowrite;
    }

    if (ll_handle_unlocking(volume, fp->forkid, offset, size)) {
        /* Somehow, we couldn't unlock the range. */
        ret = EIO;
        goto error;
    }

    return 0;
error:
    return -err;
}
