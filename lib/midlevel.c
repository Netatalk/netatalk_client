/*
    midlevel.c: some funtions to abstract some of the common functions

    Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025 Daniel Markstedt <daniel@mindani.net>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/


#include "afp.h"

#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <fcntl.h>

/* Cross-platform extended attribute error code */
#ifndef ENOATTR
#ifdef ENODATA
#define ENOATTR ENODATA
#else
#define ENOATTR ENOENT  /* Fallback for systems without ENODATA or ENOATTR */
#endif
#endif

#include "users.h"
#include "did.h"
#include "resource.h"
#include "utils.h"
#include "codepage.h"
#include "midlevel.h"
#include "afp_internal.h"
#include "forklist.h"
#include "uams.h"
#include "lowlevel.h"


#define min(a,b) (((a)<(b)) ? (a) : (b))

static int get_unixprivs(struct afp_volume * volume,
                         unsigned int dirid,
                         const char *path, struct afp_file_info * fp)
{
    switch (ll_get_directory_entry(volume, (char *)path,
                                   dirid, kFPUnixPrivsBit, kFPUnixPrivsBit, fp)) {
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

    return 0;
}


static int set_unixprivs(struct afp_volume * vol,
                         unsigned int dirid,
                         const char *basename, struct afp_file_info * fp)

{
#define TOCHECK_BITS \
	(S_IRUSR |S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | \
	 S_IROTH | S_IWOTH | S_IXOTH )
    int ret = 0, rc;
    fp->unixprivs.ua_permissions = 0;

    if (!(vol->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX)) {
        return 0;
    }

    if (fp->isdir) {
        rc = afp_setdirparms(vol, dirid, basename,
                             kFPUnixPrivsBit, fp);
    } else {
        rc = afp_setfiledirparms(vol, dirid, basename,
                                 kFPUnixPrivsBit, fp);
    }

    switch (rc) {
    case kFPAccessDenied:
        ret = EPERM;
        break;

    case kFPBitmapErr:
        ret = ENOSYS;
        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case 0:
        ret = 0;
        break;

    case kFPMiscErr:
    case kFPObjectTypeErr:
    case kFPParamErr:
    default:
        ret = EIO;
        break;
    }

    return -ret;
}

void add_file_by_name(struct afp_file_info ** base, const char *filename)
{
    struct afp_file_info * t, *new_file;
    new_file = malloc(sizeof(*new_file));
    memcpy(new_file->name, filename, AFP_MAX_PATH);
    new_file->next = NULL;

    if (*base == NULL) {
        *base = new_file;
    } else {
        for (t = *base; t->next; t = t->next);

        t->next = new_file;
    }
}

void afp_ml_filebase_free(struct afp_file_info **filebase)
{
    struct afp_file_info * p, *next;

    if (filebase == NULL) {
        return;
    }

    p = *filebase;

    while (p) {
        next = p->next;
        free(p);
        p = next;
    }

    *filebase = NULL;
}





/*
 * set_uidgid()
 *
 * This sets the userid and groupid in an afp_file_info struct using the
 * appropriate translation.  You should pass it the host's version of the
 * uid and gid.
 *
 */

static int set_uidgid(struct afp_volume * volume,
                      struct afp_file_info * fp, uid_t uid, gid_t gid)
{
    unsigned int newuid = uid;
    unsigned int newgid = gid;
    translate_uidgid_to_server(volume, &newuid, &newgid);
    fp->unixprivs.uid = newuid;
    fp->unixprivs.gid = newgid;
    return 0;
}

static void update_time(unsigned int * newtime)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *newtime = tv.tv_sec;
}

int ml_open(struct afp_volume * volume, const char *path, int flags,
            struct afp_file_info **newfp)
{
    /* FIXME:  doesn't handle create properly */
    struct afp_file_info * fp ;
    int ret;
    unsigned int dirid;
    char converted_path[AFP_MAX_PATH];

    if (convert_path_to_afp(volume->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (invalid_filename(volume->server, converted_path)) {
        return -ENAMETOOLONG;
    }

    if (volume_is_readonly(volume) &&
            (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND | O_CREAT))) {
        return -EACCES;
    }

    if ((fp = malloc(sizeof(*fp))) == NULL) {
        return -1;
    }

    *newfp = fp;
    memset(fp, 0, sizeof(*fp));
    ret = appledouble_open(volume, path, flags, fp);

    if (ret < 0) {
        free(fp);
        return ret;
    }

    if (ret == 1) {
        goto out;
    }

    if (get_dirid(volume, converted_path, fp->basename, &dirid) < 0) {
        return -ENOENT;
    }

    fp->did = dirid;
    ret = ll_open(volume, converted_path, flags, fp);

    if (ret < 0) {
        goto error;
    }

out:
    return 0;
error:
    free(fp);
    return ret;
}



int ml_creat(struct afp_volume * volume, const char *path, mode_t mode)
{
    int ret = 0;
    char basename[AFP_MAX_PATH];
    unsigned int dirid;
    struct afp_file_info fp;
    int rc;
    char converted_path[AFP_MAX_PATH];

    if (convert_path_to_afp(volume->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(volume)) {
        return -EACCES;
    }

    ret = appledouble_creat(volume, path, mode);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    if (invalid_filename(volume->server, converted_path)) {
        return -ENAMETOOLONG;
    }

    get_dirid(volume, converted_path, basename, &dirid);
    rc = afp_createfile(volume, kFPSoftCreate, dirid, basename);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPObjectExists:
        /* File exists - this can happen on retry after transient failure.
         * Continue to ensure permissions are set correctly for writing. */
        ret = EEXIST;
        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case kFPFileBusy:
    case kFPVolLocked:
        ret = EBUSY;
        break;

    case kFPNoErr:
        ret = 0;
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
    }

    /* Return early only for fatal errors, not EEXIST */
    if (ret && ret != EEXIST) {
        return -ret;
    }

    /* If we don't support unixprivs, just exit */
    if (~ volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        return 0;
    }

    /* Figure out the privs of the file we just created */
    if ((ret = get_unixprivs(volume,
                             dirid, basename, &fp))) {
        return rc;
    }

    if (ret) {
        return -ret;
    }

    /* Strip file type bits, keep only permission bits */
    mode &= 07777;

    /* If no permission bits are set, use a sensible default (0644) */
    if (mode == 0) {
        mode = 0644;
    }

    /* Ensure owner write permission for initial creation to allow subsequent writes */
    mode |= S_IWUSR;

    if (fp.unixprivs.permissions == mode) {
        return 0;
    }

    fp.unixprivs.ua_permissions = 0;
    fp.unixprivs.permissions = mode;
    fp.isdir = 0; /* Anything you make with mknod is a file */
    /* note that we're not monkeying with the ownership here */
    rc = set_unixprivs(volume, dirid, basename, &fp);

    switch (rc) {
    case kFPAccessDenied:
        ret = EPERM;
        goto error;

    case kFPObjectNotFound:
        ret = ENOENT;
        goto error;

    case 0:
        ret = 0;
        break;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPObjectTypeErr:
    case kFPParamErr:
    default:
        ret = EIO;
        goto error;
    }

error:
    return -ret;
}



int ml_readdir(struct afp_volume * volume,
               const char *path,
               struct afp_file_info **fb)
{
    int ret = 0;
    char converted_path[AFP_MAX_PATH];

    if (convert_path_to_afp(volume->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    ret = appledouble_readdir(volume, converted_path, fb);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        goto done;
    }

    return ll_readdir(volume, converted_path, fb, 0);
done:
    return 0;
}

int ml_read(struct afp_volume * volume, const char *path,
            char *buf, size_t size, off_t offset,
            struct afp_file_info *fp, int *eof)
{
    int ret = 0;
    char converted_path[AFP_MAX_PATH];
    size_t amount_copied = 0;
    *eof = 0;

    if (path && convert_path_to_afp(volume->server->path_encoding,
                                    converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (fp->resource) {
        ret = appledouble_read(volume, fp, buf, size, offset, &amount_copied, eof);

        if (ret < 0) {
            return ret;
        }

        if (ret == 1) {
            return amount_copied;
        }
    }

    ret = ll_read(volume, buf, size, offset, fp, eof);
    return ret;
}


int ml_chmod(struct afp_volume * vol, const char * path, mode_t mode)
{
    /*
    chmod has an interesting story to it.

    It is known to work with Darwin 10.3.9 (AFP 3.1), 10.4.2 and 10.5.x (AFP 3.2).

    chmod will not work properly in the following situations:

    - AFP 2.2, this needs some more verification but I don't see how it is possible

    - netatalk 3.0 and later has the option "unix priv = no" in afp.conf
      which disables support for unix privileges altogether

    - netatalk 2.0.3 and probably earlier had limited support for unix privileges,
      notably not setting the execute bit at all

    The right way to see if a volume supports chmod is to check the attributes
    found with getvolparm or volopen, then to test chmod the first time.

    */
    int ret = 0, rc;
    struct afp_file_info fp;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];
    uid_t uid;
    gid_t gid;

    if (invalid_filename(vol->server, path)) {
        return -ENAMETOOLONG;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    /* There's no way to do this in AFP < 3.0 */
    if (~ vol->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        if (vol->extra_flags & VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS) {
            struct stat stbuf;
            /* See if the file exists */
            ret = ll_getattr(vol, converted_path, &stbuf, 0);
            return ret;
        }

        return -ENOSYS;
    };

    ret = appledouble_chmod(vol, path, mode);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, converted_path, basename, &dirid);

    if ((rc = get_unixprivs(vol,
                            dirid, basename, &fp))) {
        return rc;
    }

    mode &= (~S_IFDIR);

    /* Don't bother updating it if it's already the same */
    if ((fp.unixprivs.permissions & (~S_IFDIR)) == mode) {
        return 0;
    }

    /* Check to make sure that we can; some servers (at least netatalk)
       don't report an error when you try to setfileparm when you don't
       own the file.  */
    /* Try to guess if the operation is possible */
    uid = fp.unixprivs.uid;
    gid = fp.unixprivs.gid;

    if (translate_uidgid_to_client(vol, &uid, &gid)) {
        return -EIO;
    }

    if ((gid != getgid()) && (uid != geteuid())) {
        return -EPERM;
    }

    fp.unixprivs.permissions = mode;
    rc = set_unixprivs(vol, dirid, basename, &fp);

    if (rc == -ENOSYS) {
        return -ENOSYS;
    }

    return rc;
}


int ml_unlink(struct afp_volume * vol, const char *path)
{
    int ret, rc;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_unlink(vol, path);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, (char *) converted_path, basename, &dirid);

    if (is_dir(vol, dirid, basename)) {
        return -EISDIR;
    }

    if (invalid_filename(vol->server, converted_path)) {
        return -ENAMETOOLONG;
    }

    rc = afp_delete(vol, dirid, basename);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPObjectLocked:
        ret = EBUSY;
        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case kFPVolLocked:
        ret = EACCES;
        break;

    case kFPObjectTypeErr:
    case kFPDirNotEmpty:
    case kFPMiscErr:
    case kFPParamErr:
    case -1:
        ret = EINVAL;
        break;

    default:
        remove_did_entry(vol, converted_path);
        ret = 0;
    }

    return -ret;
}




int ml_mkdir(struct afp_volume * vol, const char * path, mode_t mode)
{
    int ret, rc;
    unsigned int result_did;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];
    unsigned int dirid;

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (invalid_filename(vol->server, path)) {
        return -ENAMETOOLONG;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_mkdir(vol, path, mode);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, converted_path, basename, &dirid);
    rc = afp_createdir(vol, dirid, basename, &result_did);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPDiskFull:
        ret = ENOSPC;
        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case kFPObjectExists:
        ret = EEXIST;
        break;

    case kFPVolLocked:
        ret = EBUSY;
        break;

    case kFPFlatVol:
    case kFPMiscErr:
    case kFPParamErr:
    case -1:
        ret = EFAULT;
        break;

    default:
        ret = 0;
    }

    return -ret;
}

int ml_close(struct afp_volume * volume, const char * path,
             struct afp_file_info * fp)
{
    int ret = 0;
    char converted_path[AFP_MAX_PATH];

    if (path) {
        if (convert_path_to_afp(volume->server->path_encoding,
                                converted_path, (char *) path, AFP_MAX_PATH)) {
            return -EINVAL;
        }

        if (invalid_filename(volume->server, converted_path)) {
            return -ENAMETOOLONG;
        }
    }

    /* The logic here is that if we don't have an fp anymore, then the
       fork must already be closed. */
    if (!fp) {
        return EBADF;
    }

    if (fp->icon) {
        free(fp->icon);
    }

    if (fp->resource) {
        return appledouble_close(volume, fp);
    }

    switch (afp_closefork(volume, fp->forkid)) {
    case kFPNoErr:
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        goto error;
    }

    remove_opened_fork(volume, fp);
error:
    return ret;
}

int ml_getattr(struct afp_volume * volume, const char *path, struct stat *stbuf)
{
    char converted_path[AFP_MAX_PATH];
    int ret;
    memset(stbuf, 0, sizeof(struct stat));

    if (convert_path_to_afp(volume->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    /* If this is a fake file (comment, finderinfo, rsrc), continue since
     * we'll use the permissions of the real file */
    ret = appledouble_getattr(volume, converted_path, stbuf);

    if (ret < 0) {
        return ret;
    }

    if (ret > 0) {
        return 0;
    }

    ret = ll_getattr(volume, converted_path, stbuf, 0);
    return ret;
}

int ml_write(struct afp_volume * volume, const char * path,
             const char *data, size_t size, off_t offset,
             struct afp_file_info * fp, uid_t uid,
             gid_t gid)
{
    int ret;
    size_t totalwritten = 0;
#if 0
    uint64_t sizetowrite, ignored;
    unsigned char flags = 0;
    unsigned int max_packet_size = volume->server->tx_quantum;
#endif
    char converted_path[AFP_MAX_PATH];

    /* TODO:
       - handle nonblocking IO correctly
    */
    if ((volume->server->using_version->av_number < 30) &&
            (size > AFP_MAX_AFP2_FILESIZE)) {
        return -EFBIG;
    }

    if (path && convert_path_to_afp(volume->server->path_encoding,
                                    converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(volume)) {
        return -EACCES;
    }

    ret = appledouble_write(volume, fp, data, size, offset, &totalwritten);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return totalwritten;
    }

    /* Set the time and perms */

    /* There's no way to do this in AFP < 3.0 */
    if (volume->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
#if 0
        flags |= kFPUnixPrivsBit;
#endif
        set_uidgid(volume, fp, uid, gid);
        fp->unixprivs.permissions = 0100644;
    };

    update_time(&fp->modification_date);

#if 0
    flags |= kFPModDateBit;

#endif
    ret = ll_write(volume, data, size, offset, fp, &totalwritten);

    if (ret < 0) {
        return ret;
    }

    /* Update the cached file size if the write extended the file */
    if (offset + totalwritten > fp->size) {
        fp->size = offset + totalwritten;
    }

    return totalwritten;
}

int ml_readlink(struct afp_volume * vol, const char * path,
                char *buf, size_t size)
{
    int rc, ret;
    struct afp_file_info fp;
    struct afp_rx_buffer buffer;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];
    unsigned int dirid;
    char link_path[AFP_MAX_PATH];
    memset(buf, 0, size);
    memset(link_path, 0, AFP_MAX_PATH);
    buffer.data = link_path;
    buffer.maxsize = size;
    buffer.size = 0;

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    get_dirid(vol, converted_path, basename, &dirid);
    /* Open the fork */
    rc = afp_openfork(vol, 0, dirid,
                      AFP_OPENFORK_ALLOWWRITE | AFP_OPENFORK_ALLOWREAD,
                      basename, &fp);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPObjectNotFound:
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
    case 0:
        ret = 0;
        break;

    case -1:
    default:
        ret = EFAULT;
        goto error;
    }

    add_opened_fork(vol, &fp);

    /* Read the name of the file from it */
    if (vol->server->using_version->av_number < 30) {
        rc = afp_read(vol, fp.forkid, 0, size, &buffer);
    } else {
        rc = afp_readext(vol, fp.forkid, 0, size, &buffer);
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
    case kFPNoErr:
        break;
    }

    switch (afp_closefork(vol, fp.forkid)) {
    case kFPNoErr:
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        goto error;
    }

    remove_opened_fork(vol, &fp);
    /* Convert the name back precomposed UTF8 */
    convert_path_to_unix(vol->server->path_encoding,
                         buf, (char *) link_path, AFP_MAX_PATH);
    return 0;
error:
    return -ret;
}

int ml_rmdir(struct afp_volume * vol, const char *path)
{
    int ret, rc;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];

    if (invalid_filename(vol->server, path)) {
        return -ENAMETOOLONG;
    }

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_rmdir(vol, path);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, converted_path, basename, &dirid);

    if (!is_dir(vol, dirid, basename)) {
        return -ENOTDIR;
    }

    rc = afp_delete(vol, dirid, basename);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPObjectLocked:
        ret = EBUSY;
        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case kFPVolLocked:
        ret = EACCES;
        break;

    case kFPDirNotEmpty:
        ret = ENOTEMPTY;
        break;

    case kFPObjectTypeErr:
    case kFPMiscErr:
    case kFPParamErr:
    case -1:
        ret = EINVAL;
        break;

    default:
        remove_did_entry(vol, converted_path);
        ret = 0;
    }

    return -ret;
}

int ml_chown(struct afp_volume * vol, const char * path,
             uid_t uid, gid_t gid)
{
    int ret;
    struct afp_file_info fp;
    int rc;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (invalid_filename(vol->server, converted_path)) {
        return -ENAMETOOLONG;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_chown(vol, path, uid, gid);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    /* There's no way to do this in AFP < 3.0 */
    if (~ vol->extra_flags & VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX) {
        if (vol->extra_flags & VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS) {
            struct stat stbuf;
            /* See if the file exists */
            ret = ll_getattr(vol, converted_path, &stbuf, 0);
            return ret;
        }

        return -ENOSYS;
    };

    get_dirid(vol, converted_path, basename, &dirid);

    if ((rc = get_unixprivs(vol,
                            dirid, basename, &fp))) {
        return rc;
    }

#if 0
    FIXME
    set_uidgid(volume, &fp, uid, gid);
    THIS IS the wrong set of returns to check...
#endif
    rc = set_unixprivs(vol, dirid, basename, &fp);

    switch (rc) {
    case -ENOSYS:
        return -ENOSYS;

    case kFPNoErr:
        break;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPObjectTypeErr:
    case kFPParamErr:
    default:
        break;
    }

    return 0;
}

int ml_truncate(struct afp_volume * vol, const char * path, off_t offset)
{
    int ret = 0;
    char converted_path[AFP_MAX_PATH];
    struct afp_file_info *fp;
    int flags;

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    /* The approach here is to get the forkid by calling ml_open()
       (and not afp_openfork).  Note the fake afp_file_info used
       just to grab this forkid. */

    if (invalid_filename(vol->server, converted_path)) {
        return -ENAMETOOLONG;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_truncate(vol, path, offset);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    /* Here, we're going to use the untranslated path since it is
       translated through the ml_open() */
    flags = O_WRONLY;

    if ((ml_open(vol, path, flags, &fp))) {
        return ret;
    };

    if ((ret = ll_zero_file(vol, fp->forkid, 0))) {
        goto out;
    }

    afp_closefork(vol, fp->forkid);
    remove_opened_fork(vol, fp);
    free(fp);
out:
    return -ret;
}

int ml_setfork_size(struct afp_volume * vol, unsigned short forkid,
                    unsigned int resource, uint64_t size)
{
    return -ll_setfork_size(vol, forkid, resource, size);
}


int ml_utime(struct afp_volume * vol, const char * path,
             struct utimbuf * timebuf)
{
    int ret = 0;
    unsigned int dirid;
    struct afp_file_info fp;
    char basename[AFP_MAX_PATH];
    char converted_path[AFP_MAX_PATH];
    int rc;

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    memset(&fp, 0, sizeof(struct afp_file_info));
    fp.modification_date = timebuf->modtime;

    if (invalid_filename(vol->server, path)) {
        return -ENAMETOOLONG;
    }

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path, (char *) path, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    ret = appledouble_utime(vol, path, timebuf);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, converted_path, basename, &dirid);

    if (is_dir(vol, dirid, basename)) {
        rc = afp_setdirparms(vol,
                             dirid, basename, kFPModDateBit, &fp);
    } else {
        rc = afp_setfileparms(vol,
                              dirid, basename, kFPModDateBit, &fp);
    }

    switch (rc) {
    case kFPNoErr:
        break;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPBitmapErr:
    case kFPMiscErr:
    case kFPObjectTypeErr:
    case kFPParamErr:
    default:
        break;
    }

    return -ret;
}


int ml_symlink(struct afp_volume *vol, const char * path1, const char * path2)
{
    int ret;
    struct afp_file_info fp;
    uint64_t written;
    int rc;
    unsigned int dirid2;
    char basename2[AFP_MAX_PATH];
    char converted_path1[AFP_MAX_PATH];
    char converted_path2[AFP_MAX_PATH];

    if (vol->server->using_version->av_number < 30) {
        /* No symlinks for AFP 2.x. */
        ret = ENOSYS;
        goto error;
    }

    /* Yes, you can create symlinks for AFP >=30.  Tested with 10.3.2 */

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path1, (char *) path1, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path2, (char *) path2, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    ret = appledouble_symlink(vol, path1, path2);

    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        return 0;
    }

    get_dirid(vol, converted_path2, basename2, &dirid2);
    /* 1. create the file */
    rc = afp_createfile(vol, kFPHardCreate, dirid2, basename2);

    switch (rc) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPDiskFull:
        ret = ENOSPC;
        goto error;

    case kFPObjectExists:
        ret = EEXIST;
        goto error;

    case kFPObjectNotFound:
        ret = ENOENT;
        goto error;

    case kFPFileBusy:
    case kFPVolLocked:
        ret = EBUSY;
        goto error;

    case kFPNoErr:
        ret = 0;
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        goto error;
    }

    /* Open the fork */
    rc = afp_openfork(vol, 0,
                      dirid2,
                      AFP_OPENFORK_ALLOWWRITE | AFP_OPENFORK_ALLOWREAD,
                      basename2, &fp);

    switch (ret) {
    case kFPAccessDenied:
        ret = EACCES;
        goto error;

    case kFPObjectNotFound:
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

    case 0:
        ret = 0;
        break;

    case kFPVolLocked:
    case kFPDenyConflict:
    case kFPMiscErr:
    case kFPBitmapErr:
    case -1:
    default:
        ret = EFAULT;
        goto error;
    }

    add_opened_fork(vol, &fp);
    /* Write the name of the file to it */
    rc = afp_writeext(vol, fp.forkid, 0, strlen(converted_path1),
                      converted_path1, &written);

    switch (afp_closefork(vol, fp.forkid)) {
    case kFPNoErr:
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
        goto error;
    }

    remove_opened_fork(vol, &fp);
    /* And now for the undocumented magic */
    memset(&fp.finderinfo, 0, 32);
    fp.finderinfo[0] = 's';
    fp.finderinfo[1] = 'l';
    fp.finderinfo[2] = 'n';
    fp.finderinfo[3] = 'k';
    fp.finderinfo[4] = 'r';
    fp.finderinfo[5] = 'h';
    fp.finderinfo[6] = 'a';
    fp.finderinfo[7] = 'p';
    rc = afp_setfiledirparms(vol, dirid2, basename2,
                             kFPFinderInfoBit, &fp);

    switch (rc) {
    case kFPAccessDenied:
        ret = EPERM;
        goto error;

    case kFPBitmapErr:
        /* This is the case where it isn't supported */
        ret = ENOSYS;
        goto error;

    case kFPObjectNotFound:
        ret = ENOENT;
        goto error;

    case 0:
        ret = 0;
        break;

    case kFPMiscErr:
    case kFPObjectTypeErr:
    case kFPParamErr:
    default:
        ret = EIO;
        goto error;
    }

error:
    return -ret;
}

int ml_rename(struct afp_volume * vol,
              const char *path_from, const char *path_to)
{
    int ret, rc;
    char basename_from[AFP_MAX_PATH];
    char basename_to[AFP_MAX_PATH];
    char converted_path_from[AFP_MAX_PATH];
    char converted_path_to[AFP_MAX_PATH];
    unsigned int dirid_from, dirid_to;

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path_from, (char *) path_from, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (convert_path_to_afp(vol->server->path_encoding,
                            converted_path_to, (char *) path_to, AFP_MAX_PATH)) {
        return -EINVAL;
    }

    if (volume_is_readonly(vol)) {
        return -EACCES;
    }

    get_dirid(vol, converted_path_from, basename_from, &dirid_from);
    get_dirid(vol, converted_path_to, basename_to, &dirid_to);

    if (is_dir(vol, dirid_to, converted_path_to)) {
        rc = afp_moveandrename(vol,
                               dirid_from, dirid_to,
                               basename_from, basename_to, basename_from);
    } else {
        rc = afp_moveandrename(vol,
                               dirid_from, dirid_to,
                               basename_from, NULL, basename_to);
    }

    switch (rc) {
    case kFPObjectLocked:
    case kFPAccessDenied:
        ret = EACCES;
        break;

    case kFPCantRename:
        ret = EROFS;
        break;

    case kFPObjectExists:

        /* First, remove the old file. */
        switch (afp_delete(vol, dirid_to, basename_to)) {
        case kFPAccessDenied:
            ret = EACCES;
            break;

        case kFPObjectLocked:
            ret = EBUSY;
            break;

        case kFPObjectNotFound:
            ret = ENOENT;
            break;

        case kFPVolLocked:
            ret = EACCES;
            break;

        case kFPDirNotEmpty:
            ret = ENOTEMPTY;
            break;

        case kFPObjectTypeErr:
        case kFPMiscErr:
        case kFPParamErr:
        case -1:
            ret = EINVAL;
            break;
        }

        /* Then, do the move again */
        switch (afp_moveandrename(vol,
                                  dirid_from, dirid_to,
                                  basename_from, NULL, basename_to)) {
        case kFPObjectLocked:
        case kFPAccessDenied:
            ret = EACCES;
            break;

        case kFPCantRename:
            ret = EROFS;
            break;

        case kFPObjectExists:
        case kFPObjectNotFound:
            ret = ENOENT;
            break;

        case kFPParamErr:
        case kFPMiscErr:
            ret = EIO;
            break;

        default:
        case kFPNoErr:
            ret = 0;
            break;
        }

        break;

    case kFPObjectNotFound:
        ret = ENOENT;
        break;

    case kFPNoErr:
        ret = 0;
        break;

    default:
    case kFPParamErr:
    case kFPMiscErr:
        ret = EIO;
    }

    return -ret;
}

int ml_statfs(struct afp_volume * vol, __attribute__((unused)) const char *path,
              struct statvfs *stat)
{
    unsigned short flags;
    int ret;
    memset(stat, 0, sizeof(*stat));

    if (vol->server->using_version->av_number < 30) {
        flags = kFPVolBytesFreeBit | kFPVolBytesTotalBit ;
    } else {
        flags = kFPVolExtBytesFreeBit | kFPVolExtBytesTotalBit | kFPVolBlockSizeBit;
    }

    ret = afp_getvolparms(vol, flags);

    switch (ret) {
    case kFPNoErr:
        break;

    case kFPParamErr:
    case kFPMiscErr:
    default:
        return -EIO;
    }

    if (vol->stat.f_bsize == 0) {
        vol->stat.f_bsize = 4096;
    }

    stat->f_blocks = vol->stat.f_blocks / vol->stat.f_bsize;
    stat->f_bfree = vol->stat.f_bfree / vol->stat.f_bsize;
    stat->f_bsize = vol->stat.f_bsize;
    stat->f_frsize = vol->stat.f_bsize;
    stat->f_bavail = stat->f_bfree;
    stat->f_files = 0;
    stat->f_ffree = 0;
    stat->f_favail = 0;
    stat->f_fsid = 0;
    stat->f_flag = 0;
    stat->f_namemax = 255;
    return 0;
}

int ml_passwd(struct afp_server *server,
              char *username, char *oldpasswd, char *newpasswd)
{
    return afp_dopasswd(server, server->using_uam, username, oldpasswd, newpasswd);
}

/* Extended Attributes (AFP 3.2+) */

int ml_getxattr(struct afp_volume * volume, const char *path,
                const char *name, void *value, size_t size)
{
    struct afp_extattr_info info;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    /* Ignore EAs if server doesn't support them */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return -ENOATTR;
    }

    /* Filter internal server EAs - pretend they don't exist */
    if (IS_INTERNAL_SERVER_EA(name)) {
        return -ENOATTR;
    }

    /* Special case: root directory - EAs not supported on volume root */
    if (strcmp(path, "/") == 0) {
        return -ENOATTR;
    }

    /* Parse path into directory ID and basename */
    ret = get_dirid(volume, path, basename, &dirid);

    if (ret != 0) {
        return ret;
    }

    /* Note: We don't call ll_get_directory_entry here to avoid file locking
     * conflicts. The FUSE layer already verified the file exists via getattr.
     * If the file doesn't exist, the AFP command will return kFPObjectNotFound. */
    /* Prepare info structure
     * Add overhead for AFP response header (bitmap=2 + datalength=4 = 6 bytes) */
    info.maxsize = (size > 0 && size < 1024) ? (size + 6) : 1024;
    info.size = 0;
    /* Strip "user." prefix from EA name for AFP protocol
     * Linux requires "user." prefix, but AFP doesn't use it */
    const char *afp_name = name;
    size_t afp_namelen = strlen(name);

    if (strncmp(name, "user.", 5) == 0) {
        afp_name = name + 5;
        afp_namelen = strlen(afp_name);
    }

    /* Get the extended attribute */
    ret = afp_getextattr(volume, dirid, 0, info.maxsize,
                         basename, afp_namelen, afp_name, &info);

    switch (ret) {
    case kFPNoErr:
        break;

    case kFPItemNotFound:
    case kFPMiscErr:
        /* AFP 3.2/3.3 returns kFPMiscErr for "attribute not found"
         * AFP 3.4+ returns kFPItemNotFound instead */
        return -ENOATTR;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    default:
        return -EIO;
    }

    /* If size is 0, just return the size needed */
    if (size == 0) {
        return info.size;
    }

    /* Check if buffer is too small */
    if (size < info.size) {
        return -ERANGE;
    }

    /* Copy data to user buffer */
    if (value && info.size > 0) {
        memcpy(value, info.data, info.size);
    }

    return info.size;
}

int ml_setxattr(struct afp_volume * volume, const char *path,
                const char *name, const void *value, size_t size, int flags)
{
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    unsigned short bitmap = 0;
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    /* Silently succeed when server doesn't support EAs to allow file copies */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return 0;
    }

    /* Filter internal server EAs - silently succeed to allow file copies */
    if (IS_INTERNAL_SERVER_EA(name)) {
        return 0;
    }

    /* Special case: root directory - silently succeed to allow file copies */
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    /* Parse path into directory ID and basename */
    ret = get_dirid(volume, path, basename, &dirid);

    if (ret != 0) {
        return ret;
    }

    /* Note: We don't call ll_get_directory_entry here to avoid file locking
     * conflicts. The FUSE layer already verified the file exists via getattr.
     * If the file doesn't exist, afp_setextattr will return kFPObjectNotFound. */

    /* Handle flags (create/replace semantics) */
    if (flags & kXAttrCreate) {
        bitmap |= kXAttrCreate;
    }

    if (flags & kXAttrREplace) {
        bitmap |= kXAttrREplace;
    }

    /* Strip "user." prefix from EA name for AFP protocol
     * Linux requires "user." prefix, but AFP doesn't use it
     * Netatalk will add it back when storing on the server */
    const char *afp_name = name;
    size_t afp_namelen = strlen(name);

    if (strncmp(name, "user.", 5) == 0) {
        afp_name = name + 5;
        afp_namelen = strlen(afp_name);
    }

    /* Set the extended attribute */
    ret = afp_setextattr(volume, dirid, bitmap, 0, basename,
                         afp_namelen, afp_name, size, value);

    switch (ret) {
    case kFPNoErr:
        return 0;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    case kFPObjectExists:
        return -EEXIST;  /* CREATE flag set but attr already exists */

    case kFPMiscErr:
        return -EIO;

    default:
        return -EIO;
    }
}

int ml_listxattr(struct afp_volume * volume, const char *path,
                 char *list, size_t size)
{
    struct afp_extattr_info info;
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    int ret;

    if (!volume || !path) {
        return -EINVAL;
    }

    /* Return empty list when server doesn't support EAs */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return 0;
    }

    /* Special case: root directory - return empty list */
    if (strcmp(path, "/") == 0) {
        return 0;
    }

    /* Parse path into directory ID and basename */
    ret = get_dirid(volume, path, basename, &dirid);

    if (ret != 0) {
        return ret;
    }

    /* Note: We don't call ll_get_directory_entry here to avoid file locking
     * conflicts. The FUSE layer already verified the file exists via getattr.
     * If the file doesn't exist, the AFP command will return kFPObjectNotFound. */
    /* Prepare info structure */
    info.maxsize = (size > 0 && size < 1024) ? size : 1024;
    info.size = 0;
    /* List extended attributes */
    ret = afp_listextattr(volume, dirid, 0, basename, &info);

    switch (ret) {
    case kFPNoErr:
        break;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    default:
        return -EIO;
    }

    /* Filter out internal server EAs from the list and add "user." prefix for Linux */
    const char *src = info.data;
    char *dst = list;
    size_t remaining = info.size;
    size_t filtered_size = 0;

    while (remaining > 0) {
        size_t namelen = strnlen(src, remaining);

        if (namelen == 0) {
            break;  /* End of list */
        }

        /* Ensure we have a null terminator within bounds */
        if (namelen == remaining) {
            /* No null terminator found - malformed data from server */
            return -EIO;
        }

        /* Only include this EA if it's not filtered */
        if (!IS_INTERNAL_SERVER_EA(src)) {
            /* On Linux, add "user." prefix to EA names returned by server */
            size_t output_namelen = namelen + 5;  /* "user." + name */

            /* If we have a buffer and space, copy the name with prefix */
            if (list && size > 0) {
                if (filtered_size + output_namelen + 1 <= size) {
                    memcpy(dst, "user.", 5);
                    memcpy(dst + 5, src, namelen + 1);
                    dst += output_namelen + 1;
                } else if (size > 0) {
                    /* Buffer too small */
                    return -ERANGE;
                }
            }

            filtered_size += output_namelen + 1;
        }

        src += namelen + 1;
        remaining -= namelen + 1;
    }

    return filtered_size;
}

int ml_removexattr(struct afp_volume * volume, const char *path,
                   const char *name)
{
    unsigned int dirid;
    char basename[AFP_MAX_PATH];
    int ret;

    if (!volume || !path || !name) {
        return -EINVAL;
    }

    /* Ignore EAs if server doesn't support them to allow file deletions */
    if (!(volume->attributes & kSupportsExtAttrs)) {
        return -ENOATTR;
    }

    /* Filter internal server EAs - deny removal */
    if (IS_INTERNAL_SERVER_EA(name)) {
        return -EACCES;
    }

    /* Special case: root directory - deny removal */
    if (strcmp(path, "/") == 0) {
        return -EACCES;
    }

    /* Parse path into directory ID and basename */
    ret = get_dirid(volume, path, basename, &dirid);

    if (ret != 0) {
        return ret;
    }

    /* Note: We don't call ll_get_directory_entry here to avoid file locking
     * conflicts. The FUSE layer already verified the file exists via getattr.
     * If the file doesn't exist, the AFP command will return kFPObjectNotFound. */
    /* Strip "user." prefix from EA name for AFP protocol
     * Linux requires "user." prefix, but AFP doesn't use it */
    const char *afp_name = name;
    size_t afp_namelen = strlen(name);

    if (strncmp(name, "user.", 5) == 0) {
        afp_name = name + 5;
        afp_namelen = strlen(afp_name);
    }

    /* Remove the extended attribute */
    ret = afp_removeextattr(volume, dirid, 0, basename,
                            afp_namelen, afp_name);

    switch (ret) {
    case kFPNoErr:
        return 0;

    case kFPItemNotFound:
    case kFPMiscErr:
        /* AFP 3.2/3.3 returns kFPMiscErr for "attribute not found"
         * AFP 3.4+ returns kFPItemNotFound instead */
        return -ENOATTR;

    case kFPAccessDenied:
        return -EACCES;

    case kFPObjectNotFound:
        return -ENOENT;

    default:
        return -EIO;
    }
}
