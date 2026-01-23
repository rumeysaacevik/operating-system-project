// procx.c - ProcX Advanced Process Management System
// Build: gcc -O2 -Wall -Wextra -pthread -lrt -o procx procx.c
// NOTE: On many modern distros, -lrt is not required for mqueue, but kept for portability.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_NAME "/procx_shm"
#define SEM_NAME "/procx_sem"
#define MQ_NAME  "/procx_mq"

#define MAX_PROCESSES 50
#define CMD_LEN 256
#define MAX_ARGS 64

typedef enum {
    MODE_ATTACHED = 0,
    MODE_DETACHED = 1
} ProcessMode;

typedef enum {
    STATUS_RUNNING = 0,
    STATUS_TERMINATED = 1
} ProcessStatus;

// Process information
typedef struct {
    pid_t pid;                     // Process ID
    pid_t owner_pid;               // PID of the owner
    char command[CMD_LEN];         // Command to execute
    ProcessMode mode;              // Attached (0) or Detached (1)
    ProcessStatus status;          // Running (0) or Terminated (1)
    time_t start_time;             // Starting time
    int is_active;                 // Active (1) or not Active (0)
} ProcessInfo;

// Shared memory structure
typedef struct {
    ProcessInfo processes[MAX_PROCESSES];
    int process_count;             // Number of active processes
} SharedData;

// Message structure (fits POSIX mqueue payload)
typedef struct {
    long msg_type;                 // kept for compatibility with spec (not used)
    int command;                   // START(1)/TERMINATE(2)
    pid_t sender_pid;              // PID of the sender
    pid_t target_pid;              // PID of the target
} Message;

enum {
    IPC_CMD_START = 1,
    IPC_CMD_TERMINATE = 2
};

static volatile sig_atomic_t g_running = 1;
static pid_t g_self_pid = 0;

static int g_shm_fd = -1;
static SharedData *g_data = NULL;
static sem_t *g_sem = SEM_FAILED;
static mqd_t g_mq = (mqd_t)-1;

static pthread_t g_monitor_thr;
static pthread_t g_ipc_thr;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

static void sem_lock(void) {
    while (sem_wait(g_sem) == -1) {
        if (errno == EINTR) continue;
        die("[ERROR] sem_wait failed: %s", strerror(errno));
    }
}

static void sem_unlock(void) {
    if (sem_post(g_sem) == -1) {
        die("[ERROR] sem_post failed: %s", strerror(errno));
    }
}

static void ipc_send(int cmd, pid_t target) {
    if (g_mq == (mqd_t)-1) return;

    Message m;
    memset(&m, 0, sizeof(m));
    m.msg_type = 1;
    m.command = cmd;
    m.sender_pid = g_self_pid;
    m.target_pid = target;

    // mq_send can be interrupted by signals
    while (mq_send(g_mq, (const char *)&m, sizeof(m), 0) == -1) {
        if (errno == EINTR) continue;
        // If queue is full, we can drop to avoid blocking UI
        if (errno == EAGAIN) return;
        // other errors: just return (don’t crash whole program)
        return;
    }
}

static int shm_init(void) {
    bool created = false;

    // Try create exclusively first
    g_shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (g_shm_fd >= 0) {
        created = true;
    } else {
        if (errno != EEXIST) {
            fprintf(stderr, "[ERROR] shm_open create failed: %s\n", strerror(errno));
            return -1;
        }
        g_shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0666);
        if (g_shm_fd < 0) {
            fprintf(stderr, "[ERROR] shm_open open failed: %s\n", strerror(errno));
            return -1;
        }
    }

    // Ensure correct size
    if (ftruncate(g_shm_fd, (off_t)sizeof(SharedData)) == -1) {
        fprintf(stderr, "[ERROR] ftruncate failed: %s\n", strerror(errno));
        return -1;
    }

    void *p = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[ERROR] mmap failed: %s\n", strerror(errno));
        return -1;
    }
    g_data = (SharedData *)p;

    if (created) {
        // first creator initializes memory
        sem_lock();
        memset(g_data, 0, sizeof(*g_data));
        sem_unlock();
    }

    return 0;
}

static int sem_init_named(void) {
    g_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (g_sem == SEM_FAILED) {
        fprintf(stderr, "[ERROR] sem_open failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int mq_init_named(void) {
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_flags = 0;                 // blocking by default
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);

    g_mq = mq_open(MQ_NAME, O_RDWR | O_CREAT, 0666, &attr);
    if (g_mq == (mqd_t)-1) {
        fprintf(stderr, "[ERROR] mq_open failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int parse_command(char *cmdline, char *argv[], int max_args) {
    int argc = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(cmdline, " \t", &saveptr);
    while (tok && argc < max_args - 1) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    argv[argc] = NULL;
    return argc;
}

static int add_process_to_shared(pid_t pid, const char *command, ProcessMode mode) {
    int slot = -1;

    sem_lock();
    // Find free slot
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!g_data->processes[i].is_active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        sem_unlock();
        fprintf(stderr, "[ERROR] Process list full (max %d)\n", MAX_PROCESSES);
        return -1;
    }

    ProcessInfo *pi = &g_data->processes[slot];
    memset(pi, 0, sizeof(*pi));
    pi->pid = pid;
    pi->owner_pid = g_self_pid;
    strncpy(pi->command, command, CMD_LEN - 1);
    pi->mode = mode;
    pi->status = STATUS_RUNNING;
    pi->start_time = time(NULL);
    pi->is_active = 1;

    g_data->process_count += 1;
    sem_unlock();

    return 0;
}

static void mark_process_terminated(pid_t pid) {
    sem_lock();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *pi = &g_data->processes[i];
        if (pi->is_active && pi->pid == pid) {
            pi->status = STATUS_TERMINATED; // requested/known terminated
            break;
        }
    }
    sem_unlock();
}

static int remove_process_from_shared(pid_t pid) {
    int removed = 0;
    sem_lock();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *pi = &g_data->processes[i];
        if (pi->is_active && pi->pid == pid) {
            pi->is_active = 0;
            pi->status = STATUS_TERMINATED;
            if (g_data->process_count > 0) g_data->process_count -= 1;
            removed = 1;
            break;
        }
    }
    sem_unlock();
    return removed;
}

static void list_processes(void) {
    time_t now = time(NULL);

    sem_lock();
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                        RUNNING PROGRAMS                       ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ PID     │ Command                      │ Mode      │ Owner   │ Time  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");

    int total = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *pi = &g_data->processes[i];
        if (!pi->is_active) continue;

        // We show only “active” ones; status might be TERMINATED briefly until monitor cleans
        const char *mode_s = (pi->mode == MODE_DETACHED) ? "Detached " : "Attached ";
        long secs = (long)difftime(now, pi->start_time);
        if (secs < 0) secs = 0;

        // Print with fixed widths (best-effort)
        printf("║ %-7d│ %-28.28s │ %-9s│ %-7d│ %5lds║\n",
               (int)pi->pid, pi->command, mode_s, (int)pi->owner_pid, secs);
        total++;
    }

    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Total: %-53d║\n", total);
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    sem_unlock();
}

static void child_redirect_devnull(void) {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

static int start_process(const char *command, ProcessMode mode) {
    // Copy for child parsing (we must not modify original command for shared memory)
    char cmd_copy[CMD_LEN];
    memset(cmd_copy, 0, sizeof(cmd_copy));
    strncpy(cmd_copy, command, CMD_LEN - 1);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[ERROR] fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child
        // Attached mode: ensure child gets SIGHUP if parent dies unexpectedly (Linux-specific)
        if (mode == MODE_ATTACHED) {
            prctl(PR_SET_PDEATHSIG, SIGHUP);
        }

        if (mode == MODE_DETACHED) {
            if (setsid() == -1) {
                _exit(127);
            }
            child_redirect_devnull();
        }

        // Parse args for execvp
        char *argv[MAX_ARGS];
        int argc = parse_command(cmd_copy, argv, MAX_ARGS);
        if (argc <= 0) _exit(127);

        execvp(argv[0], argv);
        // exec failed
        _exit(127);
    }

    // Parent
    if (add_process_to_shared(pid, command, mode) == -1) {
        // If cannot register, best effort terminate child to avoid orphan
        kill(pid, SIGTERM);
        return -1;
    }

    ipc_send(IPC_CMD_START, pid);
    printf("[SUCCESS] Process started: PID %d\n", (int)pid);
    return 0;
}

static void request_terminate(pid_t pid) {
    if (kill(pid, SIGTERM) == -1) {
        fprintf(stderr, "[ERROR] kill(%d, SIGTERM) failed: %s\n", (int)pid, strerror(errno));
        return;
    }
    printf("[INFO] Signal SIGTERM sent to Process %d\n", (int)pid);
    // Mark in shared (monitor will remove when confirmed dead)
    mark_process_terminated(pid);
}

static void *monitor_thread_fn(void *arg) {
    (void)arg;

    while (g_running) {
        // Sleep in small chunks so we can exit faster if needed
        for (int i = 0; i < 20 && g_running; i++) usleep(100000); // 2s total

        if (!g_running) break;

        pid_t terminated_pids[MAX_PROCESSES];
        int term_count = 0;

        sem_lock();
        for (int i = 0; i < MAX_PROCESSES; i++) {
            ProcessInfo *pi = &g_data->processes[i];
            if (!pi->is_active) continue;

            bool is_dead = false;

            if (pi->owner_pid == g_self_pid) {
                int st = 0;
                pid_t r = waitpid(pi->pid, &st, WNOHANG);
                if (r == pi->pid) {
                    is_dead = true;
                } else if (r == 0) {
                    is_dead = false;
                } else {
                    // r == -1
                    if (errno == ECHILD) {
                        // already reaped by SIGCHLD handler or not a child anymore
                        if (kill(pi->pid, 0) == -1 && errno == ESRCH) is_dead = true;
                    }
                }
            } else {
                // Not our child: check existence
                if (kill(pi->pid, 0) == -1 && errno == ESRCH) is_dead = true;
            }

            if (is_dead) {
                terminated_pids[term_count++] = pi->pid;
                pi->is_active = 0;
                pi->status = STATUS_TERMINATED;
                if (g_data->process_count > 0) g_data->process_count -= 1;
            }
        }
        sem_unlock();

        for (int k = 0; k < term_count; k++) {
            pid_t dead = terminated_pids[k];
            printf("[MONITOR] Process %d was terminated\n", (int)dead);
            fflush(stdout);
            ipc_send(IPC_CMD_TERMINATE, dead);
        }
    }

    return NULL;
}

static void *ipc_listener_thread_fn(void *arg) {
    (void)arg;

    // Use timed receive so we can exit when g_running becomes 0
    struct mq_attr a;
    if (mq_getattr(g_mq, &a) == -1) {
        // fallback
        a.mq_msgsize = sizeof(Message);
    }

    char *buf = calloc(1, (size_t)a.mq_msgsize);
    if (!buf) return NULL;

    while (g_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; // 1 second timeout

        ssize_t n = mq_timedreceive(g_mq, buf, (size_t)a.mq_msgsize, NULL, &ts);
        if (n < 0) {
            if (errno == ETIMEDOUT) continue;
            if (errno == EINTR) continue;
            // other errors: keep looping (don’t crash)
            continue;
        }

        if ((size_t)n < sizeof(Message)) continue;
        Message m;
        memcpy(&m, buf, sizeof(m));

        if (m.sender_pid == g_self_pid) continue;

        if (m.command == IPC_CMD_START) {
            printf("[IPC] New process started: PID %d\n", (int)m.target_pid);
            fflush(stdout);
        } else if (m.command == IPC_CMD_TERMINATE) {
            printf("[IPC] Process terminated: PID %d\n", (int)m.target_pid);
            fflush(stdout);
        }
    }

    free(buf);
    return NULL;
}

static void cleanup_owned_attached(void) {
    // Kill any attached processes started by this instance
    pid_t to_kill[MAX_PROCESSES];
    int cnt = 0;

    sem_lock();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        ProcessInfo *pi = &g_data->processes[i];
        if (!pi->is_active) continue;
        if (pi->owner_pid == g_self_pid && pi->mode == MODE_ATTACHED) {
            to_kill[cnt++] = pi->pid;
            // remove from list now
            pi->is_active = 0;
            pi->status = STATUS_TERMINATED;
            if (g_data->process_count > 0) g_data->process_count -= 1;
        }
    }
    sem_unlock();

    for (int i = 0; i < cnt; i++) {
        kill(to_kill[i], SIGTERM);
        // Best-effort notify others
        ipc_send(IPC_CMD_TERMINATE, to_kill[i]);
    }
}

static void print_menu(void) {
    printf("╔════════════════════════════════════╗\n");
    printf("║            ProcX v1.0              ║\n");
    printf("╠════════════════════════════════════╣\n");
    printf("║ 1. Run a new program               ║\n");
    printf("║ 2. List running programs           ║\n");
    printf("║ 3. Terminate a program             ║\n");
    printf("║ 0. Exit                            ║\n");
    printf("╚════════════════════════════════════╝\n");
}

static void handle_stop_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_sigchld(int sig) {
    (void)sig;
    // reap all children to avoid zombies
    while (1) {
        int st;
        pid_t r = waitpid(-1, &st, WNOHANG);
        if (r <= 0) break;
    }
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    struct sigaction sc;
    memset(&sc, 0, sizeof(sc));
    sc.sa_handler = handle_sigchld;
    sigemptyset(&sc.sa_mask);
    sc.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sc, NULL);
}

static void close_ipc(void) {
    if (g_data) {
        munmap(g_data, sizeof(SharedData));
        g_data = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    if (g_sem != SEM_FAILED) {
        sem_close(g_sem);
        g_sem = SEM_FAILED;
    }
    if (g_mq != (mqd_t)-1) {
        mq_close(g_mq);
        g_mq = (mqd_t)-1;
    }

    // IMPORTANT:
    // We do NOT unlink shared objects by default (multi-instance friendly).
    // If you really want to unlink when everything is done, you can do it manually:
    //   shm_unlink(SHM_NAME); sem_unlink(SEM_NAME); mq_unlink(MQ_NAME);
}

int main(void) {
    g_self_pid = getpid();
    install_signals();

    if (sem_init_named() == -1) {
        die("Failed to init semaphore");
    }
    if (shm_init() == -1) {
        die("Failed to init shared memory");
    }
    if (mq_init_named() == -1) {
        die("Failed to init message queue");
    }

    if (pthread_create(&g_monitor_thr, NULL, monitor_thread_fn, NULL) != 0) {
        die("[ERROR] pthread_create monitor failed");
    }
    if (pthread_create(&g_ipc_thr, NULL, ipc_listener_thread_fn, NULL) != 0) {
        die("[ERROR] pthread_create ipc failed");
    }

    char line[512];

    while (g_running) {
        print_menu();
        printf("Your choice: ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            // EOF (terminal closed) -> exit gracefully
            g_running = 0;
            break;
        }
        trim_newline(line);

        int choice = -1;
        if (line[0] != '\0') choice = atoi(line);

        if (choice == 1) {
            char cmd[CMD_LEN];
            memset(cmd, 0, sizeof(cmd));

            printf("Enter the command to run: ");
            fflush(stdout);
            if (!fgets(cmd, sizeof(cmd), stdin)) {
                g_running = 0;
                break;
            }
            trim_newline(cmd);
            if (cmd[0] == '\0') {
                printf("[ERROR] Empty command.\n");
                continue;
            }

            printf("Choose running mode (0: Attached, 1: Detached): ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) {
                g_running = 0;
                break;
            }
            trim_newline(line);
            int m = atoi(line);
            ProcessMode mode = (m == 1) ? MODE_DETACHED : MODE_ATTACHED;

            start_process(cmd, mode);

        } else if (choice == 2) {
            list_processes();

        } else if (choice == 3) {
            printf("Process PID to terminate: ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) {
                g_running = 0;
                break;
            }
            trim_newline(line);
            pid_t pid = (pid_t)atoi(line);
            if (pid <= 0) {
                printf("[ERROR] Invalid PID.\n");
                continue;
            }
            request_terminate(pid);

        } else if (choice == 0) {
            g_running = 0;
            break;

        } else {
            printf("[ERROR] Invalid choice.\n");
        }
    }

    // Stop threads
    g_running = 0;
    pthread_join(g_monitor_thr, NULL);
    pthread_join(g_ipc_thr, NULL);

    // Cleanup rules: attached processes of THIS instance must die, detached continue
    cleanup_owned_attached();

    close_ipc();
    return 0;
}
