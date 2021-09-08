#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>

extern const char * const _SOCKET_PATH_NAME;
extern int _bg_main(int argc, char *argv[]);

static const char * const END_STRING = "[@return_result_value@]";

#define BUFFER_SIZE (4 * 1024 * 1024) // 4MB
static char BUFFER[BUFFER_SIZE] = {0};

static ssize_t safe_read(int fd, void *buf, size_t count)
{
    ssize_t rc = 0;
    size_t need_to_read = count;
    void * read_buf = buf;

    do {
        rc = read(fd, read_buf, need_to_read);
        if (rc <= 0) {
            if (errno == EINTR)
                continue;
            if (rc < 0)
                return rc;
            break;
        }

        read_buf = (void *)((char *)read_buf + rc);
        need_to_read -= rc;
    } while (need_to_read != 0);

    return count - need_to_read;
}

static ssize_t safe_write(int fd, const void *buf, size_t count)
{
    ssize_t rc = 0;
    size_t need_to_write = count;
    const void * write_buf = buf;

    do {
        rc = write(fd, write_buf, need_to_write);
        if (rc <= 0) {
            if (errno == EINTR)
                continue;
            if (rc < 0)
                return rc;
            break;
        }

        need_to_write -= rc;
        write_buf = (void *)((char *)write_buf + rc);
    } while (need_to_write != 0);

    return count - need_to_write;
}

static int create_listen_socket()
{
    int s = socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, _SOCKET_PATH_NAME, sizeof sa.sun_path - 1);
    if (sa.sun_path[0] == '@') {
        sa.sun_path[0] = 0;
    }

    int rc = bind(s, (struct sockaddr *) &sa, sizeof(sa));

    listen(s, 1);
    return s;
}

static void redirect_std_to_null(void)
{
    int fd;
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        (void) dup2(fd, STDIN_FILENO);
        (void) dup2(fd, STDOUT_FILENO);
        (void) dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            (void) close(fd);
        }
    }
}

static void dealwith_client(int fd)
{
    dup2(fd, STDIN_FILENO);
    dup2(STDIN_FILENO, STDOUT_FILENO);
    dup2(STDOUT_FILENO, STDERR_FILENO);

    int32_t argc = 0;
    memset(BUFFER, 0, BUFFER_SIZE);
    int ret = safe_read(fd, &argc, sizeof argc);
    if (ret < 0) {
        printf("read argc from client error: %d errno: %d\n", ret, errno);
    }

    int32_t len = 0;
    char * argv[argc];
    char * pBuf = BUFFER;
    for (int i = 0; i < argc; i++) {
        ret = safe_read(fd, &len, sizeof len);
        if (ret < 0) {
            printf("read argv[%d] length from client error: %d errno: %d\n", i, ret, errno);
        }
        ret = safe_read(fd, pBuf, len);
        if (ret < 0) {
            printf("read argv[%d] content from client error: %d errno: %d\n", i, ret, errno);
        }
        argv[i] = pBuf;
        pBuf += len;
    }

    ret = _bg_main(argc, argv);

    printf("%s\n", END_STRING);
    safe_write(fd, &ret, sizeof ret);

    redirect_std_to_null();
}

static int start_backend_service(void)
{
    switch(fork()) {
    case -1:
        return -1;
    case 0:
        break;
    default:
        return 0;
    }

    if (setsid() == -1) {
        return -1;
    }

    (void) chdir("/");
    redirect_std_to_null();

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

static int connect_to_backend()
{
    int s = socket(AF_LOCAL, SOCK_STREAM, 0);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, _SOCKET_PATH_NAME, sizeof sa.sun_path - 1);
    if (sa.sun_path[0] == '@') {
        sa.sun_path[0] = 0;
    }

    int wait_time = 100;
    int ret = 0;
    do {
        ret = connect(s, (struct sockaddr *) &sa, sizeof sa);
        if (ret == 0) {
            return s;
        }

        if (errno == ECONNREFUSED) {
            if (wait_time == 100) { // only start the backend service when the first connect failed
                start_backend_service();
            }
            wait_time--;
            usleep(100 * 1000);
        }
    } while (wait_time > 0);

    printf("Failed to connect backend %d, errno(%d)\n", ret, errno);
    close(s);
    return ret;
}

// BUFFER layout
// __________________________________________________
// |4B | 4B| payload | 4B | payload|...|4B| payload |
// --------------------------------------------------
//
static int encode_input(int32_t argc, char * argv[])
{
    memset(BUFFER, 0, BUFFER_SIZE);
    memcpy(BUFFER, &argc, sizeof(argc));
    int32_t count = sizeof(argc);
    int32_t len = 0;

    for (int i = 0; i < argc; i++) {
        len = strlen(argv[i]) + 1;
        if (count + len > BUFFER_SIZE) {
            printf("the input data is too long!\n");
            return count;
        }
        memcpy(&BUFFER[count], &len, sizeof(len));
        count += sizeof(len);
        memcpy(&BUFFER[count], argv[i], len);
        count += len;
    }
    return count;
}

static int send_input_to_backend(int s, int argc, char *argv[])
{
    int len = encode_input(argc, argv);
    if (len < 0) {
        printf("encode input data failed!!\n");
        return len;
    }

    return safe_write(s, BUFFER, len);
}

static int readline(int fd, char * buf, int size)
{
    int ret = 0;
    int i = 0;
    while(((ret = safe_read(fd, &buf[i], 1)) == 1) && (i < size)) {
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
        memset(BUFFER, 0, BUFFER_SIZE);
        ret = readline(s, BUFFER, BUFFER_SIZE);
        if (ret <= 0) {
            printf("Fail to read data from backend.[%d]\n", ret);
            return ret;
        }

        // capture the return value
        if (!strcmp(END_STRING, BUFFER)) {
            safe_read(s, &ret, sizeof ret);
            return ret;
        }
        printf("%s\n", BUFFER);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int s = connect_to_backend();
    if (s < 0) {
        return -1;
    }

    int ret = send_input_to_backend(s, argc, argv);
    ret = wait_and_print_backend_output(s);

    close(s);

    return ret;
}
