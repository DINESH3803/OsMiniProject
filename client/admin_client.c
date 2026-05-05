/*
 * client/admin_client.c
 * Connects as Admin. Full CRUD for users and patients.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/common.h"

static volatile int running = 1;
static void on_sigint(int s){ (void)s; running = 0; }
static int srv_fd = -1;

static void drain(void) { char buf[BUFFER_SIZE]; recv_line(srv_fd, buf, sizeof(buf)); }

/* ── User Management ─────────────────────────────────────────── */
static void list_users(void) 
{
    char buf[BUFFER_SIZE];
    send_line(srv_fd, "LIST_USERS");
    recv_line(srv_fd, buf, sizeof(buf));  /* USERS <N> */
    int cnt=0; sscanf(buf,"USERS %d",&cnt);
    printf(BOLD "\n  %-5s  %-20s  %-10s\n" RESET, "ID", "Username", "Role");
    print_separator();
    if (cnt == 0) printf(DIM "  (no users)\n" RESET);
    /* Read until the sentinel '.' regardless of cnt — the count is just
     * a hint for display; the dot is the true end-of-stream marker. */
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) 
    {
        if (strcmp(buf, ".") == 0) break;   /* sentinel always consumed */
        int uid; char uname[32], role[16];
        sscanf(buf,"%d|%31[^|]|%15s",&uid,uname,role);
        const char* col = RESET;
        if(strcmp(role,"ADMIN")==0)  col=BRIGHT_MAGENTA;
        if(strcmp(role,"DOCTOR")==0) col=BRIGHT_CYAN;
        if(strcmp(role,"NURSE")==0)  col=BRIGHT_GREEN;
        printf("  %-5d  %-20s  %s%-10s%s\n", uid, uname, col, role, RESET);
    }
}

static void add_user_menu(void) 
{
    char buf[BUFFER_SIZE];
    char name[32], pass[64], rolestr[16];
    printf("  Username : "); fflush(stdout); scanf("%31s", name);
    printf("  Password : "); fflush(stdout); scanf("%63s", pass);
    printf("  Role [ADMIN/DOCTOR/NURSE/GUEST]: "); fflush(stdout); scanf("%15s", rolestr);
    send_line(srv_fd, "ADD_USER %s %s %s", name, pass, rolestr);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf(GREEN "  ✓ User created (id=%s)\n" RESET, buf+3);
    else printf(RED "  ✗ %s\n" RESET, buf);
}

static void delete_user_menu(void) 
{
    char buf[BUFFER_SIZE];
    int uid;
    printf("  User ID to delete: "); fflush(stdout); scanf("%d",&uid);
    send_line(srv_fd, "DELETE_USER %d", uid);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf(GREEN "  ✓ Deleted\n" RESET);
    else printf(RED "  ✗ %s\n" RESET, buf);
}

/* ── Patient Management ──────────────────────────────────────── */
static void list_patients(void) 
{
    char buf[BUFFER_SIZE];
    send_line(srv_fd, "LIST_PATIENTS");
    recv_line(srv_fd, buf, sizeof(buf));  /* PATIENTS <N> */
    int cnt=0; sscanf(buf,"PATIENTS %d",&cnt);
    printf("\n  %-5s  %-20s  %-4s  %-20s  %-6s  %s\n",
           "ID", "Name", "Age", "Condition", "DocID", "Nurses");
    print_separator();
    if (cnt == 0) printf("  (no patients)\n");
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) 
    {
        if (strcmp(buf, ".") == 0) break;
        int pid, age, doc; char name[64], cond[128], nurses[64];
        /* Format: id|name|age|condition|doc_id|nurses */
        if (sscanf(buf, "%d|%63[^|]|%d|%127[^|]|%d|%63[^\n]",
                   &pid, name, &age, cond, &doc, nurses) < 6)
            strcpy(nurses, "-");
        printf("  %-5d  %-20s  %-4d  %-20s  %-6d  %s\n",
               pid, name, age, cond, doc, nurses);
    }
}

static void add_patient_menu(void) 
{
    char buf[BUFFER_SIZE];

    /* ── Fetch and display available doctors first ─────────────── */
    send_line(srv_fd, "LIST_USERS");
    recv_line(srv_fd, buf, sizeof(buf));   /* USERS <N> */
    printf(BOLD "\n  Available Doctors:\n" RESET);
    int found_any = 0;
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) 
    {
        if (strcmp(buf, ".") == 0) break;
        int uid; char uname[32], role[16];static void add_patient_menu(void) 
{
    char buf[BUFFER_SIZE];

    /* ── Fetch and display available doctors first ─────────────── */
    send_line(srv_fd, "LIST_USERS");
    recv_line(srv_fd, buf, sizeof(buf));   /* USERS <N> */
    printf(BOLD "\n  Available Doctors:\n" RESET);
    int found_any = 0;
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) 
    {
        if (strcmp(buf, ".") == 0) break;
        int uid; char uname[32], role[16];
        sscanf(buf, "%d|%31[^|]|%15s", &uid, uname, role);
        if (strcmp(role, "DOCTOR") == 0) {
            printf("  " BRIGHT_CYAN "  ID %-4d  %s\n" RESET, uid, uname);
            found_any = 1;
        }
    }
    if (!found_any)
        printf(DIM "  (no doctors registered — add a DOCTOR user first)\n" RESET);
    printf("\n");

    /* ── Collect patient details ────────────────────────────────── */
    char name[64], cond[128]; int age, doc;
    printf("  Patient name : "); fflush(stdout); scanf("%63s", name);
    printf("  Age          : "); fflush(stdout); scanf("%d", &age);
    printf("  Doctor ID    : "); fflush(stdout); scanf("%d", &doc);
    printf("  Condition    : "); fflush(stdout);
    scanf(" %127[^\n]", cond);
    send_line(srv_fd, "ADD_PATIENT %s %d %d %s", name, age, doc, cond);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf(GREEN "  ✓ Patient added (id=%s)\n" RESET, buf+3);
    else printf(RED "  ✗ %s\n" RESET, buf);
}
        sscanf(buf, "%d|%31[^|]|%15s", &uid, uname, role);
        if (strcmp(role, "DOCTOR") == 0) {
            printf("  " BRIGHT_CYAN "  ID %-4d  %s\n" RESET, uid, uname);
            found_any = 1;
        }
    }
    if (!found_any)
        printf(DIM "  (no doctors registered — add a DOCTOR user first)\n" RESET);
    printf("\n");

    /* ── Collect patient details ────────────────────────────────── */
    char name[64], cond[128]; int age, doc;
    printf("  Patient name : "); fflush(stdout); scanf("%63s", name);
    printf("  Age          : "); fflush(stdout); scanf("%d", &age);
    printf("  Doctor ID    : "); fflush(stdout); scanf("%d", &doc);
    printf("  Condition    : "); fflush(stdout);
    scanf(" %127[^\n]", cond);
    send_line(srv_fd, "ADD_PATIENT %s %d %d %s", name, age, doc, cond);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf(GREEN "  ✓ Patient added (id=%s)\n" RESET, buf+3);
    else printf(RED "  ✗ %s\n" RESET, buf);
}

static void delete_patient_menu(void) 
{
    char buf[BUFFER_SIZE];
    int pid;
    printf("  Patient ID to discharge: "); fflush(stdout); scanf("%d",&pid);
    send_line(srv_fd, "DELETE_PATIENT %d", pid);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf(GREEN "  ✓ Discharged\n" RESET);
    else printf(RED "  ✗ %s\n" RESET, buf);
}

static void assign_nurse_menu(void) 
{
    char buf[BUFFER_SIZE];

    /* Show available nurses */
    send_line(srv_fd, "LIST_USERS");
    recv_line(srv_fd, buf, sizeof(buf));   /* USERS <N> */
    printf("\n  Available Nurses:\n");
    int found_any = 0;
    while (recv_line(srv_fd, buf, sizeof(buf)) > 0) {
        if (strcmp(buf, ".") == 0) break;
        int uid; char uname[32], role[16];
        sscanf(buf, "%d|%31[^|]|%15s", &uid, uname, role);
        if (strcmp(role, "NURSE") == 0) {
            printf("    ID %-4d  %s\n", uid, uname);
            found_any = 1;
        }
    }
    if (!found_any)
        printf("  (no nurses registered)\n");
    printf("\n");

    int pid, nid;
    printf("  Patient ID : "); fflush(stdout); scanf("%d", &pid);
    printf("  Nurse ID   : "); fflush(stdout); scanf("%d", &nid);
    send_line(srv_fd, "ASSIGN_NURSE %d %d", pid, nid);
    recv_line(srv_fd, buf, sizeof(buf));
    if (strncmp(buf,"OK",2)==0) printf("  Nurse assigned successfully.\n");
    else printf("  Error: %s\n", buf);
}

static void server_stats(void) 
{
    char buf[BUFFER_SIZE];
    send_line(srv_fd, "SERVER_STATS");
    recv_line(srv_fd, buf, sizeof(buf));
    printf("\n  " BRIGHT_MAGENTA "%s\n" RESET, buf);
}

int admin_main(void) 
{
    running = 1;
    signal(SIGINT, on_sigint);
    print_banner("  ICU ADMIN CLIENT  ", BRIGHT_MAGENTA);

    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in s = {0};
    s.sin_family = AF_INET; s.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &s.sin_addr);

    /* ── Connect + authenticate (up to 3 attempts) ───────────── */
    char buf[BUFFER_SIZE];
    int authed = 0;
    char username[32], password[64];

    for (int attempt = 1; attempt <= 3; attempt++) {
        printf(YELLOW "Connecting to %s:%d ...\n" RESET, SERVER_HOST, SERVER_PORT);
        if (connect(srv_fd,(struct sockaddr*)&s,sizeof(s)) < 0)
            { perror("connect"); close(srv_fd); return 1; }

        recv_line(srv_fd, buf, sizeof(buf));   /* banner */

        printf(BOLD "Login attempt %d/3\n" RESET, attempt);
        printf("Username: "); fflush(stdout); scanf("%31s", username);
        printf("Password: "); fflush(stdout); scanf("%63s", password);
        send_line(srv_fd, "AUTH %s %s", username, password);
        recv_line(srv_fd, buf, sizeof(buf));

        if (strncmp(buf,"AUTH_OK ADMIN",13) == 0) {
            printf(BRIGHT_GREEN "✓ Authenticated as Admin\n\n" RESET);
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

    if (!authed) {
        printf(RED "Too many failed attempts. Exiting.\n" RESET);
        close(srv_fd); return 1;
    }

    drain(); /* READY line */

    while (running) {
        printf(BOLD BRIGHT_MAGENTA
               "\n╔═ ADMIN MENU ══════════════════════════╗\n"
               "║ [1] List Users       [2] Add User     ║\n"
               "║ [3] Delete User      [4] List Patients║\n"
               "║ [5] Add Patient      [6] Discharge Pt ║\n"
               "║ [7] Assign Nurse     [8] Server Stats ║\n"
               "║ [9] Quit                              ║\n"
               "╚═══════════════════════════════════════╝\n"
               RESET "Choice: ");
        fflush(stdout);
        int ch = 0;
        int res = scanf("%d",&ch);
        if (res == EOF) break;
        if(res != 1){ 
            int c; while ((c = getchar()) != '\n' && c != EOF);
            printf("Invalid input.\n");
            continue; 
        }
        switch(ch){
            case 1: list_users();        break;
            case 2: add_user_menu();     break;
            case 3: delete_user_menu();  break;
            case 4: list_patients();     break;
            case 5: add_patient_menu();  break;
            case 6: delete_patient_menu(); break;
            case 7: assign_nurse_menu(); break;
            case 8: server_stats();      break;
            case 9: running=0;           break;
            default: printf(YELLOW "Invalid.\n" RESET);
        }
    }

    send_line(srv_fd, "QUIT");
    recv_line(srv_fd, buf, sizeof(buf));
    close(srv_fd);
    printf(GREEN "\n[ADMIN] Disconnected.\n" RESET);
    return 0;
}
