#ifndef COMMON_H
#define COMMON_H

/* _GNU_SOURCE is set via -D flag in Makefile */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mqueue.h>
#include <dirent.h>
#include <stdarg.h>
#include <math.h>

/* ─── Network ──────────────────────────────────────────────── */
#define SERVER_PORT     8888
#define SERVER_HOST     "127.0.0.1"
#define MAX_CLIENTS     32
#define BUFFER_SIZE     4096

/* ─── Limits ────────────────────────────────────────────────── */
#define MAX_PATIENTS    50
#define MAX_USERS       100
#define MAX_HISTORY     20

/* ─── Paths ─────────────────────────────────────────────────── */
#define DATA_DIR        "data"
#define PATIENT_DIR     "data/patients"
#define USERS_FILE      "data/users.dat"
#define LOG_FILE        "logs/server.log"
#define MQUEUE_NAME     "/icu_mqueue"

/* ─── ANSI Colours (disabled — plain output) ────────────────── */
#define RESET          ""
#define BOLD           ""
#define DIM            ""
#define RED            ""
#define GREEN          ""
#define YELLOW         ""
#define BLUE           ""
#define MAGENTA        ""
#define CYAN           ""
#define WHITE          ""
#define BRIGHT_RED     ""
#define BRIGHT_GREEN   ""
#define BRIGHT_YELLOW  ""
#define BRIGHT_BLUE    ""
#define BRIGHT_MAGENTA ""
#define BRIGHT_CYAN    ""
#define BG_RED         ""
#define BG_GREEN       ""
#define BG_YELLOW      ""
#define BG_BLUE        ""

/* ─── Roles ─────────────────────────────────────────────────── */
typedef enum { ROLE_ADMIN=0, ROLE_DOCTOR, ROLE_NURSE, ROLE_GUEST, ROLE_NONE } Role;

static inline const char* role_to_str(Role r) {
    switch(r) {
        case ROLE_ADMIN:  return "ADMIN";
        case ROLE_DOCTOR: return "DOCTOR";
        case ROLE_NURSE:  return "NURSE";
        case ROLE_GUEST:  return "GUEST";
        default:          return "UNKNOWN";
    }
}
static inline Role str_to_role(const char* s) {
    if (strcasecmp(s,"ADMIN")==0)  return ROLE_ADMIN;
    if (strcasecmp(s,"DOCTOR")==0) return ROLE_DOCTOR;
    if (strcasecmp(s,"NURSE")==0)  return ROLE_NURSE;
    if (strcasecmp(s,"GUEST")==0)  return ROLE_GUEST;
    return ROLE_NONE;
}

/* ─── Alert severity ────────────────────────────────────────── */
typedef enum { SEV_NONE=0, SEV_WARNING, SEV_CRITICAL } AlertSeverity;

static inline const char* sev_str(AlertSeverity s){
    switch(s){ case SEV_WARNING: return "WARNING"; case SEV_CRITICAL: return "CRITICAL"; default: return "NONE"; }
}
static inline const char* sev_colour(AlertSeverity s){
    switch(s){ case SEV_WARNING: return BRIGHT_YELLOW; case SEV_CRITICAL: return BRIGHT_RED; default: return RESET; }
}

/* ─── Utility ───────────────────────────────────────────────── */
static inline void print_banner(const char* title, const char* colour) {
    int w = 62;
    int len = (int)strlen(title);
    int pad = (w - len) / 2;
    printf("%s%s╔", BOLD, colour);
    for(int i=0;i<w;i++) printf("═");
    printf("╗\n║");
    for(int i=0;i<pad;i++) printf(" ");
    printf("%s", title);
    for(int i=0;i<w-pad-len;i++) printf(" ");
    printf("║\n╚");
    for(int i=0;i<w;i++) printf("═");
    printf("╝\n%s", RESET);
}

static inline void print_separator(void){
    printf(DIM "────────────────────────────────────────────────────────────\n" RESET);
}

static inline char* timestamp_str(char* buf, size_t n, time_t t){
    struct tm* tm_info = localtime(&t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

/* ─── Safe send/recv helpers ────────────────────────────────── */
static inline int send_line(int fd, const char* fmt, ...) {         // helps in sending inline messages safely
    char buf[BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf)-2, fmt, ap);
    va_end(ap);
    buf[n] = '\n'; buf[n+1] = '\0';
    return (int)send(fd, buf, n+1, MSG_NOSIGNAL);
}

static inline int recv_line(int fd, char* buf, int maxlen) {
    int total = 0;
    while (total < maxlen-1) {
        char c;
        int r = (int)recv(fd, &c, 1, 0);
        if (r <= 0) return r;
        if (c == '\n') { buf[total] = '\0'; return total; }
        buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

#endif /* COMMON_H */
