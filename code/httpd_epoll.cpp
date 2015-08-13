//
// Very simple HTTP server. Preload files from the given directory.
//
// Usage: ./httpd_epoll <TCP port> <root dir> <n workers>
//
// Copyright 2015 Raphael Javaux <raphaeljavaux@gmail.com>
// University of Liege.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>              // strtok()
#include <unordered_map>

#include <arpa/inet.h>          // htons()
#include <dirent.h>             // struct dirent, opendir(), readdir()
#include <fcntl.h>              // fcntl()
#include <pthread.h>            // pthread_*
#include <sys/epoll.h>          // epoll_*
#include <sys/sendfile.h>       // sendfile()
#include <sys/socket.h>         // accept(), bind, listen(), recv(), send()
#include <sys/stat.h>           // struct stat, stat()
#include <sys/uio.h>            // writev()
#include <unistd.h>             // close()


const size_t    MAX_EVENTS      = 1024;
const int       LISTEN_BACKLOG  = 1024;

#define PRELOAD_FILE_CONTENT

using namespace std;

#ifdef NDEBUG
    #define DEBUG(MSG, ...)
#else
    #define DEBUG(MSG, ...)                                                    \
        do {                                                                   \
            fprintf(stderr, "[DEBUG] " MSG, ##__VA_ARGS__);                    \
            fprintf(stderr, " (" __FILE__ ":%d)\n", __LINE__);                 \
        } while (0)
#endif

#define ERROR(MSG, ...)                                                        \
    do {                                                                       \
        fprintf(stderr, "[ERROR] " MSG, ##__VA_ARGS__);                        \
        fprintf(stderr, " (" __FILE__ ":%d)\n", __LINE__);                     \
    } while (0)

#define DIE(MSG, ...)                                                          \
    do {                                                                       \
        fprintf(stderr, "[DIE] " MSG, ##__VA_ARGS__);                          \
        fprintf(stderr, " (" __FILE__ ":%d)\n", __LINE__);                     \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

// Parsed CLI arguments.
struct args_t {
    uint16_t                        tcp_port;
    char                            *root_dir;
    int                             n_workers;
};

// Type used to index file by their filename. Different from 'std::string', so
// we can initialize it without having to reallocate and copy the string.
struct filename_t {
    const char                      *value;
};

namespace std {

// 'std::hash<>' and 'std::equal_to<>' instances are required to index
// files by their filenames.

template <>
struct hash<filename_t> {
    inline size_t operator()(filename_t filename) const
    {
        const char *str = filename.value;

        size_t sum = 0;
        while (*str != '\0') {
            sum += hash<char>()(*str);
            str++;
        }

        return sum;
    }
};

template <>
struct equal_to<filename_t> {
    inline bool operator()(filename_t a, filename_t b) const
    {
        return strcmp(a.value, b.value) == 0;
    }
};

} /* namespace std */

// Served file and its content.
struct file_t {
    #ifdef PRELOAD_FILE_CONTENT
        char                            *content;
    #else
        int                             file_desc;
    #endif /* PRELOAD_FILE_CONTENT */
    size_t                          content_len;
};

struct worker_data_t {
    unordered_map<filename_t, file_t>   *files;
    uint16_t                            tcp_port;
};

// Function executed by each worker threads (called by 'pthread_create()').
static void *_worker_runner(void *files_void);

static void _print_usage(char **argv);

// Parses CLI arguments.
//
// Fails on a malformed command.
static bool _parse_args(int argc, char **argv, args_t *args);

// Loads all the file contents from the directory in the hash-table.
static void _preload_files(
    unordered_map<filename_t, file_t> *files, const char *dir
);

// Interprets an HTTP request and serves the requested content.
static void _on_received_data(
    unordered_map<filename_t, file_t> *files, int sock, char *buffer,
    size_t buffer_size
);

// Responds to the client with a 200 OK HTTP response containing the given file.
void _respond_with_200(int sock, const file_t *file);

// Responds to the client with a 400 Bad Request HTTP response.
void _respond_with_400(int sock);

// Responds to the client with a 404 Not Found HTTP response.
void _respond_with_404(int sock);

int main(int argc, char **argv)
{
    args_t args;
    if (!_parse_args(argc, argv, &args))
        return EXIT_FAILURE;

    //
    // Load files in memory.
    //

    unordered_map<filename_t, file_t> files { };
    _preload_files(&files, args.root_dir);

    //
    // Starts the workers
    //
    worker_data_t worker_data { &files, args.tcp_port };

    for (int i = 0; i < args.n_workers - 1; i++) {
        pthread_t thread_id;
        pthread_create(&thread_id, nullptr, _worker_runner, &worker_data);
    }

    // Starts the last worker in the current process.
    _worker_runner(&worker_data);

    return 0;
}

static void *_worker_runner(void *worker_data_void)
{
    worker_data_t *worker_data = (worker_data_t *) worker_data_void;

    printf("Thread started\n");

    //
    // Sets up the server socket.
    //

    int listen_sock;
    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        DIE("socket()");

    // Avoid "address already in use" when the listen address is still in
    // TIME-WAIT.
    int one = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one)))
        DIE("setsockopt(SO_REUSEADDR)");

    // Specifies that multiple process/threads can listen for new connections on
    // this port.
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof (one)))
        DIE("setsockopt(SO_REUSEPORT)");

    struct sockaddr_in listen_addr = { 0 };
    listen_addr.sin_family      = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port        = htons(worker_data->tcp_port);

    if (bind(
        listen_sock, (sockaddr *) &listen_addr, sizeof (listen_addr)
    ) == -1)
        DIE("bind()");

    if (listen(listen_sock, LISTEN_BACKLOG) == -1)
        DIE("listen()");

    // Creates an epoll for all the events on the sockets.
    int epoll;
    if ((epoll = epoll_create(MAX_EVENTS)) == -1)
        DIE("epoll_create()");

    // Sets epoll to listen for new connections on the 'listen_sock' socket.
    {
        struct epoll_event event;
        event.events    = EPOLLIN;
        event.data.fd   = listen_sock;
        if (epoll_ctl(epoll, EPOLL_CTL_ADD, listen_sock, &event) == -1)
            DIE("epoll_ctl(DEL, listen_sock)");
    }

    //
    // Processes any incomming event.
    //

    for (;;) {
        // Fetchs up to MAX_EVENTS in the poll.
        struct epoll_event events[MAX_EVENTS];
        int n_events;
        if ((n_events = epoll_wait(epoll, events, MAX_EVENTS, 0)) == -1)
            DIE("epoll_wait()");

        // Processes all notified events.
        for (int i = 0; i < n_events; i++) {
            int sock = events[i].data.fd;

            if (sock == listen_sock) {
                // New connection.

                int client_sock;
                if ((client_sock = accept(listen_sock, nullptr, nullptr)) == -1)
                    DIE("accept()");

                DEBUG("New client");

                // Sets the socket to be non-blocking
                int flags;
                if ((flags = fcntl(client_sock, F_GETFL, 0)) == -1)
                    DIE("fcntl(F_GETFL)");

                if (fcntl(client_sock, F_SETFL, flags | O_NONBLOCK)  == -1)
                    DIE("fcntl(F_SETFL)");

                // Sets epoll to listen for new data on the client socket.
                struct epoll_event event;
                event.events    = EPOLLIN;
                event.data.fd   = client_sock;
                if (epoll_ctl(epoll, EPOLL_CTL_ADD, client_sock, &event) == -1)
                    DIE("epoll_ctl(ADD, client_sock)");
            } else {
                // Event on a client socket.

                char buffer[1024];
                int n = recv(events[i].data.fd, buffer, sizeof (buffer), 0);

                if (n == -1)
                    DIE("recv()");
                else if (n == 0) {
                    // Connection closed
                    if (epoll_ctl(epoll, EPOLL_CTL_DEL, sock, nullptr))
                        DIE("epoll_ctl(DEL, client_sock)");
                } else {
                    // New data.
                    _on_received_data(worker_data->files, sock, buffer, n);
                }
            }
        }
    }

    return nullptr;
}

static void _print_usage(char **argv)
{
    fprintf(stderr, "Usage: %s <TCP port> <root dir> <n workers>\n", argv[0]);
}

static bool _parse_args(int argc, char **argv, args_t *args)
{
    if (argc != 4) {
        _print_usage(argv);
        return false;
    }

    args->tcp_port = atoi(argv[1]);

    args->root_dir = argv[2];

    args->n_workers = atoi(argv[3]);

    return true;
}

static void _preload_files(
    unordered_map<filename_t, file_t> *files, const char *root_dir
)
{
    DIR *dir;

    if (!(dir = opendir(root_dir)))
        DIE("Unable to open the directory");

    struct dirent *entry;

    size_t root_dir_len = strlen(root_dir);

    while ((entry = readdir(dir))) {
        filename_t filename = { strdup(entry->d_name) };

        // Filename with the directory path.
        char *filepath = new char[root_dir_len + strlen(filename.value) + 2];
        strcpy(filepath, root_dir);
        filepath[root_dir_len] = '/';
        strcpy(filepath + root_dir_len + 1, filename.value);

        // Skips directories.
        struct stat stat_buffer;
        if (stat(filepath, &stat_buffer) != 0)
            DIE("Unable to get info on a file (%s)", filename.value);
        if (S_ISDIR(stat_buffer.st_mode))
            continue;

        FILE *file;
        if (!(file = fopen(filepath, "r")))
            DIE("Unable to open a file");

        // Obtains the size of the file
        fseek(file, 0, SEEK_END);
        size_t content_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        #ifdef PRELOAD_FILE_CONTENT
            // Reads the file content

            char *content = new char[content_size + 1];
            size_t read = fread(content, 1, content_size, file);

            if (read != content_size)
                DIE("Unable to read a file %zu %zu", read, content_size);

            content[content_size] = '\0';
            fclose(file);
        #else
            int content = open(filepath, O_RDONLY);
        #endif /* PRELOAD_FILE_CONTENT */

        file_t entry = { content, content_size };

        files->emplace(filename, entry);
    }

    DEBUG("%zu file(s) preloaded", files->size());
}

static void _on_received_data(
    unordered_map<filename_t, file_t> *files, int sock, char *buffer,
    size_t buffer_size
)
{
    // Expects that the first received segment contains the entire request.

    #define BAD_REQUEST(WHY, ...)                                              \
        do {                                                                   \
            ERROR("400 Bad Request (" WHY ")", ##__VA_ARGS__);                 \
            _respond_with_400(sock);                                           \
            close(sock);                                                       \
            return;                                                            \
        } while (0)

    if (buffer_size < sizeof ("XXX / HTTP/X.X\n"))
        BAD_REQUEST("Not enough received data for the HTTP header");

    //
    // Extracts the filename from the HTTP header
    //

    size_t      get_len         = sizeof ("GET /") - sizeof ('\0');

    if (strncmp(buffer, "GET /", get_len) != 0)
        BAD_REQUEST("Not a GET request");

    const char  *path_begin     = buffer + get_len;
    const char  *path_end       = strchr(path_begin, ' ');

    const char  *http11_begin   = path_end + 1;
    size_t      http11_len      = sizeof ("HTTP/1.1") - sizeof ('\0');
    const char  *http11_end     = http11_begin + http11_len;


    if (strncmp(http11_begin, "HTTP/1.1", http11_len) != 0)
        BAD_REQUEST("Not HTTP 1.1");

    if (http11_end[0] != '\n' && http11_end[0] != '\r')
        BAD_REQUEST("Invalid header");

    size_t  path_len    = (intptr_t) path_end - (intptr_t) path_begin;
    char    *path       = (char *) alloca(path_len);

    strncpy(path, path_begin, path_len);
    path[path_len] = '\0';

    //
    // Responds to the request.
    //

    auto file_it = files->find({ path });

    if (file_it != files->end()) {
        DEBUG("200 OK - \"%s\"", path);
        _respond_with_200(sock, &file_it->second);
    } else {
        ERROR("404 Not Found - \"%s\"", path);
        _respond_with_404(sock);
    }

    close(sock);

    #undef BAD_REQUEST
}

void _respond_with_200(int sock, const file_t *file)
{
    constexpr char header[]     = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/html\r\n"
                                  "Content-Length: %10zu\r\n"
                                  "\r\n";

    constexpr size_t header_len =   sizeof (header) - sizeof ('\0')
                                  - sizeof ("%10zu")
                                  + sizeof ("4294967295");

    // Generates the header.
    char header_buffer[header_len + 1];
    snprintf(header_buffer, sizeof (header_buffer), header, file->content_len);

    #ifdef PRELOAD_FILE_CONTENT
        // Sends the header and the file.
        struct iovec iovs[2] = { 0 };

        iovs[0].iov_base = header_buffer;
        iovs[0].iov_len  = header_len;

        iovs[1].iov_base = file->content;
        iovs[1].iov_len  = file->content_len;

        writev(sock, iovs, 2);
    #else
        send(sock, header_buffer, header_len, 0);
        sendfile(sock, file->file_desc, nullptr, file->content_len);
    #endif /* PRELOAD_FILE_CONTENT */
}

#define RESPOND_WITH_CONTENT(CONTENT)                                          \
    do {                                                                       \
        constexpr char status[] = CONTENT;                                     \
        send(sock, status, sizeof (status) - 1, 0);                            \
    } while (0);

void _respond_with_400(int sock)
{
    RESPOND_WITH_CONTENT("HTTP/1.1 400 Bad Request\r\n\r\n");
}

void _respond_with_404(int sock)
{
    RESPOND_WITH_CONTENT("HTTP/1.1 404 Not Found\r\n\r\n");
}

#undef RESPOND_WITH_CONTENT

#undef COLOR
#undef DEBUG
#undef DIE
