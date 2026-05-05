#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int admin_main(void);
extern int doctor_main(void);
extern int nurse_main(void);
extern int guest_main(void);

int main(void) {
    while (1) {
        printf("\n");
        printf("=========================================\n");
        printf("         SMART ICU MONITORING            \n");
        printf("=========================================\n");
        printf("Select your role to login:\n");
        printf("  [1] Admin\n");
        printf("  [2] Doctor\n");
        printf("  [3] Nurse\n");
        printf("  [4] Guest\n");
        printf("  [0] Exit\n");
        printf("Choice: ");
        fflush(stdout);
        
        int choice = -1;
        if (scanf("%d", &choice) != 1) {
            if (feof(stdin)) return 0;
            int c; while ((c = getchar()) != '\n' && c != EOF);
            printf("Invalid choice.\n");
            continue;
        }

        switch (choice) {
            case 1: admin_main(); break;
            case 2: doctor_main(); break;
            case 3: nurse_main(); break;
            case 4: guest_main(); break;
            case 0: return 0;
            default:
                printf("Invalid choice.\n");
                break;
        }
    }
    return 0;
}
