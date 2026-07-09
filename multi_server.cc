#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#define BUFSIZE 1024
#define MAX_CLIENT  64

int clnt_socks[MAX_CLIENT];
int clnt_id = 0;

void error_handling(const char *message)
{
        fputs(message, stderr);
        fputc('\n', stderr);
        exit(-1);
}

void* handle_clnt(void* arg)
{
    int clnt_sock = *(int*)arg;
    free(arg);

    int str_len = 0;
    char msg[BUFSIZE] = {0,};
    int idx = 0;

    /* read() 함수 반환값에 따른 동작 
    반환값이 양수 : 클라이언트가 보낸 메시지가 존재한다는 의미이므로, while 안쪽 동작
    반환값이 0 : 클라이언트가 연결을 종료했다는 의미이므로, while 나감
    반환값이 음수 : 에러 발생을 의미하므로, while 나감
    */
    while(0 < (str_len=read(clnt_sock, msg, sizeof(msg)))){
        int idy = 0;
        
        while(idy < clnt_id){
            if(clnt_sock != clnt_socks[idy]){ 
                write(clnt_socks[idy], msg, str_len);
            }
            idy++;
        }
    }

    for (idx=0; idx<clnt_id; idx++)
    {
        if (clnt_sock==clnt_socks[idx])
        {
            while(idx < clnt_id - 1){
                clnt_socks[idx] = clnt_socks[idx + 1];
                idx++;
            }
            break;
        }
    }
    clnt_id--;

    close(clnt_sock);
    return NULL;
}

int main(int argc, char* argv[])
{
    int serv_sock;
    int clnt_sock;
    pthread_t t_id;

    struct sockaddr_in serv_addr;
    struct sockaddr_in clnt_addr;
    unsigned int clnt_addr_size;

    if(argc != 2) {
        printf("Usage : %s <port>\n", argv[0]);
        exit(1);
    }

    /* 브로드캐스팅 중 대상 소켓이 이미 끊긴 상태로 write()하면 SIGPIPE로
       서버 프로세스 전체가 죽는다. 무시하고 write()의 -1 반환으로만 처리한다. */
    signal(SIGPIPE, SIG_IGN);

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);    /* 서버 소켓 생성 */

    if(-1 == serv_sock){
        error_handling("socket() error");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(atoi(argv[1]));

    /* 소켓에 주소 할당 */
    if(-1 == bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr) )){
        error_handling("bind() error");
    }

    if(-1 == listen(serv_sock, 5)){  /* 연결 요청 대기 상태로 진입 */
            error_handling("listen() error");
    }

    clnt_addr_size = sizeof(clnt_addr);

    while(1){
        /* 연결 요청 수락 */
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if(-1 == clnt_sock){
            error_handling("accept() error");
        }

        clnt_socks[clnt_id++] = clnt_sock;

        int* sock = (int*)malloc(sizeof(int));
        if(NULL == sock){
            close(clnt_sock);
            continue;
        }

        *sock = clnt_sock;

        pthread_create(&t_id, NULL, handle_clnt, sock);
        pthread_detach(t_id);
    }

    return 0;
}