#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#define BUFSIZE 1024

int sock;

void error_handling(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(-1);
}

/* 서버로부터의 메시지를 전담 수신하는 쓰레드.
   송신(stdin 대기)과 수신(read 블로킹)이 같은 쓰레드면 상대가 보낸 메시지가
   내가 엔터를 치기 전까지 화면에 안 뜨므로 쓰레드를 분리한다. */
void* recv_msg(void* arg)
{
    char msg[BUFSIZE];
    int str_len;

    while (0 < (str_len = read(sock, msg, sizeof(msg) - 1))) {
        msg[str_len] = 0;
        fputs(msg, stdout);
        fputc('\n', stdout);
    }

    /* str_len <= 0 : 서버가 연결을 끊었거나 소켓 에러. 프로세스를 통째로 종료해
       메인 쓰레드(stdin 대기 중)도 함께 정리한다. */
    fputs("서버와의 연결이 종료되었습니다.\n", stdout);
    exit(0);
}

int main(int argc, char* argv[])
{
    struct sockaddr_in serv_addr;
    pthread_t recv_thread;
    char msg[BUFSIZE];

    if (argc != 4) {
        printf("Usage : %s <IP> <port> <nickname>\n", argv[0]);
        exit(1);
    }

    /* 터미널이 아닌 곳(파이프/리다이렉트)으로 출력할 때 stdio가 fully-buffered로
       바뀌어 메시지가 즉시 안 보일 수 있다. 채팅 클라이언트 특성상 수신 즉시
       렌더링돼야 하므로 명시적으로 line-buffered로 고정한다. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 서버가 먼저 끊은 소켓에 write()하면 SIGPIPE로 클라이언트가 죽는다.
       write()의 -1 반환으로만 처리하도록 무시한다. */
    signal(SIGPIPE, SIG_IGN);

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (-1 == sock) {
        error_handling("socket() error");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if (-1 == connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr))) {
        error_handling("connect() error");
    }

    printf("서버에 연결되었습니다. (%s님으로 접속)\n", argv[3]);

    pthread_create(&recv_thread, NULL, recv_msg, NULL);
    pthread_detach(recv_thread);

    while (fgets(msg, sizeof(msg), stdin) != NULL) {
        msg[strcspn(msg, "\n")] = 0;

        if (!strcmp(msg, "/quit") || !strcmp(msg, "q")) {
            break;
        }

        if (strlen(msg) == 0) continue;

        write(sock, msg, strlen(msg));
    }

    close(sock);
    return 0;
}
