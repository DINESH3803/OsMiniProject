// done
/*
 * server/filestore.c
 * Persistent patient records stored as binary files in data/patients/.
 * Every read/write uses fcntl advisory locks + semaphore to cap concurrency.
 * OS Concepts: fcntl (F_RDLCK / F_WRLCK), semaphore (sem_wait/sem_post).
 */
#include "server.h"

/* ─── Build path for a patient data file ───────────────────── */
static void patient_path(char* buf, size_t n, int id) {
    snprintf(buf, n, "%s/patient_%04d.dat", PATIENT_DIR, id);
}

/* ─── fcntl helpers ─────────────────────────────────────────── */
static void flock_set(int fd, int type) {
    struct flock fl = { .l_type=type, .l_whence=SEEK_SET, .l_start=0, .l_len=0 };
    while (fcntl(fd, F_SETLKW, &fl) == -1 && errno == EINTR);
}
static void flock_clr(int fd) {
    struct flock fl = { .l_type=F_UNLCK, .l_whence=SEEK_SET };
    fcntl(fd, F_SETLK, &fl);
}
// flock_set uses fcntl with F_SETLKW (Set Lock Wait). If another thread already has the file locked, 
// F_SETLKW tells the current thread to politely go to sleep and wait until the file is unlocked. 
// flock_clr uses F_UNLCK to release the lock when the thread is done.

/* ─── Save one patient to disk ──────────────────────────────── */
void save_patient(const Patient* p) {
    /* Semaphore: cap concurrent writers to 3 */ // to avoid disk thrashing
    sem_wait(&g_state.write_sem);

    char path[256];
    patient_path(path, sizeof(path), p->patient_id);

    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { perror("save_patient open"); sem_post(&g_state.write_sem); return; }

    flock_set(fd, F_WRLCK);   /* exclusive write lock */
    write(fd, p, sizeof(Patient));
    flock_clr(fd);

    close(fd);
    sem_post(&g_state.write_sem);
}
// writes patient details to his .dat file and before that it uses semaphores and file locking to 
// avoid race conditions

/* ─── Load one patient from disk ────────────────────────────── */
int load_patient_from_file(int id, Patient* out) {
    char path[256];
    patient_path(path, sizeof(path), id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    flock_set(fd, F_RDLCK);   /* shared read lock */
    ssize_t r = read(fd, out, sizeof(Patient));
    flock_clr(fd);
    close(fd);
    return (r == sizeof(Patient)) ? 0 : -1;
}
// reads patient details from his .dat file.
// diff between read and write locks is read can allow multiple threads to read but write 
// doesnt allow any other.

/* ─── Scan directory and load all patients into g_state ─────── */
// bootstraping func to called by main to load all the patients in to the global struct
void load_patients(void) {
    pthread_mutex_lock(&g_state.patient_mutex);
    g_state.patient_count = 0;

    DIR* d = opendir(PATIENT_DIR);
    if (!d) { pthread_mutex_unlock(&g_state.patient_mutex); return; }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "patient_", 8) != 0) continue;
        int id = atoi(entry->d_name + 8);
        if (id <= 0) continue;

        Patient p;
        if (load_patient_from_file(id, &p) == 0 && g_state.patient_count < MAX_PATIENTS)
            g_state.patients[g_state.patient_count++] = p;
    }
    closedir(d);
    pthread_mutex_unlock(&g_state.patient_mutex);
    server_log("Loaded %d patients from disk", g_state.patient_count);
}
