/*
 * client/doctor_client.c
 * Connects as a Doctor. Registers its PID with the server so it can
 * receive SIGUSR1 alerts. A background thread reads the named pipe
 * and displays alert popups. Main thread shows an interactive dashboard.
 * OS Concepts: SIGUSR1, named pipes, pthreads, IPC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/common.h"
#include "../include/vitals.h"
#include "../include/ipc.h"

static volatile int running   = 1;
static volatile int alert_flag = 0;  /* set by SIGUSR1 handler */
static int          srv_fd    = -1;
static int          doctor_id = 0;

/* ─── SIGUSR1 handler — just sets flag (async-signal-safe) ──── */
static void on_sigusr1(int s) { (void)s; alert_flag = 1; }
static void on_sigint (int s) { (void)s; running = 0; }

/* ─── Background thread: reads named pipe for alert structs ──── */
static void* pipe_reader(void* arg) 
{
    char pipe_path[128];
    alert_pipe_path(pipe_path, sizeof(pipe_path), (int)(intptr_t)arg);

    /* Open pipe in blocking read mode — will block until server writes */
    while (running) {
        int pfd = open(pipe_path, O_RDONLY);
        if (pfd < 0) { sleep(1); continue; }

        Alert a;
        ssize_t r = read(pfd, &a, sizeof(Alert));
        close(pfd);
        if (r == sizeof(Alert)) 
        {
            print_alert(&a);
            fflush(stdout);
        }
    }
    return NULL;
}

/* ─── Send a command and collect response lines ─────────────── */
static void do_my_patients(void) 
{
    char buf[BUFFER_SIZE];
    send_line(srv_fd, "MY_PATIENTS");
    recv_line(srv_fd, buf, sizeof(buf));   /* PATIENTS <N> */
    int cnt = 0; sscanf(buf, "PATIENTS %d", &cnt);
    if (cnt == 0) { printf(YELLOW "  No patients assigned.\n" RESET); 
        /* still drain the sentinel */ 
        recv_line(srv_fd, buf, sizeof(buf));
        return;
    }

    printf(BOLD "\n  %-5s  %-25s  %-5s  %s\n" RESET, "ID", "Name", "Age", "Condition");
    print_separator();
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) 
    {
        if (strcmp(buf, ".") == 0) break;  /* sentinel always consumed */
        int pid, age; char name[64], cond[128];
        sscanf(buf, "%d|%63[^|]|%d|%127[^\n]", &pid, name, &age, cond);
        printf("  %-5d  %s%-25s%s  %-5d  %s\n",
               pid, BRIGHT_CYAN, name, RESET, age, cond);
    }
}

static void do_view_patient(int pid) 
{
    char buf[BUFFER_SIZE];
    send_line(srv_fd, "GET_PATIENT %d", pid);
    recv_line(srv_fd, buf, sizeof(buf));

    if (strncmp(buf, "ERROR", 5) == 0) 
    {
        printf(RED "  %s\n" RESET, buf); return;
    }
    /* PATIENT id|name|age|condition|doctor|admitted */
    int ppid, age, doc; char name[64], cond[128], admitted[32];
    sscanf(buf, "PATIENT %d|%63[^|]|%d|%127[^|]|%d|%31[^\n]",
           &ppid, name, &age, cond, &doc, admitted);
    printf(BOLD "\n  Patient #%d — %s%s%s\n" RESET, ppid, BRIGHT_CYAN, name, RESET);
    printf("  Age: %d  |  Condition: %s  |  Admitted: %s\n\n", age, cond, admitted);

    recv_line(srv_fd, buf, sizeof(buf)); /* HISTORY <n> */
    int hcnt = 0; sscanf(buf, "HISTORY %d", &hcnt);
    if (hcnt == 0) { printf("  No vitals on record yet.\n"); }
    else 
    {
        printf(BOLD "  %-20s  %6s  %10s  %6s  %6s  %5s\n" RESET,
               "Timestamp", "HR", "BP", "SpO2", "Temp", "RR");
        print_separator();
        for (int i = 0; i < hcnt; i++) {
            recv_line(srv_fd, buf, sizeof(buf));
            if (strcmp(buf,".") == 0) break;
            char ts[32]; float hr,sbp,dbp,spo2,temp,rr;
            sscanf(buf, "V|%31[^|]|%f|%f|%f|%f|%f|%f",
                   ts, &hr, &sbp, &dbp, &spo2, &temp, &rr);

            AlertSeverity hr_s  = check_threshold(hr,   THR_HR);
            AlertSeverity sbp_s = check_threshold(sbp,  THR_SBP);
            AlertSeverity sp_s  = check_threshold(spo2, THR_SPO2);
            AlertSeverity tm_s  = check_threshold(temp, THR_TEMP);
            AlertSeverity rr_s  = check_threshold(rr,   THR_RR);

            printf("  %-20s  %s%5.1f%s  %s%4.0f/%-4.0f%s  %s%5.1f%%%s  %s%5.1f%s  %s%4.0f%s\n",
                   ts,
                   sev_colour(hr_s),  hr,  RESET,
                   sev_colour(sbp_s), sbp, dbp, RESET,
                   sev_colour(sp_s),  spo2, RESET,
                   sev_colour(tm_s),  temp, RESET,
                   sev_colour(rr_s),  rr,  RESET);
        }
    }
    /* drain trailing dot */
    recv_line(srv_fd, buf, sizeof(buf));
}

int doctor_main(void) 
{
    running    = 1;
    alert_flag = 0;
    doctor_id  = 0;
    signal(SIGUSR1, on_sigusr1);
    signal(SIGINT,  on_sigint);

    print_banner("  ICU DOCTOR CLIENT  ", BRIGHT_CYAN);

    /* ── Connect ──────────────────────────────────────────────── */
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &srv.sin_addr);

    /* ── Connect + authenticate (up to 3 attempts) ───────────── */
    char buf[BUFFER_SIZE];
    int authed = 0;
    char username[32], password[64];

    for (int attempt = 1; attempt <= 3; attempt++) 
    {
        printf(YELLOW "Connecting to %s:%d ...\n" RESET, SERVER_HOST, SERVER_PORT);
        if (connect(srv_fd, (struct sockaddr*)&srv, sizeof(srv)) < 0)
            { perror("connect"); close(srv_fd); return 1; }

        recv_line(srv_fd, buf, sizeof(buf));   /* banner */

        printf(BOLD "Login attempt %d/3\n" RESET, attempt);
        printf("Username: "); fflush(stdout); scanf("%31s", username);
        printf("Password: "); fflush(stdout); scanf("%63s", password);
        send_line(srv_fd, "AUTH %s %s", username, password);
        recv_line(srv_fd, buf, sizeof(buf));

        if (strncmp(buf, "AUTH_OK DOCTOR", 14) == 0) 
        {
            sscanf(buf, "AUTH_OK DOCTOR %d", &doctor_id);
            printf(BRIGHT_GREEN "✓ Authenticated as Doctor (id=%d)\n\n" RESET, doctor_id);
            authed = 1;
            break;
        }

        printf(RED "✗ Auth failed: %s" RESET, buf);
        if (attempt < 3)
            printf(YELLOW "  (%d attempt(s) remaining)\n" RESET, 3 - attempt);
        else
            printf("\n");
        close(srv_fd);
        srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    }

    if (!authed) 
    {
        printf(RED "Too many failed attempts. Exiting.\n" RESET);
        close(srv_fd); return 1;
    }

    recv_line(srv_fd, buf, sizeof(buf)); /* READY line */

    /* ── Register PID for SIGUSR1 ─────────────────────────────── */
    send_line(srv_fd, "REGISTER_PID %d", (int)getpid());
    recv_line(srv_fd, buf, sizeof(buf));
    printf(BRIGHT_GREEN "✓ PID %d registered for real-time alerts.\n" RESET, (int)getpid());

    /* ── Create named pipe & start background reader thread ──── */
    char pipe_path[128];
    alert_pipe_path(pipe_path, sizeof(pipe_path), doctor_id);
    mkfifo(pipe_path, 0666); /* create if not exists */
    printf(BRIGHT_GREEN "✓ Alert pipe: %s\n\n" RESET, pipe_path);

    pthread_t pipe_tid;
    pthread_create(&pipe_tid, NULL, pipe_reader, (void*)(intptr_t)doctor_id);

    /* ── Interactive menu ─────────────────────────────────────── */
    printf(DIM "\n  Alerts from the server will appear here automatically\n"
               "  as they arrive — no need to wait or poll.\n" RESET);

    while (running) 
    {
        printf(BOLD BRIGHT_CYAN "\n╔═ DOCTOR MENU ════════════════════════╗\n" RESET);
        printf(BOLD BRIGHT_CYAN "║" RESET " [1] My Patients                      "
               BOLD BRIGHT_CYAN "║\n" RESET);
        printf(BOLD BRIGHT_CYAN "║" RESET " [2] View Patient Details & Vitals    "
               BOLD BRIGHT_CYAN "║\n" RESET);
        printf(BOLD BRIGHT_CYAN "║" RESET " [3] Quit                             "
               BOLD BRIGHT_CYAN "║\n" RESET);
        printf(BOLD BRIGHT_CYAN "╚══════════════════════════════════════╝\n" RESET);
        printf("Choice: ");
        int ch = 0;
        int res = scanf("%d",&ch);
        if (res == EOF) break;
        if(res != 1)
        { 
            int c; while ((c = getchar()) != '\n' && c != EOF);
            printf("Invalid input.\n");
            continue; 
        }

        switch(ch) 
        {
            case 1:
                do_my_patients();
                break;
            case 2: 
            {
                int pid;
                printf("Patient ID: "); fflush(stdout);
                if (scanf("%d", &pid) != 1) 
                {
                    int c; while ((c = getchar()) != '\n' && c != EOF);
                    printf("Invalid input.\n");
                    break;
                }
                do_view_patient(pid);
                break;
            }
            case 3:
                running = 0;
                break;
            default:
                printf(YELLOW "Invalid choice.\n" RESET);
        }
    }

    send_line(srv_fd, "QUIT");
    recv_line(srv_fd, buf, sizeof(buf));
    running = 0;
    pthread_cancel(pipe_tid);
    pthread_join(pipe_tid, NULL);
    close(srv_fd);
    printf(GREEN "\n[DOCTOR] Disconnected.\n" RESET);
    return 0;
}
