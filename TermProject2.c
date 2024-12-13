#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h> 
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/errno.h>

#define NUM_CHILDREN 10
#define PAGE_SIZE 4096    // 4KB
#define TRUE 1
#define FALSE 0
#define TOTAL_PAGES 100 

#define TIME_QUANTUM 1
#define PAGES_PER_PROCESS 10 //프로세스마다의 페이지 수
#define FRAME_SIZE 4            // 페이지 프레임 크기
#define TOTAL_FRAMES 3         // 총 프레임 개수
#define PHYSICAL_MEMORY_SIZE (FRAME_SIZE * TOTAL_FRAMES)

// 프로세스 상태 정의
#define PROCESS_READY 0
#define PROCESS_RUNNING 1
#define PROCESS_WAITING 2

// 메시지 큐 키 정의
#define MSG_KEY 12345

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
    processes[p_num].cpu_burst = 10;
    processes[p_num].wait_burst = 10;
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
    int current_page = 0;
    
    printf("Child process %d started, waiting for signals...\n", processes[child_p_num].pid);
    
    while(1) {
        if(processes[child_p_num].is_running && !processes[child_p_num].request_sent) {
            message.page_number = current_page;
            current_page = (current_page + 1) % PAGES_PER_PROCESS;
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
        }
    }
}

// 페이지 요청 처리 함수
void handle_page_request() {
    struct msg_buffer message;
    
    // 메시지 즉시 수신 및 처리
    if(msgrcv(msgid, &message, sizeof(message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
        printf("\n[KERNEL] Received page request from process %d for page %d and offset %d\n", 
               message.process_num, message.page_number,message.offset);
        
        // 페이지 테이블 엔트리 확인
        if(page_table[message.process_num][message.page_number].valid == 0) {
            printf("Page fault: Process %d, Page %d\n", 
                   message.process_num, message.page_number);
            // TODO: 페이지 폴트 처리 로직 구현
        } else {
            printf("Page hit: Process %d accessing page %d in frame %d\n", 
                   message.process_num, 
                   message.page_number,
                   page_table[message.process_num][message.page_number].frame_number);
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

    //가상 메모리 초기화
    init_virtual_memory();
    //메인 메모리 초기화
    init_physical_memory();
    //페이지 테이블 초기화
    init_page_table();
    init_msg_queue();

    // SIGALRM 핸들러 등록
    signal(SIGALRM, alarm_handler);

    printf("Starting virtual memory simulation...\n");

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
            init_process_info(pid, i);        // 프로세스 정보 초기화
            register_process_pages(i);
            add_to_running_queue(&processes[i]); 
            printf("Created child process %d with PID: %d\n", i + 1, pid);
        }
    }

    parent_process();
      while(tick_count < 10000) {
        alarm(TIME_QUANTUM);  // 1초 후에 알람 발생
        pause();         // 1초 대기
    }

    for(int i = 0; i < NUM_CHILDREN; i++) {
        waitpid(child_pids[i], NULL, 0);
    }

     // 시뮬레이션 종료 시 메시지 큐 제거
    msgctl(msgid, IPC_RMID, NULL);



    return 0;
}