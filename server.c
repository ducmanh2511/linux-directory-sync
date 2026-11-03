#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYNC_PATH_MAX 4095
#define SYNC_HASH_SIZE 32
#define SYNC_BUFFER_SIZE 65536
#define SYNC_FILE_MAX (UINT64_C(1024) * 1024 * 1024)
enum command
{
    CMD_MKDIR = 1,
    CMD_CHECK = 2,
    CMD_PUT = 3,
    CMD_QUIT = 4,
    CMD_LS = 6
};

enum status
{
    ST_OK = 0,
    ST_READY = 1,
    ST_SAME = 2,
    ST_NEED = 3,
    ST_ERROR = 4
};

#define RESPONSE_FLAG 0x8000u
/* Cấu trúc dùng trong chương trình. Không gửi thẳng struct qua mạng. */
struct header
{
    uint16_t type;
    uint32_t status;
    uint32_t length;
};

struct request
{
    char path[SYNC_PATH_MAX + 1];
    uint64_t size;
    unsigned char hash[SYNC_HASH_SIZE];
};

struct storage
{
    int root_fd;
    int temp_fd;
    char temp_path[PATH_MAX];
    pthread_mutex_t mutex;
};

struct session
{
    int socket_fd;
    unsigned id;
    struct storage *storage;
};

static void put16(unsigned char *cursor, uint16_t value)
{
    value = htons(value);
    memcpy(cursor, &value, 2);
}

static void put32(unsigned char *cursor, uint32_t value)
{
    value = htonl(value);
    memcpy(cursor, &value, 4);
}

static uint16_t get16(const unsigned char *cursor)
{
    uint16_t value;
    memcpy(&value, cursor, 2);
    return ntohs(value);
}

static uint32_t get32(const unsigned char *cursor)
{
    uint32_t value;
    memcpy(&value, cursor, 4);
    return ntohl(value);
}

/* Tách số 64 bit thành 8 byte theo thứ tự thống nhất trên mạng. */
static uint64_t get64(const unsigned char *cursor)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value = (value << 8) | cursor[i];
    }
    return value;
}

/* Gửi hết dữ liệu, kể cả khi một lần send chưa gửi đủ. */
static int send_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    while (length)
    {
        ssize_t count = send(fd, cursor, length, MSG_NOSIGNAL);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (count == 0)
        {
            errno = EPIPE;
            return -1;
        }
        cursor += count;
        length -= (size_t) count;
    }
    return 0;
}

/* Đọc đủ số byte cần dùng. EOF giữa chừng được xem là lỗi. */
static int recv_exact(int fd, void *data, size_t length)
{
    unsigned char *cursor = data;
    size_t done = 0;
    while (done < length)
    {
        ssize_t count = recv(fd, cursor + done, length - done, 0);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (count == 0)
        {
            if (done)
            {
                errno = ECONNRESET;
                return -1;
            }
            return 0;
        }
        done += (size_t) count;
    }
    return 1;
}

static int send_header(int fd, const struct header *header)
{
    /* Ghi từng trường để không phụ thuộc cách compiler bố trí struct. */
    unsigned char raw[16];
    memcpy(raw, "FSYN", 4);
    put16(raw + 4, 1);
    put16(raw + 6, header->type);
    put32(raw + 8, header->status);
    put32(raw + 12, header->length);
    return send_all(fd, raw, sizeof raw);
}

static int recv_header(int fd, struct header *header)
{
    unsigned char raw[16];
    int return_code = recv_exact(fd, raw, sizeof raw);
    if (return_code != 1)
    {
        return return_code;
    }
    if (memcmp(raw, "FSYN", 4) || get16(raw + 4) != 1)
    {
        errno = EPROTO;
        return -1;
    }
    header->type = get16(raw + 6);
    header->status = get32(raw + 8);
    header->length = get32(raw + 12);
    return 1;
}

/* Chỉ chấp nhận đường dẫn tương đối nằm trong thư mục đồng bộ. */
static int valid_path(const char *path)
{
    size_t length = strlen(path);
    if (!length || length > SYNC_PATH_MAX || path[0] == '/')
    {
        return 0;
    }
    const char *cursor = path;
    while (* cursor)
    {
        const char *end = strchr(cursor, '/');
        size_t count;
        if (end != NULL)
        {
            count = (size_t)(end - cursor);
        }
        else
        {
            count = strlen(cursor);
        }
        if (!count || (count == 1 && cursor[0] == '.') || (count == 2 && !memcmp(cursor, "..", 2)) ||
            (count == 9 && !memcmp(cursor, ".sync_tmp", 9)))
        {
            return 0;
        }
        if (!end)
        {
            break;
        }
        cursor = end + 1;
        if (!* cursor)
        {
            return 0;
        }
    }
    return 1;
}

static int recv_request(int fd, const struct header *header, struct request *request)
{
    unsigned char data[4 + SYNC_PATH_MAX + 8 + SYNC_HASH_SIZE];
    memset(request, 0, sizeof * request);
    if (header->status || header->type < CMD_MKDIR || (header->type > CMD_QUIT && header->type != CMD_LS))
    {
        goto bad;
    }
    if (header->type == CMD_QUIT || header->type == CMD_LS)
    {
        if (header->length)
        {
            goto bad;
        }
        return 0;
    }
    if (header->length < 5 || header->length > sizeof data)
    {
        goto bad;
    }
    if (recv_exact(fd, data, header->length) != 1)
    {
        return -1;
    }
    uint32_t count = get32(data);
    uint32_t extra = 0;
    if (header->type == CMD_CHECK || header->type == CMD_PUT)
    {
        extra = 8 + SYNC_HASH_SIZE;
    }
    if (!count || count > SYNC_PATH_MAX || header->length != 4 + count + extra || memchr(data + 4, 0,
        count))
    {
        goto bad;
    }
    memcpy(request->path, data + 4, count);
    request->path[count] = 0;
    if (!valid_path(request->path))
    {
        goto bad;
    }
    if (extra)
    {
        request->size = get64(data + 4 + count);
        memcpy(request->hash, data + 4 + count + 8, SYNC_HASH_SIZE);
        if (request->size > SYNC_FILE_MAX)
        {
            goto bad;
        }
    }
    return 0;
    bad :
    errno = EPROTO;
    return -1;
}

static int send_status(int fd, uint16_t command, uint32_t status)
{
    struct header header =
    {
        (uint16_t) (RESPONSE_FLAG | command), status, 0
    };
    return send_header(fd, &header);
}

static int parse_port(const char *text, uint16_t * port)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (!* text || * end || errno || value < 1 || value > 65535)
    {
        return -1;
    }
    * port = (uint16_t) value;
    return 0;
}

/* Tính SHA-256 từ đầu tệp. Mỗi lần gọi có vùng tính hash riêng. */
static int hash_fd(int fd, unsigned char result[SYNC_HASH_SIZE])
{
    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context)
    {
        errno = ENOMEM;
        return -1;
    }
    unsigned char buffer[SYNC_BUFFER_SIZE];
    unsigned int length = 0;
    int return_code = -1;
    if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1)
    {
        errno = EIO;
        goto out;
    }
    for (;;)
    {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            goto out;
        }
        if (count == 0)
        {
            break;
        }
        if (EVP_DigestUpdate(context, buffer, (size_t) count) != 1)
        {
            errno = EIO;
            goto out;
        }
    }
    if (EVP_DigestFinal_ex(context, result, &length) != 1 || length != SYNC_HASH_SIZE)
    {
        errno = EIO;
        goto out;
    }
    return_code = 0;
out:
    EVP_MD_CTX_free(context);
    return return_code;
}

/* Mở lần lượt các thư mục cha, không đi theo liên kết mềm.
 * Hàm gọi phải giữ mutex và đóng fd được trả về. */
static int parent_directory(int root, const char *path, int create, char filename[SYNC_PATH_MAX + 1])
{
    char copy[SYNC_PATH_MAX + 1];
    strcpy(copy, path);
    char *last_slash = strrchr(copy, '/');
    int current_fd = dup(root);
    if (current_fd < 0)
    {
        return -1;
    }
    if (!last_slash)
    {
        strcpy(filename, copy);
        return current_fd;
    }
    *last_slash = 0;
    strcpy(filename, last_slash + 1);
    char *save_position = NULL;
    for (char *part = strtok_r(copy, "/", &save_position); part; part = strtok_r(NULL, "/", &save_position))
    {
        if (create && mkdirat(current_fd, part, 0755) < 0 && errno != EEXIST)
        {
            close(current_fd);
            return -1;
        }
        int next_fd = openat(current_fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_fd < 0)
        {
            close(current_fd);
            return -1;
        }
        close(current_fd);
        current_fd = next_fd;
    }

    return current_fd;
}

/* Tạo thư mục đích khi chưa có. Mutex bảo vệ phần dùng chung. */
static uint32_t make_directory(struct storage *server, const char *path)
{
    char filename[SYNC_PATH_MAX + 1];
    uint32_t status = ST_ERROR;
    pthread_mutex_lock(&server->mutex);
    int parent_fd = parent_directory(server->root_fd, path, 1, filename);
    if (parent_fd >= 0)
    {
        if (mkdirat(parent_fd, filename, 0755) == 0 || errno == EEXIST)
        {
            int fd = openat(parent_fd, filename, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0)
            {
                status = ST_OK;
                close(fd);
            }
        }
        close(parent_fd);
    }
    pthread_mutex_unlock(&server->mutex);
    return status;
}

/* So sánh nội dung đích để client biết có cần gửi tệp không. */
static uint32_t check_file(struct storage *server, const struct request *request)
{
    char filename[SYNC_PATH_MAX + 1];
    uint32_t status = ST_ERROR;
    pthread_mutex_lock(&server->mutex);
    int parent_fd = parent_directory(server->root_fd, request->path, 0, filename);
    if (parent_fd < 0)
    {
        if (errno == ENOENT)
        {
            status = ST_NEED;
        }
        goto unlock;
    }
    struct stat st;
    if (fstatat(parent_fd, filename, &st, AT_SYMLINK_NOFOLLOW) < 0)
    {
        if (errno == ENOENT)
        {
            status = ST_NEED;
        }
    }
    else if (S_ISREG(st.st_mode))
    {
        if ((uint64_t) st.st_size != request->size)
        {
            status = ST_NEED;
        }
        else
        {
            int fd = openat(parent_fd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0)
            {
                unsigned char digest[SYNC_HASH_SIZE];
                if (hash_fd(fd, digest) == 0)
                {
                    if (memcmp(digest, request->hash, SYNC_HASH_SIZE) == 0)
                    {
                        status = ST_SAME;
                    }
                    else
                    {
                        status = ST_NEED;
                    }
                }
                close(fd);
            }
        }
    }
    close(parent_fd);
unlock:
    pthread_mutex_unlock(&server->mutex);
    return status;
}

/* Ghi đủ dữ liệu vào tệp tạm, xử lý trường hợp ghi thiếu. */
static int write_all_file(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length)
    {
        ssize_t bytes_written = write(fd, p, length);
        if (bytes_written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (bytes_written == 0)
        {
            errno = EIO;
            return -1;
        }
        p += bytes_written;
        length -= (size_t) bytes_written;
    }
    return 0;
}

/* Chỉ cho phép thay đích chưa có hoặc đích là tệp thường. */
static int replaceable(int parent_fd, const char *filename)
{
    struct stat st;
    if (fstatat(parent_fd, filename, &st, AT_SYMLINK_NOFOLLOW) == 0)
    {
        return S_ISREG(st.st_mode);
    }
    return errno == ENOENT;
}

/* Nhận vào tệp tạm. Trả -1 nếu phải đóng kết nối do lỗi. */
static int receive_file(struct session *session, const struct request *request)
{
    struct storage *server = session->storage;
    int socket_fd = session->socket_fd;
    char filename[SYNC_PATH_MAX + 1], temporary[PATH_MAX];
    pthread_mutex_lock(&server->mutex);
    int parent_fd = parent_directory(server->root_fd, request->path, 1, filename);
    int allowed = parent_fd >= 0 && replaceable(parent_fd, filename);
    pthread_mutex_unlock(&server->mutex);
    if (!allowed)
    {
        if (parent_fd >= 0)
        {
            close(parent_fd);
        }
        return send_status(socket_fd, CMD_PUT, ST_ERROR);
    }
    int count = snprintf(temporary, sizeof temporary, "%s/upload-XXXXXX", server->temp_path);
    if (count < 0 || (size_t) count >= sizeof temporary)
    {
        close(parent_fd);
        return send_status(socket_fd, CMD_PUT, ST_ERROR);
    }
    int temp = mkstemp(temporary);
    if (temp < 0)
    {
        close(parent_fd);
        return send_status(socket_fd, CMD_PUT, ST_ERROR);
    }
    const char *temp_name = strrchr(temporary, '/') + 1;
    int result = -1;
    uint32_t status = ST_ERROR;
    if (send_status(socket_fd, CMD_PUT, ST_READY) < 0)
    {
        goto cleanup;
    }
    /* Mỗi phiên có tệp tạm riêng nên có thể nhận đồng thời. */
    unsigned char buffer[SYNC_BUFFER_SIZE];
    uint64_t remaining = request->size;
    while (remaining)
    {
        size_t want = sizeof buffer;
        if (remaining < sizeof buffer)
        {
            want = (size_t)remaining;
        }
        if (recv_exact(socket_fd, buffer, want) != 1 || write_all_file(temp, buffer, want) < 0)
        {
            goto cleanup;
        }
        remaining -= want;
    }
    unsigned char digest[SYNC_HASH_SIZE];
    if (hash_fd(temp, digest) < 0 || memcmp(digest, request->hash, SYNC_HASH_SIZE))
    {
        goto reply;
    }
    if (close(temp) < 0)
    {
        temp = -1;
        goto reply;
    }
    temp = -1;
    /* Chỉ khóa lúc thay tệp đích. Nhận mạng phía trên không giữ khóa. */
    pthread_mutex_lock(&server->mutex);
    if (replaceable(parent_fd, filename) && renameat(server->temp_fd, temp_name, parent_fd, filename) == 0)
    {
        status = ST_OK;
        printf("[session %u] COMMIT %s (%llu bytes)\n", session->id, request->path, (unsigned long long) request->size);
    }
    pthread_mutex_unlock(&server->mutex);
reply:
    if (status != ST_OK)
    {
        fprintf(stderr, "[session %u] PUT rejected/failed %s\n", session->id, request->path);
    }
    result = send_status(socket_fd, CMD_PUT, status);
cleanup:
    if (temp >= 0)
    {
        close(temp);
    }
    /* Sau rename thành công, tên tạm đã mất nên unlink có thể báo ENOENT. */
    unlinkat(server->temp_fd, temp_name, 0);
    close(parent_fd);
    if (result < 0)
    {
        fprintf(stderr, "[session %u] PUT interrupted %s; old destination kept if not committed\n",
            session->id, request->path);
    }
    return result;
}

static int open_storage(struct storage *storage, const char *root)
{
    if (mkdir(root, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir root (parent must exist)");
        return -1;
    }
    char resolved[PATH_MAX];
    if (!realpath(root, resolved))
    {
        perror("realpath");
        return -1;
    }
    storage->root_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (storage->root_fd < 0)
    {
        perror("open root");
        return -1;
    }
    if (mkdirat(storage->root_fd, ".sync_tmp", 0700) < 0 && errno != EEXIST)
    {
        perror("mkdir tmp");
        close(storage->root_fd);
        return -1;
    }
    storage->temp_fd = openat(storage->root_fd, ".sync_tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int path_length = snprintf(storage->temp_path, sizeof storage->temp_path, "%s/.sync_tmp", resolved);
    if (storage->temp_fd < 0 || path_length < 0 || (size_t) path_length >= sizeof storage->temp_path)
    {
        perror("temp root");
        if (storage->temp_fd >= 0)
        {
            close(storage->temp_fd);
        }
        close(storage->root_fd);
        return -1;
    }
    return 0;
}

static void close_storage(struct storage *storage)
{
    close(storage->temp_fd);
    close(storage->root_fd);
}

static int handle_transfer(struct session *session, const struct header *header,
                    const struct request *request)
{
    uint32_t status;
    switch (header->type)
    {
        case CMD_MKDIR:
            status = make_directory(session->storage, request->path);
            break;
        case CMD_CHECK:
            status = check_file(session->storage, request);
            break;
        case CMD_PUT:
            return receive_file(session, request);
        default:
            return -1;
    }
    return send_status(session->socket_fd, header->type, status);
}

static int list_storage(struct session *session)
{
    int fd = session->socket_fd;
    int directory = openat(session->storage->root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
    {
        return send_status(fd, CMD_LS, ST_ERROR);
    }
    DIR *stream = fdopendir(directory);
    if (!stream)
    {
        close(directory);
        return send_status(fd, CMD_LS, ST_ERROR);
    }

    int result = 0;
    uint32_t status = ST_OK;
    for (;;)
    {
        errno = 0;
        const struct dirent *entry = readdir(stream);
        if (!entry)
        {
            if (errno)
            {
                status = ST_ERROR;
            }
            break;
        }
        if (!valid_path(entry->d_name))
        {
            continue;
        }
        struct header item = {RESPONSE_FLAG | CMD_LS, ST_READY, strlen(entry->d_name)};
        if (send_header(fd, &item) < 0 || send_all(fd, entry->d_name, item.length) < 0)
        {
            result = -1;
            break;
        }
    }
    closedir(stream);
    if (result < 0)
    {
        return -1;
    }
    return send_status(fd, CMD_LS, status);
}

/* Mỗi luồng chạy hàm này cho một kết nối riêng. */
static void *handle_client(void *argument)
{
    struct session *session = argument;
    int fd = session->socket_fd;
    printf("[session %u] CONNECTED\n", session->id);
    for (;;)
    {
        struct header header;
        struct request request;
        int thread_result = recv_header(fd, &header);
        if (thread_result == 0)
        {
            break;
        }
        if (thread_result < 0 || recv_request(fd, &header, &request) < 0)
        {
            fprintf(stderr, "[session %u] invalid/incomplete request\n", session->id);
            break;
        }
        if (header.type == CMD_QUIT)
        {
            (void) send_status(fd, CMD_QUIT, ST_OK);
            break;
        }
        switch (header.type)
        {
            case CMD_LS:
                thread_result = list_storage(session);
                break;
            default:
                thread_result = handle_transfer(session, &header, &request);
                break;
        }
        if (thread_result < 0)
        {
            break;
        }
    }
    printf("[session %u] DISCONNECTED\n", session->id);
    close(fd);
    free(session);
    return NULL;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--root DIR] [--port PORT]\n", program);
}

int main(int argc, char **argv)
{
    const char *root = "storage2";
    uint16_t port = 9000;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc)
        {
            usage(argv[0]);
            return 1;
        }
        if (!strcmp(argv[i], "--root"))
        {
            root = argv[++i];
        }
        else if (!strcmp(argv[i], "--port"))
        {
            if (parse_port(argv[++i], &port) < 0)
            {
                usage(argv[0]);
                return 1;
            }
        }
        else
        {
            usage(argv[0]);
            return 1;
        }
    }
    struct storage state = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    if (open_storage(&state, root) < 0)
    {
        return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
    {
        perror("socket");
        close_storage(&state);
        return 1;
    }
    int yes = 1;
    (void) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr =
    {
        .sin_family = AF_INET,.sin_port = htons(port),.sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(listener, (struct sockaddr *) &addr, sizeof addr) < 0 || listen(listener, 16) < 0)
    {
        perror("bind/listen");
        close(listener);
        close_storage(&state);
        return 1;
    }
    /* In log ngay để dễ quan sát hai phiên chạy đồng thời. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("LISTEN port=%u root=%s\n", port, root);
    unsigned next_id = 1;
    for (;;)
    {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("accept");
            continue;
        }
        /* Mỗi kết nối có context riêng, không dùng chung biến socket. */
        struct session *session = malloc(sizeof *session);
        if (!session)
        {
            close(fd);
            continue;
        }
        * session = (struct session)
        {
            fd, next_id++, &state
        };
        pthread_attr_t attr;
        int thread_result = pthread_attr_init(&attr);
        if (thread_result != 0)
        {
            close(fd);
            free(session);
            continue;
        }
        thread_result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t worker;
        if (thread_result == 0)
        {
            thread_result = pthread_create(&worker, &attr, handle_client, session);
        }
        pthread_attr_destroy(&attr);
        if (thread_result != 0)
        {
            fprintf(stderr, "pthread_create: %s\n", strerror(thread_result));
            close(fd);
            free(session);
        }
    }
}
