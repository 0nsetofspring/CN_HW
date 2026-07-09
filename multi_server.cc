#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#define BUFSIZE 1024
#define MAX_CLIENT  64
#define MAX_NICK_LEN 20

/* Phase 1의 clnt_socks[] 단순 int 배열은 접속자 식별 정보(닉네임, 접속시각)를
   담을 수 없어 구조체 배열로 교체한다. 여러 클라이언트 쓰레드 + accept 쓰레드가
   동시에 읽고 쓰므로 clients_lock으로 감싼다. */
typedef struct {
    int fd;
    char nickname[MAX_NICK_LEN + 1];
    time_t join_time;
} Client;

Client clients[MAX_CLIENT];
int clnt_cnt = 0;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

/* /search 용 대화 기록. 무제한 저장하면 메모리/검색 성능이 나빠지므로
   최근 HISTORY_CAP개만 원형 버퍼로 유지한다. 귓속말은 사적인 대화라
   여기 남기지 않고 [CHAT](공개 채팅)만 기록한다. */
#define HISTORY_CAP 200
char history[HISTORY_CAP][BUFSIZE];
int history_next = 0;
int history_count = 0;
pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;

void history_add(const char* line)
{
    pthread_mutex_lock(&history_lock);
    strncpy(history[history_next], line, BUFSIZE - 1);
    history[history_next][BUFSIZE - 1] = '\0';
    history_next = (history_next + 1) % HISTORY_CAP;
    if (history_count < HISTORY_CAP) history_count++;
    pthread_mutex_unlock(&history_lock);
}

void error_handling(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(-1);
}

/* clients_lock을 이미 보유한 상태에서만 호출할 것. */
int find_index_by_fd(int fd)
{
    for (int i = 0; i < clnt_cnt; i++) {
        if (clients[i].fd == fd) return i;
    }
    return -1;
}

/* clients_lock을 이미 보유한 상태에서만 호출할 것. */
int find_index_by_nickname(const char* nickname)
{
    for (int i = 0; i < clnt_cnt; i++) {
        if (0 == strcmp(clients[i].nickname, nickname)) return i;
    }
    return -1;
}

/* TCP는 바이트 스트림이라 write() 두 번이 상대의 read() 한 번에 합쳐질 수 있다.
   그래서 모든 메시지 끝에 '\n'을 붙여 전송하고, 받는 쪽은 '\n' 단위로 잘라
   읽는다(아래 handle_clnt 참고). 이 함수들이 그 프레이밍을 전담한다. */
void send_to(int fd, const char* msg)
{
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
}

/* exclude_fd 를 제외한 전원에게 전송. exclude_fd에 -1을 주면 전원 포함. */
void broadcast(const char* msg, int exclude_fd)
{
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < clnt_cnt; i++) {
        if (clients[i].fd != exclude_fd) {
            write(clients[i].fd, msg, strlen(msg));
            write(clients[i].fd, "\n", 1);
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

/* WHISPER(Phase 3)의 대상|메시지 구분자와 충돌하지 않도록 '|'와 공백을 금지한다. */
int is_valid_nickname(const char* name)
{
    size_t len = strlen(name);
    if (len == 0 || len > MAX_NICK_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '|' || name[i] == ' ') return 0;
    }
    return 1;
}

void handle_name_change(int clnt_sock, const char* new_name)
{
    if (!is_valid_nickname(new_name)) {
        send_to(clnt_sock, "[ERR]닉네임은 1~20자, 공백/'|' 문자 사용 불가입니다.");
        return;
    }

    char old_name[MAX_NICK_LEN + 1];
    int is_first;
    char sys_msg[BUFSIZE];

    pthread_mutex_lock(&clients_lock);
    int idx = find_index_by_fd(clnt_sock);
    if (idx == -1) {
        pthread_mutex_unlock(&clients_lock);
        return;
    }
    is_first = (clients[idx].nickname[0] == '\0');
    strcpy(old_name, clients[idx].nickname);
    strncpy(clients[idx].nickname, new_name, MAX_NICK_LEN);
    clients[idx].nickname[MAX_NICK_LEN] = '\0';
    pthread_mutex_unlock(&clients_lock);

    if (is_first) {
        snprintf(sys_msg, sizeof(sys_msg), "[SYS]%s님이 입장하셨습니다.", new_name);
    } else {
        snprintf(sys_msg, sizeof(sys_msg), "[SYS]%s님이 %s(으)로 별명을 변경하셨습니다.", old_name, new_name);
    }
    /* 본인 화면에도 입장/변경 확인이 떠야 하므로 전원(-1) 대상 브로드캐스트. */
    broadcast(sys_msg, -1);
}

void handle_chat(int clnt_sock, const char* text)
{
    char nickname[MAX_NICK_LEN + 1] = "익명";
    char out[BUFSIZE];

    pthread_mutex_lock(&clients_lock);
    int idx = find_index_by_fd(clnt_sock);
    if (idx != -1 && clients[idx].nickname[0] != '\0') {
        strcpy(nickname, clients[idx].nickname);
    }
    pthread_mutex_unlock(&clients_lock);

    snprintf(out, sizeof(out), "%s: %s", nickname, text);
    history_add(out);
    broadcast(out, clnt_sock);
}

/* [RES:LIST] payload 형식: "닉네임1,접속유지초|닉네임2,접속유지초|...".
   닉네임을 아직 등록 안 한 접속(핸드셰이크 중)은 목록에서 제외. */
void handle_list(int clnt_sock)
{
    char out[BUFSIZE] = "[RES:LIST]";
    char entry[64];
    time_t now = time(NULL);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < clnt_cnt; i++) {
        if (clients[i].nickname[0] == '\0') continue;

        long elapsed = (long)difftime(now, clients[i].join_time);
        snprintf(entry, sizeof(entry), "%s%s,%ld",
                 (strlen(out) > 10) ? "|" : "", clients[i].nickname, elapsed);

        /* MAX_CLIENT가 다 차면 한 줄이 BUFSIZE를 넘을 수 있어 안전하게 자른다. */
        if (strlen(out) + strlen(entry) < sizeof(out) - 1) {
            strcat(out, entry);
        }
    }
    pthread_mutex_unlock(&clients_lock);

    send_to(clnt_sock, out);
}

/* keyword가 포함된 과거 채팅 라인들을 [RES:SEARCH] 메시지로 하나씩 보낸다.
   결과를 '|' 등으로 합쳐 보내지 않는 이유: 채팅 내용 자체에 '|'가 자유롭게
   들어갈 수 있어(닉네임과 달리 금지하지 않음) 한 줄로 합치면 클라이언트가
   각 결과를 다시 정확히 나눌 방법이 없다. 매칭 건수만큼 개행으로 프레이밍된
   메시지를 반복 전송하면 이 문제가 아예 생기지 않는다. */
void handle_search(int clnt_sock, const char* keyword)
{
    if (keyword[0] == '\0') {
        send_to(clnt_sock, "[ERR]검색어를 입력해주세요.");
        return;
    }

    char out[BUFSIZE];
    int match_count = 0;

    pthread_mutex_lock(&history_lock);
    int oldest = (history_next - history_count + HISTORY_CAP) % HISTORY_CAP;
    for (int i = 0; i < history_count; i++) {
        int idx = (oldest + i) % HISTORY_CAP;
        if (strstr(history[idx], keyword) != NULL) {
            snprintf(out, sizeof(out), "[RES:SEARCH]%s", history[idx]);
            send_to(clnt_sock, out);
            match_count++;
        }
    }
    pthread_mutex_unlock(&history_lock);

    if (match_count == 0) {
        send_to(clnt_sock, "[RES:SEARCH]");
    }
}

/* payload 형식: "대상|할말". target에는 이미 '|'/공백이 금지돼 있으므로 첫
   '|' 는 반드시 우리가 클라이언트에서 삽입한 구분자다. 할말 안에 '|'가 더
   있어도 최초 1개만 잘라 안전하게 분리된다. */
void handle_whisper(int clnt_sock, char* payload)
{
    char* sep = strchr(payload, '|');
    if (sep == NULL) {
        send_to(clnt_sock, "[ERR]귓속말 형식이 올바르지 않습니다. /w 대상 메시지");
        return;
    }
    *sep = '\0';
    const char* target = payload;
    const char* text = sep + 1;

    char to_target[BUFSIZE];
    char to_sender[BUFSIZE];
    int found = 0;

    /* 대상의 fd를 락 밖으로 들고 나가 write()하면, 그 사이 대상이 접속을
       끊고 fd가 다른 신규 클라이언트에게 재사용될 경우 엉뚱한 사람에게
       귓속말이 전달될 수 있다. 조회~전송을 같은 lock 구간에서 처리한다. */
    pthread_mutex_lock(&clients_lock);
    int self_idx = find_index_by_fd(clnt_sock);
    int target_idx = find_index_by_nickname(target);
    if (self_idx != -1 && target_idx != -1) {
        found = 1;
        snprintf(to_target, sizeof(to_target), "[WHISPER]%s|%s", clients[self_idx].nickname, text);
        snprintf(to_sender, sizeof(to_sender), "[WHISPER_SENT]%s|%s", clients[target_idx].nickname, text);
        int target_fd = clients[target_idx].fd;
        write(target_fd, to_target, strlen(to_target));
        write(target_fd, "\n", 1);
    }
    pthread_mutex_unlock(&clients_lock);

    if (found) {
        send_to(clnt_sock, to_sender);
    } else {
        char err[BUFSIZE];
        snprintf(err, sizeof(err), "[ERR]대상을 찾을 수 없습니다: %s", target);
        send_to(clnt_sock, err);
    }
}

void process_message(int clnt_sock, char* msg)
{
    if (0 == strncmp(msg, "[REQ:NAME]", 10)) {
        handle_name_change(clnt_sock, msg + 10);
    } else if (0 == strncmp(msg, "[CHAT]", 6)) {
        handle_chat(clnt_sock, msg + 6);
    } else if (0 == strncmp(msg, "[REQ:WHISPER]", 13)) {
        handle_whisper(clnt_sock, msg + 13);
    } else if (0 == strncmp(msg, "[REQ:LIST]", 10)) {
        handle_list(clnt_sock);
    } else if (0 == strncmp(msg, "[REQ:SEARCH]", 12)) {
        handle_search(clnt_sock, msg + 12);
    } else {
        send_to(clnt_sock, "[ERR]알 수 없는 명령입니다.");
    }
}

void* handle_clnt(void* arg)
{
    int clnt_sock = *(int*)arg;
    free(arg);

    int str_len = 0;
    char chunk[BUFSIZE];
    /* 여러 read() 호출에 걸쳐 미완성 메시지를 이어 붙이는 누적 버퍼.
       한 줄('\n' 기준)이 완성될 때만 process_message로 넘긴다. */
    char linebuf[BUFSIZE * 4];
    int linelen = 0;

    /* read() 함수 반환값에 따른 동작
    반환값이 양수 : 클라이언트가 보낸 메시지가 존재한다는 의미이므로, while 안쪽 동작
    반환값이 0 : 클라이언트가 연결을 종료했다는 의미이므로, while 나감
    반환값이 음수 : 에러 발생을 의미하므로, while 나감
    */
    while (0 < (str_len = read(clnt_sock, chunk, sizeof(chunk)))) {
        if (linelen + str_len >= (int)sizeof(linebuf)) {
            /* 개행 없이 버퍼가 꽉 참 - 비정상 입력으로 보고 버린다 (오버플로 방지) */
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
                process_message(clnt_sock, linebuf + start);
                start = i + 1;
            }
        }
        if (start > 0) {
            memmove(linebuf, linebuf + start, linelen - start);
            linelen -= start;
        }
    }

    /* 접속 종료: 목록에서 제거 후 남은 유저에게 퇴장을 알린다.
       닉네임은 제거 전에 복사해둬야 브로드캐스트 메시지에 쓸 수 있다. */
    char left_name[MAX_NICK_LEN + 1] = {0,};
    pthread_mutex_lock(&clients_lock);
    int idx = find_index_by_fd(clnt_sock);
    if (idx != -1) {
        strcpy(left_name, clients[idx].nickname);
        while (idx < clnt_cnt - 1) {
            clients[idx] = clients[idx + 1];
            idx++;
        }
        clnt_cnt--;
    }
    pthread_mutex_unlock(&clients_lock);

    if (left_name[0] != '\0') {
        char sys_msg[BUFSIZE];
        snprintf(sys_msg, sizeof(sys_msg), "[SYS]%s님이 퇴장하셨습니다.", left_name);
        broadcast(sys_msg, -1);
    }

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

        pthread_mutex_lock(&clients_lock);
        if (clnt_cnt >= MAX_CLIENT) {
            /* 정원 초과 - 원본 코드엔 없던 배열 오버플로 방지 체크. 구조체 크기가
               커진 만큼 넘치면 메모리를 침범하므로 반드시 막아야 한다. */
            pthread_mutex_unlock(&clients_lock);
            send_to(clnt_sock, "[ERR]서버 접속 정원이 가득 찼습니다.");
            close(clnt_sock);
            continue;
        }
        clients[clnt_cnt].fd = clnt_sock;
        clients[clnt_cnt].nickname[0] = '\0';
        clients[clnt_cnt].join_time = time(NULL);
        clnt_cnt++;
        pthread_mutex_unlock(&clients_lock);

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
