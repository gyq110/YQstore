#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PAYLOAD_SIZE = 512 };
static const char pathname[] = "/zvfs/db/a.txt";

static int write_file(const unsigned char *expected, int flags)
{

    int fd = open(pathname, flags, 0644);
    if (fd < 0)
    {
        perror("open a.txt for writing");
        return 1;
    }

    ssize_t n = write(fd, expected, PAYLOAD_SIZE);
    if (n != PAYLOAD_SIZE)
    {
        fprintf(stderr, "write returned %zd, expected %d\n", n, PAYLOAD_SIZE);
        close(fd);
        return 1;
    }

    if (close(fd) < 0)
    {
        perror("close after write");
        return 1;
    }

    return 0;
}

static int read_file(const unsigned char *expected, const char *phase)
{
    struct stat64 st;
    unsigned char actual[PAYLOAD_SIZE];

    if (stat64(pathname, &st) < 0)
    {
        perror("stat64 a.txt");
        return 1;
    }

    int fd = open(pathname, O_RDONLY);
    if (fd < 0)
    {
        perror("reopen a.txt");
        return 1;
    }

    ssize_t n = read(fd, actual, sizeof(actual));
    if (close(fd) < 0)
    {
        perror("close after read");
        return 1;
    }

    int data_ok = n == PAYLOAD_SIZE &&
                  memcmp(actual, expected, sizeof(actual)) == 0;
    int size_ok = st.st_size == PAYLOAD_SIZE;
    printf("%s: ino=%llu size=%lld read=%zd data=%s length=%s\n",
           phase, (unsigned long long)st.st_ino, (long long)st.st_size, n,
           data_ok ? "OK" : "FAIL", size_ok ? "OK" : "FAIL");
    return !(data_ok && size_ok);
}

int main(int argc, char **argv)
{
    if (argc != 2 ||
            (strcmp(argv[1], "write") != 0 &&
             strcmp(argv[1], "rewrite") != 0 &&
             strcmp(argv[1], "read") != 0))
    {
        fprintf(stderr, "usage: %s write|rewrite|read\n", argv[0]);
        return 2;
    }

    unsigned char expected[PAYLOAD_SIZE];
    for (size_t i = 0; i < sizeof(expected); i++)
    {
        expected[i] = (unsigned char)(i * 37u + 0x5au);
    }

    if (strcmp(argv[1], "read") == 0)
    {
        return read_file(expected, "new-process mount");
    }

    int flags = O_WRONLY;
    if (strcmp(argv[1], "write") == 0)
    {
        if (mkdir("/zvfs/db", 0755) < 0)
        {
            perror("mkdir /zvfs/db");
            return 1;
        }
        flags |= O_CREAT | O_EXCL;
    }

    if (write_file(expected, flags) != 0)
    {
        return 1;
    }

    return read_file(expected, "same-process reopen");
}
