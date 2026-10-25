/*
 * lpsh — Leopard Shell
 * 用户态 shell：workdir 概念 + 读取 path 变量解析命令（替代硬编码 /apps）。
 *
 * 输入采用整行读取（XAPI_INPUT）。不再做逐字节行编辑器 / 内联 IME：
 * 输入法（leopard-ime）作为独立输入中间层存在，lpsh 只消费整行文本。
 */
#include "xtuiapi.h"

#define LINE_CAP   256
#define MAX_ARGS   32
#define PATH_CAP   512
#define CONFIG_CAP 512
#define USER_CAP   64

#define DEFAULT_PATH "/apps/system:/apps/leopard"

static char g_home[PATH_CAP];
static char g_workdir[PATH_CAP];
static char g_path[CONFIG_CAP];

static int config_get(const char *, const char *, char *, size_t);
static void normalize_path(char *, size_t, const char *);
static inline int64_t spawn(char *path, char *args[])
{
        return (int64_t)xapi_RunArgs((WSTR)path, (WSTR *)args);
}
static int64_t run_command(char *args[]);
static void do_cd(char *arg);
static inline void read_line(char *buf, size_t cap)
{
        /* 读一整行（阻塞）。XAPI_INPUT 在内核里找不到 is_shell 终端时会兜底直读键盘，
         * 自带回显 + 退格 + 换行处理，因此不依赖逐字节 Getch / 终端标记。 */
        if (buf == NULL || cap == 0)
                return;
        buf[0] = '\0';
        enter_syscall(XAPI_INPUT, (uint64_t)buf, (uint64_t)cap, 0, 0, 0, 0);
}

int main(int argc, char *argv[], char *envp[])
{
        (void)argc;
        (void)argv;
        (void)envp;

        /* 当前登录用户 */
        UserInfo ui;
        memset(&ui, 0, sizeof(ui));
        xapi_GetCurrentUser(&ui);
        const char *username = ui.name[0] != '\0' ? ui.name : "user";

        /* home = /users/<用户名> */
        snprintf(g_home, sizeof(g_home), "/users/%s", username);
        strncpy(g_workdir, g_home, sizeof(g_workdir) - 1);
        g_workdir[sizeof(g_workdir) - 1] = '\0';

        /* path 变量（读不到用默认） */
        if (config_get(username, "path", g_path, sizeof(g_path)) != 0) {
                strncpy(g_path, DEFAULT_PATH, sizeof(g_path) - 1);
                g_path[sizeof(g_path) - 1] = '\0';
        }

        for (;;) {
                char line[LINE_CAP] = {0};
                xapi_Printf("lpsh$ ");
                read_line(line, sizeof(line));
                if (line[0] == '\0')
                        continue;

                char *args[MAX_ARGS];
                int n = 0;
                for (char *tok = strtok(line, " \t"); tok != NULL && n < MAX_ARGS - 1;
                                tok = strtok(NULL, " \t"))
                        args[n++] = tok;
                args[n] = NULL;
                if (n == 0)
                        continue;

                if (strcmp(args[0], "exit") == 0)
                        return 0;
                if (strcmp(args[0], "clear") == 0) {
                        xapi_Printf("\033[2J\033[H");
                        continue;
                }
                if (strcmp(args[0], "pwd") == 0) {
                        xapi_Printf("%s\n", g_workdir);
                        continue;
                }
                if (strcmp(args[0], "cd") == 0) {
                        do_cd(args[1]);
                        continue;
                }

                int64_t pid = run_command(args);
                if (pid < 0) {
                        xapi_Printf("command not found: %s\n", args[0]);
                } else {
                        int status = 0;
                        enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&status, 0, 0, 0, 0);
                }
        }
}

static int config_get(const char *username, const char *key, char *out, size_t out_cap)
{
        /* 读取 /users/<用户名>/config 中某个 key= 的值，成功返回 0，失败返回 -1 */
        char config_path[PATH_CAP];
        snprintf(config_path, sizeof(config_path), "/users/%s/config", username);

        char buf[CONFIG_CAP];
        memset(buf, 0, sizeof(buf));
        int64_t n = (int64_t)xapi_ReadFile((WSTR)config_path, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
                return -1;

        size_t key_len = strlen(key);
        char *line = buf;
        while (*line != '\0') {
                char *eol = strchr(line, '\n');
                if (eol != NULL)
                        *eol = '\0';

                if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
                        if (eol == NULL)
                                break;
                        line = eol + 1;
                        continue;
                }
                
                char *val = line + key_len + 1;
                size_t len = strlen(val);
                while (len > 0 && (val[len - 1] == '\r'
                                   || val[len - 1] == ' '
                                   || val[len - 1] == '\t'))
                        val[--len] = '\0';
                if (len == 0 || len >= out_cap)
                        return -1;
                memcpy(out, val, len);
                out[len] = '\0';
                return 0;
        }
        return -1;
}

static void normalize_path(char *dst, size_t dst_cap, const char *src)
{
        /* 就地规范化绝对路径：折叠 /./ 与 /../ */
        char tmp[PATH_CAP];
        strncpy(tmp, src, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        const char *segs[MAX_ARGS];
        int nseg = 0;
        for (char *tok = strtok(tmp, "/"); tok != NULL; tok = strtok(NULL, "/")) {
                if (tok[0] == '\0' || strcmp(tok, ".") == 0)
                        continue;
                if (strcmp(tok, "..") == 0) {
                        if (nseg > 0) nseg--;
                        continue;
                }
                segs[nseg++] = tok;
        }

        if (nseg == 0) {
                snprintf(dst, dst_cap, "/");
                return;
        }
        dst[0] = '\0';
        for (int i = 0; i < nseg; i++) {
                strcat(dst, "/");
                strcat(dst, segs[i]);
        }
}

static int64_t run_command(char *args[])
{
        /* 启动进程，成功返回 pid，失败返回负数 */
        char cmd[PATH_CAP];
        strncpy(cmd, args[0], sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';

        /* 绝对路径：直接跑 */
        if (cmd[0] == '/')
                return spawn(cmd, args);

        /* 含 '/'：相对 workdir 解析 */
        if (strchr(cmd, '/') != NULL) {
                char joined[PATH_CAP];
                snprintf(joined, sizeof(joined), "%s/%s", g_workdir, cmd);
                char norm[PATH_CAP];
                normalize_path(norm, sizeof(norm), joined);
                return spawn(norm, args);
        }

        /* 裸命令名：按 path 变量逐项查找 */
        char path_copy[CONFIG_CAP];
        strncpy(path_copy, g_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        for (char *entry = strtok(path_copy, ":"); entry != NULL; entry = strtok(NULL, ":")) {
                char cand[PATH_CAP];
                snprintf(cand, sizeof(cand), "%s/%s", entry, cmd);
                int64_t pid = spawn(cand, args);
                if (pid >= 0) return pid;

                snprintf(cand, sizeof(cand), "%s/%s.elf", entry, cmd);
                pid = spawn(cand, args);
                if (pid >= 0) return pid;
        }
        return -1;
}

static void do_cd(char *arg)
{
        if (arg == NULL || arg[0] == '\0') {
                strncpy(g_workdir, g_home, sizeof(g_workdir) - 1);
                g_workdir[sizeof(g_workdir) - 1] = '\0';
                return;
        }

        char joined[PATH_CAP];
        if (arg[0] == '/') {
                snprintf(joined, sizeof(joined), "%s", arg);
        } else {
                snprintf(joined, sizeof(joined), "%s/%s", g_workdir, arg);
        }
        char norm[PATH_CAP];
        normalize_path(norm, sizeof(norm), joined);
        strncpy(g_workdir, norm, sizeof(g_workdir) - 1);
        g_workdir[sizeof(g_workdir) - 1] = '\0';
}

