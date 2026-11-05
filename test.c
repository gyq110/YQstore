#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include<string.h>
int main()
{
    const char message[] = "hello yqstore";
    char buffer[128] = {0};
    int fd = open("a.txt", O_CREAT | O_RDWR, 0644);
    if (fd < 0)
    {
        puts("open failed");
        return 1;
    }
    ssize_t written = write(fd, message, sizeof(message));
    ssize_t received = read(fd, buffer, sizeof(buffer));
    int closed = close(fd);

    printf("write return %zd\n", written);
    printf("read return %zd\n", received);
    printf("read receive %s\n", buffer);

    if (written < 0 || received < 0 || closed < 0)
    {
        puts("file operation failed");
        return 1;
    }
    if (strcmp(message, buffer) != 0)
    {
        puts("content mismatch");
        return 1;
    }
    close(fd);
    puts("pass");
    return 0;
}