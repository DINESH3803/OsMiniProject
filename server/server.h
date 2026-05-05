// done
/*
 * server/server.h
 * Global server state shared across all server translation units.
 */
#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include "../include/common.h"
#include "../include/auth.h"
#include "../include/patient.h"
#include "../include/ipc.h"

/* ─── Global server state ───────────────────────────────────── */
typedef struct ServerState {
    Patient         patients[MAX_PATIENTS];
    int             patient_count;
    pthread_mutex_t patient_mutex;
    // protects patients array and patient_count in memory
    // imagine a Nurse is updating a patient's blood pressure, but at the exact same 
    // millisecond, a Doctor requests to view that patient's history. Without this mutex, 
    // the Doctor might read the data while it is only half-written (a "dirty read"). 

    User            users[MAX_USERS];
    int             user_count;
    pthread_mutex_t user_mutex;
    // Similar to the patient mutex, this prevents the system from crashing if an Admin is 
    // deleting a user account at the exact same moment that user is trying to log in.

    sem_t           write_sem;   /* caps concurrent file writes */
    // prevents multiple threads from writing to the same file at the same time(race conditions).

    mqd_t           mqueue;
    //  the server drops an alert message into this queue.

    FILE*           logfile;
    // pointer to the logfile
    pthread_mutex_t log_mutex;
    // ensures that only one thread can write to the log file at a time(prevents race conditions)

    int             server_fd;
    volatile int    running;
} ServerState;

/* The one global instance — defined in server.c, extern everywhere else */
extern ServerState g_state;

/* Per-client thread argument */
typedef struct {
    int                fd;
    struct sockaddr_in addr;
} ClientArg;

/* Function prototypes (implemented across server source files) */

/* server.c */
void server_log(const char* fmt, ...);

/* auth.c */
void  load_users(void);
void  save_users(void);
User* find_user_by_name(const char* name);
User* find_user_by_id(int id);
User* authenticate(const char* name, const char* pass);
int   add_user(const char* name, const char* pass, Role role);
int   delete_user(int uid);

/* filestore.c */
void load_patients(void);
void save_patient(const Patient* p);
int  load_patient_from_file(int id, Patient* out);

/* anomaly.c */
AlertSeverity detect_anomaly(const Patient* p, const Vitals* v, Alert* out);

/* ipc_alert.c */
void create_doctor_pipes(void);
void send_alert(const Alert* a);

/* handler.c */
void* client_thread(void* arg);

#endif /* SERVER_H */
