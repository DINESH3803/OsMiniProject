// done
/*
 * server/handler.c
 * Per-client thread: authenticates, then dispatches to role handler.
 * OS Concepts: pthread (one per client), mutex (shared patient DB),
 *              semaphore (file write cap via filestore).
 *
 * Protocol (line-delimited text):
 *   Client -> Server:  COMMAND arg1 arg2 ...
 *   Server -> Client:  OK ...  |  ERROR <reason>
 *   Multi-line data ends with a lone "." line.
 */
#include "server.h"


/* ─── Utility ───────────────────────────────────────────────── */
static Patient* find_patient(int id) {
    for (int i = 0; i < g_state.patient_count; i++)
        if (g_state.patients[i].patient_id == id && g_state.patients[i].active)
            return &g_state.patients[i];
    return NULL;
}


// assigns an id to the new patient 
static int next_patient_id(void) {
    int mx = 100;
    for (int i = 0; i < g_state.patient_count; i++)
        if (g_state.patients[i].patient_id > mx)
            mx = g_state.patients[i].patient_id;
    return mx + 1;
}


/* Send multi-line patient data block */
// it takes the patient's data, iterates backwards through their circular history buffer, and formats everything 
// into neat strings to push over the TCP socket back to the Doctor.

static void send_patient_data(int fd, const Patient* p) {
    char ts[32]; timestamp_str(ts, sizeof(ts), p->admitted_at);
    send_line(fd, "PATIENT %d|%s|%d|%s|%d|%s",
              p->patient_id, p->name, p->age,
              p->condition, p->assigned_doctor_id, ts);
    /* Send last vitals readings */
    send_line(fd, "HISTORY %d", p->history_count);
    for (int i = 0; i < p->history_count; i++) {
        int idx = (p->history_head - p->history_count + i + MAX_HISTORY) % MAX_HISTORY;
        const Vitals* v = &p->history[idx];
        char vts[32]; timestamp_str(vts, sizeof(vts), v->timestamp);
        send_line(fd, "V|%s|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f",
                  vts, v->heart_rate, v->systolic_bp, v->diastolic_bp,
                  v->spo2, v->temperature, v->resp_rate);
    }
    send_line(fd, ".");
}

/* ─── NURSE handler ─────────────────────────────────────────── */
static void handle_nurse(int fd, User* user) {
    char buf[BUFFER_SIZE];
    send_line(fd, "READY nurse %s", user->username);

    while (recv_line(fd, buf, sizeof(buf)) > 0) {
        /* VITALS <pid> <hr> <sbp> <dbp> <spo2> <temp> <rr> */
        if (strncmp(buf, "VITALS ", 7) == 0) {
            int pid;
            float hr, sbp, dbp, spo2, temp, rr;
            if (sscanf(buf+7, "%d %f %f %f %f %f %f",
                       &pid, &hr, &sbp, &dbp, &spo2, &temp, &rr) != 7) {
                send_line(fd, "ERROR bad vitals format");
                continue;
            }

            /* Acquire patient mutex — protect in-memory DB */
            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);
            if (!p) {
                pthread_mutex_unlock(&g_state.patient_mutex);
                send_line(fd, "ERROR patient %d not found", pid);
                continue;
            }

            /* Check nurse is assigned to this patient */
            int allowed = 0;
            for (int i = 0; i < p->nurse_count; i++)
                if (p->assigned_nurses[i] == user->user_id) { allowed = 1; break; }
            /* Admin nurses can access any patient */
            if (!allowed) {
                pthread_mutex_unlock(&g_state.patient_mutex);
                send_line(fd, "ERROR not assigned to patient %d", pid);
                continue;
            }

            Vitals v = {
                .patient_id  = pid,
                .heart_rate  = hr, .systolic_bp = sbp, .diastolic_bp = dbp,
                .spo2        = spo2, .temperature = temp, .resp_rate = rr,
                .nurse_id    = user->user_id,
                .timestamp   = time(NULL)
            };

            patient_add_vitals(p, &v);   /* update circular buffer */
            Patient p_copy = *p;          /* copy for async use */
            pthread_mutex_unlock(&g_state.patient_mutex);

            /* Persist to disk (sem-limited, fcntl-locked) */
            save_patient(&p_copy);

            /* Anomaly detection */
            Alert alert;
            AlertSeverity sev = detect_anomaly(&p_copy, &v, &alert);
            if (sev != SEV_NONE) {
                send_line(fd, "ALERT %s %s", sev_str(sev), alert.message);
                send_alert(&alert);
                server_log("[ANOMALY] Patient#%d %s: %s",
                           pid, sev_str(sev), alert.message);
            } else {
                send_line(fd, "OK vitals recorded");
            }

        } else if (strncmp(buf, "GET_PATIENT ", 12) == 0) {
            int pid = atoi(buf+12);
            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);
            if (!p) { pthread_mutex_unlock(&g_state.patient_mutex); send_line(fd,"ERROR not found"); continue; }
            Patient cp = *p;
            pthread_mutex_unlock(&g_state.patient_mutex);
            send_patient_data(fd, &cp);

        } else if (strncmp(buf, "CHECK_PATIENT ", 14) == 0) {
            /* Lightweight liveness check — no data sent back, just OK or ERROR.
             * Used by the client to verify the patient is still active and
             * assigned before generating/displaying a vitals reading. */
            int pid = atoi(buf+14);
            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);   /* returns NULL if inactive */
            int assigned = 0;
            if (p) {
                for (int i = 0; i < p->nurse_count; i++)
                    if (p->assigned_nurses[i] == user->user_id) { assigned = 1; break; }
            }
            pthread_mutex_unlock(&g_state.patient_mutex);
            if (!p)       send_line(fd, "ERROR patient %d has been discharged", pid);
            else if (!assigned) send_line(fd, "ERROR no longer assigned to patient %d", pid);
            else          send_line(fd, "OK");

        } else if (strcmp(buf, "LIST_PATIENTS") == 0) {
            pthread_mutex_lock(&g_state.patient_mutex);
            /* Count only patients assigned to this nurse */
            int assigned_cnt = 0;
            for (int i = 0; i < g_state.patient_count; i++) {
                Patient* p = &g_state.patients[i];
                if (!p->active) continue;
                for (int j = 0; j < p->nurse_count; j++) {
                    if (p->assigned_nurses[j] == user->user_id) { assigned_cnt++; break; }
                }
            }
            send_line(fd, "PATIENTS %d", assigned_cnt);
            for (int i = 0; i < g_state.patient_count; i++) {
                Patient* p = &g_state.patients[i];
                if (!p->active) continue;
                for (int j = 0; j < p->nurse_count; j++) {
                    if (p->assigned_nurses[j] == user->user_id) {
                        send_line(fd, "%d|%s", p->patient_id, p->name);
                        break;
                    }
                }
            }
            send_line(fd, ".");
            pthread_mutex_unlock(&g_state.patient_mutex);
            // lists the all the patients assigned to that particular nurse
        

        } else if (strcmp(buf, "QUIT") == 0) {
            send_line(fd, "OK bye");
            break;
        } else {
            send_line(fd, "ERROR unknown command");
        }
    }
}

/* ─── DOCTOR handler ────────────────────────────────────────── */
//  The very first thing a Doctor client does is send its Process ID (PID) to the server. The server stores this 
//  PID in RAM. This is critical so that the server knows exactly who to send the SIGUSR1 signal to later when an anomaly occurs!
// 3 things - REGISTER_PID, GET_PATIENT, MY_PATIENTS
static void handle_doctor(int fd, User* user) {
    send_line(fd, "READY doctor %s", user->username);
    char buf[BUFFER_SIZE];

    while (recv_line(fd, buf, sizeof(buf)) > 0) {

        if (strncmp(buf, "REGISTER_PID ", 13) == 0) {
            pid_t pid = (pid_t)atoi(buf+13);
            pthread_mutex_lock(&g_state.user_mutex);
            for (int i = 0; i < g_state.user_count; i++)
                if (g_state.users[i].user_id == user->user_id)
                    g_state.users[i].pid = pid;
            pthread_mutex_unlock(&g_state.user_mutex);
            server_log("Doctor %s registered pid=%d", user->username, pid);
            send_line(fd, "OK pid registered");

        } else if (strncmp(buf, "GET_PATIENT ", 12) == 0) {
            int pid = atoi(buf+12);
            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);
            if (!p) { pthread_mutex_unlock(&g_state.patient_mutex); send_line(fd,"ERROR not found"); continue; }
            /* Verify this patient is assigned to the doctor */
            if (p->assigned_doctor_id != user->user_id) {
                pthread_mutex_unlock(&g_state.patient_mutex);
                send_line(fd,"ERROR access denied");
                continue;
            }
            Patient cp = *p;
            pthread_mutex_unlock(&g_state.patient_mutex);
            send_patient_data(fd, &cp);

        } else if (strcmp(buf, "MY_PATIENTS") == 0) {
            pthread_mutex_lock(&g_state.patient_mutex);
            int cnt = 0;
            for (int i = 0; i < g_state.patient_count; i++)
                if (g_state.patients[i].active &&
                    g_state.patients[i].assigned_doctor_id == user->user_id) cnt++;
            send_line(fd, "PATIENTS %d", cnt);
            for (int i = 0; i < g_state.patient_count; i++) {
                Patient* p = &g_state.patients[i];
                if (p->active && p->assigned_doctor_id == user->user_id)
                    send_line(fd, "%d|%s|%d|%s", p->patient_id, p->name,
                              p->age, p->condition);
            }
            send_line(fd, ".");
            pthread_mutex_unlock(&g_state.patient_mutex);

        } else if (strcmp(buf, "QUIT") == 0) {
            send_line(fd, "OK bye");
            break;
        } else {
            send_line(fd, "ERROR unknown command");
        }
    }
}

/* ─── ADMIN handler ─────────────────────────────────────────── */
static void handle_admin(int fd, User* user) {
    send_line(fd, "READY admin %s", user->username);
    char buf[BUFFER_SIZE];

    while (recv_line(fd, buf, sizeof(buf)) > 0) {

        if (strcmp(buf, "LIST_USERS") == 0) {
            pthread_mutex_lock(&g_state.user_mutex);
            int cnt = 0;
            for (int i = 0; i < g_state.user_count; i++) if(g_state.users[i].active) cnt++;
            send_line(fd, "USERS %d", cnt);
            for (int i = 0; i < g_state.user_count; i++) {
                User* u = &g_state.users[i];
                if (u->active)
                    send_line(fd, "%d|%s|%s", u->user_id, u->username,
                              role_to_str(u->role));
            }
            send_line(fd, ".");
            pthread_mutex_unlock(&g_state.user_mutex);

        } else if (strncmp(buf, "ADD_USER ", 9) == 0) {
            /* ADD_USER <name> <pass> <role> */
            char name[32], pass[64], rolestr[16];
            if (sscanf(buf+9, "%31s %63s %15s", name, pass, rolestr) != 3) {
                send_line(fd, "ERROR bad format"); continue;
            }
            Role r = str_to_role(rolestr);
            if (r == ROLE_NONE) { send_line(fd,"ERROR invalid role"); continue; }
            int uid = add_user(name, pass, r);
            if (uid < 0) send_line(fd, "ERROR user exists or limit reached");
            else {
                send_line(fd, "OK %d", uid);
                /* If doctor, create named pipe */
                if (r == ROLE_DOCTOR) {
                    char path[128];
                    alert_pipe_path(path, sizeof(path), uid);
                    mkfifo(path, 0666);
                }
            }

        } else if (strncmp(buf, "DELETE_USER ", 12) == 0) {
            int uid = atoi(buf+12);

            /* Guard 1: cannot delete yourself */
            if (uid == user->user_id) {
                send_line(fd, "ERROR cannot delete your own account");
                continue;
            }

            /* Guard 2: cannot delete another admin */
            pthread_mutex_lock(&g_state.user_mutex);
            User* target = find_user_by_id(uid);
            int target_is_admin = (target != NULL && target->role == ROLE_ADMIN);
            pthread_mutex_unlock(&g_state.user_mutex);
            if (target_is_admin) {
                send_line(fd, "ERROR cannot delete another admin account");
                continue;
            }

            if (delete_user(uid) == 0) send_line(fd, "OK deleted");
            else send_line(fd, "ERROR user not found");

        } else if (strcmp(buf, "LIST_PATIENTS") == 0) {
            pthread_mutex_lock(&g_state.patient_mutex);
            int active_cnt = 0;
            for (int i = 0; i < g_state.patient_count; i++)
                if (g_state.patients[i].active) active_cnt++;
            send_line(fd, "PATIENTS %d", active_cnt);
            for (int i = 0; i < g_state.patient_count; i++) {
                Patient* p = &g_state.patients[i];
                if (!p->active) continue;
                /* Build comma-separated nurse ID list */
                char nurses_str[64] = "-";
                if (p->nurse_count > 0) {
                    int pos = 0;
                    for (int j = 0; j < p->nurse_count; j++) {
                        pos += snprintf(nurses_str + pos,
                                        sizeof(nurses_str) - pos,
                                        j == 0 ? "%d" : ",%d",
                                        p->assigned_nurses[j]);
                    }
                }
                send_line(fd, "%d|%s|%d|%s|%d|%s",
                          p->patient_id, p->name, p->age,
                          p->condition, p->assigned_doctor_id, nurses_str);
            }
            send_line(fd, ".");
            pthread_mutex_unlock(&g_state.patient_mutex);

        } else if (strncmp(buf, "ADD_PATIENT ", 12) == 0) {
            /* ADD_PATIENT <name> <age> <doctor_id> <condition...> */
            char name[64]; int age, doc_id; char cond[128];
            if (sscanf(buf+12, "%63s %d %d %127[^\n]", name, &age, &doc_id, cond) < 3)
                { send_line(fd,"ERROR bad format"); continue; }
            if (strlen(cond)==0) strcpy(cond,"Unknown");

            /* ── Validate that doc_id belongs to an active DOCTOR ── */
            pthread_mutex_lock(&g_state.user_mutex);
            User* doc = find_user_by_id(doc_id);
            int doc_ok = (doc != NULL && doc->active && doc->role == ROLE_DOCTOR);
            pthread_mutex_unlock(&g_state.user_mutex);
            if (!doc_ok) {
                send_line(fd, "ERROR doctor_id %d is not a valid active doctor", doc_id);
                continue;
            }

            pthread_mutex_lock(&g_state.patient_mutex);
            if (g_state.patient_count >= MAX_PATIENTS)
                { pthread_mutex_unlock(&g_state.patient_mutex); send_line(fd,"ERROR limit"); continue; }
            int newid = next_patient_id();
            Patient* p = &g_state.patients[g_state.patient_count++];
            memset(p, 0, sizeof(Patient));
            p->patient_id = newid;
            strncpy(p->name, name, sizeof(p->name)-1);
            p->age = age;
            strncpy(p->condition, cond, sizeof(p->condition)-1);
            p->assigned_doctor_id = doc_id;
            p->active = 1;
            p->admitted_at = time(NULL);
            Patient cp = *p;
            pthread_mutex_unlock(&g_state.patient_mutex);
            save_patient(&cp);
            send_line(fd, "OK %d", newid);
            server_log("Admin added patient #%d %s (doctor=%d)", newid, name, doc_id);

        } else if (strncmp(buf, "DELETE_PATIENT ", 15) == 0) {
            int pid = atoi(buf+15);
            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);
            if (!p) { pthread_mutex_unlock(&g_state.patient_mutex); send_line(fd,"ERROR not found"); continue; }
            p->active = 0;
            Patient cp = *p;
            pthread_mutex_unlock(&g_state.patient_mutex);
            save_patient(&cp);
            send_line(fd, "OK deleted");

        } else if (strncmp(buf, "ASSIGN_NURSE ", 13) == 0) {
            /* ASSIGN_NURSE <patient_id> <nurse_id> */
            int pid, nid;
            if (sscanf(buf+13, "%d %d", &pid, &nid) != 2)
                { send_line(fd,"ERROR bad format"); continue; }

            /* Validate nurse_id is an active ROLE_NURSE */
            pthread_mutex_lock(&g_state.user_mutex);
            User* nurse = find_user_by_id(nid);
            int nurse_ok = (nurse != NULL && nurse->active && nurse->role == ROLE_NURSE);
            pthread_mutex_unlock(&g_state.user_mutex);
            if (!nurse_ok) {
                send_line(fd, "ERROR nurse_id %d is not a valid active nurse", nid);
                continue;
            }

            pthread_mutex_lock(&g_state.patient_mutex);
            Patient* p = find_patient(pid);
            if (!p) { pthread_mutex_unlock(&g_state.patient_mutex); send_line(fd,"ERROR no patient"); continue; }

            /* Only one nurse per patient */
            if (p->nurse_count > 0) {
                int existing = p->assigned_nurses[0];
                pthread_mutex_unlock(&g_state.patient_mutex);
                send_line(fd, "ERROR patient %d already assigned to nurse %d", pid, existing);
            } else {
                p->assigned_nurses[0] = nid;
                p->nurse_count = 1;
                Patient cp = *p;
                pthread_mutex_unlock(&g_state.patient_mutex);
                save_patient(&cp);
                send_line(fd, "OK assigned");
                server_log("Nurse %d assigned to patient %d", nid, pid);
            }
            

        } else if (strcmp(buf, "SERVER_STATS") == 0) {
            pthread_mutex_lock(&g_state.patient_mutex);
            int active_p = 0;
            for(int i=0;i<g_state.patient_count;i++) if(g_state.patients[i].active) active_p++;
            pthread_mutex_unlock(&g_state.patient_mutex);
            pthread_mutex_lock(&g_state.user_mutex);
            int active_u = 0;
            for(int i=0;i<g_state.user_count;i++) if(g_state.users[i].active) active_u++;
            pthread_mutex_unlock(&g_state.user_mutex);
            send_line(fd, "STATS patients=%d users=%d", active_p, active_u);

        } else if (strcmp(buf, "QUIT") == 0) {
            send_line(fd, "OK bye"); break;
        } else {
            send_line(fd, "ERROR unknown command");
        }
    }
}

/* ─── GUEST handler ─────────────────────────────────────────── */
static void handle_guest(int fd) {
    send_line(fd, "READY guest");
    char buf[BUFFER_SIZE];
    while (recv_line(fd, buf, sizeof(buf)) > 0) {
        if (strcmp(buf, "STATS") == 0) {
            pthread_mutex_lock(&g_state.patient_mutex);
            int total = 0;
            for(int i=0;i<g_state.patient_count;i++) if(g_state.patients[i].active) total++;
            pthread_mutex_unlock(&g_state.patient_mutex);
            send_line(fd, "ICU_STATS total_patients=%d", total);
        } else if (strcmp(buf, "QUIT") == 0) {
            send_line(fd, "OK bye"); break;
        } else {
            send_line(fd, "ERROR guests have read-only limited access");
        }
    }
}

/* ─── Main client thread ────────────────────────────────────── */
// entry point for the client after connecting to the server and then based on the role it will be
// sent to the corresponding handler
void* client_thread(void* arg) {
    ClientArg* ca = (ClientArg*)arg;
    int fd = ca->fd;
    free(ca);

    char buf[BUFFER_SIZE];

    /* Step 1: Authentication challenge */
    send_line(fd, "ICU_SERVER 1.0 — authenticate with: AUTH <user> <pass>");
    if (recv_line(fd, buf, sizeof(buf)) <= 0) { close(fd); return NULL; }

    User* user = NULL;
    Role  role = ROLE_GUEST;

    if (strncmp(buf, "AUTH ", 5) == 0) {
        char uname[32], pass[64];
        if (sscanf(buf+5, "%31s %63s", uname, pass) == 2) {
            user = authenticate(uname, pass);
            if (user) {
                role = user->role;
                send_line(fd, "AUTH_OK %s %d", role_to_str(role), user->user_id);
                server_log("Login: %s [%s]", uname, role_to_str(role));
            } else {
                send_line(fd, "AUTH_FAIL invalid credentials");
                server_log("Failed login attempt: %s", uname);
                close(fd); return NULL;
            }
        }
    } else if (strcmp(buf, "GUEST") == 0) {
        send_line(fd, "AUTH_OK GUEST 0");
        role = ROLE_GUEST;
    } else {
        send_line(fd, "AUTH_FAIL expected AUTH or GUEST");
        close(fd); return NULL;
    }

    /* Step 2: Route to role-specific handler */
    switch (role) {
        case ROLE_NURSE:  handle_nurse(fd, user);  break;
        case ROLE_DOCTOR: handle_doctor(fd, user); break;
        case ROLE_ADMIN:  handle_admin(fd, user);  break;
        case ROLE_GUEST:  handle_guest(fd);        break;
        default: send_line(fd, "ERROR unknown role"); break;
    }

    close(fd);
    return NULL;
}
