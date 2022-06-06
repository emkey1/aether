#include <stddef.h>
#include <stdbool.h>

//extern static ssize_t proc_show_dev(struct proc_entry * UNUSED(entry), char *buf);
//extern bool (*remove_user_default)(const char *name);
extern static int sys_show_net(struct proc_entry * UNUSED(entry), struct proc_data *buf);
