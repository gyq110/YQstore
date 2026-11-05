
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