#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#define BUFSIZE 1024
#define MAX_NICK_LEN 20

int sock;
/* 멘션(@닉네임) 감지와 귓속말 표시(누구에게 보냈는지)에 필요해서 클라이언트도
   자신의 현재 닉네임을 들고 있는다. 서버 검증 결과를 기다리지 않고, /name
   입력이 로컬 규칙(is_valid_nickname)을 통과하면 낙관적으로 갱신한다. */
char my_nickname[MAX_NICK_LEN + 1];

void error_handling(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(-1);
}

/* 서버와 동일한 규칙(공백/'|' 금지, 20자 이하)을 클라이언트에서도 확인해
   my_nickname을 잘못 갱신하지 않도록 한다. */
int is_valid_nickname(const char* name)
{
    size_t len = strlen(name);
    if (len == 0 || len > MAX_NICK_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '|' || name[i] == ' ') return 0;
    }
    return 1;
}

/* '@닉네임' 뒤가 boundary 문자(공백/문장부호/끝)일 때만 멘션으로 인정한다.
   한글 조사가 곧바로 붙는 경우(예: "@길동아")는 UTF-8 연속 바이트가 전부
   0x80 이상이라 boundary 목록에 없으므로 자동으로 "다른 닉네임의 일부"로
   취급되어 오탐되지 않는다. */
int is_mention_boundary(char c)
{
    return c == '\0' || c == ' ' || c == '\t' ||
           c == '.' || c == ',' || c == '!' || c == '?' || c == '~' || c == ':';
}

int contains_mention(const char* text, const char* nickname)
{
    if (nickname[0] == '\0') return 0;

    char pattern[MAX_NICK_LEN + 2];
    snprintf(pattern, sizeof(pattern), "@%s", nickname);
    size_t pat_len = strlen(pattern);

    const char* p = text;
    while ((p = strstr(p, pattern)) != NULL) {
        if (is_mention_boundary(p[pat_len])) return 1;
        p++;
    }
    return 0;
}

/* [SYS]/[ERR]/[WHISPER] 태그가 붙은 서버 메시지는 태그를 떼고 구분되게
   보여준다. 태그 없는 일반 브로드캐스팅(닉네임: 내용)은 그대로 출력하되,
   내 닉네임이 멘션됐으면 강조 + 비프음('\a') 처리. */
void render_incoming(char* msg)
{
    if (0 == strncmp(msg, "[SYS]", 5)) {
        printf("[SYSTEM] %s\n", msg + 5);
    } else if (0 == strncmp(msg, "[ERR]", 5)) {
        printf("[ERROR] %s\n", msg + 5);
    } else if (0 == strncmp(msg, "[WHISPER_SENT]", 14)) {
        char* sep = strchr(msg + 14, '|');
        if (sep) {
            *sep = '\0';
            printf("[귓속말 -> %s] %s\n", msg + 14, sep + 1);
        }
    } else if (0 == strncmp(msg, "[WHISPER]", 9)) {
        char* sep = strchr(msg + 9, '|');
        if (sep) {
            *sep = '\0';
            printf("\a[귓속말 <- %s] %s\n", msg + 9, sep + 1);
        }
    } else if (contains_mention(msg, my_nickname)) {
        printf("\a[멘션됨] %s\n", msg);
    } else {
        printf("%s\n", msg);
    }
}

/* 서버로부터의 메시지를 전담 수신하는 쓰레드.
   송신(stdin 대기)과 수신(read 블로킹)이 같은 쓰레드면 상대가 보낸 메시지가
   내가 엔터를 치기 전까지 화면에 안 뜨므로 쓰레드를 분리한다.

   TCP는 바이트 스트림이라 서버가 연속으로 보낸 두 메시지가 read() 한 번에
   합쳐질 수 있다. 그래서 누적 버퍼에 쌓아두고 '\n' 단위로만 잘라 렌더링한다
   (서버 쪽 send_to/broadcast가 각 메시지 끝에 '\n'을 붙여 보낸다). */
void* recv_msg(void* arg)
{
    char chunk[BUFSIZE];
    char linebuf[BUFSIZE * 4];
    int linelen = 0;
    int str_len;

    while (0 < (str_len = read(sock, chunk, sizeof(chunk)))) {
        if (linelen + str_len >= (int)sizeof(linebuf)) {
            linelen = 0;
            continue;
        }
        memcpy(linebuf + linelen, chunk, str_len);
        linelen += str_len;

        int start = 0;
        for (int i = 0; i < linelen; i++) {
            if (linebuf[i] == '\n') {
                linebuf[i] = '\0';
                if (i > start && linebuf[i - 1] == '\r') linebuf[i - 1] = '\0';
                render_incoming(linebuf + start);
                start = i + 1;
            }
        }
        if (start > 0) {
            memmove(linebuf, linebuf + start, linelen - start);
            linelen -= start;
        }
    }

    /* str_len <= 0 : 서버가 연결을 끊었거나 소켓 에러. 프로세스를 통째로 종료해
       메인 쓰레드(stdin 대기 중)도 함께 정리한다. */
    fputs("서버와의 연결이 종료되었습니다.\n", stdout);
    exit(0);
}

/* 서버와 마찬가지로 메시지 끝에 '\n'을 붙여 프레이밍한다. */
void send_line(const char* msg)
{
    write(sock, msg, strlen(msg));
    write(sock, "\n", 1);
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

    strncpy(my_nickname, argv[3], MAX_NICK_LEN);
    my_nickname[MAX_NICK_LEN] = '\0';

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

    printf("서버에 연결되었습니다. (%s님으로 접속 시도)\n", argv[3]);

    pthread_create(&recv_thread, NULL, recv_msg, NULL);
    pthread_detach(recv_thread);

    /* 서버는 닉네임이 빈 상태로 접속을 받아들이므로, 접속 직후 초기 닉네임을
       [REQ:NAME]으로 등록해야 서버가 입장 알림을 브로드캐스트한다. */
    {
        char reg[BUFSIZE];
        snprintf(reg, sizeof(reg), "[REQ:NAME]%s", argv[3]);
        send_line(reg);
    }

    while (fgets(msg, sizeof(msg), stdin) != NULL) {
        msg[strcspn(msg, "\n")] = 0;

        if (!strcmp(msg, "/quit") || !strcmp(msg, "q")) {
            break;
        }

        if (strlen(msg) == 0) continue;

        char out[BUFSIZE];
        if (0 == strncmp(msg, "/name ", 6)) {
            const char* new_name = msg + 6;
            snprintf(out, sizeof(out), "[REQ:NAME]%s", new_name);
            if (is_valid_nickname(new_name)) {
                strncpy(my_nickname, new_name, MAX_NICK_LEN);
                my_nickname[MAX_NICK_LEN] = '\0';
            }
        } else if (0 == strncmp(msg, "/w ", 3)) {
            char* rest = msg + 3;
            char* sp = strchr(rest, ' ');
            if (sp == NULL) {
                printf("사용법: /w 대상 메시지\n");
                continue;
            }
            *sp = '\0';
            snprintf(out, sizeof(out), "[REQ:WHISPER]%s|%s", rest, sp + 1);
        } else {
            snprintf(out, sizeof(out), "[CHAT]%s", msg);
        }
        send_line(out);
    }

    close(sock);
    return 0;
}
