/*
 * lpsh — Leopard Shell
 * 全新用户态 shell：workdir 概念 + 读取 path 变量解析命令（替代硬编码 /apps）。
 */
#include "xtuiapi.h"

#define LINE_CAP   256
#define MAX_ARGS   32
#define PATH_CAP   512
#define CONFIG_CAP 512
#define USER_CAP   64

#define DEFAULT_PATH "/apps/system:/apps/leopard"

/* ---- leopard-ime 文件 IPC 客户端（方案 A：用户态输入法守护进程） ---- */
#define IME_REQ      "/system/leopard/ime/request"
#define IME_RESP     "/system/leopard/ime/response"
#define IME_RESP_CAP 256

static char g_home[PATH_CAP];
static char g_workdir[PATH_CAP];
static char g_path[CONFIG_CAP];

/* 读取 /users/<用户名>/config 中某个 key= 的值，成功返回 0，失败返回 -1 */
static int config_get(const char *username, const char *key, char *out, size_t out_cap)
{
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

        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            char *val = line + key_len + 1;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\r' || val[len - 1] == ' ' || val[len - 1] == '\t'))
                val[--len] = '\0';
            if (len > 0 && len < out_cap) {
                memcpy(out, val, len);
                out[len] = '\0';
                return 0;
            }
            return -1;
        }

        if (eol == NULL)
            break;
        line = eol + 1;
    }
    return -1;
}

/* 就地规范化绝对路径：折叠 /./ 与 /../ */
static void normalize_path(char *dst, size_t dst_cap, const char *src)
{
    char tmp[PATH_CAP];
    strncpy(tmp, src, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    const char *segs[MAX_ARGS];
    int nseg = 0;
    for (char *tok = strtok(tmp, "/"); tok != NULL; tok = strtok(NULL, "/")) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (nseg > 0)
                nseg--;
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

/* 启动进程，成功返回 pid，失败返回负数 */
static int64_t spawn(char *path, char *args[])
{
    return (int64_t)xapi_RunArgs((WSTR)path, (WSTR *)args);
}

/* 解析并启动命令，成功返回 pid，失败返回负数 */
static int64_t run_command(char *args[])
{
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
        if (pid >= 0)
            return pid;

        snprintf(cand, sizeof(cand), "%s/%s.elf", entry, cmd);
        pid = spawn(cand, args);
        if (pid >= 0)
            return pid;
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

/* 手写小整数解析，避免依赖 atoi */
static int parse_index(const char *s)
{
    int v = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    return v;
}

/* 发送一条请求，等 daemon ack（清空 request），读回响应 */
static void ime_request(const char *cmd, char *resp, size_t cap)
{
    xapi_WriteFile((WSTR)IME_REQ, (char *)cmd, strlen(cmd) + 1, 0);
    for (int i = 0; i < 300; i++) {              /* 最多 ~3s */
        char probe[8] = {0};
        int64_t n = (int64_t)xapi_ReadFile((WSTR)IME_REQ, probe, sizeof(probe) - 1, 0);
        if (n <= 0 || probe[0] == '\0')
            break;
        xapi_Sleep(10);
    }
    int64_t rn = (int64_t)xapi_ReadFile((WSTR)IME_RESP, resp, cap - 1, 0);
    resp[rn > 0 ? rn : 0] = '\0';
}

/* ime <拼音> [序号]：查询候选 / 选中第 n 个候选 */
static void do_ime(char *args[], int n)
{
    if (n < 2) {
        xapi_Printf("usage: ime <pinyin> [index]\n");
        return;
    }

    char cmd[LINE_CAP];
    snprintf(cmd, sizeof(cmd), "COMPOSE %s", args[1]);

    char resp[IME_RESP_CAP] = {0};
    ime_request(cmd, resp, sizeof(resp));

    if (strncmp(resp, "OK", 2) != 0) {
        xapi_Printf("ime: no candidates for %s\n", args[1]);
        return;
    }

    const char *cand[MAX_ARGS];
    int nc = 0;
    char *tok = strtok(resp + 2, " \t");
    while (tok != NULL && nc < MAX_ARGS - 1) {
        cand[nc++] = tok;
        tok = strtok(NULL, " \t");
    }

    if (nc == 0) {
        xapi_Printf("ime: no candidates for %s\n", args[1]);
        return;
    }

    if (n >= 3) {
        int idx = parse_index(args[2]);
        if (idx < 1 || idx > nc) {
            xapi_Printf("ime: index %d out of range 1..%d\n", idx, nc);
            return;
        }
        xapi_Printf("%s\n", cand[idx - 1]);
    } else {
        for (int i = 0; i < nc; i++)
            xapi_Printf("%d.%s%s", i + 1, cand[i], (i + 1 < nc) ? "  " : "\n");
    }
}

/* 查询 daemon 候选，解析 "OK c1 c2 ..." 到 cands[]，返回候选数 */
static int ime_compose_cands(const char *py, const char *cands[MAX_ARGS])
{
    char cmd[LINE_CAP];
    snprintf(cmd, sizeof(cmd), "COMPOSE %s", py);
    char resp[IME_RESP_CAP] = {0};
    ime_request(cmd, resp, sizeof(resp));
    if (strncmp(resp, "OK", 2) != 0) return 0;
    int nc = 0;
    char *tok = strtok(resp + 2, " \t");
    while (tok && nc < MAX_ARGS - 1) { cands[nc++] = tok; tok = strtok(NULL, " \t"); }
    return nc;
}

/* 候选格式化成 "1.a 2.b 3.c"（最多 9 个） */
static int fmt_cands(const char *cands[MAX_ARGS], int nc, char *out, size_t cap)
{
    int off = 0;
    for (int i = 0; i < nc && i < 9; i++) {
        int w = snprintf(out + off, cap - off, "%d.%s ", i + 1, cands[i]);
        if (w < 0 || off + w >= (int)cap - 1) break;
        off += w;
    }
    return off;
}

/* 重绘整行：\r + 清行 + 重写（依赖 ANSI，clear 内置已同样依赖） */
static void redraw_line(const char *line, int composing, const char *py, const char *candstr)
{
    char buf[512];
    int off = snprintf(buf, sizeof(buf), "lpsh$ %s", line);
    if (composing) {
        off += snprintf(buf + off, sizeof(buf) - off, " %s", py);
        if (candstr[0])
            off += snprintf(buf + off, sizeof(buf) - off, " [%s]", candstr);
    }
    xapi_Printf("\r\x1B[K%s", buf);
}

/* 逐键 inline 行编辑器 + IME：Esc(128) 切换组合；组合时 a-z 拼音 / 1-9 选候选 / 空格选首 / 退格删拼音 / 回车提交拼音 */
static void readline_ime(char *line, size_t cap)
{
    size_t len = 0;
    int composing = 0;
    char py[64] = {0};
    size_t pylen = 0;
    char candstr[256] = {0};

    line[0] = '\0';
    xapi_Printf("lpsh$ ");

    for (;;) {
        unsigned char ch = (unsigned char)xapi_Getch();

        if (composing) {
            if (ch >= 'a' && ch <= 'z') {
                if (pylen < sizeof(py) - 1) py[pylen++] = (char)ch;
                py[pylen] = '\0';
                const char *cands[MAX_ARGS];
                int nc = ime_compose_cands(py, cands);
                candstr[0] = '\0';
                fmt_cands(cands, nc, candstr, sizeof(candstr));
                redraw_line(line, 1, py, candstr);
                continue;
            }
            if (ch >= '1' && ch <= '9') {
                const char *cands[MAX_ARGS];
                int nc = ime_compose_cands(py, cands);
                int idx = ch - '1';
                if (idx < nc) {
                    size_t wl = strlen(cands[idx]);
                    if (len + wl < cap) { memcpy(line + len, cands[idx], wl); len += wl; line[len] = '\0'; }
                }
                composing = 0; py[0] = '\0'; pylen = 0; candstr[0] = '\0';
                redraw_line(line, 0, py, candstr);
                continue;
            }
            if (ch == ' ') {
                const char *cands[MAX_ARGS];
                int nc = ime_compose_cands(py, cands);
                if (nc > 0) {
                    size_t wl = strlen(cands[0]);
                    if (len + wl < cap) { memcpy(line + len, cands[0], wl); len += wl; line[len] = '\0'; }
                }
                composing = 0; py[0] = '\0'; pylen = 0; candstr[0] = '\0';
                redraw_line(line, 0, py, candstr);
                continue;
            }
            if (ch == 0x08) {
                if (pylen > 0) {
                    pylen--; py[pylen] = '\0';
                    const char *cands[MAX_ARGS];
                    int nc = ime_compose_cands(py, cands);
                    candstr[0] = '\0';
                    fmt_cands(cands, nc, candstr, sizeof(candstr));
                    redraw_line(line, 1, py, candstr);
                } else {
                    composing = 0; py[0] = '\0'; candstr[0] = '\0';
                    redraw_line(line, 0, py, candstr);
                }
                continue;
            }
            if (ch == '\n' || ch == '\r') {
                size_t pl = strlen(py);
                if (len + pl < cap) { memcpy(line + len, py, pl); len += pl; line[len] = '\0'; }
                composing = 0; py[0] = '\0'; pylen = 0; candstr[0] = '\0';
                redraw_line(line, 0, py, candstr);
                continue;
            }
            if (ch == 128) {
                composing = 0; py[0] = '\0'; pylen = 0; candstr[0] = '\0';
                redraw_line(line, 0, py, candstr);
                continue;
            }
            continue;
        }

        /* 普通模式 */
        if (ch == '\n' || ch == '\r') {
            line[len] = '\0';
            xapi_Printf("\n");
            return;
        }
        if (ch == 0x08) {
            if (len > 0) { len--; line[len] = '\0'; redraw_line(line, 0, py, candstr); }
            continue;
        }
        if (ch == 128) {
            composing = 1; py[0] = '\0'; pylen = 0; candstr[0] = '\0';
            redraw_line(line, 1, py, candstr);
            continue;
        }
        if (ch >= 32 && ch < 127) {
            if (len < cap - 1) {
                line[len++] = (char)ch; line[len] = '\0';
                xapi_Printf("%c", ch);
            }
            continue;
        }
    }
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
        readline_ime(line, sizeof(line));
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
        if (strcmp(args[0], "ime") == 0) {
            do_ime(args, n);
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
