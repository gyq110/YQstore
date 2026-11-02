
#define _GNU_SOURCE
#include <dlfcn.h>
#include<stdio.h>
//open


//read


//write


//close


typedef int (*open_t)(const char *pathname, int flags);
open_t open_f = NULL;
typedef ssize_t (*read_t)(int fd, void *buf, size_t count);
read_t read_f = NULL;
typedef ssize_t (*write_t)(int fd, const void *buf, size_t count);
write_t write_f = NULL;
typedef int (*close_t)(int fd);
close_t close_f = NULL;


int open(const char *pathname, int flags)
{
    if (open_f == NULL)
    {
        open_f = dlsym(RTLD_NEXT, "open");
    }
    printf("open..%s\n", pathname);
    return open_f(pathname, flags);
}



ssize_t write(int fd, const void *buf, size_t count)
{
    if (write_f == NULL)
    {
        write_f = dlsym(RTLD_NEXT, "write");
    }
    printf("write..\n");
    return write_f(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (read_f == NULL)
    {
        read_f = dlsym(RTLD_NEXT, "read");
    }
    printf("read..\n");
    return read_f(fd, buf, count);
}


int close(int fd)
{
    if (close_f == NULL)
    {
        close_f = dlsym(RTLD_NEXT, "close");
    }
    printf("close..\n");
    return close_f(fd);

}

