# YQstore

YQstore 是用 C 和 SPDK 实现的用户态文件系统原型。核心实现称为 ZVFS，通过 `LD_PRELOAD` 加载 `libstore.so`，将应用对 `/zvfs` 的文件访问转到 SPDK Blobstore 和 NVMe。应用仍通过 `open`、`read`、`write` 等接口访问文件。

目前已用 fio、RocksDB、FastDFS 和 MySQL 验证已实现的文件接口。MySQL 8.0.46 已跑通初始化、启动、建表、写入、正常关库和重启读回，并使用 `mysqlslap` 完成了与 Linux ext4 的对照测试。

## 文件目录展示

```text
YQstore/
├── README.md
├── LearnSpdk.md         # 开发笔记、复现命令和原始测试日志
├── Makefile             # 编译 libstore.so，可选开启 TSan
├── shim.c / shim.h      # POSIX 接口拦截、路径分流和同步保护
├── zvfs.c / zvfs.h      # 挂载、文件操作和 Blobstore I/O
├── inode.c / inode.h    # inode 表、目录树和 fd 映射
├── zvfs.json            # SPDK NVMe bdev 配置示例
├── zvfs_sync.fio        # fio 写入测试配置
├── presist_demo.c      # 跨进程写入、挂载与读回测试
├── clear_all_inode.c   # 递归清理 /zvfs 下的内容
├── test.c              # 早期文件接口测试
└── test_so/            # 动态库加载示例
```

## 1. 功能与进度

| 模块 | 当前实现内容 | 验证与边界 |
| --- | --- | --- |
| 文件接口拦截 | 按 `/zvfs` 路径或 fd 分流，接入文件读写、目录遍历、元数据查询和部分 64 位接口 | 已覆盖下表应用的测试路径，完整 POSIX 兼容性仍待验证 |
| 文件与目录 | 多级目录、路径查找、inode 分配、目录项更新及 inode 表保存 | 已验证新进程挂载后的目录、文件内容与长度读回 |
| SPDK 存储 | Blobstore 保存文件数据和 inode 表，使用回调完成 I/O | 当前测试使用 PCIe NVMe bdev `Nvme0n1` |
| 多线程访问 | 在 shim 入口使用进程内互斥锁，保护 inode、fd、虚拟当前目录和 SPDK 线程状态 | 已跑通 MySQL 启动及 4 客户端混合压测，当前采用粗粒度锁 |
| 应用接入 | fio 的 `sync` / `psync`、RocksDB `db_bench`、FastDFS 上传与下载、MySQL InnoDB | MySQL 已验证 SQL 读写、正常关库后读回及压测中的建库、删库 |

## 2. 编译与运行

### 2.1 环境

| 项目 | 测试环境 |
| --- | --- |
| 系统 | Ubuntu 22.04 LTS 虚拟机，x86-64，8 个 vCPU |
| 编译 | GCC、GNU Make；链接已构建的 SPDK 及其 DPDK 依赖 |
| SPDK | 测试机源码提交 `87f0c9d07`，配置大页及 SPDK 可访问的测试 NVMe |
| 应用 | fio 3.35、RocksDB 10.10.1、FastDFS、MySQL 8.0.46 |
| Linux 对照 | 虚拟机内的 ext4 文件系统，MySQL 数据目录为 `/tmp/mysql-linux-bench` |

### 2.2 运行前准备

1. 准备已编译的 SPDK，并在 Linux 上配置大页和测试 NVMe 的驱动绑定。当前挂载路径调用 `spdk_bs_load()`，需要设备上已有可加载的 Blobstore。
2. 根据测试盘的 PCI 地址修改 [zvfs.json](zvfs.json) 中的 `traddr`。测试配置将控制器命名为 `Nvme0`，源码挂载其 `Nvme0n1` 命名空间。
3. 将配置放到源码 `json_file` 指定的位置。测试机使用 `/home/gyq2/spdk/build/examples/zvfs.json`；换机器时按实际路径调整 [zvfs.c](zvfs.c) 并重新编译。

### 2.3 编译动态库

在项目根目录执行，`SPDK_ROOT_DIR` 指向已构建的 SPDK：

```bash
make lib
```

产物是 `libstore.so`。需要切换普通构建与 TSan 构建时，先 `make clean`，再执行 `make lib TSAN=0` 或 `make lib TSAN=1`。测试机的 TSan 版在运行时使用 `LD_PRELOAD="$(gcc -print-file-name=libtsan.so):$PWD/libstore.so"`。

### 2.4 运行文件读回测试

在项目根目录的 root 终端执行。`write` 会创建 `/zvfs/db/a.txt`，示例以 `/zvfs/db` 尚未创建为前提。

```bash
cc -Wall -O2 presist_demo.c -o /tmp/yqstore-presist-demo
LD_PRELOAD="$PWD/libstore.so" /tmp/yqstore-presist-demo write
LD_PRELOAD="$PWD/libstore.so" /tmp/yqstore-presist-demo read
```

第一条运行写入 512 字节，并在同一进程重新打开检查。第二条通过新进程重新挂载，校验同一文件的长度与内容，预期输出 `data=OK length=OK`。

## 3. 实现方案

### 3.1 分层与文件描述符

```text
fio / RocksDB / FastDFS / MySQL
                │ POSIX 文件接口
                ▼
libstore.so / shim.c
  ├─ Linux 路径或 fd ──► libc
  └─ ZVFS 路径或 fd ───► zvfs.c + inode.c
                            │
                            ▼
                     SPDK Blobstore → NVMe bdev
```

ZVFS 用 `eventfd()` 获取内核分配的 fd，再用 `file_table` 将 fd 映射到 `zvfs_file_t`。inode 记录文件名、类型、长度、目录关系和 `blob_id`，打开后的对象保存当前偏移、打开标志和读写缓冲区。

### 3.2 目录树与元数据

目录树通过 inode 编号维护父节点、首个子节点和兄弟节点。文件内容放在各自的 Blob，目录树和 inode 表序列化后保存到 super blob。挂载时加载这份 inode 表，重建内存中的文件与目录关系。

### 3.3 异步 I/O 与并发

文件接口提交 SPDK 请求并轮询 `global_thread`，等完成回调后返回。普通文件使用 128 KiB 读写缓冲区，连续对齐的数据按批提交，非对齐写入先读取对应 I/O 单元、覆盖修改的字节，再写回。

MySQL 的启动线程和 redo 文件管理线程曾经同时扩容 inode 表。当前实现通过 shim 的递归互斥锁串行化共享状态访问和 SPDK 轮询。`access()` 对明确属于 Linux 的绝对路径提前分流，使 DPDK 初始化线程检查 `/sys` 时可以继续执行。

### 3.4 MySQL 接口兼容

补齐了 `lseek` 的 `SEEK_CUR` 和 `SEEK_END`，以及 `posix_fallocate64`、`fallocate64`、`ftruncate64` 等 MySQL 用到的入口。虚拟 `chdir` 和 `realpath` 让 InnoDB 能扫描正确的数据目录。普通虚拟目录上的 `readlink` 返回 `EINVAL`，MySQL 删库时会继续走 `rmdir`，从而支持 `mysqlslap` 多轮建库、删库。

## 4. 测试方案

### 4.1 fio、RocksDB 和 FastDFS

| 应用 | 测试方法 |
| --- | --- |
| fio | 使用 `sync` / `psync` 引擎测试文件写入。以 [zvfs_sync.fio](zvfs_sync.fio) 为起点，将 `filename` 指向 `/zvfs` 下的测试文件；Linux 对照使用真实文件路径 |
| RocksDB | `db_bench` 单线程执行 100,000 条顺序写入、flush，然后重新打开数据库执行随机读取，具体参数见 [RocksDB 测试记录](LearnSpdk.md#rockdb) |
| FastDFS | 分别把存储目录放在 ZVFS 和 Linux 文件系统，通过测试客户端执行上传和下载，配置与日志见 [FastDFS 测试记录](LearnSpdk.md#fastdfs) |

### 4.2 MySQL 功能测试

使用 MySQL 8.0.46。按 [MySQL 测试记录](LearnSpdk.md#跑通并测试性能) 初始化空的 `/zvfs/mysql-test`，再以前台模式启动服务。ZVFS 侧只给 `mysqld` 设置 `LD_PRELOAD`，通过 `/run/mysql-zvfs/mysql.sock` 连接。验证 `@@datadir` 后，在 InnoDB 上建表、插入并查询，随后正常关库并重启，检查原记录仍能读回。

### 4.3 MySQL 性能测试

两边使用同一个 `mysqld` 和相同的服务器选项。关闭 native AIO、binlog 和缓冲池预热，InnoDB 读、写 I/O 线程分别设为 1。用相同的 `mysqlslap` 参数分别测 ZVFS 和 Linux，每次只运行一个 MySQL 实例。

```bash
/home/gyq2/src/mysql-8.0.46/install/bin/mysqlslap \
  --no-defaults -uroot --socket=/run/mysql-zvfs/mysql.sock \
  --create-schema=mysqlslap_bench --engine=InnoDB \
  --auto-generate-sql --auto-generate-sql-load-type=mixed \
  --concurrency=1 --iterations=3 --number-of-queries=10000
```

Linux 侧使用 `/tmp/mysql-linux-bench` 作为数据目录，将压测命令的 socket 换成 `/run/mysql-linux-bench/mysql.sock`。完整的初始化、启动和关库命令见 [测试记录](LearnSpdk.md#跑通并测试性能)。`mysqlslap_bench` 专用于本测试，`mysqlslap` 会创建并删除这个库。

## 5. 性能数据

### 5.1 当前阶段汇总

| 负载 | Linux | ZVFS |
| --- | ---: | ---: |
| RocksDB `fillseq`，单线程 100,000 条 | 1,133 ops/s | 4,580 ops/s |
| RocksDB `readrandom`，单线程 30 秒 | 968,816 ops/s | 900,354 ops/s |
| FastDFS 上传，6,556 个文件 | 约 728 文件/s | 约 437 文件/s |
| FastDFS 下载，300 秒 | 约 1,831 文件/s | 约 1,408 文件/s |
| MySQL `mysqlslap mixed`，单客户端、每轮 10,000 条 SQL | 16.731 s/轮 | 13.737 s/轮 |

这些是开发过程中不同应用、不同负载的测试结果。fio 早期记录使用相对文件名 `a.txt`，这里保留接口验证结果，原始输出见 [fio 记录](LearnSpdk.md#fio)。

### 5.2 MySQL 明细

| 数据目录 | 轮数 | 平均耗时 | 最快–最慢 | 按平均耗时折算 |
| --- | ---: | ---: | ---: | ---: |
| ZVFS `/zvfs/mysql-test` | 3 | 13.737 s | 12.936–14.262 s | 约 728 条 SQL/s |
| Linux ext4 `/tmp/mysql-linux-bench` | 3 | 16.731 s | 16.431–16.985 s | 约 598 条 SQL/s |

在这组混合负载中，ZVFS 平均每轮少用 2.994 秒，耗时降低约 17.9%，折算吞吐高约 21.8%。当前 ZVFS 的 `fsync`、`fdatasync` 对虚拟 fd 直接返回成功，两侧刷盘语义仍待等价验证。当前数据验证覆盖正常关库后的重启读回，断电恢复仍待测试。

## 6. 开发记录

- [LearnSpdk.md](LearnSpdk.md) 记录从 SPDK 异步 I/O 封装、inode 持久化到 RocksDB、FastDFS、MySQL 的接入过程，以及各项测试的原始输出。
- FastDFS 调优记录包含 `perf` 分析、128 KiB 缓冲区和批量读写的修改过程。见 [FastDFS 性能调优](LearnSpdk.md#性能调优和分析)。
- MySQL 接入记录包含 inode 表并发问题、锁保护范围、启动参数以及 Linux 对照。见 [MySQL 接入](LearnSpdk.md#mysql)。
