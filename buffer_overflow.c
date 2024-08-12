#include "types.h"
#include "user.h"
#include "fcntl.h"

void foo()
{
    printf(1, "SECRET_STRING");
}


void vulnerable_func(char *payload)
{
    char buffer[4];
    strcpy(buffer, payload);
}

int main(int argc, char *argv[])
{
    int fd;

    fd = open("payload", O_RDONLY);
    char payload[100];

    read(fd, payload, 100);

    vulnerable_func(payload);

    close(fd);
    exit();
}
