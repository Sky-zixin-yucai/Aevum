// Linux i386 对照基准的最小 init。
// 作为 PID 1 运行：挂 proc/sys/cgroup，按内核命令行参数跑一组 CPU-bound 任务，
// 用 wait4 的 rusage 统计每个任务的 CPU 时间，然后关机。
//
// 命令行参数（写在 QEMU -append 里）：
//   bench_policy=cfs|fifo
//   bench_tasks=2|4|8|12
//   bench_ms=5000
//
// CFS 优先用 cgroup v2 的 cpu.weight 做 800/400/200/100（8:4:2:1）；
// cgroup 不可用时退回 nice。

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TASKS 12

static volatile sig_atomic_t stop;

static void on_stop(int sig) {
    (void)sig;
    stop = 1;
}

static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}

static int get_param(const char *name, char *out, size_t out_size) {
    char cmdline[4096];
    if (read_file("/proc/cmdline", cmdline, sizeof(cmdline)) != 0) return -1;
    size_t name_len = strlen(name);
    const char *p = cmdline;
    while ((p = strstr(p, name)) != NULL) {
        if ((p == cmdline || p[-1] == ' ') && p[name_len] == '=') {
            p += name_len + 1;
            size_t i = 0;
            while (*p && !isspace((unsigned char)*p) && i + 1 < out_size) {
                out[i++] = *p++;
            }
            out[i] = 0;
            return i > 0 ? 0 : -1;
        }
        p++;
    }
    return -1;
}

static int get_param_int(const char *name, int fallback) {
    char value[32];
    if (get_param(name, value, sizeof(value)) != 0) return fallback;
    return atoi(value);
}

static void setup_console(void) {
    mkdir("/dev", 0755);
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    int fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }
}

static int setup_mounts(void) {
    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, NULL);

    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    mkdir("/sys/fs/cgroup", 0755);
    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL);

    if (write_text("/sys/fs/cgroup/cgroup.subtree_control", "+cpu") != 0) {
        return -1;
    }
    return 0;
}

static int setup_cgroup(int index, int weight) {
    char path[160];
    char text[32];

    snprintf(path, sizeof(path), "/sys/fs/cgroup/bench%d", index);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;

    snprintf(path, sizeof(path), "/sys/fs/cgroup/bench%d/cpu.weight", index);
    snprintf(text, sizeof(text), "%d", weight);
    return write_text(path, text);
}

static int move_to_cgroup(int index, pid_t pid) {
    char path[160];
    char text[32];

    snprintf(path, sizeof(path), "/sys/fs/cgroup/bench%d/cgroup.procs", index);
    snprintf(text, sizeof(text), "%d", (int)pid);
    return write_text(path, text);
}

static int priority_for(int index, int count) {
    if (count == 2) return index == 0 ? 0 : 3;
    int per = count / 4;
    if (per < 1) per = 1;
    int priority = index / per;
    if (priority > 3) priority = 3;
    return priority;
}

static int weight_for(int priority) {
    return 800 >> priority;
}

static void child_body(int index, int priority, const char *policy, int cgroup_ok) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    if (strcmp(policy, "fifo") == 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 90 - priority * 10;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            fprintf(stderr, "SCHED_FIFO failed for task %d: %s\n", index, strerror(errno));
        }
    } else if (!cgroup_ok) {
        setpriority(PRIO_PROCESS, 0, -10 + priority * 5);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    volatile unsigned long x = 1;
    while (!stop) {
        x = x * 1103515245UL + 12345UL;
        __asm__ __volatile__("" : "+r"(x));
    }
    _exit(0);
}

static double timeval_ms(struct timeval tv) {
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(void) {
    const char *policy = "cfs";
    int count = 4;
    int duration_ms = 5000;
    char policy_arg[16];

    setup_console();
    int mounts_ok = setup_mounts() == 0;
    int cgroup_ok = mounts_ok;
    if (get_param("bench_policy", policy_arg, sizeof(policy_arg)) == 0) {
        if (strcmp(policy_arg, "cfs") == 0 || strcmp(policy_arg, "fifo") == 0) {
            policy = strcmp(policy_arg, "fifo") == 0 ? "fifo" : "cfs";
        }
    }
    count = get_param_int("bench_tasks", count);
    if (count < 2 || count > MAX_TASKS) count = 4;
    duration_ms = get_param_int("bench_ms", duration_ms);
    if (duration_ms < 1000) duration_ms = 1000;

    if (strcmp(policy, "fifo") == 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 99;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            printf("BENCH_ERROR parent SCHED_FIFO failed: %s\n", strerror(errno));
            fflush(stdout);
            syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                    LINUX_REBOOT_CMD_POWER_OFF, NULL);
            _exit(1);
        }
    }

    int use_cgroup = strcmp(policy, "cfs") == 0 && cgroup_ok;
    if (use_cgroup) {
        for (int i = 0; i < count; i++) {
            int priority = priority_for(i, count);
            int weight = weight_for(priority);
            if (setup_cgroup(i, weight) == 0) {
                continue;
            }
            cgroup_ok = 0;
            use_cgroup = 0;
            break;
        }
    }

    printf("BENCH_BEGIN policy=%s tasks=%d duration_ms=%d cgroup=%s\n",
           policy, count, duration_ms, use_cgroup ? "on" : "off");
    fflush(stdout);

    pid_t pids[MAX_TASKS];
    int priorities[MAX_TASKS];
    int weights[MAX_TASKS];
    for (int i = 0; i < count; i++) {
        priorities[i] = priority_for(i, count);
        weights[i] = weight_for(priorities[i]);
        pid_t pid = fork();
        if (pid == 0) {
            child_body(i, priorities[i], policy, cgroup_ok);
        }
        if (pid < 0) {
            printf("BENCH_ERROR fork %d failed: %s\n", i, strerror(errno));
            fflush(stdout);
            syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                    LINUX_REBOOT_CMD_POWER_OFF, NULL);
            _exit(1);
        }
        pids[i] = pid;
        if (use_cgroup) {
            if (move_to_cgroup(i, pid) != 0) {
                printf("BENCH_WARN cgroup move %d failed: %s\n", i, strerror(errno));
            }
        }
        printf("TASK_START id=%d pid=%d priority=%d weight=%d\n",
               i, (int)pid, priorities[i], weights[i]);
    }
    fflush(stdout);

    // 先让子进程进入忙等，再开始计时窗口。
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&settle, NULL);

    double wall_start = now_ms();
    struct timespec req = { .tv_sec = duration_ms / 1000,
                            .tv_nsec = (duration_ms % 1000) * 1000000L };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}

    for (int i = 0; i < count; i++) {
        kill(pids[i], SIGUSR1);
    }

    double total_cpu_ms = 0;
    double cpu_ms[MAX_TASKS];
    for (int i = 0; i < count; i++) {
        struct rusage ru;
        memset(&ru, 0, sizeof(ru));
        int status = 0;
        wait4(pids[i], &status, 0, &ru);
        cpu_ms[i] = timeval_ms(ru.ru_utime) + timeval_ms(ru.ru_stime);
        total_cpu_ms += cpu_ms[i];
    }
    double wall_ms = now_ms() - wall_start;

    double max_wait_ms = 0;
    for (int i = 0; i < count; i++) {
        double share = total_cpu_ms > 0 ? cpu_ms[i] / total_cpu_ms : 0;
        printf("TASK_RESULT id=%d priority=%d weight=%d cpu_ms=%.1f share=%.4f\n",
               i, priorities[i], weights[i], cpu_ms[i], share);
        if (cpu_ms[i] < 1.0 && strcmp(policy, "fifo") == 0) max_wait_ms = duration_ms;
    }
    printf("BENCH_END policy=%s tasks=%d total_cpu_ms=%.1f wall_ms=%.1f max_wait_ms=%.1f\n",
           policy, count, total_cpu_ms, wall_ms, max_wait_ms);
    fflush(stdout);

    sleep(1);
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_POWER_OFF, NULL);
    while (1) pause();
}
