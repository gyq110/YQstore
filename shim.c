
#include "shim.h"
#include <limits.h>
#include <sys/statvfs.h>
// 这里有一个问题，就是我们的fd如果不是我们自己在内部通过eventfd创建的，但是依旧调用了这些接口，那我们这里应该首先调用一个检查

static int is_inner(int fd)
{
    return fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL && !S_ISDIR(file_table[fd]->inode->mode);
}


static char zvfs_cwd[PATH_MAX];//这个进程里面当前的目录

static int zvfs_inner_path(const char *path, char resolved[PATH_MAX],
                           const char **inner_path)
{
    *inner_path = NULL;

    if (path[0] == '/')
    {
        //这里说明是绝对路径，需要判断一下是不是有zvfs/来分流
        const size_t prefix_len = sizeof(YQ_MOUNT_PREFIX) - 1;

        if (strncmp(path, YQ_MOUNT_PREFIX, prefix_len) == 0)
        {
            const char *suffix = path + prefix_len;

            if (*suffix == '\0')
                *inner_path = YQ_SPDK_ROOT_DIR;
            else if (*suffix == '/')
                *inner_path = suffix;
        }
        return 0;
    }
//下面就全是相对路径的处理
    if (zvfs_cwd[0] == '\0')//这里如果没有cwd说明我们这个是linux内部的
        return 0;

    if (path[0] == '\0')
    {
        //判断传过来的数据不是空字符串
        errno = ENOENT;
        return -1;
    }
    const char *base = strcmp(zvfs_cwd, "/") == 0 ? "" : zvfs_cwd;
    int len = snprintf(resolved, PATH_MAX, "%s/%s", base, path);
    if (len < 0 || len >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    *inner_path = resolved;
    return 0;
}


int open(const char *pathname, int flags, ...)
{
    static int (*open_f)(const char *, int, ...);
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nopen(inner_path, flags);
    }
    if (open_f == NULL)
    {
        open_f = dlsym(RTLD_NEXT, "open");
        if (open_f == NULL)
        {
            return -1;
        }
    }
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE)
    {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        va_end(args);
        return open_f(pathname, flags, mode);
    }
    return open_f(pathname, flags);
}

int open64(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE)
    {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return open(pathname, flags | O_LARGEFILE, mode);
}
ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*read_f)(int, void *, size_t);
    if (is_inner(fd))
    {
        return nread(fd, buf, count);
    }
    if (read_f == NULL)
    {
        read_f = dlsym(RTLD_NEXT, "read");
    }
    return read_f(fd, buf, count);
}
ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*write_f)(int, const void *, size_t);
    if (is_inner(fd))
    {
        return nwrite(fd, buf, count);
    }
    if (write_f == NULL)
    {
        write_f = dlsym(RTLD_NEXT, "write");
    }
    return write_f(fd, buf, count);
}
int close(int fd)
{
    static int (*close_f)(int);
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        //这里如果是目录也需要关闭
        return nclose(fd);
    }
    if (close_f == NULL)
    {
        close_f = dlsym(RTLD_NEXT, "close");
    }
    return close_f(fd);
}




off64_t lseek64(int fd, off64_t offset, int whence)
{
    static off64_t (*lseek_f)(int, off64_t, int);

    if (is_inner(fd))
    {
        return nlseek(fd, offset, whence);
    }

    if (lseek_f == NULL)
    {
        lseek_f = dlsym(RTLD_NEXT, "lseek64");
    }

    return lseek_f(fd, offset, whence);
}
int stat64(const char *pathname, struct stat64 *st)
{
    static int (*stat_f)(const char *, struct stat64 *);

    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nstat(inner_path, st);
    }
    if (stat_f == NULL)
    {
        stat_f = dlsym(RTLD_NEXT, "stat64");
    }

    return stat_f(pathname, st);
}
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset)
{
    static ssize_t (*pread_f)(int, void *, size_t, off64_t);

    if (is_inner(fd))
    {
        return npread(fd, buf, count, offset);
    }

    if (pread_f == NULL)
    {
        pread_f = dlsym(RTLD_NEXT, "pread64");
    }

    return pread_f(fd, buf, count, offset);
}

ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset)
{
    static ssize_t (*pwrite_f)(int, const void *, size_t, off64_t);

    if (is_inner(fd))
    {
        return npwrite(fd, buf, count, offset);
    }

    if (pwrite_f == NULL)
    {
        pwrite_f = dlsym(RTLD_NEXT, "pwrite64");
    }

    return pwrite_f(fd, buf, count, offset);
}
int mkdir(const char *pathname, mode_t mode)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nmkdir(inner_path, mode);
    }

    int (*mkdir_f)(const char *, mode_t) = dlsym(RTLD_NEXT, "mkdir");
    if (mkdir_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return mkdir_f(pathname, mode);
}

DIR *opendir(const char *pathname)
{
    DIR *(*opendir_f)(const char *) = dlsym(RTLD_NEXT, "opendir");
    if (opendir_f == NULL)
    {
        errno = ENOSYS;
        return NULL;
    }

    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return NULL;
    if (inner_path == NULL)
    {
        return opendir_f(pathname);
    }

    zvfs_t *dir_fs;
    zvfs_inode_t *inode = zvfs_lookup_dir(inner_path, &dir_fs);
    if (inode == NULL)
    {
        return NULL;
    }

    DIR *dirp = opendir_f("/");
    if (dirp == NULL)
    {
        return NULL;
    }

    int fd = dirfd(dirp);
    if (fd < 0 || fd >= YQ_MAX_FD)
    {
        int error = fd < 0 ? errno : EMFILE;
        closedir(dirp);
        errno = error;
        return NULL;
    }

    zvfs_file_t *file = calloc(1, sizeof(*file));
    struct dirent *entry = calloc(1, sizeof(*entry));
    if (file == NULL || entry == NULL)
    {
        free(file);
        free(entry);
        closedir(dirp);
        errno = ENOMEM;
        return NULL;
    }

    entry->d_reclen = sizeof(*entry);
    file->fs = dir_fs;
    file->inode = inode;
    file->offset = inode->first_child_ino;
    file->read_buffer = (uint8_t *)entry;
    file_table[fd] = file;
    return dirp;
}

struct dirent *readdir(DIR *dirp)
{
    int fd = dirfd(dirp);
    zvfs_file_t *file = fd >= 0 && fd < YQ_MAX_FD ? file_table[fd] : NULL;
    if (file == NULL || !S_ISDIR(file->inode->mode))
    {
        struct dirent *(*readdir_f)(DIR *) = dlsym(RTLD_NEXT, "readdir");
        if (readdir_f == NULL)
        {
            errno = ENOSYS;
            return NULL;
        }
        return readdir_f(dirp);
    }

    if (file->offset == 0)
    {
        return NULL;
    }

    zvfs_inode_t *child = file->fs->inode_table[file->offset];
    struct dirent *entry = (struct dirent *)file->read_buffer;
    entry->d_ino = child->ino;
    entry->d_type = S_ISDIR(child->mode) ? DT_DIR : DT_REG;
    strcpy(entry->d_name, child->filename);
    file->offset = child->next_sibling_ino;
    return entry;
}

int closedir(DIR *dirp)
{
    int (*closedir_f)(DIR *) = dlsym(RTLD_NEXT, "closedir");
    if (closedir_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }

    int fd = dirfd(dirp);
    zvfs_file_t *file = fd >= 0 && fd < YQ_MAX_FD ? file_table[fd] : NULL;
    if (file != NULL && S_ISDIR(file->inode->mode))
    {
        file_table[fd] = NULL;
        free(file->read_buffer);
        zvfs_inode_t *inode = file->inode;
        free(file);

        if (inode->parent_ino == 0)
        {
            zvfs_reclaim_inode(inode);
        }
    }

    return closedir_f(dirp);
}
int stat(const char *pathname, struct stat *st)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path == NULL)
    {
        int (*stat_f)(const char *, struct stat *) = dlsym(RTLD_NEXT, "stat");
        if (stat_f == NULL)
        {
            errno = ENOSYS;
            return -1;
        }
        return stat_f(pathname, st);
    }

    struct stat64 info;
    if (nstat(inner_path, &info) != 0)
    {
        return -1;
    }

    *st = (struct stat)
    {
        .st_ino = info.st_ino,
        .st_mode = info.st_mode,
        .st_nlink = info.st_nlink,
        .st_size = info.st_size,
        .st_blksize = info.st_blksize,
    };
    return 0;
}
int fstat(int fd, struct stat *st)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        zvfs_file_t *file = file_table[fd];
        zvfs_inode_t *inode = file->inode;

        *st = (struct stat)
        {
            .st_ino = inode->ino,
            .st_mode = inode->mode,
            .st_nlink = 1,
            .st_size = inode->size,
            .st_blksize = file->fs->io_unit_size,
        };
        return 0;
    }

    int (*fstat_f)(int, struct stat *) = dlsym(RTLD_NEXT, "fstat");
    if (fstat_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return fstat_f(fd, st);
}
int access(const char *pathname, int mode)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        if (mode != F_OK)
        {
            errno = EOPNOTSUPP;
            return -1;
        }

        struct stat64 info;
        return nstat(inner_path, &info);
    }

    int (*access_f)(const char *, int) = dlsym(RTLD_NEXT, "access");
    if (access_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return access_f(pathname, mode);
}
int fstatfs(int fd, struct statfs *buf)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        zvfs_t *fs = file_table[fd]->fs;
        uint64_t free_clusters = spdk_bs_free_cluster_count(fs->blobstore);

        *buf = (struct statfs)
        {
            .f_type = 0,
            .f_bsize = fs->cluster_size,
            .f_frsize = fs->cluster_size,
            .f_blocks = spdk_bs_total_data_cluster_count(fs->blobstore),
            .f_bfree = free_clusters,
            .f_bavail = free_clusters,
            .f_namelen = YQ_MAX_FILENAME,
        };
        return 0;
    }

    int (*fstatfs_f)(int, struct statfs *) = dlsym(RTLD_NEXT, "fstatfs");
    if (fstatfs_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return fstatfs_f(fd, buf);
}
ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (is_inner(fd))
    {
        return npread(fd, buf, count, offset);
    }

    ssize_t (*pread_f)(int, void *, size_t, off_t) =
        dlsym(RTLD_NEXT, "pread");
    if (pread_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return pread_f(fd, buf, count, offset);
}
off_t lseek(int fd, off_t offset, int whence)
{
    if (is_inner(fd))
    {
        return nlseek(fd, offset, whence);
    }

    off_t (*lseek_f)(int, off_t, int) = dlsym(RTLD_NEXT, "lseek");
    if (lseek_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return lseek_f(fd, offset, whence);
}
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (is_inner(fd))
    {
        return npwrite(fd, buf, count, offset);
    }

    ssize_t (*pwrite_f)(int, const void *, size_t, off_t) =
        dlsym(RTLD_NEXT, "pwrite");
    if (pwrite_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return pwrite_f(fd, buf, count, offset);
}

//下面是我们还没有实现的接口
int ftruncate(int fd, off_t length)
{
    if (is_inner(fd))
    {
        return nftruncate(fd, length);
    }

    int (*ftruncate_f)(int, off_t) = dlsym(RTLD_NEXT, "ftruncate");
    if (ftruncate_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return ftruncate_f(fd, length);
}
int unlink(const char *pathname)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nunlink(inner_path);
    }

    int (*unlink_f)(const char *) = dlsym(RTLD_NEXT, "unlink");
    if (unlink_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return unlink_f(pathname);
}
int rmdir(const char *pathname)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nrmdir(inner_path);
    }

    int (*rmdir_f)(const char *) = dlsym(RTLD_NEXT, "rmdir");
    if (rmdir_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return rmdir_f(pathname);
}

int rename(const char *oldpath, const char *newpath)
{
    char resolved[PATH_MAX];
    const char *old_inner;
    if (zvfs_inner_path(oldpath, resolved, &old_inner) < 0)
        return -1;
    char resolved1[PATH_MAX];
    const char *new_inner;
    if (zvfs_inner_path(newpath, resolved1, &new_inner) < 0)
        return -1;
    if ((old_inner == NULL) != (new_inner == NULL))
    {
        errno = EXDEV;
        return -1;
    }
    if (old_inner != NULL)
    {
        return nrename(old_inner, new_inner);
    }

    int (*rename_f)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
    if (rename_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return rename_f(oldpath, newpath);
}

int statvfs(const char *pathname, struct statvfs *buf)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(pathname, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        return nstatvfs(inner_path, buf);
    }

    int (*statvfs_f)(const char *, struct statvfs *) =
        dlsym(RTLD_NEXT, "statvfs");
    if (statvfs_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return statvfs_f(pathname, buf);
}
FILE *fdopen(int fd, const char *mode)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        if (!is_inner(fd) || strcmp(mode, "r") != 0 ||
                (file_table[fd]->flags & O_ACCMODE) == O_WRONLY)
        {
            errno = EINVAL;
            return NULL;
        }

        return (FILE *)file_table[fd];
    }

    FILE *(*fdopen_f)(int, const char *) = dlsym(RTLD_NEXT, "fdopen");
    if (fdopen_f == NULL)
    {
        errno = ENOSYS;
        return NULL;
    }
    return fdopen_f(fd, mode);
}

int fallocate(int fd, int mode, off_t offset, off_t length)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        errno = EOPNOTSUPP;
        return -1;
    }

    int (*fallocate_f)(int, int, off_t, off_t) =
        dlsym(RTLD_NEXT, "fallocate");
    if (fallocate_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return fallocate_f(fd, mode, offset, length);
}

int sync_file_range(int fd, off64_t offset, off64_t length, unsigned int flags)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        errno = ENOSYS;
        return -1;
    }

    int (*sync_file_range_f)(int, off64_t, off64_t, unsigned int) =
        dlsym(RTLD_NEXT, "sync_file_range");
    if (sync_file_range_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return sync_file_range_f(fd, offset, length, flags);
}

int fsync(int fd)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        return 0;
    }

    int (*fsync_f)(int) = dlsym(RTLD_NEXT, "fsync");
    if (fsync_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return fsync_f(fd);
}

int fdatasync(int fd)
{
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL)
    {
        return 0;
    }

    int (*fdatasync_f)(int) = dlsym(RTLD_NEXT, "fdatasync");
    if (fdatasync_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return fdatasync_f(fd);
}
int fcntl(int fd, int cmd, ...)
{
    if (is_inner(fd) && cmd == F_SETLK)
    {
        return 0;
    }
    if (fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL &&
            cmd != F_GETFD && cmd != F_SETFD)
    {
        errno = EOPNOTSUPP;
        return -1;
    }

    int (*fcntl_f)(int, int, ...) = dlsym(RTLD_NEXT, "fcntl");
    if (fcntl_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }

    int rc;
    va_list args;
    va_start(args, cmd);

    switch (cmd)
    {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_SETFL:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
    case F_SETPIPE_SZ:
    case F_ADD_SEALS:
        rc = fcntl_f(fd, cmd, va_arg(args, int));
        break;

    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
        rc = fcntl_f(fd, cmd, va_arg(args, struct flock *));
        break;

    case F_GETOWN_EX:
    case F_SETOWN_EX:
        rc = fcntl_f(fd, cmd, va_arg(args, struct f_owner_ex *));
        break;

    case F_GET_RW_HINT:
    case F_SET_RW_HINT:
    case F_GET_FILE_RW_HINT:
    case F_SET_FILE_RW_HINT:
        rc = fcntl_f(fd, cmd, va_arg(args, uint64_t *));
        break;

    default:
        rc = fcntl_f(fd, cmd);
        break;
    }

    va_end(args);
    return rc;
}

#undef fread_unlocked

size_t fread_unlocked(void *buf, size_t size, size_t count, FILE *fp)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file == NULL || (void *)fp != (void *)file)
            continue;

        if (size == 0 || count == 0 || file->stream_eof)
            return 0;

        size_t bytes;
        if (__builtin_mul_overflow(size, count, &bytes))
        {
            errno = EOVERFLOW;
            file->stream_error = true;
            return 0;
        }

        ssize_t n = 0;
        if (file->offset < file->inode->size)
            n = nread(fd, buf, bytes);

        if (n < 0)
        {
            file->stream_error = true;
            return 0;
        }
        if ((size_t)n < bytes)
            file->stream_eof = true;

        return (size_t)n / size;
    }

    size_t (*fread_f)(void *, size_t, size_t, FILE *) =
        dlsym(RTLD_NEXT, "fread_unlocked");
    return fread_f(buf, size, count, fp);
}

int feof(FILE *fp)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && (void *)fp == (void *)file)
            return file->stream_eof;
    }

    int (*feof_f)(FILE *) = dlsym(RTLD_NEXT, "feof");
    return feof_f(fp);
}

int ferror(FILE *fp)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && (void *)fp == (void *)file)
            return file->stream_error;
    }

    int (*ferror_f)(FILE *) = dlsym(RTLD_NEXT, "ferror");
    return ferror_f(fp);
}

void clearerr(FILE *fp)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && (void *)fp == (void *)file)
        {
            file->stream_eof = false;
            file->stream_error = false;
            return;
        }
    }

    void (*clearerr_f)(FILE *) = dlsym(RTLD_NEXT, "clearerr");
    clearerr_f(fp);
}

int fclose(FILE *fp)
{
    for (int fd = 0; fd < YQ_MAX_FD; fd++)
    {
        zvfs_file_t *file = file_table[fd];
        if (file != NULL && (void *)fp == (void *)file)
            return nclose(fd);
    }

    int (*fclose_f)(FILE *) = dlsym(RTLD_NEXT, "fclose");
    return fclose_f(fp);
}
int chdir(const char *path)
{
    //这里是给我的cwd赋值
    char resolved[PATH_MAX];
    const char *inner_path;

    if (zvfs_inner_path(path, resolved, &inner_path) < 0)
        return -1;

    if (inner_path == NULL)
    {
        //这里走libc
        int (*chdir_f)(const char *) = dlsym(RTLD_NEXT, "chdir");
        if (chdir_f == NULL)
        {
            errno = ENOSYS;
            return -1;
        }

        int rc = chdir_f(path);
        if (rc == 0)
            zvfs_cwd[0] = '\0';
        return rc;
    }
//这里走我们的zvfs
//首先需要判断这个路径是不是文件夹
    zvfs_t *dir_fs;
    zvfs_inode_t *dir = zvfs_lookup_dir(inner_path, &dir_fs);
    if (dir == NULL)
        return -1;
//reslve存的是我们的完整的路径，这里先给最后路径复制为0
    char *cwd = resolved + sizeof(resolved) - 1;
    *cwd = '\0';

    while (dir != dir_fs->root)
    {
        size_t len = strlen(dir->filename);
        if ((size_t)(cwd - resolved) < len + 1)
        {
            errno = ENAMETOOLONG;
            return -1;
        }

        cwd -= len;
        memcpy(cwd, dir->filename, len);
        *--cwd = '/';
        dir = dir_fs->inode_table[dir->parent_ino];
    }

    strcpy(zvfs_cwd, *cwd == '\0' ? YQ_SPDK_ROOT_DIR : cwd);
    return 0;
}
int statvfs64(const char *path, struct statvfs64 *buf)
{
    char resolved[PATH_MAX];
    const char *inner_path;
    if (zvfs_inner_path(path, resolved, &inner_path) < 0)
        return -1;
    if (inner_path != NULL)
    {
        struct statvfs info;
        if (nstatvfs(inner_path, &info) != 0)
            return -1;
//这里和statvfs不一样的就是把结构体转换一下就行了
        *buf = (struct statvfs64)
        {
            .f_bsize   = info.f_bsize,
            .f_frsize  = info.f_frsize,
            .f_blocks  = info.f_blocks,
            .f_bfree   = info.f_bfree,
            .f_bavail  = info.f_bavail,
            .f_namemax = info.f_namemax,
        };
        return 0;
    }

    int (*statvfs_f)(const char *, struct statvfs64 *) =
        dlsym(RTLD_NEXT, "statvfs64");
    if (statvfs_f == NULL)
    {
        errno = ENOSYS;
        return -1;
    }
    return statvfs_f(path, buf);

}