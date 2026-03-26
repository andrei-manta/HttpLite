#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT            8080
#define BACKLOG         128
#define THREAD_COUNT    8
#define QUEUE_SIZE      256
#define BUFFER_SIZE     8192

typedef struct {
    int fds[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} queue_t;

static queue_t   queue;
static pthread_t workers[THREAD_COUNT];
static volatile int running = 1;

static void queue_init(queue_t *q) {
    q->head  = 0;
    q->tail  = 0;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}

static void queue_push(queue_t *q, int fd) {
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_SIZE)
        pthread_cond_wait(&q->not_full, &q->lock);
    q->fds[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

static int queue_pop(queue_t *q) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && running)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (!running && q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return fd;
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif")  == 0) return "image/gif";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}

static void send_response(int fd, int status, const char *status_text,
                           const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    write(fd, header, hlen);
    if (body && body_len > 0)
        write(fd, body, body_len);
}

static void handle_request(int fd) {
    char buf[BUFFER_SIZE];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    char method[8], path[256], proto[16];
    if (sscanf(buf, "%7s %255s %15s", method, path, proto) != 3) {
        send_response(fd, 400, "Bad Request", "text/plain", "Bad Request", 11);
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_response(fd, 405, "Method Not Allowed", "text/plain", "Method Not Allowed", 18);
        return;
    }

    if (strstr(path, "..")) {
        send_response(fd, 403, "Forbidden", "text/plain", "Forbidden", 9);
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), ".%s", path);
    if (filepath[strlen(filepath) - 1] == '/')
        strncat(filepath, "index.html", sizeof(filepath) - strlen(filepath) - 1);

    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
        const char *body = "<html><body><h1>404 Not Found</h1></body></html>";
        send_response(fd, 404, "Not Found", "text/html", body, strlen(body));
        return;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        send_response(fd, 403, "Forbidden", "text/plain", "Forbidden", 9);
        return;
    }

    char *body = malloc(st.st_size);
    if (!body) {
        fclose(f);
        send_response(fd, 500, "Internal Server Error", "text/plain", "Internal Server Error", 21);
        return;
    }

    fread(body, 1, st.st_size, f);
    fclose(f);
    send_response(fd, 200, "OK", mime_type(filepath), body, st.st_size);
    free(body);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        int fd = queue_pop(&queue);
        if (fd < 0) break;
        handle_request(fd);
        close(fd);
    }
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    queue_init(&queue);

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_create(&workers[i], NULL, worker, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    printf("Listening on port %d with %d worker threads\n", PORT, THREAD_COUNT);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        queue_push(&queue, client_fd);
    }

    close(server_fd);

    pthread_mutex_lock(&queue.lock);
    pthread_cond_broadcast(&queue.not_empty);
    pthread_mutex_unlock(&queue.lock);

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_join(workers[i], NULL);

    printf("\nShutdown complete.\n");
    return 0;
}
