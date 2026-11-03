#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
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

struct sync_stats
{
    unsigned checked;
    unsigned sent;
    unsigned same;
    unsigned directories;
    unsigned skipped;
    unsigned failed;
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
static void put64(unsigned char *cursor, uint64_t value)
{
    for (int i = 7; i >= 0; --i)
    {
        cursor[i] = (unsigned char) value;
        value >>= 8;
    }
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

static int send_request(int fd, uint16_t command, const struct request *request)
{
    unsigned char metadata[4 + SYNC_PATH_MAX + 8 + SYNC_HASH_SIZE];
    uint32_t length = 0;
    if (command != CMD_QUIT && command != CMD_LS)
    {
        if (!request || !valid_path(request->path))
        {
            errno = EINVAL;
            return -1;
        }
        uint32_t count = (uint32_t) strlen(request->path);
        put32(metadata, count);
        memcpy(metadata + 4, request->path, count);
        length = 4 + count;
        if (command == CMD_CHECK || command == CMD_PUT)
        {
            put64(metadata + length, request->size);
            length += 8;
            memcpy(metadata + length, request->hash, SYNC_HASH_SIZE);
            length += SYNC_HASH_SIZE;
        }
        else if (command != CMD_MKDIR)
        {
            errno = EINVAL;
            return -1;
        }
    }
    struct header header =
    {
        command, 0, length
    };
    if (send_header(fd, &header) < 0)
    {
        return -1;
    }
    return send_all(fd, metadata, length);
}

static int recv_status(int fd, uint16_t command, uint32_t * status)
{
    struct header header;
    if (recv_header(fd, &header) != 1)
    {
        return -1;
    }
    if (header.type != (RESPONSE_FLAG | command) || header.length || header.status > ST_ERROR)
    {
        errno = EPROTO;
        return -1;
    }
    * status = header.status;
    return 0;
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

/* Kiểm tra các dấu hiệu cho thấy tệp vừa bị thay đổi. */
static int unchanged(const struct stat *before, const struct stat *after)
{
    return before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

/* Kiểm tra một tệp; chỉ gửi nội dung khi server cần. */
static int sync_file(int socket_fd, int directory_fd, const char *filename, const char *relative_path,
    struct sync_stats *stats)
{
    struct request request =
    {
        0
    };
    struct stat before_info, after_info;
    uint32_t status;
    int fd = openat(directory_fd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        goto local_error;
    }
    if (fstat(fd, &before_info) < 0)
    {
        goto file_error;
    }
    if (!S_ISREG(before_info.st_mode) || before_info.st_size < 0 || (uint64_t) before_info.st_size > SYNC_FILE_MAX)
    {
        errno = EINVAL;
        goto file_error;
    }
    strcpy(request.path, relative_path);
    request.size = (uint64_t) before_info.st_size;
    if (hash_fd(fd, request.hash) < 0 || fstat(fd, &after_info) < 0)
    {
        goto file_error;
    }
    if (!unchanged(&before_info, &after_info))
    {
        errno = EAGAIN;
        goto file_error;
    }
    ++stats->checked;
    if (send_request(socket_fd, CMD_CHECK, &request) < 0 || recv_status(socket_fd, CMD_CHECK, &status) < 0)
    {
        goto network_error;
    }
    if (status == ST_SAME)
    {
        printf("SAME %s\n", relative_path);
        ++stats->same;
        close(fd);
        return 0;
    }
    if (status == ST_ERROR)
    {
        errno = EIO;
        goto file_error;
    }
    if (status != ST_NEED)
    {
        errno = EPROTO;
        goto network_error;
    }
    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        goto file_error;
    }
    if (send_request(socket_fd, CMD_PUT, &request) < 0 || recv_status(socket_fd, CMD_PUT, &status) < 0)
    {
        goto network_error;
    }
    if (status == ST_ERROR)
    {
        errno = EIO;
        goto file_error;
    }
    if (status != ST_READY)
    {
        errno = EPROTO;
        goto network_error;
    }
    unsigned char buffer[SYNC_BUFFER_SIZE];
    uint64_t remaining = request.size;
    while (remaining)
    {
        size_t want = sizeof buffer;
        if (remaining < sizeof buffer)
        {
            want = (size_t)remaining;
        }
        ssize_t bytes_read = read(fd, buffer, want);
        if (bytes_read < 0 && errno == EINTR)
        {
            continue;
        }
        /* Đã nhận READY thì phải gửi đủ nội dung hoặc đóng kết nối. */
        if (bytes_read <= 0)
        {
            if (bytes_read == 0)
            {
                errno = EIO;
            }
            goto network_error;
        }
        if (send_all(socket_fd, buffer, (size_t) bytes_read) < 0)
        {
            goto network_error;
        }
        remaining -= (uint64_t) bytes_read;
    }
    if (recv_status(socket_fd, CMD_PUT, &status) < 0)
    {
        goto network_error;
    }
    if (status == ST_ERROR)
    {
        errno = EIO;
        goto file_error;
    }
    if (status != ST_OK)
    {
        errno = EPROTO;
        goto network_error;
    }
    if (fstat(fd, &after_info) < 0)
    {
        goto file_error;
    }
    if (!unchanged(&before_info, &after_info))
    {
        fprintf(stderr,
            "SOURCE CHANGED %s: server accepted earlier content; run again after source is stable\n",
            relative_path);
        ++stats->failed;
        close(fd);
        return 0;
    }
    printf("PUT OK %s (%llu bytes)\n", relative_path, (unsigned long long) request.size);
    ++stats->sent;
    close(fd);
    return 0;
/* Các nhãn bên dưới gom việc dọn fd, tránh đóng thiếu khi có lỗi. */
file_error:
    {
        int saved_error = errno;
        close(fd);
        errno = saved_error;
    }
local_error:
    fprintf(stderr, "FILE ERROR %s: %s\n", relative_path, strerror(errno));
    ++stats->failed;
    return 0;
network_error:
    close(fd);
    return -1;
}

/* Duyệt thư mục con trước khi chuyển sang mục kế tiếp. */
static int scan(int socket_fd, int directory_fd, const char *prefix, unsigned depth, struct sync_stats *stats)
{
    if (depth > 64)
    {
        fprintf(stderr, "Too many directory levels: %s\n", prefix);
        ++stats->failed;
        return 0;
    }
    /* Mỗi lượt mở lại cây nguồn để quét từ đầu, không giữ vị trí lượt cũ. */
    int scan_fd = openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan_fd < 0)
    {
        perror("open scan directory");
        ++stats->failed;
        return 0;
    }
    DIR * stream = fdopendir(scan_fd);
    if (!stream)
    {
        close(scan_fd);
        ++stats->failed;
        return 0;
    }
    int result = 0;
    for (;;)
    {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry)
        {
            if (errno)
            {
                perror("readdir");
                ++stats->failed;
            }
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        {
            continue;
        }
        if (!strcmp(entry->d_name, ".sync_tmp"))
        {
            ++stats->skipped;
            continue;
        }
        char relative_path[SYNC_PATH_MAX + 1];
        int length;
        if (prefix[0] == '\0')
        {
            length = snprintf(relative_path, sizeof relative_path,
                              "%s", entry->d_name);
        }
        else
        {
            length = snprintf(relative_path, sizeof relative_path,
                              "%s/%s", prefix, entry->d_name);
        }

        if (length < 0 || (size_t) length >= sizeof relative_path || !valid_path(relative_path))
        {
            fprintf(stderr, "Invalid/long source path\n");
            ++stats->failed;
            continue;
        }
        struct stat entry_info;
        if (fstatat(directory_fd, entry->d_name, &entry_info, AT_SYMLINK_NOFOLLOW) < 0)
        {
            fprintf(stderr, "STAT ERROR %s: %s\n", relative_path, strerror(errno));
            ++stats->failed;
            continue;
        }
        if (S_ISDIR(entry_info.st_mode))
        {
            struct request request =
            {
                0
            };
            uint32_t status;
            strcpy(request.path, relative_path);
            if (send_request(socket_fd, CMD_MKDIR, &request) < 0 || recv_status(socket_fd, CMD_MKDIR,
                &status) < 0)
            {
                result = -1;
                break;
            }
            if (status == ST_ERROR)
            {
                fprintf(stderr, "MKDIR ERROR %s\n", relative_path);
                ++stats->failed;
                continue;
            }
            if (status != ST_OK)
            {
                errno = EPROTO;
                result = -1;
                break;
            }
            ++stats->directories;
            int child_fd = openat(directory_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child_fd < 0)
            {
                perror(relative_path);
                ++stats->failed;
                continue;
            }
            result = scan(socket_fd, child_fd, relative_path, depth + 1, stats);
            close(child_fd);
            if (result < 0)
            {
                break;
            }
        }
        else if (S_ISREG(entry_info.st_mode))
        {
            if (sync_file(socket_fd, directory_fd, entry->d_name, relative_path, stats) < 0)
            {
                result = -1;
                break;
            }
        }
        else
        {
            printf("SKIP special/symlink %s\n", relative_path);
            ++stats->skipped;
        }
    }
    closedir(stream);
    return result;
}

/* Gửi nội dung thư mục nguồn vào đường dẫn đích đã chọn. */
static int sync_once(int socket_fd, int root_fd, const char *prefix, struct sync_stats *stats)
{
    memset(stats, 0, sizeof * stats);
    return scan(socket_fd, root_fd, prefix, 0, stats);
}

/* Trả -1 khi mất đồng bộ giao thức, 0 nếu còn dùng được kết nối. */
static int upload_directory(int fd, const char *local, const char *remote)
{
    int source = open(local, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (source < 0)
    {
        perror(local);
        return 0;
    }

    struct request request = {0};
    strcpy(request.path, remote);
    uint32_t status;
    int result = -1;
    if (send_request(fd, CMD_MKDIR, &request) == 0 &&
        recv_status(fd, CMD_MKDIR, &status) == 0)
    {
        if (status == ST_OK)
        {
            struct sync_stats stats;
            result = sync_once(fd, source, remote, &stats);
            if (result == 0)
            {
                printf("rsync: sent=%u same=%u failed=%u\n", stats.sent, stats.same, stats.failed);
            }
        }
        else
        {
            fprintf(stderr, "Khong tao duoc thu muc dich tren server.\n");
            result = 0;
        }
    }
    close(source);
    return result;
}

static int list_remote(int fd)
{
    if (send_request(fd, CMD_LS, NULL) < 0)
    {
        return -1;
    }
    puts("Thu muc goc tren server:");
    for (;;)
    {
        struct header item;
        char name[SYNC_PATH_MAX + 1];
        if (recv_header(fd, &item) != 1 || item.type != (RESPONSE_FLAG | CMD_LS))
        {
            return -1;
        }
        if (item.length == 0 && (item.status == ST_OK || item.status == ST_ERROR))
        {
            if (item.status == ST_ERROR)
            {
                fprintf(stderr, "Khong doc duoc danh sach tren server.\n");
            }
            return 0;
        }
        if (item.status != ST_READY || item.length == 0 || item.length > SYNC_PATH_MAX ||
            recv_exact(fd, name, item.length) != 1)
        {
            return -1;
        }
        name[item.length] = '\0';
        puts(name);
    }
}

static void help(void)
{
    puts("ls                         Xem muc trong thu muc goc cua server");
    puts("rsync <nguon-local> <dich-server>");
    puts("help                       Xem huong dan");
    puts("quit                       Dong ket noi");
}

static int run_commands(int fd)
{
    char *line = NULL;
    size_t capacity = 0;
    int result = 0;
    help();
    for (;;)
    {
        fputs("sync> ", stdout);
        fflush(stdout);
        if (getline(&line, &capacity, stdin) < 0)
        {
            break;
        }

        const char *separators = " \t\r\n";
        char *command = strtok(line, separators);
        if (!command)
        {
            continue;
        }
        char *first = strtok(NULL, separators);
        char *second = strtok(NULL, separators);
        char *extra = strtok(NULL, separators);

        if (!first && strcmp(command, "quit") == 0)
        {
            break;
        }
        if (!first && strcmp(command, "help") == 0)
        {
            help();
        }
        else if (!first && strcmp(command, "ls") == 0)
        {
            result = list_remote(fd);
        }
        else if (second && !extra && strcmp(command, "rsync") == 0 && valid_path(second))
        {
            result = upload_directory(fd, first, second);
        }
        else
        {
            fprintf(stderr, "Lenh hoac duong dan khong hop le. Go help.\n");
        }
        if (result < 0)
        {
            break;
        }
    }
    free(line);
    if (result == 0)
    {
        uint32_t status;
        if (send_request(fd, CMD_QUIT, NULL) < 0 ||
            recv_status(fd, CMD_QUIT, &status) < 0 || status != ST_OK)
        {
            result = -1;
        }
    }
    return result;
}

int main(int argc, char **argv)
{
    uint16_t port = 9000;
    if (argc > 3 || (argc == 3 && parse_port(argv[2], &port) < 0))
    {
        fprintf(stderr, "Usage: %s [IPv4 [port]]\n", argv[0]);
        return 1;
    }
    const char *host = argc >= 2 ? argv[1] : "127.0.0.1";
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        fprintf(stderr, "Dia chi IPv4 khong hop le: %s\n", host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }
    int result = run_commands(fd);
    close(fd);
    if (result < 0)
    {
        fprintf(stderr, "Ket noi bi ngat hoac phan hoi khong hop le.\n");
        return 1;
    }
    return 0;
}
