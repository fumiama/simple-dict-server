#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <simple_protobuf.h>
#include "server.h"
#include "dict.h"
#include "config.h"

#if !__APPLE__
    #include <sys/sendfile.h> 
#endif

#ifdef LISTEN_ON_IPV6
    static socklen_t struct_len = sizeof(struct sockaddr_in6);
    static struct sockaddr_in6 server_addr;
#else
    static socklen_t struct_len = sizeof(struct sockaddr_in);
    static struct sockaddr_in server_addr;
#endif

static int fd;      //server fd
static pthread_t accept_threads[THREADCNT];
static DICT d;
static uint32_t* items_len;
static CONFIG* cfg;
static char *setpass, *delpass;

#define showUsage(program) printf("Usage: %s [-d] listen_port try_times dict_file config_file\n\t-d: As daemon\n", program)

int bind_server(uint16_t port, int try_times) {
    int fail_count = 0;
    int result = -1;
    #ifdef LISTEN_ON_IPV6
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_port = htons(port);
        bzero(&(server_addr.sin6_addr), sizeof(server_addr.sin6_addr));
        fd = socket(PF_INET6, SOCK_STREAM, 0);
    #else
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;
        bzero(&(server_addr.sin_zero), 8);
        fd = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    while(!~(result = bind(fd, (struct sockaddr *)&server_addr, struct_len)) && fail_count++ < try_times) sleep(1);
    if(!~result && fail_count >= try_times) {
        puts("Bind server failure!");
        return 0;
    } else{
        puts("Bind server success!");
        return 1;
    }
}

int listen_socket(int try_times) {
    int fail_count = 0;
    int result = -1;
    while(!~(result = listen(fd, 10)) && fail_count++ < try_times) sleep(1);
    if(!~result && fail_count >= try_times) {
        puts("Listen failed!");
        return 0;
    } else{
        puts("Listening....");
        return 1;
    }
}

int send_data(int accept_fd, char *data, size_t length) {
    if(!~send(accept_fd, data, length, 0)) {
        puts("Send data error");
        return 0;
    } else {
        printf("Send data: ");
        puts(data);
        return 1;
    }
}

int free_after_send(int accept_fd, char *data, size_t length) {
    int re = send_data(accept_fd, data, length);
    free(data);
    return re;
}

int send_all(THREADTIMER *timer) {
    int re = 1;
    FILE *fp = open_dict(LOCK_SH, timer->index);
    if(fp) {
        timer->lock_type = LOCK_SH;
        off_t len = 0, file_size = get_dict_size();
        sprintf(timer->data, "%u$", file_size);
        printf("Get file size: %s bytes.\n", timer->data);
        uint32_t head_len = strlen(timer->data);
        #if __APPLE__
            struct sf_hdtr hdtr;
            struct iovec headers;
            headers.iov_base = timer->data;
            headers.iov_len = head_len;
            hdtr.headers = &headers;
            hdtr.hdr_cnt = 1;
            hdtr.trailers = NULL;
            hdtr.trl_cnt = 0;
            re = !sendfile(fileno(fp), timer->accept_fd, 0, &len, &hdtr, 0);
        #else
            send_data(timer->accept_fd, timer->data, head_len);
            re = sendfile(timer->accept_fd, fileno(fp), &len, file_size) >= 0;
        #endif
        printf("Send %u bytes.\n", len);
        close_dict(LOCK_SH, timer->index);
    }
    return re;
}

int sm1_pwd(THREADTIMER *timer) {
    if(!strcmp(cfg->pwd, timer->data)) timer->status = 0;
    return !timer->status;
}

int s0_init(THREADTIMER *timer) {
    if(!strcmp("get", timer->data)) timer->status = 1;
    else if(!strcmp(setpass, timer->data)) timer->status = 2;
    else if(!strcmp(delpass, timer->data)) timer->status = 4;
    else if(!strcmp("md5", timer->data)) timer->status = 5;
    else if(!strcmp("cat", timer->data)) return send_all(timer);
    else if(!strcmp("quit", timer->data)) return 0;
    return send_data(timer->accept_fd, timer->data, timer->numbytes);
}

#define has_next(fp, ch) ((ch=getc(fp)),(feof(fp)?0:(ungetc(ch,fp),1)))

int s1_get(THREADTIMER *timer) {
    FILE *fp = open_dict(LOCK_SH, timer->index);
    timer->status = 0;
    if(fp) {
        int ch;
        timer->lock_type = LOCK_SH;
        while(has_next(fp, ch)) {
            SIMPLE_PB* spb = get_pb(fp);
            DICT* d = (DICT*)spb->target;
            if(!strcmp(timer->data, d->key)) {
                int r = close_and_send(timer, d->data, last_nonnull(d->data, ITEMSZ));
                free(spb);
                return r;
            } else free(spb);
        }
    }
    return close_and_send(timer, "null", 4);
}

int s2_set(THREADTIMER *timer) {
    FILE *fp = open_dict(LOCK_EX, timer->index);
    if(fp) {
        timer->lock_type = LOCK_EX;
        timer->status = 3;
        memset(&d, 0, sizeof(DICT));
        strncpy(d.key, timer->data, ITEMSZ-1);
        fseek(fp, 0, SEEK_END);
        return send_data(timer->accept_fd, "data", 4);
    } else {
        timer->status = 0;
        return send_data(timer->accept_fd, "erro", 4);
    }
}

int s3_set_data(THREADTIMER *timer) {
    timer->status = 0;
    uint32_t datasize = (timer->numbytes > (ITEMSZ-1))?(ITEMSZ-1):timer->numbytes;
    printf("Set data size: %u\n", datasize);
    memcpy(d.data, timer->data, datasize);
    puts("Data copy to dict succ");
    if(!set_pb(get_dict_fp(timer->index), items_len, sizeof(DICT), &d)) {
        printf("Error set data: dict[%s]=%s\n", d.key, timer->data);
        return close_and_send(timer, "erro", 4);
    } else {
        printf("Set data: dict[%s]=%s\n", d.key, timer->data);
        return close_and_send(timer, "succ", 4);
    }
}

int s4_del(THREADTIMER *timer) {
    FILE *fp = open_dict(LOCK_EX, timer->index);
    timer->status = 0;
    if(fp) {
        int ch;
        timer->lock_type = LOCK_EX;
        while(has_next(fp, ch)) {
            SIMPLE_PB* spb = get_pb(fp);
            DICT* d = (DICT*)spb->target;
            if(!strcmp(timer->data, d->key)) {
                uint32_t next = ftell(fp);
                uint32_t this = next - spb->real_len;
                fseek(fp, 0, SEEK_END);
                uint32_t end = ftell(fp);
                if(next == end) {
                    if(!ftruncate(fileno(fp), end - spb->real_len)) {
                        free(spb);
                        return close_and_send(timer, "succ", 4);
                    } else {
                        free(spb);
                        return close_and_send(timer, "erro", 4);
                    }
                } else {
                    uint32_t cap = end - next;
                    printf("this: %u, next: %u, end: %u, cap: %u\n", this, next, end, cap);
                    char* data = malloc(cap);
                    if(data) {
                        fseek(fp, next, SEEK_SET);
                        if(fread(data, cap, 1, fp) == 1) {
                            if(!ftruncate(fileno(fp), end - spb->real_len)) {
                                fseek(fp, this, SEEK_SET);
                                if(fwrite(data, cap, 1, fp) == 1) {
                                    free(data);
                                    free(spb);
                                    return close_and_send(timer, "succ", 4);
                                }
                            }
                        }
                        free(data);
                    }
                    free(spb);
                    return close_and_send(timer, "erro", 4);
                }
            } else free(spb);
        }
    }
    return close_and_send(timer, "null", 4);
}

int s5_md5(THREADTIMER *timer) {
    timer->status = 0;
    fill_md5();
    if(is_md5_equal(timer->data)) return send_data(timer->accept_fd, "null", 4);
    else return send_data(timer->accept_fd, "nequ", 4);
}

int check_buffer(THREADTIMER *timer) {
    printf("Status: %d\n", timer->status);
    switch(timer->status) {
        case -1: return sm1_pwd(timer); break;
        case 0: return s0_init(timer); break;
        case 1: return s1_get(timer); break;
        case 2: return s2_set(timer); break;
        case 3: return s3_set_data(timer); break;
        case 4: return s4_del(timer); break;
        case 5: return s5_md5(timer); break;
        default: return -1; break;
    }
}

void handle_quit(int signo) {
    printf("Handle quit with sig %d\n", signo);
    pthread_exit(NULL);
}

#define timer_pointer_of(x) ((THREADTIMER*)(x))
#define touch_timer(x) timer_pointer_of(x)->touch = time(NULL)

void accept_timer(void *p) {
    pthread_detach(pthread_self());
    THREADTIMER *timer = timer_pointer_of(p);
    uint32_t index = timer->index;
    while(accept_threads[index] && !pthread_kill(accept_threads[index], 0)) {
        sleep(MAXWAITSEC / 4);
        puts("Check accept status");
        if(time(NULL) - timer->touch > MAXWAITSEC) break;
    }
    puts("Call kill thread");
    kill_thread(timer);
    puts("Free timer");
    free(timer);
    puts("Finish calling kill thread");
}

void kill_thread(THREADTIMER* timer) {
    puts("Start killing.");
    uint32_t index = timer->index;
    pthread_t thread = accept_threads[index];
    if(thread) {
        pthread_kill(thread, SIGQUIT);
        accept_threads[index] = 0;
        puts("Kill thread.");
    }
    if(timer->accept_fd) {
        close(timer->accept_fd);
        timer->accept_fd = 0;
        puts("Close accept.");
    }
    if(timer->data) {
        free(timer->data);
        timer->data = NULL;
        puts("Free data.");
    }
    if(timer->lock_type) close_dict(timer->lock_type, timer->index);
    puts("Finish killing.");
}

void handle_pipe(int signo) {
    printf("Pipe error: %d\n", signo);
}

#define chkbuf(p) if(!check_buffer(timer_pointer_of(p))) break
#define take_word(p, w) if(timer_pointer_of(p)->numbytes > strlen(w) && strstr(buff, w) == buff) {\
                        int l = strlen(w);\
                        char store = buff[l];\
                        buff[l] = 0;\
                        ssize_t n = timer_pointer_of(p)->numbytes - l;\
                        timer_pointer_of(p)->numbytes = l;\
                        chkbuf(p);\
                        buff[0] = store;\
                        memmove(buff + 1, buff + l + 1, n - 1);\
                        buff[n] = 0;\
                        timer_pointer_of(p)->numbytes = n;\
                        printf("Split cmd: %s\n", w);\
                    }

void handle_accept(void *p) {
    pthread_detach(pthread_self());
    int accept_fd = timer_pointer_of(p)->accept_fd;
    if(accept_fd > 0) {
        puts("Connected to the client.");
        signal(SIGQUIT, handle_quit);
        signal(SIGPIPE, handle_pipe);
        pthread_t thread;
        if (pthread_create(&thread, NULL, (void *)&accept_timer, p)) puts("Error creating timer thread");
        else puts("Creating timer thread succeeded");
        send_data(accept_fd, "Welcome to simple dict server.", 31);
        timer_pointer_of(p)->status = -1;
        uint32_t index = timer_pointer_of(p)->index;
        char *buff = calloc(BUFSIZ, sizeof(char));
        if(buff) {
            timer_pointer_of(p)->data = buff;
            while(accept_threads[index] && (timer_pointer_of(p)->numbytes = recv(accept_fd, buff, BUFSIZ, 0)) > 0) {
                touch_timer(p);
                buff[timer_pointer_of(p)->numbytes] = 0;
                printf("Get %u bytes: %s\n", timer_pointer_of(p)->numbytes, buff);
                puts("Check buffer");
                take_word(p, cfg->pwd);
                take_word(p, "cat");
                take_word(p, "md5");
                take_word(p, setpass);
                take_word(p, delpass);
                if(timer_pointer_of(p)->numbytes > 0) chkbuf(p);
            }
            printf("Break: recv %u bytes\n", timer_pointer_of(p)->numbytes);
        } else puts("Error allocating buffer");
        accept_threads[index] = 0;
        kill_thread(timer_pointer_of(p));
    } else puts("Error accepting client");
}

void accept_client() {
    pid_t pid = fork();
    while (pid > 0) {      //主进程监控子进程状态，如果子进程异常终止则重启之
        wait(NULL);
        puts("Server subprocess exited. Restart...");
        pid = fork();
    }
    if(pid < 0) puts("Error when forking a subprocess.");
    else while(1) {
        puts("Ready for accept, waitting...");
        int p = 0;
        while(p < THREADCNT && accept_threads[p] && !pthread_kill(accept_threads[p], 0)) p++;
        if(p < THREADCNT) {
            printf("Run on thread No.%d\n", p);
            THREADTIMER *timer = malloc(sizeof(THREADTIMER));
            if(timer) {
                struct sockaddr_in client_addr;
                timer->accept_fd = accept(fd, (struct sockaddr *)&client_addr, &struct_len);
                timer->index = p;
                timer->touch = time(NULL);
                timer->data = NULL;
                signal(SIGQUIT, handle_quit);
                signal(SIGPIPE, handle_pipe);
                if (pthread_create(accept_threads + p, NULL, (void *)&handle_accept, timer)) puts("Error creating thread");
                else puts("Creating thread succeeded");
            } else puts("Allocate timer error");
        } else {
            puts("Max thread cnt exceeded");
            sleep(1);
        }
    }
}

int close_and_send(THREADTIMER* timer, char *data, size_t numbytes) {
    close_dict(timer->lock_type, timer->index);
    return send_data(timer->accept_fd, data, numbytes);
}

#define set_pass(pass, sps, slen, cmd) (pass=malloc(strlen(cmd)+slen+1),((pass)?(strcpy(pass,cmd),strcpy(pass+strlen(cmd),sps),1):0))

int main(int argc, char *argv[]) {
    if(argc != 5 && argc != 6) showUsage(argv[0]);
    else {
        int port = 0;
        int as_daemon = !strcmp("-d", argv[1]);
        sscanf(argv[as_daemon?2:1], "%d", &port);
        if(port > 0 && port < 65536) {
            int times = 0;
            sscanf(argv[as_daemon?3:2], "%d", &times);
            if(times > 0) {
                if(!as_daemon || (as_daemon && (daemon(1, 1) >= 0))) {
                    FILE *fp = NULL;
                    fp = fopen(argv[as_daemon?4:3], "rb+");
                    if(!fp) fp = fopen(argv[as_daemon?4:3], "wb+");
                    if(fp) {
                        fclose(fp);
                        init_dict(argv[as_daemon?4:3]);
                        fp = NULL;
                        fp = fopen(argv[as_daemon?5:4], "rb");
                        if(fp) {
                            SIMPLE_PB* spb = get_pb(fp);
                            cfg = (CONFIG*)spb->target;
                            fclose(fp);
                            int slen = strlen(cfg->sps);
                            if(set_pass(setpass, cfg->sps, slen, "set") && set_pass(delpass, cfg->sps, slen, "del")) {
                                items_len = align_struct(sizeof(DICT), 2, d.key, d.data);
                                if(items_len) {
                                    if(bind_server(port, times)) if(listen_socket(times)) accept_client();
                                } else puts("Align struct error.");
                            } else puts("Allocate memory error.");
                        } else printf("Error opening config file: %s\n", argv[as_daemon?5:4]);
                    } else printf("Error opening dict file: %s\n", argv[as_daemon?4:3]);
                } else puts("Start daemon error");
            } else printf("Error times: %d\n", times);
        } else printf("Error port: %d\n", port);
    }
    close(fd);
    exit(EXIT_FAILURE);
}
