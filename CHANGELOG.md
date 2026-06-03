# 5.12 阶段完成记录

> 本文用于记录当前阶段已经完成的主链工作、为了完成这些工作曾经侵入过哪些非主责模块，以及哪些侵入最终保留在当前版本中。  
> 它不是设计文档，而是一个“当前版本快照”。

## 1. 核心摘要

当前阶段已经完成的主线可以概括为：

`cache 主链`
`-> inode / dir_inode 主链`
`-> fs_manager / super_manager`
`-> cow_reclaim_registry`
`-> fs_handler 当前主模型`

并且这些主线大多已经进入：

`实现成立`
`-> 单测锁住`
`-> 文档对齐`

当前仍保留在代码中的跨模块侵入，主要是为了：

- 提升可测性
- 补足 `superblock journal`
- 补齐 `tx complete -> reclaim`

## 2. 当前已经完成的内容

### 2.1 缓存主链

已完成并整理文档/测试的组件：

- `block_buffer`
- `cache_index_manager`
- `cache_lru_replacer`
- `generic_cache_manager`
- `page_cache`
- `sit_nat_cache`
- `node_block_cache`
- `super_cache`

### 2.2 inode / dir_inode 主链

已完成并整理文档/测试的组件：

- `inode`
- `inode_loader`
- `dir_inode`
- `dir_inode_resolver`
- `dir_handler`

### 2.3 fs 主链

已完成并整理文档/测试的组件：

- `fs_manager`
- `super_manager`
- `cow_reclaim_registry`
- `nat_utils`
- `sit_utils`
- `fs_handler` 当前主模型

### 2.4 一致性主线

当前已经推进到：

- `data COW`
- `node COW`
- `NAT/SIT/SuperBlock journal`
- `tx complete hook`
- `old version reclaim`

对应总文档：

- [COW 元数据日志一致性总设计](/home/rtfs/codex/COW元数据日志一致性总设计.md)

## 3. 为完成主线而侵入过的其他模块

这里记录“曾经为了主线推进而修改过，但不完全属于当前主责范围”的模块。

### 3.1 journal

侵入目的：

- 为 `tx complete -> reclaim` 提供正式回调点
- 修正多轮 `Setup/Fini` 生命周期中的后台状态机问题

保留的改动包括：

- `journal_processor`
  - 初始化 pending/txRecord 链表头
  - 线程退出前显式 destroy processor
  - 支持 default tx complete hook
- `journal_process_env`
  - 增加提交队列/退出状态的测试观测接口

这些改动当前仍保留。

### 3.2 super_cache

侵入目的：

- 让 `fs_manager` 的 `Setup()` 可以在单测里脱离真实读盘

保留的改动包括：

- `superCacheSetReadBlockHook(...)`

这些改动当前仍保留。

### 3.3 node_block_cache

侵入目的：

- 让 `inode_loader` 与 node-COW 路径能在单测里脱离真实 I/O
- 修正析构 warning 条件错误

保留的改动包括：

- `nodeBlockCacheSetReadBlockHook(...)`
- `nodeBlockCacheDestroy()` 的 warning 条件修正

这些改动当前仍保留。

### 3.4 sit_nat_cache / sit_utils

侵入目的：

- 让 `SitNatCache` 命中/miss/replace 主链可单测
- 修正 `SIT` handle 生命周期

保留的改动包括：

- `sitNatCacheSetReadBlockHook(...)`
- `sitChangeLpaState()` 结束前 destroy handle

这些改动当前仍保留。

## 4. 当前版本中仍保留的跨模块修改

这是“当前代码里真实还在”的保留项，不是历史痕迹。

### 4.1 `fs_manager`

当前版本保留：

- 接入 `srmap_utils_`
- 删除历史占位：
  - `dir_data_cache_`
  - `fd_arr_`
- 增加测试故障注入：
  - `fileSystemManagerSetSetupFailureStepForTest(...)`

### 4.2 `super_manager`

当前版本保留：

- `SuperBlockJournalEntry` 追加逻辑
- `LPA` 分配无空闲段时返回 `INVALID_LPA`
- `uncommit_*_segs` 使用真实旧 `segid`
- NAT/SIT handle 生命周期修正

### 4.3 `cow_reclaim_registry`

当前版本保留：

- 独立模块实现
- 独立测试
- 文档入口

### 4.4 测试目录对齐

当前版本保留：

- `test/inode/*`
- `test/dir_inode/*`

也就是测试目录已经按生产代码结构收敛，不再全部堆在 `test/fs`。

## 5. 侵入过但当前未继续扩大的方向

这些方向已经被识别出来，但当前没有继续硬做。

### 5.1 SRMAP 闭环

当前只记录问题，不继续扩展实现。

原因：

`SRMAP` 的剩余缺口不是 `super_manager` 单文件问题，而是：

- `node/data` 写出点拿到 owner 语义
- `SRMAP` 更新时机
- `SRMAP` 持久化策略
- 是否进入 journal

所以当前只作为后续跨模块问题记录在：

- [super_manager 组件说明](/home/rtfs/codex/7super_manager/组件说明.md)
- [COW 元数据日志一致性总设计](/home/rtfs/codex/COW元数据日志一致性总设计.md)

### 5.2 非 fs_manager 范围的死代码清理

本阶段曾短暂清理过：

- `comm_api`
- `rtfs_multithread`
- `journal_writer`

中的注释残骸，但已按职责边界回退。  
当前版本里仅保留 `fs_manager` 范围内你认可的收紧。

## 6. 当前仍明确未完成、但不计入本阶段主责收尾的内容

这些是“全项目仍有缺口”，但不属于本轮已完成主线的直接否定。

- `journal` 更深执行链与恢复链增强
- `SRMAP` 一致性闭环
- `file_handler`
- `comm_api` 扩展接口族
- `fs_handler` 的 `chown / symlink / readlink`
- `replace protection / 更深 GC 协同`

## 7. 当前结论

当前版本最准确的状态是：

`你负责的主链已经基本完成`
`并且为了把它做实，已经对少量底层模块做了最薄、最必要的侵入增强`

而这些侵入的核心特征是：

- 以测试可观测性为主
- 以生命周期正确性为主
- 以一致性主链闭环为主
- 没有继续扩张到你未接管的更深模块实现

## 8. 一句话总结

> 当前版本不是“所有模块都完成了”，而是：**主责链路已经完成，并且与之直接耦合的少量底层能力，也已经被补到足以支撑这条主链稳定运行和验证。**
