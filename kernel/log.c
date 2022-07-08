#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/uio.h>
#if LOG_HANDLER_NSLOG
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "kernel/calls.h"
#include "util/sync.h"
#include "util/fifo.h"
#include "kernel/task.h"
#include "misc.h"

#define LOG_BUF_SHIFT 20
static char log_buffer[1 << LOG_BUF_SHIFT];
static struct fifo log_buf = FIFO_INIT(log_buffer);
static size_t log_max_since_clear = 0;
static lock_t log_lock = LOCK_INITIALIZER;

#define SYSLOG_ACTION_CLOSE_ 0
#define SYSLOG_ACTION_OPEN_ 1
#define SYSLOG_ACTION_READ_ 2
#define SYSLOG_ACTION_READ_ALL_ 3
#define SYSLOG_ACTION_READ_CLEAR_ 4
#define SYSLOG_ACTION_CLEAR_ 5
#define SYSLOG_ACTION_CONSOLE_OFF_ 6
#define SYSLOG_ACTION_CONSOLE_ON_ 7
#define SYSLOG_ACTION_CONSOLE_LEVEL_ 8
#define SYSLOG_ACTION_SIZE_UNREAD_ 9
#define SYSLOG_ACTION_SIZE_BUFFER_ 10

static int syslog_read(addr_t buf_addr, int_t len, int flags) {
    if (len < 0)
        return _EINVAL;
    if (flags & FIFO_LAST) {
        if ((size_t) len > log_max_since_clear)
            len = log_max_since_clear;
    } else {
        if ((size_t) len > fifo_capacity(&log_buf))
            len = fifo_capacity(&log_buf);
    }
    char *buf = malloc(len);
    fifo_read(&log_buf, buf, len, flags);
    
    // Here we will split on \n and do one entry per line
    int fail;
    // Keep printing tokens while one of the
    // delimiters present in str[].
    addr_t pointer = buf_addr; // Where we are in the buffer
    unsigned count = 1;
    char *token = strtok(buf, "\n"); // Inject an extra end of line to get sanish output from dmesg.  -mke
    
    fail = user_write(pointer, "\n", 1);
    if (fail) {
        free(buf);
        return _EFAULT;
    }
    pointer = pointer + 1;
    
    while (token != NULL) {
        size_t length = strlen(token);
        fail = user_write(pointer, token, length);
        if (fail) {
            free(buf);
            return _EFAULT;
        }
        pointer += length;
        fail = user_write(pointer, "\n", 1);
        pointer++;
        token = strtok(NULL, "\n");
        if(count > 12000)
            token = NULL; // We're going to overrun something.  Need to fix this, but for now, just abort.  -mke
        count++;
    }

    free(buf);
    
    if (fail)
        return _EFAULT;
    return len;
}

static int do_syslog(int type, addr_t buf_addr, int_t len) {
    int res;
    switch (type) {
        case SYSLOG_ACTION_READ_:
            return syslog_read(buf_addr, len, 0);
        case SYSLOG_ACTION_READ_ALL_:
            return syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);

        case SYSLOG_ACTION_READ_CLEAR_:
            res = syslog_read(buf_addr, len, FIFO_LAST | FIFO_PEEK);
            if (res < 0)
                return res;
            fallthrough;
        case SYSLOG_ACTION_CLEAR_:
            log_max_since_clear = 0;
            return 0;

        case SYSLOG_ACTION_SIZE_UNREAD_:
            return fifo_size(&log_buf);
        case SYSLOG_ACTION_SIZE_BUFFER_:
            return fifo_capacity(&log_buf);

        case SYSLOG_ACTION_CLOSE_:
        case SYSLOG_ACTION_OPEN_:
        case SYSLOG_ACTION_CONSOLE_OFF_:
        case SYSLOG_ACTION_CONSOLE_ON_:
        case SYSLOG_ACTION_CONSOLE_LEVEL_:
            return 0;
        default:
            return _EINVAL;
    }
}
int_t sys_syslog(int_t type, addr_t buf_addr, int_t len) {
    critical_region_count_increase(current);
    lock(&log_lock, 0);
    int retval = do_syslog(type, buf_addr, len);
    unlock(&log_lock);
    critical_region_count_decrease(current);
    return retval;
}

static void log_buf_append(const char *msg) {
    fifo_write(&log_buf, msg, strlen(msg), FIFO_OVERWRITE);
    log_max_since_clear += strlen(msg);
    if (log_max_since_clear > fifo_capacity(&log_buf))
        log_max_since_clear = fifo_capacity(&log_buf);
}

static void log_line(const char *line);

static void output_line(const char *line) {
    time_t t=time(NULL);
    char* c_time_string;
    c_time_string = ctime(&t);
    c_time_string[strcspn(c_time_string, "\n")] = 0;  // Remove trailing newline
    //double tstamp = difftime(t, (time_t) 0);
    int mybuff_size = 512;
    char tmpbuff[mybuff_size];
    //sprintf(tmpbuff, "[   %f] %s", tstamp, line);
    sprintf(tmpbuff, "[   %s] %s", c_time_string, line);
    
    // send it to stdout or wherever
    log_line(tmpbuff);
    // add it to the circular buffer
    log_buf_append(tmpbuff);
    log_buf_append("\n");
}

void ish_vprintk(const char *msg, va_list args) {
    // format the message
    // I'm trusting you to not pass an absurdly long message
    static __thread char buf[16384] = "";
    static __thread size_t buf_size = 0;
    
    buf_size += vsprintf(buf + buf_size, msg, args);

    // output up to the last newline, leave the rest in the buffer
    lock(&log_lock, 1);
    char *b = buf;
    char *p;
    while ((p = strchr(b, '\n')) != NULL) {
        *p = '\0';
        output_line(b);
        *p = '\n';
        buf_size -= p + 1 - b;
        b = p + 1;
    }
    unlock(&log_lock);
    memmove(buf, b, strlen(b) + 1);
}

void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    ish_vprintk(msg, args);
    va_end(args);
}

#if LOG_HANDLER_DPRINTF
#define NEWLINE "\r\n"
static void log_line(const char *line) {
    struct iovec output[2] = {{(void *) line, strlen(line)}, {"\n", 1}};
    writev(666, output, 2);
}
#elif LOG_HANDLER_NSLOG
static void log_line(const char *line) {
    extern void NSLog(CFStringRef msg, ...);
    NSLog(CFSTR("%s"), line);
}
#endif

static void default_die_handler(const char *msg) {
    printk("%s\n", msg);
}
void (*die_handler)(const char *msg) = default_die_handler;
_Noreturn void die(const char *msg, ...);
void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    char buf[4096];
    vsprintf(buf, msg, args);
    die_handler(buf);
    abort();
    va_end(args);
}

// fun little utility function
int current_pid() {
    if (current)
        return current->pid;
    return -1;
}

char * current_comm() {
    if (current) {
        return current->comm;
    }
    return calloc(1, 1); 
}

// Because sometimes we can't #include "kernel/task.h" -mke
unsigned current_critical_region_count() {
    return current->critical_region_count;
}

unsigned current_locks_held_count() {
    return current->locks_held_count;
}

void modify_current_critical_region_count(int value) { // value Should only be -1 or 1.  -mke
    if((value != -1) || (value != 1))
        value = 0;
    current->critical_region_count = current->critical_region_count + value;
    //return current->critical_region_count;
}

void modify_current_locks_held_count(int value) { // value Should only be -1 or 1.  -mke
    if((value < -1) || (value > 1))
        value = 0;
    current->locks_held_count = current->locks_held_count + value;
    //return current->locks_held_count;
}
