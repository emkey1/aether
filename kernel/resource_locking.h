// Because sometimes we can't #include "kernel/task.h" -mke

extern unsigned critical_region_count(struct task*);
extern void modify_critical_region_count(struct task*, int);
extern unsigned locks_held_count(struct task*);
extern void modify_locks_held_count(struct task*, int);

