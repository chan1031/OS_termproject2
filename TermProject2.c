#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h> 

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

// 전역 변수로 틱 카운트 추가
int tick_count = 0;

// 자식 프로세스 정보를 담는 구조체
struct Process {
   pid_t pid;          // 프로세스 ID
   int p_num;          // 프로세스 생성 순서 번호 (0,1,2...)
   int cpu_burst;      // CPU 버스트 시간
   int wait_burst;     // 대기 시간
};
// 전체 프로세스 관리를 위한 배열
struct Process processes[NUM_CHILDREN];
// 프로세스 생성 시 정보를 초기화하는 함수
void init_process_info(pid_t pid, int p_num) {
   processes[p_num].pid = pid;
   processes[p_num].p_num = p_num;
   processes[p_num].cpu_burst = 10;
   processes[p_num].wait_burst = 10;
}


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

// 물리 메모리 초기화 함수
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

// 다음 할당 가능한 페이지 인덱스를 추적하는 전역 변수
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
// 알람 핸들러 함수
void alarm_handler(int signo) {
    tick_count++;
    printf("\nTick %d...\n", tick_count);
    
    // 부모 프로세스의 스케줄링 함수 호출
    parent_process();
    
    // 다음 타임 틱을 위한 알람 설정
    alarm(1);
}


// 자식 프로세스가 실행할 함수
void child_process(int pid) {
    printf("Child process %d started\n", pid);
    while(1) {
        sleep(1);  // 임시로 1초 대기
    }
    exit(0);
}

// 부모 프로세스(커널)가 실행할 함수
void parent_process() {
    printf("Parent (Kernel) process started\n");
    //스케줄러 관리
    
    
}



int main() {
    pid_t pid;
    pid_t child_pids[NUM_CHILDREN];

    //가상 메모리 초기화
    init_virtual_memory();
    //메인 메모리 초기화
    init_physical_memory();
    //페이지 테이블 초기화
    init_page_table();

    printf("Starting virtual memory simulation...\n");

    // 자식 프로세스 생성
    for(int i = 0; i < NUM_CHILDREN; i++) {
        pid = fork();
        
        if(pid < 0) {
            fprintf(stderr, "Fork failed for process %d\n", i);
            exit(1);
        }
        else if(pid == 0) {
            child_process(i + 1);
            exit(0);
        }
        else {
            child_pids[i] = pid;
            init_process_info(pid, i);        // 프로세스 정보 초기화
            register_process_pages(pid);
            printf("Created child process %d with PID: %d\n", i + 1, pid);
        }
    }

    parent_process();

    for(int i = 0; i < NUM_CHILDREN; i++) {
        waitpid(child_pids[i], NULL, 0);
    }



    return 0;
}