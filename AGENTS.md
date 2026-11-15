# PROJECT KNOWLEDGE BASE — leopard-shell

Leopard 的全新用户态 shell，二进制名 `lpsh`。

## 特性
- **workdir 概念**：`cd`/`pwd` 维护当前工作目录，相对路径据此解析。
- **fd stdio**：fd 0 读整行、fd 1/2 输出；PTY slave 的 canonical 行规程负责
  回显/退格。子进程用 `fork`+`execve` 启动，继承 0/1/2，不再走
  `xapi_RunArgs` 的 `/dev/stdio` 回退。
- 读 `path` 变量：从 `/users/<用户名>/config` 读 `path`（`:` 分隔目录列表）解析命令，
  替代旧 xsh 硬编码的 `/apps/%s`。
- 内置命令：`cd`、`pwd`、`exit`、`clear`。

## 配置（每用户 `/users/<用户名>/config`，由 leopard-oobe 建号时写）
    shell=/apps/system/lpsh.elf
    path=/apps/system:/apps/leopard

## 构建
    python3 gen_ninja.py && ninja build   # → out/lpsh.elf
    ninja pack

- 静态 ELF，依赖 `../XJ380-SDK/out/crt`（`include/` + `lib/x86_64/`）。
