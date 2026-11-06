
#include "shim.h"

// 这里有一个问题，就是我们的fd如果不是我们自己在内部通过eventfd创建的，但是依旧调用了这些接口，那我们这里应该首先调用一个检查

static int is_inner(int fd)
{
    return fd >= 0 && fd < YQ_MAX_FD && file_table[fd] != NULL;
}

int open(const char *pathname, int flags, ...)
{
    static int (*open_f)(const char *, int, ...);
    if (strcmp(pathname, "a.txt") == 0)
    {
        // 这里我们目前区分调用我们open的是我们固定使用a.txt
        return nopen(pathname, flags);
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
#if ENABLE_FIO
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
#endif
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
    if (is_inner(fd))
    {
        return nclose(fd);
    }
    if (close_f == NULL)
    {
        close_f = dlsym(RTLD_NEXT, "close");
    }
    return close_f(fd);
}


#if ENABLE_FIO

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

    if (strcmp(pathname, "a.txt") == 0)
    {
        return nstat(pathname, st);
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
#endif