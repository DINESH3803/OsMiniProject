// done
/*
 * server/server.c
 * Main ICU server: initialises state, loads/creates default data,
 * binds TCP socket, accepts connections, spawns detached threads.
 * OS Concepts: pthread, mutex, semaphore, POSIX mqueue, signals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mqueue.h>
#include "server.h"

/* ─── Global state (declared extern everywhere else) ─────────── */
ServerState g_state;

/* ─── Logging ───────────────────────────────────────────────── */
void server_log(const char* fmt, ...) {
    pthread_mutex_lock(&g_state.log_mutex);
    char ts[32]; time_t now = time(NULL); timestamp_str(ts, sizeof(ts), now);
    if (g_state.logfile) {
        va_list ap; va_start(ap, fmt);
        fprintf(g_state.logfile, "[%s] ", ts);
        vfprintf(g_state.logfile, fmt, ap);
        fprintf(g_state.logfile, "\n");
        fflush(g_state.logfile);
        va_end(ap);
    }

    va_list ap2; va_start(ap2, fmt);
    printf(DIM "[%s] " RESET, ts);
    vprintf(fmt, ap2);
    printf("\n");
    va_end(ap2);
    pthread_mutex_unlock(&g_state.log_mutex);
}
// custom logging func that writes messeges to both terminal and log file and uses synchornization
// holding the lock(mutex)


/* ─── Signal handler ────────────────────────────────────────── */
static void handle_sigint(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
    g_state.running = 0;
    shutdown(g_state.server_fd, SHUT_RDWR);
}
// catches ctrl+c and shuts down the server


/* ─── Seed default users & patients if DB is empty ──────────── */
// kind of a bootstrapping func to initially load dummy patients(if empty)
static void init_defaults(void) {
    if (g_state.user_count == 0) {
        struct { const char* u; const char* p; Role r; } udefs[] = {
            {"admin",   "admin123",  ROLE_ADMIN  },
            {"drsmith", "doc123",    ROLE_DOCTOR },
            {"drpatel", "doc456",    ROLE_DOCTOR },
            {"nurse1",  "nurse123",  ROLE_NURSE  },
            {"nurse2",  "nurse456",  ROLE_NURSE  },
            {"guest",   "guest",     ROLE_GUEST  },
        };
        for (int i = 0; i < 6; i++)
            add_user(udefs[i].u, udefs[i].p, udefs[i].r);
        server_log("Default users created");
    }
    // creates default users and logs them.

    if (g_state.patient_count == 0) {
        /* Find doctor IDs */
        int doc1 = -1, doc2 = -1;
        pthread_mutex_lock(&g_state.user_mutex);
        for (int i = 0; i < g_state.user_count; i++) {
            if (g_state.users[i].role == ROLE_DOCTOR && g_state.users[i].active) {
                if (doc1 < 0) doc1 = g_state.users[i].user_id;
                else if (doc2 < 0) doc2 = g_state.users[i].user_id;
            }
        }
        
        /* Find both nurse IDs */
        int nurse1 = -1, nurse2 = -1;
        for (int i = 0; i < g_state.user_count; i++) {
            if (g_state.users[i].role == ROLE_NURSE && g_state.users[i].active) {
                if (nurse1 < 0) nurse1 = g_state.users[i].user_id;
                else if (nurse2 < 0) nurse2 = g_state.users[i].user_id;
            }
        }
        pthread_mutex_unlock(&g_state.user_mutex);

        /* 6 patients: first 3 → nurse1, last 3 → nurse2 */
        struct { const char* name; int age; int doc; const char* cond; int nurse; } pdefs[] = {
            {"John Doe",       65, doc1, "Cardiac Arrhythmia",   nurse1},
            {"Jane Smith",     72, doc1, "Respiratory Failure",  nurse1},
            {"Bob Johnson",    55, doc1, "Hypertensive Crisis",  nurse1},
            {"Alice Williams", 48, doc2, "Post-Cardiac Surgery", nurse2},
            {"Carlos Reyes",   61, doc2, "Septic Shock",         nurse2},
            {"Maria Garcia",   39, doc2, "Acute Kidney Injury",  nurse2},
        };
        pthread_mutex_lock(&g_state.patient_mutex);
        int base_id = 101;
        for (int i = 0; i < 6; i++) {
            Patient* p = &g_state.patients[g_state.patient_count++];
            memset(p, 0, sizeof(Patient));
            p->patient_id = base_id + i;
            strncpy(p->name, pdefs[i].name, sizeof(p->name)-1);
            p->age = pdefs[i].age;
            strncpy(p->condition, pdefs[i].cond, sizeof(p->condition)-1);
            p->assigned_doctor_id = pdefs[i].doc;
            if (pdefs[i].nurse > 0) {
                p->assigned_nurses[0] = pdefs[i].nurse;
                p->nurse_count = 1;
            }
            p->active = 1;
            p->admitted_at = time(NULL);
            save_patient(p);
        }
        pthread_mutex_unlock(&g_state.patient_mutex);
        server_log("Default patients created (6 patients, 3 per nurse)");
    }
}

/* ─── Main ──────────────────────────────────────────────────── */
int main(void) {
    /* Create required directories */
    mkdir(DATA_DIR,    0755);
    mkdir(PATIENT_DIR, 0755);
    mkdir("logs",      0755);

    /* Print banner */
    printf("\n");
    print_banner("  SMART ICU MONITORING & ALERT SYSTEM v1.0  ", BRIGHT_CYAN);
    print_banner("         Server — OS Mini Project            ", CYAN);
    printf("\n");

    /* Initialise global state */
    memset(&g_state, 0, sizeof(g_state));
    g_state.running = 1;
    pthread_mutex_init(&g_state.patient_mutex, NULL);
    pthread_mutex_init(&g_state.user_mutex,    NULL);
    pthread_mutex_init(&g_state.log_mutex,     NULL);
    sem_init(&g_state.write_sem, 0, 3);   /* max 3 concurrent disk writes */

    /* Open log file */
    g_state.logfile = fopen(LOG_FILE, "a");
    if (!g_state.logfile) { perror("fopen log"); return 1; }

    server_log("=== Server starting ===");

    /* Load persisted data */
    load_users();
    load_patients();
    init_defaults();

    /* POSIX message queue */
    //uses mq_open() to create a posix mq and doctor_pipes() to create and prepare the named pipes
    struct mq_attr mqa = { .mq_flags=0, .mq_maxmsg=10,
                           .mq_msgsize=sizeof(Alert), .mq_curmsgs=0 };
    mq_unlink(MQUEUE_NAME);                         /* clean stale queue */
    g_state.mqueue = mq_open(MQUEUE_NAME, O_CREAT|O_RDWR|O_NONBLOCK, 0644, &mqa);
    if (g_state.mqueue == (mqd_t)-1)
        server_log("WARNING: mq_open failed (%s) — mqueue disabled", strerror(errno));
    else
        server_log("POSIX message queue opened: %s", MQUEUE_NAME);

    /* Named pipes for doctors */
    create_doctor_pipes();


    /* Signal handlers */
    signal(SIGINT,  handle_sigint);
    signal(SIGPIPE, SIG_IGN);       /* ignore broken-pipe from dead clients */

    /* TCP socket */
    g_state.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_state.server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(g_state.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // creates the master TCP socket and sets the so_reuseadr option to prevent addr "already in use" errors 
    

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(g_state.server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        { perror("bind"); return 1; }
    if (listen(g_state.server_fd, 10) < 0)
        { perror("listen"); return 1; }
    // and bind()s it to the port 8888 and listens.

    printf(GREEN BOLD "\n[SERVER] Listening on port %d  (Ctrl+C to stop)\n\n" RESET,
           SERVER_PORT);
    server_log("Listening on port %d", SERVER_PORT);

    /* ── Accept loop ─────────────────────────────────────────── */
    // Enters a while (g_state.running) loop. It waits at accept() until a client connects.
    while (g_state.running) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(g_state.server_fd, (struct sockaddr*)&cli, &cli_len);
        if (cfd < 0) {
            if (g_state.running) perror("accept");
            break;
        }
        server_log("Connection from %s:%d", inet_ntoa(cli.sin_addr),
                   ntohs(cli.sin_port));

        ClientArg* ca = malloc(sizeof(ClientArg));
        ca->fd = cfd; ca->addr = cli;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED); // gets the resourses as soon as the thread finishes its exec
        pthread_create(&tid, &attr, client_thread, ca);
        pthread_attr_destroy(&attr);
    }
    //  As soon as a client connects, it calls 
    // pthread_create() to spawn a brand new, detached thread pointing to client_thread() (which lives in handler.c). The loop immediately 
    // goes back to waiting for the next client.

    /* ── Shutdown ─────────────────────────────────────────────── */
    // cleans socket, mutexes, semaphores, mq and log file
    server_log("=== Server shutting down ===");
    if (g_state.mqueue != (mqd_t)-1) {
        mq_close(g_state.mqueue);
        mq_unlink(MQUEUE_NAME);
    }
    close(g_state.server_fd);
    pthread_mutex_destroy(&g_state.patient_mutex);
    pthread_mutex_destroy(&g_state.user_mutex);
    pthread_mutex_destroy(&g_state.log_mutex);
    sem_destroy(&g_state.write_sem);
    fclose(g_state.logfile);
    printf(GREEN "\n[SERVER] Clean shutdown. Goodbye.\n" RESET);
    return 0;
}
