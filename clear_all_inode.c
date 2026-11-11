#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int clear_dir(char path[PATH_MAX], size_t len)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
        return -1;

    int rc = 0;
    for (;;)
    {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL)
        {
            if (errno != 0)
                rc = -1;
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        int n = snprintf(path + len, PATH_MAX - len, "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= PATH_MAX - len)
        {
            path[len] = '\0';
            errno = ENAMETOOLONG;
            rc = -1;
            break;
        }

        if (entry->d_type == DT_DIR)
        {
            rc = clear_dir(path, len + (size_t)n);
            if (rc == 0)
                rc = rmdir(path);
        }
        else
        {
            rc = unlink(path);
        }

        if (rc < 0)
            break;

        path[len] = '\0';
    }

    int error = errno;
    if (closedir(dir) < 0 && rc == 0)
        return -1;
    errno = error;
    return rc;
}

int main(void)
{
    char path[PATH_MAX] = "/zvfs";
    if (clear_dir(path, strlen(path)) < 0)
    {
        perror(path);
        return 1;
    }

    puts("ZVFS root is empty");
    return 0;
}
