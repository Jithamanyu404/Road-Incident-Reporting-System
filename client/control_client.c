#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common.h"
#include "../server/mq_alerts.h"

static int sock_fd = -1;
static void send_cmd(const char *cmd){char b[BUFFER_SIZE];snprintf(b,sizeof(b),"%s\n",cmd);send(sock_fd,b,strlen(b),0);}
static int recv_line(char *buf,size_t size){size_t i=0;char c;while(i<size-1){ssize_t n=recv(sock_fd,&c,1,0);if(n<=0)return -1;if(c=='\n')break;if(c!='\r')buf[i++]=c;}buf[i]='\0';return(int)i;}
static void recv_list(void){char line[BUFFER_SIZE];while(recv_line(line,sizeof(line))>=0){if(strcmp(line,RESP_END)==0)break;printf("  %s\n",line);}}

int main(void) {
    printf("=== Traffic System — Control Room v2 ===\n");
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv; memset(&srv,0,sizeof(srv));
    srv.sin_family=AF_INET; srv.sin_port=htons(SERVER_PORT);
    inet_pton(AF_INET,SERVER_IP,&srv.sin_addr);
    if(connect(sock_fd,(struct sockaddr*)&srv,sizeof(srv))<0){perror("connect");return 1;}

    char line[BUFFER_SIZE];
    recv_line(line,sizeof(line));printf("Server: %s\n",line);
    char username[MAX_USER_LEN],password[MAX_PASS_LEN];
    printf("Username: ");fflush(stdout);scanf("%49s",username);
    printf("Password: ");fflush(stdout);scanf("%49s",password);
    char lcmd[BUFFER_SIZE];snprintf(lcmd,sizeof(lcmd),"LOGIN %s %s",username,password);
    send_cmd(lcmd);recv_line(line,sizeof(line));
    if(strncmp(line,RESP_AUTH_OK,strlen(RESP_AUTH_OK))!=0){printf("Login failed: %s\n",line);close(sock_fd);return 1;}
    printf("✓ %s\n\n",line);

    // Open message queue for live escalation alerts
    mq_alerts_init_client();

    printf("Commands:\n");
    printf("  LISTALL | LIST <zone> | CLAIM <id>\n");
    printf("  UPDATE <id> INVESTIGATING|RESOLVED | CLOSE <id>\n");
    printf("  STATS        (shared memory live stats)\n");
    printf("  ALERTS       (drain escalation message queue)\n");
    printf("  REPORT <zone> CRITICAL <desc>\n");
    printf("  QUIT\n\n");
    printf("Live escalation alerts will appear automatically.\n\n");

    char input[BUFFER_SIZE];
    while(1){
        // Check message queue for escalation alerts (non-blocking)
        EscalationAlert alert;
        while(mq_alerts_receive(&alert)==1){
            printf("\n\033[31m!! ESCALATION ALERT: Incident #%d | Zone %d | %s | \"%s\"\033[0m\n\n",
                alert.incident_id, alert.zone,
                priority_to_str(alert.priority), alert.description);
        }

        printf("> ");fflush(stdout);
        int c;while((c=getchar())=='\n'||c=='\r');ungetc(c,stdin);
        if(!fgets(input,sizeof(input),stdin))break;
        input[strcspn(input,"\n")]='\0';
        if(!strlen(input))continue;
        if(strcmp(input,"QUIT")==0){send_cmd("QUIT");recv_line(line,sizeof(line));printf("%s\n",line);break;}
        if(strncmp(input,"LIST",4)==0||strncmp(input,"STATS",5)==0||strncmp(input,"ALERTS",6)==0){
            send_cmd(input);printf("\n");recv_list();printf("\n");continue;
        }
        send_cmd(input);recv_line(line,sizeof(line));printf("Server: %s\n",line);
    }
    mq_alerts_close_client();
    close(sock_fd);return 0;
}

