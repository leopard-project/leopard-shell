# leopard-shell (lpsh)

Leopard 的全新用户态 shell，二进制名 `lpsh`。

## 特性
- **workdir 概念**：`cd` / `pwd` 维护当前工作目录，相对路径据此解析。
- **fd stdio**：直接从 fd 0 读整行、向 fd 1/2 输出；PTY slave 的 canonical
  行规程负责回显/退格。`XAPI_INPUT` 不再是 shell 主输入路径。
- **读取 `path` 变量**：从 `/users/<用户名>/config` 读取 `path`（`:` 分隔的目录列表），
  用它解析命令，替代旧 xsh 硬编码的 `/apps/%s`。
- 内置命令：`cd`、`pwd`、`exit`（与 Bash 的常用有状态内建对齐）：
  - `cd` 必须内置，因为外部进程不能改变 shell 的 cwd；
  - `pwd` 内置，直接输出 shell 维护的逻辑工作目录；
  - `clear` 不内置（Bash 也没有），由 `leopard-utils` 提供；
  - 其他命令全部通过 `path=` 解析。

## 配置
每用户配置文件 `/users/<用户名>/config`（由 leopard-oobe 在建号时写入）：

```
shell=/apps/system/lpsh.elf
path=/apps/system:/apps/leopard
```

## 构建
```
python3 gen_ninja.py && ninja
```
产物：`out/lpsh.elf`。
