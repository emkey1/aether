#include <string.h>
#include "kernel/calls.h"

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;

static int __user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    char *cbuf = (char *) buf;
    addr_t p = addr;
    while (p < addr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > addr + count)
            chunk_end = addr + count;
        const char *ptr = mem_ptr(task->mem, p, MEM_READ);
        if (ptr == NULL)
            return 1;
        memcpy(&cbuf[p - addr], ptr, chunk_end - p);
        p = chunk_end;
    }
    return 0;
}

static int __user_write_task(struct task *task, addr_t addr, const void *buf, size_t count) {
    delay_task_delete_up_vote(task);
    const char *cbuf = (const char *) buf;
    addr_t p = addr;
    while (p < addr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > addr + count)
            chunk_end = addr + count;
        char *ptr = mem_ptr(task->mem, p, MEM_WRITE);
        if (ptr == NULL) {
            delay_task_delete_down_vote(task);
            return 1;
        }
        memcpy(ptr, &cbuf[p - addr], chunk_end - p);
        p = chunk_end;
    }
    
    delay_task_delete_down_vote(task);
    return 0;
}

int user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    delay_task_delete_up_vote(task);
    read_lock(&task->mem->lock);

    int res = __user_read_task(task, addr, buf, count);

    read_unlock(&task->mem->lock);
    delay_task_delete_down_vote(task);
    return res;
}

int user_read(addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

int user_write_task(struct task *task, addr_t addr, const void *buf, size_t count) { // This function has 'write' in the name, yet uses a read lock?  -mke
    read_lock(&task->mem->lock);
    int res = __user_write_task(task, addr, buf, count);
    read_unlock(&task->mem->lock);
    return res;
}

int user_write(addr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(addr_t addr, char *buf, size_t max) {
    delay_task_delete_up_vote(current);
    if (addr == 0) {
        delay_task_delete_down_vote(current);
        return 1;
    }
    read_lock(&current->mem->lock);
    size_t i = 0;
    while (i < max) {
        if (__user_read_task(current, addr + i, &buf[i], sizeof(buf[i]))) {
            read_unlock(&current->mem->lock);
            delay_task_delete_down_vote(current);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    read_unlock(&current->mem->lock);
    delay_task_delete_down_vote(current);
    return 0;
}

int user_write_string(addr_t addr, const char *buf) {
    if (addr == 0)
        return 1;
    read_lock(&current->mem->lock);
    size_t i = 0;
    do {
        if (__user_write_task(current, addr + i, &buf[i], sizeof(buf[i]))) {
            read_unlock(&current->mem->lock);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    read_unlock(&current->mem->lock);
    return 0;
}
