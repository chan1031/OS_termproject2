#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h> 
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>  // 이 헤더 추가

#define NUM_CHILDREN 10
#define PAGE_SIZE 4096    // 4KB
#define TRUE 1
#define FALSE 0
#define TOTAL_PAGES 100 

#define TIME_QUANTUM 1
#define PAGES_PER_PROCESS 10 //프로세스마다의 페이지 수
#define FRAME_SIZE 4            // 페이지 프레임 크기
#define TOTAL_FRAMES 20    // 총 프레임 개수
#define PHYSICAL_MEMORY_SIZE (FRAME_SIZE * TOTAL_FRAMES)

// 프로세스 상태 정의
#define PROCESS_READY 0
#define PROCESS_RUNNING 1
#define PROCESS_WAITING 2

// 메시지 큐 키 정의
#define MSG_KEY 12345

FILE* log_file;

// 페이지 요청을 위한 메시지 구조체
struct msg_buffer {
    long msg_type;      // 메시지 타입
    int process_num;    // 프로세스 번호
    int page_number; // 요청할 페이지 번호 10개
    int offset; 
};
// 메시지 큐 ID를 저장할 전역 변수
int msgid;

// 메시지 큐 초기화 함수
void init_msg_queue() {
    // 메시지 큐 생성
    msgid = msgget(MSG_KEY, 0666 | IPC_CREAT);
    if(msgid == -1) {
        perror("msgget failed");
        exit(1);
    }
    printf("Message queue initialized with ID: %d\n", msgid);
}

// 전역 통계 변수 추가
struct Statistics {
    int total_memory_accesses;
    int total_page_faults;
    int total_page_hits;
    int total_page_replacements;
    int page_faults_per_process[NUM_CHILDREN];
    int page_hits_per_process[NUM_CHILDREN];
} stats = {0};


// 전역 변수로 틱 카운트 추가
int tick_count = 0;
// 전역 변수로 상태 플래그 추가
int is_running = 0;

// 자식 프로세스 정보를 담는 구조체
struct Process {
   pid_t pid;          // 프로세스 ID
   int p_num;          // 프로세스 생성 순서 번호 (0,1,2...)
   int cpu_burst;      // CPU 버스트 시간
   int wait_burst;     // 대기 시간
   int state;          // 프로세스 상태
   int is_running;     // 실행 상태 여부
   int request_sent;   // 페이지 요청 여부
};
// 전체 프로세스 관리를 위한 배열
struct Process processes[NUM_CHILDREN];
// 프로세스 생성 시 정보를 초기화하는 함수
void init_process_info(pid_t pid, int p_num) {
    processes[p_num].pid = pid;
    processes[p_num].p_num = p_num;
    // cpu_burst: 3~10 사이의 랜덤 값
    processes[p_num].cpu_burst = 3 + rand() % 8;  
    // wait_burst: 3~10 사이의 랜덤 값
    processes[p_num].wait_burst = 2 + rand() % 4;  // 2 + (0~3) = 2~5 사이의 값
    processes[p_num].state = PROCESS_READY;
    processes[p_num].is_running = 0;
    processes[p_num].request_sent = 0;
}

// running queue 선언
struct Process* running_queue[NUM_CHILDREN];
int running_queue_size = 0;
// waiting queue를 위한 전역 변수 추가
struct Process* waiting_queue[NUM_CHILDREN];
int waiting_queue_size = 0;

// running queue에 프로세스 추가하는 함수
void add_to_running_queue(struct Process* process) {
    if (running_queue_size < NUM_CHILDREN) {
        running_queue[running_queue_size++] = process;
        printf("Process %d added to running queue at position %d\n", process->pid, running_queue_size-1);
    }
}
/* ----------------------------------------------------------------------- */


// 프로세스의 페이지 정보를 저장할 구조체
struct Page {
    int pid;      // 프로세스 ID
    int pagenum;  // 페이지 번호 (0~9)
};

//메인 메모리 구현 part 
// 프레임 구조체 수정
struct Frame {
    int is_used;       // 프레임 사용 여부 (0: 미사용, 1: 사용중)
    struct Page page;  // 가상 메모리에서 가져온 페이지 정보
    int last_access_time; // LRU 구현을 위한 변수 추가
};
// 메인 메모리 구조체
struct PhysicalMemory {
    struct Frame frames[TOTAL_FRAMES];     // 3개의 프레임
    int free_frame_count;                 // 사용 가능한 프레임 수
};
//전역 변수로 메인 메모리 구현
struct PhysicalMemory pmem;

static int child_p_num;

// 메인 메모리 초기화 함수
void init_physical_memory() {
    // 모든 프레임을 미사용 상태로 초기화
    for(int i = 0; i < TOTAL_FRAMES; i++) {
        pmem.frames[i].is_used = 0;
        pmem.frames[i].page.pid = -1;
        pmem.frames[i].page.pagenum = -1;
        pmem.frames[i].last_access_time = -1;
    }
    pmem.free_frame_count = TOTAL_FRAMES;
    
    printf("Physical Memory Initialized:\n");
    printf("Total Frames: %d\n", TOTAL_FRAMES);
    printf("Total Size: %d bytes\n", PHYSICAL_MEMORY_SIZE);
}
/* ----------------------------------------------------------------------- */


// 가상 메모리 생성 영역
struct VirtualMemory {
    struct Page pages[TOTAL_PAGES];
};

// 전역 변수로 가상 메모리 선언
struct VirtualMemory virtual_memory;
// 가상 메모리의 현재 할당 상태를 출력하는 함수
void print_virtual_memory_status() {
    printf("\n=== Virtual Memory Status ===\n");
    for(int i = 0; i < TOTAL_PAGES; i++) {
        if(virtual_memory.pages[i].pid != -1) {
            printf("Page[%d]: Process %d, Page number %d\n",
                   i,
                   virtual_memory.pages[i].pid,
                   virtual_memory.pages[i].pagenum);
        }
    }
    printf("==========================\n\n");
}

int next_available_page = 0;

// 가상 메모리에 프로세스의 페이지 정보를 등록하는 함수
void register_process_pages(int pid) {
    // 페이지 공간이 충분한지 확인
    if (next_available_page + PAGES_PER_PROCESS > TOTAL_PAGES) {
        printf("Error: Not enough pages available in virtual memory\n");
        return;
    }

    // 프로세스의 10개 페이지 할당
    for(int i = 0; i < PAGES_PER_PROCESS; i++) {
        virtual_memory.pages[next_available_page].pid = pid;
        virtual_memory.pages[next_available_page].pagenum = i;
        next_available_page++;
        if(next_available_page == 100){
            print_virtual_memory_status();
        }
    }

    // 할당 결과 출력
    printf("Process %d allocated pages from index %d to %d\n", 
           pid, 
           next_available_page - PAGES_PER_PROCESS, 
           next_available_page - 1);
}


// 가상 메모리 초기화 함수
void init_virtual_memory() {
    // 모든 페이지 초기화
    for(int i = 0; i < TOTAL_PAGES; i++) {
        virtual_memory.pages[i].pid = -1;      // 미할당 상태
        virtual_memory.pages[i].pagenum = -1;  // 미할당 상태
    }
    printf("Virtual Memory Initialized with %d pages\n", TOTAL_PAGES);
}
/*--------------------------------------------------------------------------------- */


// 페이지 테이블 엔트리 구조체
struct PageTable {
    int frame_number;        // 매핑된 물리 메모리 프레임 번호
    int valid;              // 페이지가 물리 메모리에 있는지 여부 (0: invalid, 1: valid)
    int virtual_page_index; // 가상 메모리에서의 페이지 인덱스
};

// 전체 프로세스의 페이지 테이블 엔트리들을 관리하는 2차원 배열
// [프로세스 인덱스][페이지 번호]
struct PageTable page_table[NUM_CHILDREN][PAGES_PER_PROCESS];

void init_page_table() {
    for(int i = 0; i < NUM_CHILDREN; i++) {
        for(int j = 0; j < PAGES_PER_PROCESS; j++) {
            page_table[i][j].frame_number = -1;    // 아직 물리 메모리에 할당되지 않음
            page_table[i][j].valid = 0;           // invalid 상태
            // 가상 메모리 인덱스 계산: (프로세스 번호 * 페이지 개수) + 페이지 번호
            page_table[i][j].virtual_page_index = (i * PAGES_PER_PROCESS) + j;
        }
    }
    printf("Page Table Initialized\n");
}
/*--------------------------------------------------------------------------------- */

//로깅 관련 함수들

// 통계 업데이트 함수들
void record_page_fault(int process_num) {
    stats.total_page_faults++;
    stats.page_faults_per_process[process_num]++;
}

void record_page_hit(int process_num) {
    stats.total_page_hits++;
    stats.page_hits_per_process[process_num]++;
}

void record_page_replacement() {
    stats.total_page_replacements++;
}
// 로그 작성을 위한 함수
void write_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    fflush(log_file);
    va_end(args);
}
// 최종 통계 출력 함수
void print_final_statistics() {
    fprintf(log_file, "\n======================================================\n");
    fprintf(log_file, "Final Memory Management Statistics\n");
    fprintf(log_file, "======================================================\n\n");
    
    fprintf(log_file, "Overall Statistics:\n");
    fprintf(log_file, "Total Memory Accesses: %d\n", 
            stats.total_page_faults + stats.total_page_hits);
    fprintf(log_file, "Total Page Faults: %d\n", stats.total_page_faults);
    fprintf(log_file, "Total Page Hits: %d\n", stats.total_page_hits);
    fprintf(log_file, "Total Page Replacements: %d\n", stats.total_page_replacements);
    fprintf(log_file, "Page Fault Rate: %.2f%%\n", 
            (float)stats.total_page_faults / (stats.total_page_faults + stats.total_page_hits) * 100);
    fprintf(log_file, "Page Hit Rate: %.2f%%\n", 
            (float)stats.total_page_hits / (stats.total_page_faults + stats.total_page_hits) * 100);
    
    fprintf(log_file, "\nPer-Process Statistics:\n");
    for(int i = 0; i < NUM_CHILDREN; i++) {
        int total = stats.page_faults_per_process[i] + stats.page_hits_per_process[i];
        fprintf(log_file, "Process P%d:\n", i);
        fprintf(log_file, "  Total Accesses: %d\n", total);
        fprintf(log_file, "  Page Faults: %d\n", stats.page_faults_per_process[i]);
        fprintf(log_file, "  Page Hits: %d\n", stats.page_hits_per_process[i]);
        if(total > 0) {
            fprintf(log_file, "  Page Fault Rate: %.2f%%\n",
                    (float)stats.page_faults_per_process[i] / total * 100);
            fprintf(log_file, "  Page Hit Rate: %.2f%%\n",
                    (float)stats.page_hits_per_process[i] / total * 100);
        }
        fprintf(log_file, "\n");
    }
    
    fprintf(log_file, "======================================================\n");
}

// 로그 파일 초기화 함수
void init_logging() {
    log_file = fopen("memory_management.txt", "w");
    if (log_file == NULL) {
        perror("Failed to open log file");
        exit(1);
    }
    
    // 로그 파일 헤더 작성
    fprintf(log_file, "======================================================\n");
    fprintf(log_file, "Virtual Memory Management Simulation Log\n");
    fprintf(log_file, "======================================================\n\n");
}

// 구분선 출력 함수
void log_separator() {
    fprintf(log_file, "------------------------------------------------------\n");
}

// 메모리 접근 로깅 함수
void log_memory_access(int tick, int process_num, int page_num, int offset, int frame_num, const char* status) {
    write_log("[Tick %d] Memory Access\n", tick);
    write_log("Process: P%d\n", process_num);
    write_log("Virtual Address: Page %d, Offset 0x%x\n", page_num, offset);
    
    if (frame_num != -1) {
        write_log("Physical Address: Frame %d, Offset 0x%x\n", frame_num, offset);
    }
    
    write_log("Status: %s\n", status);
    write_log("------------------------------------------------------\n");
}

// 페이지 폴트 로깅 함수
void log_page_fault(int tick, int process_num, int page_num) {
    write_log("[Tick %d] PAGE FAULT\n", tick);
    write_log("Process: P%d\n", process_num);
    write_log("Faulting Page: %d\n", page_num);
    write_log("------------------------------------------------------\n");
}

// 페이지 테이블 변경 로깅 함수
void log_page_table_update(int tick, int process_num, int page_num, int new_frame) {
    write_log("[Tick %d] Page Table Update\n", tick);
    write_log("Process: P%d\n", process_num);
    write_log("Page Number: %d\n", page_num);
    write_log("New Frame: %d\n", new_frame);
    write_log("------------------------------------------------------\n");
}

// LRU 페이지 교체 로깅 함수
void log_page_replacement(int tick, int evicted_pid, int evicted_page, int new_pid, int new_page, int frame) {
    write_log("[Tick %d] LRU Page Replacement\n", tick);
    write_log("Evicted Process: P%d, Page: %d\n", evicted_pid, evicted_page);
    write_log("New Process: P%d, Page: %d\n", new_pid, new_page);
    write_log("Frame Number: %d\n", frame);
    write_log("------------------------------------------------------\n");
}

// 메모리 상태 스냅샷 로깅 함수
void log_memory_snapshot() {
    fprintf(log_file, "\n=== Physical Memory Snapshot ===\n");
    for(int i = 0; i < TOTAL_FRAMES; i++) {
        if(pmem.frames[i].is_used) {
            fprintf(log_file, "Frame %2d: Process P%d, Page 0x%x, Last Access: %d\n",
                    i, pmem.frames[i].page.pid, pmem.frames[i].page.pagenum,
                    pmem.frames[i].last_access_time);
        } else {
            fprintf(log_file, "Frame %2d: Free\n", i);
        }
    }
    fprintf(log_file, "Free Frames: %d\n", pmem.free_frame_count);
    log_separator();
}

// 주기적인 통계 로깅 함수
void log_statistics(int tick) {
    static int total_page_faults = 0;
    static int total_page_hits = 0;
    static int total_replacements = 0;
    
    fprintf(log_file, "\n=== Statistics at Tick %d ===\n", tick);
    fprintf(log_file, "Total Page Faults: %d\n", total_page_faults);
    fprintf(log_file, "Total Page Hits: %d\n", total_page_hits);
    fprintf(log_file, "Total Page Replacements: %d\n", total_replacements);
    fprintf(log_file, "Page Fault Rate: %.2f%%\n", 
            (float)total_page_faults / (total_page_faults + total_page_hits) * 100);
    log_separator();
}

// 로깅 종료 함수
// close_logging 함수 수정
void close_logging() {
    if (log_file != NULL) {
        print_final_statistics();
        fclose(log_file);
    }
}
/*--------------------------------------------------------------------------------- */


// running queue의 첫 번째 프로세스를 running 상태로 만드는 함수
void set_process_running() {
    if(running_queue_size > 0) {
        kill(running_queue[0]->pid, SIGUSR1);
        printf("[KERNEL] Set process %d to RUNNING state\n", running_queue[0]->pid);
    }
}

// 프로세스를 waiting 상태로 만드는 함수
void set_process_waiting(struct Process* process) {
    kill(process->pid, SIGUSR2);
    printf("[KERNEL] Set process %d to WAITING state\n", process->pid);
}
// 프로세스 실행 관련 함수들
void print_queue_status() {
    printf("\n==============================================\n");
    if(running_queue_size > 0) {
        printf("현재 실행중인 프로세스: %d번\n", running_queue[0]->p_num);
    } else {
        printf("현재 실행중인 프로세스: 없음\n");
    }
    
    // Running Queue 출력
    printf("Running Queue : |");
    for(int i = 0; i < running_queue_size; i++) {
        printf(" %d |", running_queue[i]->p_num);
    }
    printf("\n");
    
    // Waiting Queue 출력
    printf("Waiting Queue : |");
    for(int i = 0; i < waiting_queue_size; i++) {
        printf(" %d |", waiting_queue[i]->p_num);
    }
    printf("\n");
    printf("==============================================\n");
}
// 현재 실행 중인 프로세스의 CPU burst 감소
void decrease_cpu_burst() {
    if(running_queue[0] != NULL && running_queue[0]->cpu_burst > 0) {
        running_queue[0]->cpu_burst--;
        printf("Process %d's CPU burst decreased to %d\n", 
               running_queue[0]->p_num, 
               running_queue[0]->cpu_burst);
    }
}
// running queue에서 프로세스를 맨 뒤로 이동
void move_to_back_of_running_queue() {
    if(running_queue_size <= 1) return;  // 프로세스가 1개 이하면 이동 필요없음
    
    struct Process* current = running_queue[0];
    // 모든 프로세스를 한 칸씩 앞으로 이동
    for(int i = 0; i < running_queue_size - 1; i++) {
        running_queue[i] = running_queue[i + 1];
    }
    // 현재 프로세스를 맨 뒤로 이동
    running_queue[running_queue_size - 1] = current;
    
    printf("Process %d moved to back of running queue\n", current->pid);
}
// running queue에서 waiting queue로 프로세스 이동
void move_to_waiting_queue() {
    if(running_queue_size <= 0) return;
    
    struct Process* process = running_queue[0];
    set_process_waiting(process); 
    process->wait_burst = 10;  // waiting burst 초기화
    process->cpu_burst = 10;   // CPU burst도 다음을 위해 초기화

    // waiting queue에 추가
    if(waiting_queue_size < NUM_CHILDREN) {
        waiting_queue[waiting_queue_size++] = process;
        
        // running queue에서 제거 (한 칸씩 앞으로 이동)
        for(int i = 0; i < running_queue_size - 1; i++) {
            running_queue[i] = running_queue[i + 1];
        }
        running_queue_size--;
        
        printf("Process %d moved to waiting queue\n", process->pid);
    }
}
// waiting queue에서 running queue로 프로세스 이동
void move_to_running_queue(int waiting_idx) {
    if(waiting_idx >= waiting_queue_size) return;
    
    struct Process* process = waiting_queue[waiting_idx];
    process->cpu_burst = 10;  // CPU burst 초기화
    process->wait_burst = 10;  // wait burst도 다음을 위해 초기화
    
    // running queue에 추가
    if(running_queue_size < NUM_CHILDREN) {
        running_queue[running_queue_size++] = process;
        
        // waiting queue에서 제거 (한 칸씩 앞으로 이동)
        for(int i = waiting_idx; i < waiting_queue_size - 1; i++) {
            waiting_queue[i] = waiting_queue[i + 1];
        }
        waiting_queue_size--;
        
        printf("Process %d moved to running queue\n", process->pid);
    }
}
// 프로세스 번호 찾기 함수 추가
int get_process_num(pid_t pid) {
    for(int i = 0; i < NUM_CHILDREN; i++) {
        if(processes[i].pid == pid) {
            return i;
        }
    }
    return -1;
}
void process_waiting_queue() {
    if(waiting_queue_size > 0) {  // waiting queue에 프로세스가 있을 때만
        // 첫 번째 프로세스의 wait_burst만 감소
        waiting_queue[0]->wait_burst--;
        printf("Process %d's wait burst decreased to %d\n", 
               waiting_queue[0]->pid, 
               waiting_queue[0]->wait_burst);
        
        // wait_burst가 0이 되면 running queue로 이동
        if(waiting_queue[0]->wait_burst == 0) {
            move_to_running_queue(0);
        }
    }
}
// 자식 프로세스의 시그널 핸들러
void child_signal_handler(int signo) {
    int p_num = child_p_num;
    //int p_num = get_process_num(getpid());  // pid로 프로세스 번호 찾기
    if(signo == SIGUSR1) {
        processes[p_num].is_running = 1;
        processes[p_num].request_sent = 0;  // 실행 상태가 될 때마다 초기화
        printf("Process %d received SIGUSR1: Changed to RUNNING state\n", getpid());
    }
    else if(signo == SIGUSR2) {
        processes[p_num].is_running = 0;
        printf("Process %d received SIGUSR2: Changed to WAITING state\n", getpid());
    }
}

// child_process 함수 수정
void child_process(int p_num) {
    child_p_num = p_num;
    signal(SIGUSR1, child_signal_handler);
    signal(SIGUSR2, child_signal_handler);
    
    struct msg_buffer message;
    message.msg_type = 1;
    message.process_num = p_num;
    int current_page = 0;  // 0부터 시작해서 9까지 반복
    
    printf("Child process %d started, waiting for signals...\n", processes[child_p_num].pid);
    
    while(1) {
        if(processes[child_p_num].is_running && !processes[child_p_num].request_sent) {
            message.page_number = current_page;
            message.offset = rand() % PAGE_SIZE;
            
            while(1) {
                if(msgsnd(msgid, &message, sizeof(message) - sizeof(long), 0) == -1) {
                    if(errno == EINTR) continue;
                    perror("msgsnd failed");
                    exit(1);
                }
                break;
            }
            
            printf("Process %d requested page %d with offset 0x%x\n", 
                   child_p_num, message.page_number, message.offset);
            processes[p_num].request_sent = 1;

            // 다음 요청 페이지는 (current_page+1)%10
            current_page = (current_page + 1) % PAGES_PER_PROCESS;
        }
    }
}

// 페이지 요청 처리 함수
// handle_page_request() 내부 LRU 알고리즘 적용 부분
void handle_page_request() {
    static int last_snapshot_tick = 0;
    struct msg_buffer message;
    
    if(msgrcv(msgid, &message, sizeof(message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
        int proc_num = message.process_num;
        int page_num = message.page_number;
        int offset = message.offset;

        stats.total_memory_accesses++; // 전체 메모리 접근 횟수 증가
        
        // 메모리 접근 시도 로깅
        log_memory_access(tick_count, proc_num, page_num, offset, -1, "Memory Access Attempted");

        struct PageTable* pte = &page_table[proc_num][page_num];

        // 페이지 히트
        if(pte->valid == 1) {
             printf("Page Hit!! \n");
            int frame_num = pte->frame_number;
            
            stats.total_page_hits++; // 페이지 히트 수 증가
            stats.page_hits_per_process[proc_num]++; // 프로세스별 히트 수 증가
            
            log_memory_access(tick_count, proc_num, page_num, offset, 
                            frame_num, "Page Hit - Memory Access Successful");
            return;
        }

        // 페이지 폴트
        printf("Page Fault!! \n");
        stats.total_page_faults++; // 페이지 폴트 수 증가
        stats.page_faults_per_process[proc_num]++; // 프로세스별 폴트 수 증가
        log_page_fault(tick_count, proc_num, page_num);

        // 빈 프레임이 있는 경우
        if(pmem.free_frame_count > 0) {
            int free_frame = -1;
            for(int i = 0; i < TOTAL_FRAMES; i++) {
                if(!pmem.frames[i].is_used) {
                    free_frame = i;
                    break;
                }
            }

            pmem.frames[free_frame].is_used = 1;
            pmem.frames[free_frame].page.pid = proc_num;
            pmem.frames[free_frame].page.pagenum = page_num;
            pmem.free_frame_count--;

            pte->frame_number = free_frame;
            pte->valid = 1;

            log_page_table_update(tick_count, proc_num, page_num, free_frame);
            log_memory_access(tick_count, proc_num, page_num, offset, 
                            free_frame, "New Page Loaded Successfully");
        } else {
            // Optimal 교체 알고리즘 적용
            // 모든 프레임에 대해 미래 사용까지의 거리를 계산
            printf("DO: Optimal page replacement \n");
            int victim_frame = -1;
            int max_distance = -1;

            // 현재 요청 페이지 page_num에 대해
            // 각 프레임에 있는 페이지 p에 대해 future_distance 계산
            for (int i = 0; i < TOTAL_FRAMES; i++) {
                if (pmem.frames[i].is_used) {
                    int p = pmem.frames[i].page.pagenum;
                    // future_distance 계산
                    int future_distance = (p - page_num + PAGES_PER_PROCESS) % PAGES_PER_PROCESS;
                    // future_distance가 클수록 나중에 사용됨 -> victim 후보
                    if (future_distance > max_distance) {
                        max_distance = future_distance;
                        victim_frame = i;
                    }
                }
            }

            // victim_frame 선택 완료
            int evict_pid = pmem.frames[victim_frame].page.pid;
            int evict_pagenum = pmem.frames[victim_frame].page.pagenum;

            stats.total_page_replacements++; 
            
            log_page_replacement(tick_count, evict_pid, evict_pagenum, 
                                 proc_num, page_num, victim_frame);

            page_table[evict_pid][evict_pagenum].valid = 0;
            page_table[evict_pid][evict_pagenum].frame_number = -1;

            pmem.frames[victim_frame].page.pid = proc_num;
            pmem.frames[victim_frame].page.pagenum = page_num;
            // pmem.frames[victim_frame].last_access_time = tick_count; // LRU 코드 제거

            pte->frame_number = victim_frame;
            pte->valid = 1;

            log_memory_access(tick_count, proc_num, page_num, offset, 
                              victim_frame, "Page Replacement Complete");
        }

        // 100 틱마다 메모리 스냅샷과 통계 출력
        if(tick_count - last_snapshot_tick >= 100) {
            log_memory_snapshot();
            log_statistics(tick_count);
            last_snapshot_tick = tick_count;
        }
    }
}

void parent_process() {
    static int current_running_pid = -1;
    
    // running queue의 첫 번째 프로세스가 바뀌었는지 확인
    if(running_queue_size > 0 && running_queue[0]->pid != current_running_pid) {
        set_process_running();
        current_running_pid = running_queue[0]->pid;
    }

    if(running_queue_size > 0) {
         handle_page_request();
        if(running_queue[0]->cpu_burst == 0) {
            printf("\n[KERNEL] Process %d's CPU burst finished. Moving to waiting queue...\n", 
                   running_queue[0]->pid);
            move_to_waiting_queue();
            print_queue_status();
            return;
        }

        decrease_cpu_burst();
        
        if(tick_count % TIME_QUANTUM == 0) {
            printf("\n[KERNEL] Time quantum expired. Performing round robin...\n");
            move_to_back_of_running_queue();
        }
    }

    process_waiting_queue();
    print_queue_status();
}
// 알람 핸들러 함수
void alarm_handler(int signo) {
    tick_count++;
    printf("\nTick %d...\n", tick_count);
    
    parent_process();
    
 
}
/*--------------------------------------------------------------------------------- */

int main() {
    pid_t pid;
    pid_t child_pids[NUM_CHILDREN];
    time_t start_time = time(NULL);
    
    // 난수 생성기 초기화
    srand(time(NULL));
    
    // 각종 초기화
    init_virtual_memory();
    init_physical_memory();
    init_page_table();
    init_msg_queue();
    init_logging();
    
    // 타이머 설정
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 1000;  // 1000 마이크로초 = 0.001초
    timer.it_interval = timer.it_value;

    // SIGALRM 핸들러 등록
    signal(SIGALRM, alarm_handler);

    printf("\n=== Starting Virtual Memory Management Simulation ===\n");
    printf("Total Pages: %d, Total Frames: %d\n", TOTAL_PAGES, TOTAL_FRAMES);
    printf("Page Size: %d bytes\n", PAGE_SIZE);
    printf("Physical Memory Size: %d bytes\n\n", PHYSICAL_MEMORY_SIZE);

    // 자식 프로세스 생성
    for(int i = 0; i < NUM_CHILDREN; i++) {
        pid = fork();
        
        if(pid < 0) {
            fprintf(stderr, "Fork failed for process %d\n", i);
            exit(1);
        }
        else if(pid == 0) {
            child_process(i);
            exit(0);
        }
        else {
            child_pids[i] = pid;
            init_process_info(pid, i);
            register_process_pages(i);
            add_to_running_queue(&processes[i]); 
            printf("Created child process %d with PID: %d\n", i + 1, pid);
        }
    }

    // 첫 번째 프로세스 실행
    parent_process();
    
    // 타이머 시작
    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        perror("setitimer");
        exit(1);
    }
    
    printf("\nTimer started. Running simulation for 10000 ticks...\n");
    
    // 10000 틱까지 실행
    while(tick_count < 10000) {
        pause();
    }

    // 타이머 중지
    struct itimerval stop_timer = {0};
    setitimer(ITIMER_REAL, &stop_timer, NULL);

    printf("\n=== Simulation completed at tick %d ===\n", tick_count);
    time_t end_time = time(NULL);
    printf("Total simulation time: %ld seconds\n", end_time - start_time);

    // 자식 프로세스들 종료
    printf("\nTerminating child processes...\n");
    for(int i = 0; i < NUM_CHILDREN; i++) {
        kill(child_pids[i], SIGTERM);
    }

    // 자식 프로세스들이 완전히 종료될 때까지 대기
    for(int i = 0; i < NUM_CHILDREN; i++) {
        waitpid(child_pids[i], NULL, 0);
        printf("Child process %d terminated\n", child_pids[i]);
    }

    // 메시지 큐 제거
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("Failed to remove message queue");
    } else {
        printf("Message queue removed successfully\n");
    }

    // 최종 통계 출력 및 로그 파일 닫기
    printf("\nWriting final statistics to log file...\n");
    close_logging();
    
    printf("\n=== Simulation Ended Successfully ===\n");
    return 0;
}