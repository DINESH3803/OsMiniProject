/*
 * server/ipc_alert.c
 * Alert delivery via three IPC mechanisms:
 *   1. Named pipe  (mkfifo) — primary fast-path to doctor_client
 *   2. SIGUSR1              — wakes the doctor process
 *   3. POSIX message queue  — auditable persistent queue
 * OS Concepts: signals, named pipes, POSIX mqueue.
 */
#include "server.h"


/* ─── Create named pipes for every doctor on startup ────────── */
// It iterates through the global user list, finds every active user with the ROLE_DOCTOR role, and creates 
// a special file in the /tmp/ directory (e.g., /tmp/icu_alert_2).
void create_doctor_pipes(void) {
    pthread_mutex_lock(&g_state.user_mutex);
    for (int i = 0; i < g_state.user_count; i++) {
        User* u = &g_state.users[i];
        if (!u->active || u->role != ROLE_DOCTOR) continue;
        char path[128];
        alert_pipe_path(path, sizeof(path), u->user_id);
        /* Create only if it doesn't already exist */
        if (mkfifo(path, 0666) < 0 && errno != EEXIST)
            perror("mkfifo");
        else
            server_log("Named pipe ready: %s", path);
    }
    pthread_mutex_unlock(&g_state.user_mutex);
}
//  A Named Pipe looks like a regular file on Linux, but it doesn't store data on the hard drive. Instead, 
//  it acts as a direct memory tunnel. The server can push data into one end, and the Doctor's client process instantly 
//  reads it out the other end.

/* ─── Deliver one alert ─────────────────────────────────────── */
// It opens the Doctor's specific Named Pipe (/tmp/icu_alert_<doctor_id>) in O_NONBLOCK mode (so the server doesn't freeze if the doctor is offline) 
// and write()s the exact details of the medical emergency directly into the pipe.
// Finally, it uses mq_send to drop a copy of the Alert struct into the global POSIX Message Queue (/icu_mqueue). 
void send_alert(const Alert* a) {
    server_log("ALERT [%s] Patient#%d -> Doctor#%d : %s",
               sev_str(a->severity), a->patient_id, a->doctor_id, a->message);

    /* 1. Named pipe — non-blocking write so server thread never stalls */
    char pipe_path[128];
    alert_pipe_path(pipe_path, sizeof(pipe_path), a->doctor_id);
    int pfd = open(pipe_path, O_WRONLY | O_NONBLOCK);
    if (pfd >= 0) {
        /* Write atomically (Alert struct ≪ PIPE_BUF on Linux) */
        write(pfd, a, sizeof(Alert));
        close(pfd);
        server_log("Alert written to named pipe %s", pipe_path);
    } else {
        /* Doctor not yet connected — non-fatal */
        server_log("Named pipe %s not open (doctor offline?)", pipe_path);
    }

    /* 2. SIGUSR1 — fetch doctor's registered PID */
    pthread_mutex_lock(&g_state.user_mutex);
    pid_t dpid = 0;
    for (int i = 0; i < g_state.user_count; i++) {
        if (g_state.users[i].user_id == a->doctor_id &&
            g_state.users[i].active) {
            dpid = g_state.users[i].pid;
            break;
        }
    }
    pthread_mutex_unlock(&g_state.user_mutex);

    if (dpid > 0) {
        if (kill(dpid, SIGUSR1) == 0)
            server_log("SIGUSR1 sent to doctor pid=%d", dpid);
        else
            server_log("SIGUSR1 failed for pid=%d: %s", dpid, strerror(errno));
    }

    /* 3. POSIX message queue — enqueue for audit log */
    if (g_state.mqueue != (mqd_t)-1) {
        if (mq_send(g_state.mqueue, (const char*)a, sizeof(Alert), 0) < 0)
            server_log("mq_send failed: %s", strerror(errno));
        else
            server_log("Alert enqueued in mqueue");
    }
}

// A POSIX Signal (kill(pid, SIGUSR1)) is basically just a tiny electrical zap from the Linux Kernel.
// It is incredibly fast and instantly interrupts whatever the Doctor's program is doing, but it cannot carry text or data.
// You cannot attach a string like "Patient 102 Heart Rate is Critical!" to a SIGUSR1 signal. so to send the data 
// we use pipe that a particular patient's vital have crosses the threshold.