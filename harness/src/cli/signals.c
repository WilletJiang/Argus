#include "cli.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t signal_fd = -1;
static volatile sig_atomic_t shutdown_fd = -1;
static int read_fd = -1;
static int shutdown_read_fd = -1;
static pthread_t watcher;
static ah_engine *active_engine;
static struct sigaction previous_int, previous_term;

static void notify_signal(int fd, char number) {
    if (fd < 0)
        return;
    ssize_t written;
    do {
        written = write(fd, &number, 1);
    } while (written < 0 && errno == EINTR);
    /* EAGAIN means the pipe already contains a pending wakeup. */
}
static void on_signal(int number) {
    int saved = errno;
    char c = (char)number;
    if (number == SIGTERM)
        notify_signal(shutdown_fd, c);
    notify_signal(signal_fd, c);
    errno = saved;
}
static void *watch_signals(void *context) {
    (void)context;
    char c;
    for (;;) {
        ssize_t n = read(read_fd, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n != 1)
            break;
        ah_cancel(active_engine);
    }
    return NULL;
}
static bool make_pipe(int fds[2], bool nonblocking_reader) {
    if (pipe(fds) != 0)
        return false;
    for (size_t i = 0; i < 2; ++i) {
        if (fds[i] >= 3)
            continue;
        int moved = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
        if (moved < 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
        close(fds[i]);
        fds[i] = moved;
    }
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0 ||
        (nonblocking_reader && fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0)) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    return true;
}
bool cli_signals_start(ah_engine *engine) {
    int fds[2], shutdown[2];
    if (!make_pipe(fds, false))
        return false;
    if (!make_pipe(shutdown, true)) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    read_fd = fds[0];
    signal_fd = fds[1];
    shutdown_read_fd = shutdown[0];
    shutdown_fd = shutdown[1];
    active_engine = engine;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_int) != 0) {
        goto failed;
    }
    if (sigaction(SIGTERM, &action, &previous_term) != 0) {
        sigaction(SIGINT, &previous_int, NULL);
        goto failed;
    }
    if (pthread_create(&watcher, NULL, watch_signals, NULL) != 0) {
        sigaction(SIGINT, &previous_int, NULL);
        sigaction(SIGTERM, &previous_term, NULL);
        goto failed;
    }
    return true;
failed:
    signal_fd = -1;
    shutdown_fd = -1;
    close(fds[0]);
    close(fds[1]);
    close(shutdown[0]);
    close(shutdown[1]);
    read_fd = -1;
    shutdown_read_fd = -1;
    active_engine = NULL;
    return false;
}
int cli_shutdown_fd(void) {
    return shutdown_read_fd;
}
void cli_signals_stop(void) {
    int fd = signal_fd, shutdown = shutdown_fd;
    signal_fd = -1;
    shutdown_fd = -1;
    sigaction(SIGINT, &previous_int, NULL);
    sigaction(SIGTERM, &previous_term, NULL);
    close(fd);
    pthread_join(watcher, NULL);
    close(read_fd);
    close(shutdown);
    close(shutdown_read_fd);
    read_fd = -1;
    shutdown_read_fd = -1;
    active_engine = NULL;
}
