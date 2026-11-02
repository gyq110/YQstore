#include<stdio.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<string.h>
int main()
{
    int fd = open("a.txt", O_CREAT | O_RDWR);
    char *buffer = "abcdefghigklmnopqrstuvwxyz";
    write(fd, buffer, strlen(buffer));
    char *rbuffer[128] = {0};
    read(fd, buffer, 128);
    close(fd);
}