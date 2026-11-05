
#include"zvfs.h"

//这里有一个问题，就是我们的fd如果不是我们自己在内部通过eventfd创建的，但是依旧调用了这些接口，那我们这里应该首先调用一个检查


int is_inner(int fd)
{
    if (file_table[fd] == NULL)
    {
        return -1;
    }
    return 0;
}

int open(const char *pathname, int flags)
{
    return nopen(pathname, flags);
}
ssize_t read(int fd, void *buf, size_t count)
{
    if (is_inner(fd))
    {

    }
    return nread(fd, buf, count);

}
ssize_t write(int fd, const void *buf, size_t count)
{
    if (is_inner(fd))
    {

    }
    return nwrite(fd, buf, count);
}
int close(int fd)
{
    if (is_inner(fd))
    {

    }
    return nclose(fd);

}