#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>    // Required for SYS_gettid

#include "ui.h"
#include "device.h"
#include "log.h"
#include "platform.h"
#include "runtime_patcher.h"
#if PRIMESHIM_HOST_DEVICE
#include "usbdhid.h"
#endif
#include <fcntl.h>
#include <limits.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>


const char *SDKLIB_GetCurrentPathA() {return "\\\\.\\ARMFIR.ROM";}
const uint16_t *SDKLIB_GetCurrentPathW() {return u"\\\\.\\ARMFIR.ROM";}
extern int SDKLIB__OpenFile(const char *pathname, const char *mode);
extern FILE *SDKLIB__afopen(const char *pathname, const char *mode);

/* A factory-fresh profile has no A:\\dummy.DAT.  The firmware's app launcher
 * dereferences the handle from SDKLIB__OpenFile, and the built-in fonts/help
 * resources live in this database.  The official A: volume carries
 * programs/misc/armfir.dat, which is the same generation and contains the
 * Prime Sans faces, so use it as A:\\dummy.DAT.  Only if that file is missing
 * do we fall back to a blank image. */
static void primeshim_ensure_dummy(void) {
    FILE *probe = SDKLIB__afopen("A:\\dummy.DAT", "rb");
    if (probe) {
        fclose(probe);
        return;
    }

    FILE *source = SDKLIB__afopen("A:\\programs\\misc\\armfir.dat", "rb");
    if (!source) {
        source = SDKLIB__afopen("A:\\PROGRAMS\\MISC\\ARMFIR.DAT", "rb");
    }
    if (source) {
        long expected = -1;
        if (fseek(source, 0, SEEK_END) == 0) {
            expected = ftell(source);
            fseek(source, 0, SEEK_SET);
        }
        FILE *dest = SDKLIB__afopen("A:\\dummy.DAT", "wb");
        if (dest) {
            unsigned char *buffer = malloc(256 * 1024);
            size_t count;
            long total = 0;
            if (buffer) {
                while ((count = fread(buffer, 1, 256 * 1024, source)) > 0) {
                    if (fwrite(buffer, 1, count, dest) != count) break;
                    total += (long)count;
                }
                free(buffer);
            }
            fflush(dest);
            fclose(dest);
            fclose(source);
            if (total > 0 && (expected < 0 || total == expected)) {
                PS_LOG_INFO("fs", "created A:\\dummy.DAT from A:\\programs\\misc\\armfir.dat (%ld bytes)", total);
                return;
            }
            PS_LOG_INFO("fs", "cannot copy A:\\programs\\misc\\armfir.dat");
        } else {
            fclose(source);
        }
    }

    FILE *blank = SDKLIB__afopen("A:\\dummy.DAT", "wb");
    if (!blank) {
        PS_LOG_INFO("fs", "cannot create blank A:\\dummy.DAT");
        return;
    }
    if (ftruncate(fileno(blank), 8896512) != 0) {
        PS_LOG_INFO("fs", "cannot size blank A:\\dummy.DAT: %s", strerror(errno));
    }
    fclose(blank);
    PS_LOG_INFO("fs", "created blank A:\\dummy.DAT (8896512 bytes) for first run");
}

void* SDKLIB_ProgramIsRunningA(char* path){
    static void* process_struct=NULL;
    /*struct process{
        char[80] executable_name;
        int slot_index;
        int is_registered;
        int is_running; // using by prime
        ....unk
        108: wchar* longname
        112 datfile
        116 elffile
        //... length 152
    }*/
    if(!strcmp(path,"\\\\.\\ARMFIR.ROM")){
        if(!process_struct) process_struct=malloc(152);
        *(uint32_t*)(process_struct+88)=1;
        primeshim_ensure_dummy();
        uint32_t dat_handle = SDKLIB__OpenFile("A:\\dummy.DAT","rb");
        if (!dat_handle) {
            PS_LOG_INFO("fs", "A:\\dummy.DAT is not readable");
        }
        *(uint32_t*)(process_struct+112)=dat_handle;
        return process_struct;
    }
}
int SDKLIB__LoadLibraryA(char* path){
    if(!strcmp(path,"sdklib.dll")) return 0x5d11bd11;//magic value to recognize
    return 0;
}
void SDKLIB__GetModuleFileNameA(int handle,char* buff,int len){
    if(handle==0x5d11bd11){
        strncpy(buff,"A:\\WINDOW\\SYSTEM\\SDKLIB.DLL",len);
    }
}
int SDKLIB_WriteComDebugMsg(const char* fmt,...)
{
    char message[1024];
    size_t length;
    va_list args;

    if (!fmt) return 0;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    length = strlen(message);
    while (length && (message[length - 1] == '\n' ||
                      message[length - 1] == '\r'))
        message[--length] = '\0';
    PS_LOG_DEBUG("firmware", "%s", message);
    return 0;
}


 // ====== File Operations ======

//-------------------------Essential---------------------------
// ------------------------- 精准 UAF 捕获器 ---------------------------
/*#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

// 我们需要在内存块前面藏一个 Header，用来在 free 的时候知道原来分配了多大
typedef struct {
    size_t mmap_size;
    uint32_t magic;
} ChunkHeader;

#define PAGE_SIZE 4096
#define MAGIC_NUM 0xDEADBEEF

void *SDKLIB_lmalloc(size_t size) {
    // 实际分配大小 = 用户需要的大小 + Header大小
    size_t total_size = size + sizeof(ChunkHeader);
    // 向上取整到页大小的整数倍
    size_t mmap_size = ((total_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    // 使用 mmap 匿名映射分配内存
    void *mem = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    // 填充 Header
    ChunkHeader *h = (ChunkHeader *)mem;
    h->mmap_size = mmap_size;
    h->magic = MAGIC_NUM;

    // 返回给 RTOS 的是指针往后偏移 Header 的位置
    return (void *)((uint8_t *)mem + sizeof(ChunkHeader));
}

void SDKLIB__lfree(void *ptr) {
    if (!ptr) return;

    ChunkHeader *h = (ChunkHeader *)((uint8_t *)ptr - sizeof(ChunkHeader));

    // 检查是不是我们分配的内存
    if (h->magic == MAGIC_NUM) {
        // 核心魔法：不释放内存，而是直接剥夺读写权限！(Poisoning)
        // 只要 RTOS 发生 UAF 再次读写这个 ptr，立刻触发 SIGSEGV！
        mprotect((void *)h, h->mmap_size, PROT_NONE);
    } else {
        // 防止 RTOS 释放非堆内存或者静态内存导致崩溃
        // (可选: 在这里打印警告)
    }
}

void *SDKLIB_lcalloc(size_t n1, size_t n2) {
    size_t total = n1 * n2;
    void *ptr = SDKLIB_lmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *SDKLIB_lrealloc(void* ptr, size_t new_size) {
    if (!ptr) return SDKLIB_lmalloc(new_size);
    if (new_size == 0) { SDKLIB__lfree(ptr); return NULL; }

    ChunkHeader *h = (ChunkHeader *)((uint8_t *)ptr - sizeof(ChunkHeader));
    if (h->magic != MAGIC_NUM) return NULL; // 异常块

    // 简单粗暴的 realloc 实现：新分配 -> 拷贝 -> 释放旧的
    void *new_ptr = SDKLIB_lmalloc(new_size);
    if (new_ptr) {
        size_t old_user_size = h->mmap_size - sizeof(ChunkHeader);
        size_t copy_size = (old_user_size < new_size) ? old_user_size : new_size;
        memcpy(new_ptr, ptr, copy_size);
        SDKLIB__lfree(ptr); // 剥夺旧块权限
    }
    return new_ptr;
}*/
// ----------------------------------------------------------------------
void *SDKLIB_lcalloc(size_t n1,size_t n2){return calloc(n1,n2);}
void SDKLIB__lfree(void *ptr){free(ptr);}
/* Record the firmware's second 320x240x4 buffer so the display backend can
 * mirror it (see PRIMESHIM_MIRROR_OFFSCREEN in the PC backend). */
static void *g_offscreen_buffer;

void *primeshim_offscreen_buffer(void) { return g_offscreen_buffer; }

void *SDKLIB_lmalloc(size_t size) {
    void *p = malloc(size);

    if (p && size == 307200 && !g_offscreen_buffer) {
        g_offscreen_buffer = p;
        PS_LOG_INFO("alloc", "offscreen 320x240 buffer at %p", p);
    }
    return p;
}
void *SDKLIB_lrealloc(void* ptr, size_t new_size){return realloc(ptr,new_size);}
//-------------------------------------------------------------
typedef pthread_mutex_t threading_waitable_t;
typedef pthread_t thread_t;

typedef struct critical_section_t {
    int magic_count;      // 0x202
    thread_t *thr;        // Owner thread (stored as pointer)
    int entered_count;    // Recursion counter
    threading_waitable_t* wait_state; // The OS mutex
} critical_section_t;
// --- User Definitions End ---

#define CS_MAGIC_VAL 0x202

// 初始化
void SDKLIB_OSInitCriticalSection(critical_section_t *cs) {
    if (cs == NULL) return;

    // 1. 初始化 Magic Number
    cs->magic_count = CS_MAGIC_VAL;
    cs->entered_count = 0;

    // 2. 为互斥锁分配内存并初始化
    cs->wait_state = (threading_waitable_t*)malloc(sizeof(threading_waitable_t));
    if (cs->wait_state) {
        // 使用默认属性初始化互斥锁 (非递归，我们在逻辑层实现递归)
        pthread_mutex_init(cs->wait_state, NULL);
    }

    // 3. 为线程ID存储分配内存
    // 因为结构体里 thread_t *thr 是指针，我们需要一块内存来存放具体的 ID
    cs->thr = (thread_t*)malloc(sizeof(thread_t));
    if (cs->thr) {
        memset(cs->thr, 0, sizeof(thread_t));
    }
}

// 进入临界区 (支持重入)
void SDKLIB_OSEnterCriticalSection(critical_section_t *cs) {
    if (cs == NULL || cs->magic_count != CS_MAGIC_VAL) return;

    thread_t current_thread = pthread_self();

    // 检查是否发生重入 (Recursion)
    // 注意：读取 cs->thr 在这里是安全的。
    // 如果我们是拥有者，没人能改它。
    // 如果我们不是拥有者，且 entered_count > 0，那一定是别人，pthread_equal 会返回 false。
    if (cs->entered_count > 0) {
        if (pthread_equal(*cs->thr, current_thread)) {
            // 当前线程已经持有锁，增加计数器并直接返回
            cs->entered_count++;
            return;
        }
    }

    // 如果不是当前线程持有，或者是第一次进入，则获取物理锁
    // 这一步会阻塞，直到其他线程释放锁
    pthread_mutex_lock(cs->wait_state);

    // 获取到锁之后，标记当前线程为拥有者
    *cs->thr = current_thread;
    cs->entered_count = 1;
}

// 离开临界区
void SDKLIB_OSLeaveCriticalSection(critical_section_t *cs) {
    if (cs == NULL || cs->magic_count != CS_MAGIC_VAL) return;

    // 只有锁的拥有者才能释放
    // (虽然逻辑上如果调用正确不该出现非拥有者释放的情况，但加一层保险)
    thread_t current_thread = pthread_self();
    if (cs->entered_count > 0 && pthread_equal(*cs->thr, current_thread)) {

        cs->entered_count--;

        // 只有当计数器归零时，才真正释放物理锁
        if (cs->entered_count == 0) {
            // 清除拥有者信息 (可选，但在调试时很有用)
            memset(cs->thr, 0, sizeof(thread_t));

            // 释放互斥锁
            pthread_mutex_unlock(cs->wait_state);
        }
    }
}

// 删除临界区
void OSDeleteCriticalSection(critical_section_t *cs) {
    if (cs == NULL || cs->magic_count != CS_MAGIC_VAL) return;

    // 销毁并释放 Mutex
    if (cs->wait_state) {
        pthread_mutex_destroy(cs->wait_state);
        free(cs->wait_state);
        cs->wait_state = NULL;
    }

    // 释放线程ID内存
    if (cs->thr) {
        free(cs->thr);
        cs->thr = NULL;
    }

    // 清除 Magic，防止再次使用
    cs->magic_count = 0;
    cs->entered_count = 0;
}

uint32_t SDKLIB_OSSleep(uint32_t time) {
    struct timespec ts = {
        .tv_sec = time / 1000,
        .tv_nsec = (time % 1000) * 1000000
    };
    nanosleep(&ts, NULL);
    return time;
}
typedef struct {
	uint16_t Year;
	uint16_t Month;
	uint16_t DayOfWeek;
	uint16_t Day;
	uint16_t Hour;
	uint16_t Minute;
	uint16_t Second;
	uint16_t Milliseconds;
} SystemTime;

SystemTime* SDKLIB_GetSysTime(SystemTime* sysTime) {
    struct timeval tv;
    struct tm* parts;

    gettimeofday(&tv, NULL);
    parts = localtime(&tv.tv_sec);

    sysTime->Year = parts->tm_year + 1900;
    sysTime->Month = parts->tm_mon + 1;
    sysTime->DayOfWeek = parts->tm_wday;
    sysTime->Day = parts->tm_mday;
    sysTime->Hour = parts->tm_hour;
    sysTime->Minute = parts->tm_min;
    sysTime->Second = parts->tm_sec;
    sysTime->Milliseconds = (uint16_t)(tv.tv_usec / 1000);

    return sysTime;
}

SystemTime* SDKLIB_SetSysTime(SystemTime* sysTime) {
    struct tm tm_time;
    struct timeval tv;
    time_t t;
    if (getuid() != 0) {
        PS_LOG_ERROR("system", "setting the system time requires root");
        return NULL;
    }
    tm_time.tm_year = sysTime->Year - 1900; // tm_year is years since 1900
    tm_time.tm_mon = sysTime->Month - 1;    // tm_mon is 0-11
    tm_time.tm_mday = sysTime->Day;
    tm_time.tm_hour = sysTime->Hour;
    tm_time.tm_min = sysTime->Minute;
    tm_time.tm_sec = sysTime->Second;
    tm_time.tm_isdst = -1; // Let the system determine if DST is in effect

    t = timegm(&tm_time);
    if (t == (time_t)-1) {
        PS_LOG_ERROR("system", "invalid date passed to SetSysTime");
        return NULL;
    }
    tv.tv_sec = t;
    tv.tv_usec = sysTime->Milliseconds * 1000;

    if (settimeofday(&tv, NULL) != 0) {
        PS_LOG_ERRNO_ERROR("system", errno, "settimeofday failed");
        return NULL;
    }

    PS_LOG_INFO("system", "time set to %d-%02d-%02d %02d:%02d:%02d.%03u",
                sysTime->Year, sysTime->Month, sysTime->Day, sysTime->Hour,
                sysTime->Minute, sysTime->Second, sysTime->Milliseconds);

    return sysTime;
}

typedef struct {
    void* (*real_func)(void*);
    void* real_arg;
    pid_t creator_tid;
} thread_context_t;

// The "Wrapper" function that runs first in the new thread
void* thread_wrapper(void* context) {
    thread_context_t* ctx = (thread_context_t*)context;

    // 1. Get the NEW thread's TID (as seen in htop)
    pid_t my_tid = syscall(SYS_gettid);

    PS_LOG_DEBUG("thread", "started tid=%d creator=%d pid=%d", my_tid,
                 ctx->creator_tid, getpid());

    // 2. Extract original data and free context
    void* (*func)(void*) = ctx->real_func;
    void* arg = ctx->real_arg;
    free(ctx);

    /* Arm this thread's runtime interception only at the final transition
     * into firmware code. */
    if (runtime_patcher_enter_firmware_thread() != 0) {
        PS_LOG_ERRNO_ERROR("thread", errno,
                           "cannot enable syscall dispatch");
        return NULL;
    }
    {
        void *result = func(arg);
        runtime_patcher_leave_firmware_thread();
        return result;
    }
}

pthread_t* SDKLIB_OSCreateThread(void* func, void* arg) {
    pthread_t *thread = malloc(sizeof(pthread_t));
    if (thread == NULL) return NULL;

    // Capture the CREATOR'S TID (the thread calling this function)
    pid_t creator_tid = syscall(SYS_gettid);

    // Create a context to pass into the wrapper
    thread_context_t* ctx = malloc(sizeof(thread_context_t));
    ctx->real_func = (void* (*)(void*))func;
    ctx->real_arg = arg;
    ctx->creator_tid = creator_tid;

    PS_LOG_DEBUG("thread", "tid=%d launching firmware function %p",
                 creator_tid, func);

    int result = pthread_create(thread, NULL, thread_wrapper, ctx);

    if (result != 0) {
        free(ctx);
        free(thread);
        return NULL;
    }

    return thread;
}


bool SDKLIB_OSSetThreadPriority(pthread_t *thread, short new_slot) {
    PS_LOG_TRACE("thread", "SetThreadPriority thread=%p slot=%d",
                 (void *)thread, new_slot);
    struct sched_param param;
    int policy;
    int min_prio, max_prio;

    // Get current scheduling policy
    if (pthread_getschedparam(*thread, &policy, &param) != 0) {
        return false;
    }

    // Determine priority range for the policy
    min_prio = sched_get_priority_min(policy);
    max_prio = sched_get_priority_max(policy);

    // Map the slot number to Linux priority range
    // Assuming Besta RTOS has 32 slots (0-31) like your example shows 8 and 18
    // We'll map slot 0 to min_prio and slot 31 to max_prio
    if(new_slot <0 || new_slot >63)
      PS_LOG_ERROR("thread", "illegal priority slot %d", new_slot);
    int linux_prio = min_prio + (new_slot * (max_prio - min_prio)) / 63;

    param.sched_priority = linux_prio;

    if (pthread_setschedparam(*thread, policy, &param) != 0) {
        return false;

    }

    return true;
}



//---------------------event---------------------------


typedef enum wait_result_e {
    /** Timeout before the event is set. */
    WAIT_RESULT_TIMEOUT = 0x82,
    /** The event is set. */
    WAIT_RESULT_RESOLVED = 0x83,
    /** An error occurred. */
    WAIT_RESULT_ERROR = 0x84,
} wait_result_t;

// 将 pthread 原语嵌入到结构体中
// 注意：虽然这是一个"不透明指针"，但在模拟器内部我们需要具体的定义
typedef struct event_s {
    /** Magic. Always 0x201. */
    int magic;
    /** Flag value. 1 is set and 0 is clear. */
    int flag;
    /** Set to non-0 will inhibit the event from getting cleared after a OSWaitForEvent() is resolved. */
    short latch_on;

    // 原逆向结构中的 wait_state 和 padding 在模拟实现中用作 pthread 载体
    // 实际内存布局并不重要，因为这是一个指针句柄，只要模拟器内部一致即可
    pthread_mutex_t mutex;
    pthread_cond_t cond;

} event_t;

// ---------------------------------------------------------
// 实现部分
// ---------------------------------------------------------

/**
 * @brief Create an event descriptor.
 * @param latch_on Set to non-0 (manual reset), 0 (auto reset).
 * @param flag The initial flag value (1 = signaled, 0 = non-signaled).
 * @return The event descriptor.
 */
event_t *SDKLIB_OSCreateEvent(short latch_on, int flag) {
    PS_LOG_TRACE("event", "OSCreateEvent latch=%d flag=%d", latch_on, flag);

    event_t* ev = (event_t*)malloc(sizeof(event_t));
    if (!ev) return NULL;

    // 初始化逆向字段
    ev->magic = 0x201;
    ev->latch_on = latch_on;
    ev->flag = (flag != 0) ? 1 : 0; // 规范化为 0 或 1

    // 初始化 Pthread 原语
    if (pthread_mutex_init(&ev->mutex, NULL) != 0) {
        free(ev);
        return NULL;
    }
    if (pthread_cond_init(&ev->cond, NULL) != 0) {
        pthread_mutex_destroy(&ev->mutex);
        free(ev);
        return NULL;
    }

    return ev;
}

/**
 * @brief Set the event flag.
 * @details This sets the event_t::flag to 1.
 * @param event The event context.
 * @retval true ok
 * @retval false ng
 */
bool SDKLIB_OSSetEvent(event_t *event) {
    PS_LOG_TRACE("event", "OSSetEvent event=%p", (void *)event);
    if (!event) return false;

    pthread_mutex_lock(&event->mutex);

    event->flag = 1;

    // 如果是 latch_on (手动复位)，通常唤醒所有等待者
    // 如果是 auto reset (非 latch_on)，通常唤醒一个
    // 但为了稳健性，broadcast 总是安全的，唤醒的线程会再次检查 flag
    pthread_cond_broadcast(&event->cond);

    pthread_mutex_unlock(&event->mutex);
    return true;
}

/**
 * @brief Reset the event flag.
 * @details This sets the event_t::flag to 0.
 * @param event The event context.
 * @retval true ok
 * @retval false ng
 */
bool SDKLIB_OSResetEvent(event_t *event) {
    PS_LOG_TRACE("event", "OSResetEvent event=%p", (void *)event);
    if (!event) return false;

    pthread_mutex_lock(&event->mutex);
    event->flag = 0;
    pthread_mutex_unlock(&event->mutex);
    return true;
}

/**
 * @brief Wait for an event.
 * @param event The event context.
 * @param timeout Timeout in OSSleep() units (assuming ms).
 * @return The result.
 */
wait_result_t SDKLIB_OSWaitForEvent(event_t *event, short timeout) {
    PS_LOG_TRACE("event", "OSWaitForEvent event=%p timeout=%d",
                 (void *)event, timeout);
    if (!event) return WAIT_RESULT_ERROR;

    pthread_mutex_lock(&event->mutex);

    // 1. 检查是否已经 Signaled
    if (event->flag == 1) {
        // 如果不是 latch_on (即自动复位模式)，则在这里复位
        if (event->latch_on == 0) {
            event->flag = 0;
        }
        pthread_mutex_unlock(&event->mutex);
        return WAIT_RESULT_RESOLVED;
    }

    // 2. 如果超时时间为 0，且当前未 Signaled，直接返回超时
    if (timeout == 0) {
        pthread_mutex_unlock(&event->mutex);
        return WAIT_RESULT_TIMEOUT;
    }

    // 3. 准备超时结构
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // 处理无限等待的情况 (假设负数表示无限等待，视具体 OS 实现而定，这里参考常用逻辑)
    // 如果逆向明确 short timeout 是无符号的 tick，则需要修改此处逻辑
    bool infinite = (timeout < 0);

    if (!infinite) {
        ts.tv_sec += timeout / 1000;
        ts.tv_nsec += (timeout % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
    }

    int rc = 0;
    while (event->flag == 0 && rc == 0) {
        if (infinite) {
            rc = pthread_cond_wait(&event->cond, &event->mutex);
        } else {
            rc = pthread_cond_timedwait(&event->cond, &event->mutex, &ts);
        }
    }

    wait_result_t result;

    if (event->flag == 1) {
        // 等待成功
        if (event->latch_on == 0) {
            event->flag = 0; // 自动复位
        }
        result = WAIT_RESULT_RESOLVED;
    } else if (rc == ETIMEDOUT) {
        // 超时
        result = WAIT_RESULT_TIMEOUT;
        PS_LOG_TRACE("event", "OSWaitForEvent timed out");
    } else {
        // 其他错误
        result = WAIT_RESULT_ERROR;
        PS_LOG_ERROR("event", "OSWaitForEvent failed with status %d", rc);
    }

    pthread_mutex_unlock(&event->mutex);
    return result;
}

/**
 * @brief Destroy the event descriptor.
 * @param event The event context.
 * @retval true ok
 * @retval false ng
 */
bool SDKLIB_OSCloseEvent(event_t *event) {
    PS_LOG_TRACE("event", "OSCloseEvent event=%p", (void *)event);
    if (!event) return false;

    // 防止在销毁时有其他线程操作，先加锁（虽然此时理论上不应有竞争）
    pthread_mutex_lock(&event->mutex);
    pthread_mutex_unlock(&event->mutex);

    pthread_cond_destroy(&event->cond);
    pthread_mutex_destroy(&event->mutex);

    free(event);
    return true;
}

//----------ui event--------

static pthread_mutex_t events_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t events_cond = PTHREAD_COND_INITIALIZER;
static ui_event_prime_s packet_queue[8];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

bool EnqueuePacketBlocking(ui_event_prime_s* packet, const int *keep_running) {
    struct timespec deadline;

    pthread_mutex_lock(&events_mutex);
    while (queue_count >= 8 && (!keep_running || *keep_running)) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 50000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            ++deadline.tv_sec;
        }
        pthread_cond_timedwait(&events_cond, &events_mutex, &deadline);
    }
    if (keep_running && !*keep_running) {
        pthread_mutex_unlock(&events_mutex);
        return false;
    }
    memcpy(&packet_queue[queue_tail], packet, sizeof(ui_event_prime_s));
    queue_tail = (queue_tail + 1) % 8;
    queue_count++;
    pthread_cond_signal(&events_cond);
    pthread_mutex_unlock(&events_mutex);
    return true;
}

void EnqueuePacket(ui_event_prime_s* packet) {
    pthread_mutex_lock(&events_mutex);

    if (queue_count < 8) {
        //packet_queue[queue_tail] = *packet;
        memcpy(&packet_queue[queue_tail],packet,sizeof(ui_event_prime_s));
        queue_tail = (queue_tail + 1) % 8;
        queue_count++;
        pthread_cond_signal(&events_cond);
    } else {
        PS_LOG_WARN("event", "event queue full; dropping frame");
    }

    pthread_mutex_unlock(&events_mutex);
}

uint32_t SDKLIB_GetEvent(ui_event_prime_s* event_prime) {
    PS_LOG_TRACE("event", "GetEvent output=%p", (void *)event_prime);
    struct timespec ts;
    struct timeval tp;
    int rc = 0;
    static struct timeval last_heartbeat = {0, 0};

    memset(event_prime, 0, sizeof(ui_event_prime_s));

    pthread_mutex_lock(&events_mutex);
    #define VERSION_AFTER_20250915
    #ifdef VERSION_AFTER_20250915
    if (queue_count == 0) {
        // Calculate time until next second boundary
        gettimeofday(&tp, NULL);

        // Check if we need to trigger immediately (more than 1 second since last heartbeat)
        if (last_heartbeat.tv_sec != 0) {
            long long time_since_last = (tp.tv_sec - last_heartbeat.tv_sec) * 1000000LL +
                                       (tp.tv_usec - last_heartbeat.tv_usec);
            if (time_since_last >= 1000000LL) {  // 1 second in microseconds
                // Trigger immediately
                PS_LOG_TRACE("event", "emitting catch-up heartbeat");
                event_prime->event_type = UI_EVENT_TYPE_INDICATOR_TIMER;
                last_heartbeat = tp;
                pthread_mutex_unlock(&events_mutex);
                return 0;
            }
        }

        // Calculate time until next second boundary
        ts.tv_sec = tp.tv_sec + 1;
        ts.tv_nsec = 0;  // Beginning of next second

        rc = pthread_cond_timedwait(&events_cond, &events_mutex, &ts);
    }

    if (rc == ETIMEDOUT) {
        // Generate Heartbeat at second boundary
        PS_LOG_TRACE("event", "emitting heartbeat");
        event_prime->event_type = UI_EVENT_TYPE_INDICATOR_TIMER;
        gettimeofday(&last_heartbeat, NULL);
    }
    else if (queue_count > 0) {
    #else
    if (queue_count == 0) {
        rc = pthread_cond_wait(&events_cond, &events_mutex);
    }
    if (queue_count > 0) {
    #endif
        // Pop the oldest packet
        memcpy(event_prime,&packet_queue[queue_head],sizeof(ui_event_prime_s));
        //*event_prime = packet_queue[queue_head];
        queue_head = (queue_head + 1) % 8;
        queue_count--;
        pthread_cond_broadcast(&events_cond);

        if (primeshim_log_enabled(PRIMESHIM_LOG_TRACE) &&
            (event_prime->event_type == UI_EVENT_TYPE_TOUCH ||
             event_prime->event_type == UI_EVENT_TYPE_KEY_BATCH)) {
            UIMultipressEvent *input = &event_prime->multipress_events[0];
            PS_LOG_TRACE("input",
                         "firmware dequeued batch=0x%x type=%u value=%u,%u",
                         event_prime->event_type, input->type, input->touch_x,
                         input->touch_y);
        }

    }
    else {
        event_prime->event_type = UI_EVENT_TYPE_INVALID;
    }


    pthread_mutex_unlock(&events_mutex);
    return 0;
}

//------misc----------
#pragma pack(push, 1)
typedef struct _MASTER_ID_INFO {
	char a1[24];//EA656XXX.DAT
	char a2[16];//V0.048.635
	char a3[16];//V0.050.640 --in use
	uint16_t a4;//1
	char a5[16];//7382000058004 --in use
	uint32_t a6;//0x409
	char a7[16];//V1.21
	char a8[16];//EA656XXX.ROM
	uint16_t a9;//1 --in use
	uint16_t a10;//1 --in use
	char a11[58];        // 从属ID（MAC、IMEI 或其他）
	//0x70 6e 0x28 0x3a
} MASTER_ID_INFO;//length 172
#pragma pack(pop)

uint32_t SDKLIB_GetMasterIDInfo(struct _MASTER_ID_INFO* ptr) {
	memset(ptr, 0, sizeof(MASTER_ID_INFO));
	strcpy(ptr->a3, "V0.050.640");
	strcpy(ptr->a5, "7382000058004");
	ptr->a9=1;
	ptr->a10=1;
	return 0;
}

/*
 * The 2025 firmware's internal reference-release helper dereferences its
 * argument before checking it. Some normal teardown paths pass a null object,
 * so provide the null-safe behavior that the original RTOS runtime supplied.
 */
uint32_t PRIMESHIM_FirmwareReleaseReference(void *object) {
    uint16_t *reference_count;
    uint8_t flags;

    if (!object) return 1;

    reference_count = (uint16_t *)object;
    flags = *((uint8_t *)object + 2);
    if (!(flags & 0x10)) return 1;

    *reference_count = (uint16_t)(*reference_count - 1);
    return *reference_count;
}

uint32_t SDKLIB_BatteryLowCheck(){sched_yield();return 0;}
int SDKLIB_SysPowerOff(void)
{
#if !PRIMESHIM_HOST_DEVICE
    /* Only the calculator owns /sys/power/state; desktop and Android hosts
     * must never suspend the machine running the emulator. */
    PS_LOG_INFO("power", "power-off request ignored on this host");
    return 0;
#else
    #define POWER_STATE_PATH "/sys/power/state"
    #define SUSPEND_COMMAND "mem"
    int fd;
    ssize_t bytes_written;

    PS_LOG_INFO("power", "suspending system to RAM");

    fd = open(POWER_STATE_PATH, O_WRONLY);
    if (fd < 0) {
        PS_LOG_ERRNO_ERROR("power", errno, "cannot open " POWER_STATE_PATH);
        return -1;
    }

    bytes_written = write(fd, SUSPEND_COMMAND, strlen(SUSPEND_COMMAND));

    if (bytes_written < 0) {
        PS_LOG_ERRNO_ERROR("power", errno, "cannot write to " POWER_STATE_PATH);
        close(fd); // Always clean up resources
        return -2;
    }

    if (bytes_written < (ssize_t)strlen(SUSPEND_COMMAND)) {
        PS_LOG_ERROR("power", "incomplete write to %s", POWER_STATE_PATH);
        close(fd);
        return -2;
    }

    close(fd);

    PS_LOG_INFO("power", "system resumed from sleep");

    return 0; // Success
#endif
}

uint32_t SDKLIB_FSGetDiskRoomState(int unk0, void* data) {
    char current_path[PATH_MAX];
    struct statvfs fs_stats;

    // 1. Get the current working directory path.
    if (getcwd(current_path, sizeof(current_path)) == NULL) {
        PS_LOG_ERRNO_ERROR("filesystem", errno,
                           "FSGetDiskRoomState cannot get working directory");
        return -1;
    }
    if (statvfs(current_path, &fs_stats) != 0) {
        PS_LOG_ERRNO_ERROR("filesystem", errno,
                           "FSGetDiskRoomState statvfs failed for %s",
                           current_path);
        return -1;
    }
    uint64_t free_bytes = (uint64_t)fs_stats.f_bavail * fs_stats.f_frsize;
    if (free_bytes > UINT_MAX) {
        PS_LOG_WARN("filesystem",
                    "free disk space %llu exceeds 4 GiB and will be truncated",
                    (unsigned long long)free_bytes);
    }
    uint32_t* target_address = (uint32_t*)((uintptr_t)data + 52);
    *target_address = (uint32_t)free_bytes;

    return 0; // Success
}

//------io rel--------
uint32_t SDKLIB_SetSystemVariable(uint32_t var,uint32_t type,uint32_t value) {
#if PRIMESHIM_HOST_DEVICE
    if(var==6 && type==2){
        int fd = open("/sys/class/backlight/backlight/brightness",O_WRONLY);
        if(fd<0){return 0;}
        char brightness[10];
        int bytes = snprintf(brightness, sizeof(brightness), "%d\n", value*2+1);
        write(fd, brightness, bytes);
        close(fd);
        return 0;
    }
#endif
    if (var == 89) {
       /*struct sysinfo info;
        if (sysinfo(&info) != 0) {
            perror("sysinfo failed");
            return 0;
        }
        unsigned long free_ram_bytes = (unsigned long)info.freeram * info.mem_unit;*/
        long available_pages = get_avphys_pages();
        if (available_pages == -1) {
            PS_LOG_ERRNO_ERROR("system", errno, "get_avphys_pages failed");
            return 1;
        }

        long page_size = sysconf(_SC_PAGESIZE);
        unsigned long free_ram_bytes = available_pages * page_size;
        if (free_ram_bytes > UINT_MAX) {
            PS_LOG_WARN("system",
                        "free RAM %lu exceeds 4 GiB and will be truncated",
                        free_ram_bytes);
        }

        return (uint32_t)free_ram_bytes;
    }
	PS_LOG_DEBUG("system", "unimplemented SetSystemVariable id=%u type=%u value=%u", var,
                 type, value);
	return 0;
}


static int32_t usb_device_io_control(void *context, uint32_t request,
                                     const void *input, uint32_t input_size,
                                     void *output, uint32_t output_size)
{
    (void)context;
    (void)output;
    (void)output_size;
#if PRIMESHIM_HOST_DEVICE
    return usbd_deviceio_handler((int)request, (void *)input, (int)input_size);
#else
    PS_LOG_DEBUG("device", "USB ioctl 0x%x ignored on PC host", request);
    return 0;
#endif
}

static int32_t battery_device_io_control(void *context, uint32_t request,
                                         const void *input,
                                         uint32_t input_size, void *output,
                                         uint32_t output_size)
{
    float *voltage = output;
    unsigned int i;

    (void)context;
    (void)request;
    (void)input;
    (void)input_size;
    if (!output || output_size < 4u * sizeof(*voltage)) {
        errno = EINVAL;
        return 0;
    }
    for (i = 0; i < 4; ++i) voltage[i] = 3.0f;
    return 1;
}

static int32_t arch_device_io_control(void *context, uint32_t request,
                                      const void *input, uint32_t input_size,
                                      void *output, uint32_t output_size)
{
    (void)context;
    (void)request;
    (void)input;
    (void)input_size;
    if (output && output_size >= sizeof(uint32_t)) {
        ((uint32_t *)output)[0] = 0; /* Hardware revision A. */
    }
    return 1;
}

static const primeshim_device_driver_t usb_device_driver = {
    .name = "USB", .io_control = usb_device_io_control
};
static const primeshim_device_driver_t battery_device_driver = {
    .name = "BAT", .io_control = battery_device_io_control
};
static const primeshim_device_driver_t arch_device_driver = {
    .name = "ARCH", .io_control = arch_device_io_control
};
#define COMPATIBILITY_DEVICE(symbol, device_name)                           \
    static int32_t symbol##_io_control(                                     \
        void *context, uint32_t request, const void *input,                  \
        uint32_t input_size, void *output, uint32_t output_size)             \
    {                                                                       \
        (void)context;                                                       \
        (void)input;                                                         \
        PS_LOG_DEBUG("device",                                              \
                     "unimplemented %s ioctl=0x%x in=%u out=%u",            \
                     (device_name), request, input_size, output_size);       \
        if (output && output_size) memset(output, 0xff, output_size);        \
        return 1;                                                           \
    }                                                                       \
    static const primeshim_device_driver_t symbol = {                       \
        .name = (device_name), .io_control = symbol##_io_control             \
    };                                                                      \
    PRIMESHIM_REGISTER_DEVICE_DRIVER(symbol)

PRIMESHIM_REGISTER_DEVICE_DRIVER(usb_device_driver)
PRIMESHIM_REGISTER_DEVICE_DRIVER(battery_device_driver)
PRIMESHIM_REGISTER_DEVICE_DRIVER(arch_device_driver)
COMPATIBILITY_DEVICE(usbhost_device_driver, "USBHOST")
COMPATIBILITY_DEVICE(back_device_driver, "BACK")
COMPATIBILITY_DEVICE(factory_device_driver, "FACTORY")
COMPATIBILITY_DEVICE(indicator_device_driver, "INDICATOR")

uint32_t SDKLIB_CreateFile(const char *name)
{
    PS_LOG_TRACE("device", "CreateFile(%s)", name ? name : "<null>");
    return device_open(name);
}

uint32_t SDKLIB_DeviceIoControl(uint32_t handle, uint32_t request, char *input,
                                uint32_t input_size, char *output,
                                int output_size, uint32_t *result,
                                void *overlapped)
{
    if (output_size < 0) {
        if (result) *result = 0;
        errno = EINVAL;
        PS_LOG_ERROR("device", "negative ioctl output size %d", output_size);
        return 0;
    }
    if (overlapped)
        PS_LOG_DEBUG("device", "overlapped ioctl argument %p is ignored",
                     overlapped);
    return (uint32_t)device_io_control(handle, request, input, input_size,
                                       output, (uint32_t)output_size, result);
}

uint32_t SDKLIB_CloseHandle(uint32_t handle)
{
    return (uint32_t)device_close(handle);
}

















uint32_t Stub_SDKVersion(){
return 0x10008;
}
