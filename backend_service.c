#include <stdio.h>
#include <stdio.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>

#include <syslog.h>
#include <errno.h>

const char * const socket_path = "/tmp/backend.service.sock";
const char * const end_string = "[end]";

#define buffer_size (4 * 1024 * 1024) // 4MB
static char buffer[buffer_size] = {0};

static int create_listen_socket()
{
    int s = socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0);

    unlink(socket_path);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, socket_path, sizeof sa.sun_path - 1);

    int rc = bind(s, (struct sockaddr *) &sa, sizeof(sa));

    listen(s, 1);
    return s;
}

static void dealwith_client(int fd)
{
    dup2(fd, STDIN_FILENO);
    dup2(STDIN_FILENO, STDOUT_FILENO);
    dup2(STDOUT_FILENO, STDERR_FILENO);

    int32_t argc = 0;
    memset(buffer, 0, buffer_size);
    int ret = read(fd, &argc, sizeof argc);
    if (ret < 0) {
        printf("read argc from client error: %d errno: %d\n", ret, errno);
    }

    int32_t len = 0;
    char * argv[argc];
    char * pBuf = buffer;
    for (int i = 0; i < argc; i++) {
        ret = read(fd, &len, sizeof len);
        if (ret < 0) {
            printf("read argv[%d] length from client error: %d errno: %d\n", i, ret, errno);
        }
        ret = read(fd, pBuf, len);
        if (ret < 0) {
            printf("read argv[%d] content from client error: %d errno: %d\n", i, ret, errno);
        }
        argv[i] = pBuf;
        pBuf += len;
    }

    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }

    printf("%s\n", end_string);
}

static int start_backend_service(void)
{
    switch(fork()) {
    case -1:
        return -1;
    case 0:
        break;
    default: {
        // wait bacend startup
        int wait_time = 100;
        while (access(socket_path, F_OK) != 0 && (wait_time-- > 0)) {
            usleep(100 * 1000);
        }
    }
        return 0;
    }

    if (setsid() == -1) {
        return -1;
    }

    (void) chdir("/");

    int fd;
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        (void) dup2(fd, STDIN_FILENO);
        (void) dup2(fd, STDOUT_FILENO);
        (void) dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            (void) close(fd);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    int s = create_listen_socket();

    struct sockaddr_un sa;
    socklen_t socklen = sizeof sa;

    fd_set rd;

    while (1) {
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        select(s + 1, &rd, NULL, NULL, NULL);
        if(FD_ISSET(s, &rd)) {
            int cli = accept(s, (struct sockaddr *)&sa, &socklen);

            dealwith_client(cli);

            close(cli);
        }
    }

    return 0;
}

// buffer layout
// __________________________________________________
// |4B | 4B| payload | 4B | payload|...|4B| payload |
// --------------------------------------------------
//
static int encode_input(int32_t argc, char * argv[])
{
    memset(buffer, 0, buffer_size);
    memcpy(buffer, &argc, sizeof(argc));
    int32_t count = sizeof(argc);
    int32_t len = 0;

    for (int i = 0; i < argc; i++) {
        len = strlen(argv[i]) + 1;
        if (count + len > buffer_size) {
            printf("the input data is too long!\n");
            return count;
        }
        memcpy(&buffer[count], &len, sizeof(len));
        count += sizeof(len);
        memcpy(&buffer[count], argv[i], len);
        count += len;
    }
    return count;
}

static int connect_to_backend()
{
    int s = socket(AF_LOCAL, SOCK_STREAM, 0);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, socket_path, sizeof sa.sun_path - 1);

    int ret = connect(s, (struct sockaddr *) &sa, sizeof sa);
    if (ret != 0) {
        printf("Failed to connect backend\n");
        close(s);
        return ret;
    }
    return s;
}

static int send_input_to_backend(int s, int argc, char *argv[])
{
    int len = encode_input(argc, argv);
    if (len < 0) {
        printf("encode input data failed!!\n");
        return len;
    }

    return write(s, buffer, len);
}

static int readline(int fd, char * buf, int size)
{
    int ret = 0;
    int i = 0;
    while(((ret = read(fd, &buf[i], 1)) == 1) && (i < size)) {
        if (buf[i++] == '\n') {
            buf[i - 1] = 0;
            break;
        }
    }

    return ret < 0 ? ret : i;
}

static int wait_and_print_backend_output(int s)
{
    int ret = 0;
    while (1) {
        memset(buffer, 0, buffer_size);
        ret = readline(s, buffer, buffer_size);
        if (!strcmp(end_string, buffer)) {
            break;
        }
        printf("%s\n", buffer);
        if (ret < 0) {
            break;
        }
    }
    return ret;
}

int main(int argc, char *argv[])
{
    if (access(socket_path, F_OK)) {
        start_backend_service();
    }

    int s = connect_to_backend();
    if (s < 0) {
        return -1;
    }

    int ret = send_input_to_backend(s, argc, argv);
    ret = wait_and_print_backend_output(s);

    close(s);

    return 0;
}
