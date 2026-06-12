#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common.h"

static int sock_fd = -1;

static void send_cmd(const char *cmd) {
    char b[BUFFER_SIZE];
    snprintf(b, sizeof(b), "%s\n", cmd);
    send(sock_fd, b, strlen(b), 0);
}

static int recv_line(char *buf, size_t size) {
    size_t i = 0; char c;
    while (i < size - 1) {
        ssize_t n = recv(sock_fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static void recv_list(void) {
    char line[BUFFER_SIZE];
    int count = 0;
    printf("\n");
    printf("  %-4s  %-8s  %-13s  %-28s  %-14s  %-s\n",
           "ID", "PRIORITY", "STATUS", "DESCRIPTION", "REPORTED BY", "CLAIMED BY");
    printf("  %s\n",
           "----  --------  -------------  ----------------------------  --------------  ----------");
    while (recv_line(line, sizeof(line)) >= 0) {
        if (strcmp(line, RESP_END) == 0) break;
        printf("  %s\n", line);
        count++;
    }
    if (count == 0) printf("  (no incidents in your zone)\n");
    printf("\n");
}

static void print_header(void) {
    printf("\n");
    printf("  ================================================\n");
    printf("   TRAFFIC INCIDENT SYSTEM  |  Officer Terminal  \n");
    printf("  ================================================\n");
}

static void print_menu(const char *username, int zone) {
    printf("\n");
    printf("  Officer : %-20s  Zone : %d\n", username, zone);
    printf("  ------------------------------------------------\n");
    printf("  LIST                           View your zone incidents\n");
    printf("  CLAIM  <id>                    Claim an incident\n");
    printf("  UPDATE <id> INVESTIGATING       Mark as investigating\n");
    printf("  UPDATE <id> RESOLVED            Mark as resolved\n");
    printf("  REPORT <zone> <priority> <desc> File new incident\n");
    printf("  STATS                          View live statistics\n");
    printf("  QUIT\n");
    printf("  ------------------------------------------------\n");
    printf("  > ");
    fflush(stdout);
}

int main(void) {
    print_header();

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &srv.sin_addr);

    printf("\n  Connecting to server...");
    fflush(stdout);
    if (connect(sock_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        printf(" FAILED\n  Is the server running on port %d?\n\n", SERVER_PORT);
        return 1;
    }
    printf(" Connected.\n");

    char line[BUFFER_SIZE];
    recv_line(line, sizeof(line));

    printf("\n");
    char username[MAX_USER_LEN], password[MAX_PASS_LEN];
    printf("  Username : "); fflush(stdout); scanf("%49s", username);
    printf("  Password : "); fflush(stdout); scanf("%49s", password);

    char lcmd[BUFFER_SIZE];
    snprintf(lcmd, sizeof(lcmd), "LOGIN %s %s", username, password);
    send_cmd(lcmd);
    recv_line(line, sizeof(line));

    if (strncmp(line, RESP_AUTH_OK, strlen(RESP_AUTH_OK)) != 0) {
        printf("\n  Login failed. Check your credentials.\n\n");
        close(sock_fd); return 1;
    }

    // Parse zone from welcome message
    int zone = -1;
    char *zp = strstr(line, "Zone:");
    if (zp) sscanf(zp + 5, "%d", &zone);
    printf("\n  Login successful. Assigned to Zone %d.\n", zone);

    char input[BUFFER_SIZE];
    while (1) {
        print_menu(username, zone);

        int c; while ((c = getchar()) == '\n' || c == '\r'); ungetc(c, stdin);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';
        if (!strlen(input)) continue;

        if (strcmp(input, "QUIT") == 0) {
            send_cmd("QUIT");
            recv_line(line, sizeof(line));
            printf("\n  Session ended. Goodbye, %s.\n\n", username);
            break;
        }

        if (strncmp(input, "LIST", 4) == 0 ) {
            send_cmd(input);
            printf("\n  --- Zone %d Incidents (sorted by priority) ---", zone);
            recv_list();
            continue;
        }

        if (strncmp(input, "STATS", 5) == 0) {
            send_cmd(input);
            printf("\n");
            while (recv_line(line, sizeof(line)) >= 0) {
                if (strcmp(line, RESP_END) == 0) break;
                printf("  %s\n", line);
            }
            printf("\n");
            continue;
        }

        send_cmd(input);
        recv_line(line, sizeof(line));
        printf("\n  %s\n", line);
    }

    close(sock_fd);
    return 0;
}
