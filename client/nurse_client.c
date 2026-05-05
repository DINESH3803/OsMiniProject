/*
 * client/nurse_client.c
 * Connects to ICU server as a Nurse.
 * Streams simulated vitals every 3 s continuously.
 *
 * Press Enter at any time to pause streaming and open the
 * in-session sub-menu (resume / switch patient / quit) —
 * the TCP connection stays alive throughout.
 *
 * Usage: ./nurse_client
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/common.h"
#include "../include/vitals.h"

/* ─── Shared state between streaming loop & input watcher ───── */
static volatile int running = 1;   /* overall kill switch (Ctrl+C) */
static volatile int paused  = 0;   /* 1 = user hit Enter → show menu */

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_sigint(int s){ (void)s; running = 0; }

/* ─── Random float in [lo, hi] ─────────────────────────────── */
static float randf(float lo, float hi){
    return lo + (hi - lo) * ((float)rand() / RAND_MAX);
}

/* ─── Simulated vitals: 85% normal, 15% one-vital anomaly ───── */
static Vitals make_vitals(int patient_id, int nurse_id) {
    Vitals v = {0};
    v.patient_id = patient_id;
    v.nurse_id   = nurse_id;
    v.timestamp  = time(NULL);

    int anomaly = (rand() % 100) < 15;

    /* Start with normal baseline */
    v.heart_rate   = randf(62, 98);
    v.systolic_bp  = randf(92, 138);
    v.diastolic_bp = randf(62, 88);
    v.spo2         = randf(96, 100);
    v.temperature  = randf(36.2f, 37.1f);
    v.resp_rate    = randf(12, 19);

    if (anomaly) {
        switch (rand() % 6) {
            case 0: v.heart_rate   = (rand()%2) ? randf(35,48)    : randf(115,145); break;
            case 1: v.systolic_bp  = (rand()%2) ? randf(60,82)    : randf(150,190); break;
            case 2: v.diastolic_bp = randf(92, 110); break;
            case 3: v.spo2         = randf(85, 93);  break;
            case 4: v.temperature  = (rand()%2) ? randf(33,34.8f) : randf(38.0f,40.5f); break;
            case 5: v.resp_rate    = (rand()%2) ? randf(5,9)      : randf(24,35);  break;
        }
    }
    return v;
}

static void show_vitals(const Vitals* v) {
    char ts[32]; timestamp_str(ts, sizeof(ts), v->timestamp);
    printf("  [%s]  " CYAN "HR:" RESET " %.1f  "
           CYAN "BP:" RESET " %.0f/%.0f  "
           CYAN "SpO2:" RESET " %.1f%%  "
           CYAN "Temp:" RESET " %.1f°C  "
           CYAN "RR:" RESET " %.0f\n",
           ts,
           v->heart_rate, v->systolic_bp, v->diastolic_bp,
           v->spo2, v->temperature, v->resp_rate);
}

/* ─── Input-watcher thread ──────────────────────────────────── *
 * Polls stdin in raw non-blocking mode for an Enter keypress.
 * When detected, sets paused=1 so the streaming loop stops and
 * the main thread shows the pause sub-menu.
 *
 * CRITICAL: skips read() while paused==1 so it does not steal
 * keystrokes from the menu's scanf() — see race condition notes.
 * ─────────────────────────────────────────────────────────────*/
static void* input_watcher(void* arg) {
    (void)arg;

    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    raw.c_lflag    &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;   /* non-blocking */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    while (running) {
        pthread_mutex_lock(&state_lock);
        int is_paused = paused;
        pthread_mutex_unlock(&state_lock);

        if (!is_paused) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1 && (c == '\n' || c == '\r')) {
                pthread_mutex_lock(&state_lock);
                paused = 1;
                pthread_mutex_unlock(&state_lock);
            }
        }
        usleep(50000); /* poll every 50 ms */
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    return NULL;
}

/* ─── Terminal helpers ──────────────────────────────────────── */
static void term_canonical(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void term_raw(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag    &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

/* ─── In-session pause sub-menu ─────────────────────────────── *
 * Returns: 1 → resume streaming (possibly different patient)
 *          0 → quit requested
 * ─────────────────────────────────────────────────────────────*/
static int show_pause_menu(int fd, int* patient_id)
{
    term_canonical();
    char buf[BUFFER_SIZE];
    int pids[MAX_PATIENTS];
    char pnames[MAX_PATIENTS][64];

    while (1) {
        int pcount = 0;
        send_line(fd, "LIST_PATIENTS");
        recv_line(fd, buf, sizeof(buf));
        sscanf(buf, "PATIENTS %d", &pcount);
        int pi = 0;
        while (recv_line(fd, buf, sizeof(buf)) > 0) {
            if (strcmp(buf, ".") == 0) break;
            sscanf(buf, "%d|%63[^\n]", &pids[pi], pnames[pi]);
            pi++;
        }
        pcount = pi;

        printf("\n");
        print_separator();
        printf(BOLD BRIGHT_YELLOW "  ⏸  STREAMING PAUSED\n" RESET);
        print_separator();
        printf(BOLD "  Current patient: #%d", *patient_id);
        for (int i = 0; i < pcount; i++)
            if (pids[i] == *patient_id) { printf(" — %s", pnames[i]); break; }
        printf("\n\n" RESET);

        printf("  [1] Resume streaming\n");
        printf("  [2] Switch patient\n");
        printf("  [3] Quit\n");
        printf("\nChoice: ");        int ch = 0;
        int res = scanf("%d", &ch);
        if (res == EOF) return 0; /* Quit */
        if (res != 1) ch = 3;

        switch (ch) {
            case 1:
                printf(GREEN "▶ Resuming…\n" RESET);
                term_raw();
                return 1;

            case 2: {
                if (pcount <= 1) {
                    if (pcount == 1) printf(YELLOW "Only one patient assigned — nothing to switch.\n" RESET);
                    else printf(YELLOW "No patients currently assigned to you.\n" RESET);
                    break;
                }
                printf("\nAssigned patients:\n");
                for (int i = 0; i < pcount; i++)
                    printf("  [%d] Patient #%d — %s%s\n",
                           i+1, pids[i], pnames[i],
                           pids[i] == *patient_id ? BRIGHT_CYAN " (current)" RESET : "");
                printf("Select [1-%d]: ", pcount); fflush(stdout);
                int sel = 0;
                if (scanf("%d", &sel) == 1 && sel >= 1 && sel <= pcount) {
                    *patient_id = pids[sel-1];
                    printf(GREEN "Switched to patient #%d — %s\n" RESET,
                           *patient_id, pnames[sel-1]);
                } else {
                    int c; while ((c = getchar()) != '\n' && c != EOF);
                    printf(YELLOW "Invalid choice.\n" RESET);
                }
                break;
            }

            case 3:
                printf(YELLOW "Quitting…\n" RESET);
                term_raw();
                return 0;

            default:
                printf(YELLOW "Invalid choice.\n" RESET);
        }
    }
}

/* ════════════════════════════════════════════════════════════ */
int nurse_main(void) {
    running = 1;
    paused  = 0;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    signal(SIGINT, on_sigint);

    print_banner("  ICU NURSE CLIENT  ", BRIGHT_GREEN);

    /* ── Connect ──────────────────────────────────────────────── */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &srv.sin_addr);

    /* ── Connect + authenticate (up to 3 attempts) ───────────── */
    char buf[BUFFER_SIZE];
    int user_id = 0;
    int authed  = 0;
    char username[32], password[64];

    for (int attempt = 1; attempt <= 3; attempt++) {
        printf(YELLOW "Connecting to %s:%d …\n" RESET, SERVER_HOST, SERVER_PORT);
        if (connect(fd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
            perror("connect"); close(fd); return 1;
        }

        recv_line(fd, buf, sizeof(buf));   /* banner */
        printf(DIM "%s\n" RESET, buf);

        printf(BOLD "Login attempt %d/3\n" RESET, attempt);
        printf("Username: "); fflush(stdout); scanf("%31s", username);
        printf("Password: "); fflush(stdout); scanf("%63s", password);

        send_line(fd, "AUTH %s %s", username, password);
        recv_line(fd, buf, sizeof(buf));

        if (strncmp(buf, "AUTH_OK NURSE", 13) == 0) {
            sscanf(buf, "AUTH_OK NURSE %d", &user_id);
            printf(BRIGHT_GREEN "✓ Authenticated as Nurse (id=%d)\n" RESET, user_id);
            authed = 1;
            break;
        }

        /* Wrong credentials — server closes socket after AUTH_FAIL */
        printf(RED "✗ Authentication failed: %s" RESET, buf);
        if (attempt < 3) {
            printf(YELLOW "  (%d attempt(s) remaining)\n" RESET, 3 - attempt);
        } else {
            printf("\n");
        }
        close(fd);
        /* Reopen socket for the next attempt */
        fd = socket(AF_INET, SOCK_STREAM, 0);
    }

    if (!authed) {
        printf(RED "Too many failed attempts. Exiting.\n" RESET);
        close(fd); return 1;
    }

    recv_line(fd, buf, sizeof(buf));   /* READY */
    printf(DIM "%s\n" RESET, buf);

    /* ── Fetch patient list ───────────────────────────────────── */
    send_line(fd, "LIST_PATIENTS");
    recv_line(fd, buf, sizeof(buf));   /* PATIENTS <N> */
    int pcount = 0; sscanf(buf, "PATIENTS %d", &pcount);
    int pids[MAX_PATIENTS]; char pnames[MAX_PATIENTS][64];
    int pi = 0;
    while (recv_line(fd, buf, sizeof(buf)) > 0) {
        if (strcmp(buf, ".") == 0) break;
        sscanf(buf, "%d|%63[^\n]", &pids[pi], pnames[pi]);
        pi++;
    }
    pcount = pi;

    if (pcount == 0) {
        printf(YELLOW "No patients assigned. Ask admin to assign you patients.\n" RESET);
        send_line(fd, "QUIT"); close(fd); return 0;
    }

    printf("\n" BOLD "Assigned patients:\n" RESET);
    for (int i = 0; i < pcount; i++)
        printf("  [%d] Patient #%d — %s\n", i+1, pids[i], pnames[i]);

    /* ── Select initial patient (re-prompt on bad input) ─────── */
    int choice = 0;
    if (pcount == 1) {
        choice = 0;
        printf("Auto-selected patient #%d (%s)\n", pids[0], pnames[0]);
    } else {
        do {
            printf("Select patient [1-%d]: ", pcount); fflush(stdout);
            if (scanf("%d", &choice) != 1) choice = 0;
            choice--;
            if (choice < 0 || choice >= pcount)
                printf(YELLOW "  Invalid choice, please enter a number between 1 and %d.\n" RESET, pcount);
        } while (choice < 0 || choice >= pcount);
    }
    int patient_id = pids[choice];
    printf(GREEN "\n[NURSE] Monitoring patient #%d — %s\n\n" RESET,
           patient_id, pnames[choice]);

    /* ── Start input-watcher thread ───────────────────────────── */
    pthread_t watcher_tid;
    pthread_create(&watcher_tid, NULL, input_watcher, NULL);

    /* ── Main streaming loop ──────────────────────────────────── */
    print_separator();
    printf(CYAN "Streaming vitals every 3 s.\n"
           DIM  "  Press " RESET BOLD "Enter" DIM " at any time to pause & open the menu.\n\n" RESET);

    while (running) {

        /* Check for pause request */
        pthread_mutex_lock(&state_lock);
        int is_paused = paused;
        pthread_mutex_unlock(&state_lock);

        if (is_paused) {
            int cont = show_pause_menu(fd, &patient_id);
            pthread_mutex_lock(&state_lock);
            paused = 0;
            pthread_mutex_unlock(&state_lock);

            if (!cont) { running = 0; break; }

            print_separator();
            printf(GREEN "[NURSE] Monitoring patient #%d\n", patient_id);
            printf(CYAN "Streaming resumed. Press Enter to pause.\n\n" RESET);
            continue;
        }

        /* Verify patient is still active and assigned before doing anything */
        send_line(fd, "CHECK_PATIENT %d", patient_id);
        recv_line(fd, buf, sizeof(buf));
        if (strncmp(buf, "OK", 2) != 0) {
            printf("\n  [!] %s\n", buf + 6);
            printf("  Opening menu — please switch patient or quit.\n\n");
            pthread_mutex_lock(&state_lock);
            paused = 1;
            pthread_mutex_unlock(&state_lock);
            continue;
        }

        /* Generate & display vitals */
        Vitals v = make_vitals(patient_id, user_id);
        show_vitals(&v);

        /* Interruptible 3-second sleep (100 ms slices) */
        for (int i = 0; i < 30 && !paused && running; i++)
            usleep(100000);

        if (!running || paused) continue;  /* skip send if paused mid-sleep */

        /* Send to server */
        send_line(fd, "VITALS %d %.1f %.1f %.1f %.1f %.1f %.1f",
                  patient_id,
                  v.heart_rate, v.systolic_bp, v.diastolic_bp,
                  v.spo2, v.temperature, v.resp_rate);

        recv_line(fd, buf, sizeof(buf));

        if (strncmp(buf, "ALERT", 5) == 0) {
            printf("  *** SERVER ALERT: %s\n", buf + 6);
        } else if (strncmp(buf, "OK", 2) == 0) {
            printf("  Recorded.\n");
        } else if (strncmp(buf, "ERROR", 5) == 0) {
            printf("\n  [!] Server error: %s\n", buf + 6);
            printf("  Opening menu — please switch patient or quit.\n\n");
            pthread_mutex_lock(&state_lock);
            paused = 1;
            pthread_mutex_unlock(&state_lock);
        } else {
            printf("  Server: %s\n", buf);
        }
    }

    /* ── Cleanup ──────────────────────────────────────────────── */
    pthread_join(watcher_tid, NULL);
    send_line(fd, "QUIT");
    recv_line(fd, buf, sizeof(buf));
    close(fd);
    term_canonical();
    printf(GREEN "\n[NURSE] Disconnected.\n" RESET);
    return 0;
}
