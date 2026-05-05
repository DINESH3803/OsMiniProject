/*
 * client/guest_client.c
 * Read-only, limited access — shows anonymised ICU statistics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/common.h"

static volatile int running = 1;
static void on_sigint(int s){ (void)s; running=0; }

int guest_main(void) 
{
    running = 1;
    signal(SIGINT, on_sigint);
    print_banner("  ICU GUEST VIEWER  ", DIM);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in s = {0};
    s.sin_family=AF_INET; s.sin_port=htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &s.sin_addr);

    printf(YELLOW "Connecting to %s:%d ...\n" RESET, SERVER_HOST, SERVER_PORT);
    if (connect(fd,(struct sockaddr*)&s,sizeof(s))<0){ perror("connect"); return 1; }

    char buf[BUFFER_SIZE];
    recv_line(fd, buf, sizeof(buf));

    /* Guest login — no credentials required */
    send_line(fd, "GUEST");
    recv_line(fd, buf, sizeof(buf));
    if (strncmp(buf,"AUTH_OK",7)!=0){
        printf(RED "Guest login failed: %s\n" RESET, buf);
        close(fd); return 1;
    }
    printf(GREEN "✓ Connected as Guest (read-only)\n\n" RESET);
    recv_line(fd, buf, sizeof(buf)); /* READY */

    printf(DIM "Guest access: only anonymised statistics are available.\n" RESET);

    while (running) {
        printf(BOLD "\n╔═ GUEST MENU ════════════════╗\n"
               "║ [1] ICU Statistics          ║\n"
               "║ [2] Refresh (auto in 10s)   ║\n"
               "║ [3] Quit                    ║\n"
               "╚═════════════════════════════╝\n" RESET "Choice: ");
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
            case 1:
            case 2: {
                if (ch == 2) {
                    printf(DIM "Auto-refresh every 10s. Press Ctrl+C to stop.\n" RESET);
                    for (int i=0; i<6 && running; i++) {
                        send_line(fd, "STATS");
                        recv_line(fd, buf, sizeof(buf));
                        char ts[32]; timestamp_str(ts, sizeof(ts), time(NULL));
                        printf("\n  [%s]  %s%s%s\n", ts, BRIGHT_CYAN, buf, RESET);
                        sleep(10);
                    }
                } else {
                    send_line(fd, "STATS");
                    recv_line(fd, buf, sizeof(buf));
                    char ts[32]; timestamp_str(ts, sizeof(ts), time(NULL));
                    printf("\n  [%s]  %s%s%s\n", ts, BRIGHT_CYAN, buf, RESET);
                    printf(DIM "  (Names and vitals are hidden for privacy)\n" RESET);
                }
                break;
            }
            case 3: running=0; break;
            default: printf(YELLOW "Invalid.\n" RESET);
        }
    }

    send_line(fd, "QUIT");
    recv_line(fd, buf, sizeof(buf));
    close(fd);
    printf(GREEN "\n[GUEST] Disconnected.\n" RESET);
    return 0;
}
