#include <stdio.h>

const char * const _SOCKET_PATH_NAME = "@backend.service.sock";

int _bg_main(int argc, char *argv[])
{
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }

    return argc;
}
