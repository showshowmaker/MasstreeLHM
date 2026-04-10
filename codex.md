# LHM over Masstree: Codex Context

## 1. 项目目标

本项目希望基于 Masstree 设计一个面向高带宽 SSD 的高性能命名空间元数据管理系统 LHM。核心目标不是把 Masstree 当作普通字符串 KV 索引来使用，而是把它改造成一个能够表达文件系统路径层级语义的内存路由层。

整体上，LHM 的设计目标包括：

- 将扁平字符串路径映射为具有层级语义的查找过程。
- 利用 Masstree/B+Tree 的局部有序性提升 `readdir`/目录扫描效率。
- 通过子树挂接与摘除实现目录级 `rename` 的 O(1) 复杂度。
- 通过路径分量哈希和键值分离降低空间冗余，提高缓存友好性。
- 将 Masstree 作为纯内存路由网，把真实 inode 数据放到底层目录感知的 DMB 存储中。

## 2. 核心设计理解

### 2.1 路径 key 结构重构

原始文件路径如 `/A/B/C` 不再作为一个变长字符串整体参与比较，而是：

1. 先按 `/` 切分为路径分量。
2. 每一级路径分量哈希为一个 `uint64_t`。
3. 整条路径最终表示为一个 `vector<uint64_t>`。

这个 `vector<uint64_t>` 不是普通序列化 key，而是表示一条逐层路由路径。目标是让 Masstree 能够逐级消费这些 64-bit 分量，并在逻辑上表现为一棵按目录层级展开的 64-bit 前缀树。

这样做的直接结果是：

- 同一目录下的孩子会自然聚集在同一层或同一子树中。
- 目录遍历更容易转化为局部扫描。
- 路径语义进入了索引结构本身，而不是只存在于上层 API。

### 2.2 键值解耦与键值分离

LHM 的另一个核心点是同时做两层解耦：

- 键值解耦：key 不再是原始变长路径字符串，而是层级化的固定长度 64-bit 指纹序列。
- 键值分离：Masstree 的叶子节点不直接存放完整 inode，而是存放一个紧凑物理地址指针，例如 `uint32_t block_id + uint32_t offset`。

因此，Masstree 在 LHM 中承担的是“纯内存命名空间路由层”的职责，而不是完整的元数据承载层。真正的 inode 内容位于底层 DMB 中。

### 2.3 目录级 O(1) rename

目录重命名不是去批量修改所有后代路径的前缀，而是把被重命名目录视为一棵可独立引用的子树。

例如将 `/A/B/Y` 重命名到 `/X/Y` 时，目标不是改写 `/A/B/Y/...` 下所有 key，而是：

1. 在 `/X` 下新增一个指向 `Y` 子树的入口。
2. 删除 `/A/B` 下原来指向 `Y` 子树的入口。

因此，rename 的本质是“父节点对子树引用关系的切换”，而不是“全量 key 迁移”。这也是实现 O(1) 的关键。

要做到这一点，Masstree 内部必须支持或扩展出以下语义：

- 能够显式定位目录子树入口。
- 能够区分普通文件项和目录项/子树边界项。
- 能够在并发读场景下安全切换引用并延迟回收旧指针。

### 2.3.1 基于源码的语义判断

已经阅读 `masstree_struct.hh`、`masstree_get.hh`、`masstree_insert.hh`、`masstree_remove.hh` 和 `masstree_tcursor.hh`，当前判断如下。

#### A. 是否支持显式定位目录子树入口

结论：支持“结构层面”的子树入口表示与定位，但还不是文件系统语义下的显式目录对象。

依据：

- `leafvalue` 本身就是一个联合体，既可以存普通 `value_type`，也可以存 `node_base*` 形式的下一层树指针。
- `leaf::keylenx_` 用 `layer_keylenx = 128` 区分“该槽位是 layer 入口”而不是普通值。
- `leaf::is_layer()`、`leafvalue::layer()`、`ksuf_matches()` 已经把“命中普通值”与“命中下一层树入口”区分开。
- `unlocked_tcursor::find_unlocked()` 和 `tcursor::find_locked()` 在 `match < 0` 时会自动 `shift` 当前 key 并跳转到下一层 root。
- `tcursor::make_new_layer()` 已经实现了在插入时把一个原普通 value 槽位改造成 layer 入口，并挂接新的下层子树。

这说明 Masstree 原生就支持“树中某个槽位代表下一层子树入口”这件事，所以 LHM 所需的“目录子树入口”不是从零开始设计，而是可以复用现有 layer 机制。

但限制也很明确：

- 当前这个 layer 语义本质上服务于 Masstree 自身的字符串分层存储，不是“目录 inode”或“目录边界节点”的一等抽象。
- 代码能定位“某个 key 对应的 layer 槽位”，但没有直接提供“目录节点对象”这一更高层 API。

因此，对 LHM 来说，可以认为“目录子树入口”在内核结构上是存在的，但需要再包一层目录语义。

#### B. 是否支持区分普通文件项和目录项/子树边界项

结论：部分支持。当前只支持“普通 value”与“layer 指针”二分，不支持 LHM 语义下完整的目录项/文件项类型系统。

依据：

- `leafvalue` 联合体只区分两种存储形态：`value_type` 或 `node_base*`。
- 读路径根据 `keylenx_` 是否是 `layer_keylenx` 来判断该槽位是否进入下一层。
- `make_new_layer()` 会把已有普通值替换成 layer 入口，并通过 `mark_insert()` 让并发读者重试。

这意味着 Masstree 内核已经具备以下区分能力：

- 普通叶子 value
- 指向下一层树的 layer 入口

但它还不具备以下 LHM 需要的能力：

- “普通文件 inode 指针”与“目录 inode 指针”的显式类型区分
- “目录项本身”与“目录的子树边界项”之间的高层语义区分
- 后续为了 O(1) rename 可能需要的“可搬移目录入口对象”表示

所以这部分不能直接说“源码已经支持目录项/文件项区分”，更准确的说法是：源码支持一种足够接近的底层二元结构，我们需要在 `value_type` 或上层目录项封装中补齐真正的文件系统类型语义。

#### C. 是否支持并发读场景下安全切换引用并延迟回收旧指针

结论：支持，而且这是当前源码里最可直接复用的部分。

依据：

- 所有节点都带 `nodeversion`，读路径通过稳定版本检查、变化检测和重试来避免读到不一致状态。
- `make_new_layer()` 在把普通 value 槽位改成 layer 槽位时，会先 `mark_insert()`，再更新 `lv_` 和 `keylenx_`，用版本变化逼迫无锁读者重试。
- `find_locked()` 在遇到 layer 切换、非 root layer、`deleted_layer()` 等状态时，都有专门的重试与父指针修正逻辑。
- `gc_layer()` 和 `remove_leaf()` 在删除空层、折叠冗余层、移除叶子和 internode 时，统一通过 `deallocate_rcu()` 延迟释放。
- `gc_layer_rcu_callback` 说明源码已经把“结构修改后延迟回收”的流程接入 RCU 回调。
- 删除路径中还维护了 `phantom_epoch`，用于在节点责任区间变化时保留时间戳/可见性语义。

这说明 Masstree 已经原生支持：

- 并发读线程无锁遍历
- 写线程通过版本位和锁安全切换指针
- 旧节点通过 RCU 延迟回收，避免悬挂指针

不过需要注意两点边界：

- 当前安全切换逻辑主要覆盖的是 Masstree 自己的 layer 建立、层折叠、叶子删除和内部节点替换。
- LHM 如果要实现“目录从一个父节点摘下，再原子挂到另一个父节点”的 rename，需要在现有机制上扩展成“跨父节点的子树引用重定向”，这比当前的单路径分层/回收更强。

因此，这一项的结论不是“无需改动”，而是“并发安全基础已经存在，可以作为 O(1) rename 的实现底座”。

### 2.4 inode 与文件名

由于目录项只保留了 64-bit 哈希值，原始文件名不能从树结构中直接恢复，因此 inode 中必须保存文件名。这样做主要有三个目的：

- 处理哈希冲突时做真实名字校验。
- 支持 `ls`/`readdir` 输出原始文件名。
- 支持目录项一致性验证。

当前设计假设 inode 固定 128B，以换取更简单的布局和更强的块内组织性。

### 2.5 DMB 目录感知存储

底层 DMB 以 16KB 为基本单位管理 inode 数据。系统希望把同一目录下写入的 inode 尽量组织到同一个 16KB 块中，并在块头记录该块所属目录。

这样可以把“目录局部性”同时体现在：

- 上层 Masstree 的层级路由局部性。
- 下层 DMB 的物理布局局部性。

最终目标是让目录扫描、批量 stat、目录级预取等操作能够受益于连续块布局。当前阶段先用大内存模拟 SSD，优先打通分配、寻址和块管理逻辑。

## 3. 阶段性实现路线

### 第一阶段：切断字符串，重塑数据结构

目标是修改 Masstree 的 key 表达方式，使其能够接受 `vector<uint64_t>` 形式的层级 key，并逐层完成路由。

这一阶段的本质是把 Masstree 从“字符串导向”改造成“路径分量导向”。

#### 第一阶段当前推进结论

在阅读 `masstree_key.hh` 后，当前最稳妥的第一阶段实现路线不是直接重写 Masstree 的核心 `key` 类型，而是先做一层 `vector<uint64_t>` 适配。

原因是：

- 现有 `Masstree::key` 的底层比较单元本来就是 `uint64_t ikey`。
- 现有 `shift()` / `shift_by()` / `suffix()` 机制本来就是按固定 `ikey_size` 字节步长逐层下降。
- 如果把 `vector<uint64_t>` 序列化成连续的 8-byte big-endian 字节流，那么现有 Masstree 就会天然把每个 `uint64_t` 分量当成一层消费。

因此，第一阶段的核心策略更新为：

- 不立即推翻现有字符串 key 内核。
- 将“路径 key”实现为一个外部适配层，把 `vector<uint64_t>` 包装成 Masstree 可接受的 `Str`。
- 利用现有 layer 机制验证每个路径分量是否能被当成独立层级路由。

#### 第一阶段适配方案

当前方案选择新增一个轻量 `PathKey` 适配类，其职责是：

1. 接收 `vector<uint64_t>` 风格的路径分量数组。
2. 将每个分量按 big-endian 序编码为 8 字节。
3. 将所有分量顺序拼接成连续字节串。
4. 对外暴露 `Str as_str()`，供现有 `Masstree::basic_table`、`tcursor`、`get`、`scan` 直接使用。

这样做有几个直接好处：

- 复用现有 `Masstree::key` 与并发控制逻辑，避免第一阶段就深入改动内核。
- 每一级路径分量都严格对应一个 8-byte slice，与 LHM 的设计目标一致。
- 后续如果决定进一步把 `key` 真正改造成原生 `vector<uint64_t>` 视图，也可以沿着这个编码协议演进，不会推翻上层调用方式。

#### 为什么使用 big-endian 序列化

Masstree 当前使用 `string_slice<ikey_type>::make_comparable()` 将字符串前 8 字节解释成网络序整数，以保证字典序与整数比较顺序一致。

因此，`vector<uint64_t>` 适配层必须把每个分量编码成 big-endian 顺序，才能满足：

- Masstree 内部比较规则不变。
- 不同路径 key 之间的层级排序行为稳定。
- 同层目录扫描仍然遵守组件级顺序比较。

#### 第一阶段当前代码落地

目前已新增 `path_key.hh`，提供：

- `PathKey(std::vector<uint64_t>)`
- `as_str()`
- `debug_string()`
- `decode(Str)`
- `hash_component(const std::string&)`
- `parse_absolute_path(const std::string&)`
- `from_absolute_path(const std::string&)`

其中 `decode()` 主要用于测试和扫描输出验证，确保 scan 返回的二进制 key 能被还原回组件数组。

#### 从适配层推进到真正路径解析器

第一阶段已经进一步推进为“绝对路径字符串 -> 哈希组件数组 -> PathKey”的完整链路。

当前路径解析规则如下：

- 仅支持绝对路径，即必须以 `/` 开头。
- 自动忽略重复的 `/`，例如 `//usr///bin` 会被规范化处理。
- 当前不支持 `.` 和 `..`，遇到时直接报错，因为这两者属于更完整的 POSIX 规范化语义，现阶段先不混入。
- 根路径 `/` 会被解析为空组件数组，表示空前缀。

当前哈希策略如下：

- 每个路径分量独立哈希为一个 `uint64_t`。
- 目前使用确定性的 FNV-1a 64-bit 哈希，优点是实现简单、无状态、便于测试复现。
- 哈希结果随后按 big-endian 编码拼接，继续复用现有 Masstree 的按 8-byte slice 分层能力。

这意味着当前已经可以直接从 `/usr/local/bin/gcc` 得到：

- 规范化路径字符串
- 原始路径分量数组
- 每一级分量的 64-bit 哈希数组
- 最终可直接送入 Masstree 的 `PathKey`

#### 当前小型测试更新

`main.cpp` 现在除了原有的字符串 key 测试和手工 `vector<uint64_t>` 测试之外，又新增了一组“真实路径字符串”测试：

- 将 `/usr/local/bin/gcc`、`/usr/local/bin/clang`、`/usr/local/share/doc`、`/var/log/messages` 解析成哈希路径 key 后插入。
- 对这些绝对路径执行点查。
- 输出每条路径解析后的哈希组件数组。
- 检查共享前缀路径 `/usr/local/bin/gcc` 与 `/usr/local/bin/clang` 的分层效果。
- 对全表 scan 后反解出组件数组，确认路径 key 的编码与解码一致。

这一阶段的意义是：

- 我们已经不再只是在测试“人工构造的 vector key”，而是在测试“真实路径字符串经过解析后能否进入 Masstree”。
- 第二阶段 `lookup_file(path)` 和 `creat_file(path, data)` 所需的最核心前置能力已经具备雏形。
- 后续只需要在此基础上叠加目录项类型、inode 指针和值语义，就可以继续往文件系统 API 靠拢。

#### 第一阶段测试策略

`main.cpp` 中新增了 vector 风格路径 key 的最小测试，当前覆盖以下验证点：

- 插入多个 `vector<uint64_t>` 路径 key。
- 对这些路径 key 做点查。
- 验证共享前缀路径（如 `{1,10,100}` 和 `{1,10,101}`）可以同时正确命中。
- 对全表做 scan，并把返回的二进制 key 反解成 `vector<uint64_t>` 输出。

这一测试的目标不是证明文件系统语义已经完成，而是先验证：

- “路径分量数组 -> Masstree key” 的适配是可行的。
- 现有 layer 机制可以被复用于 LHM 的路径分层。
- 第一阶段可以先在不重写内核的前提下继续推进。

#### 第一阶段 demo 输出所证明的结论

根据当前 `./masstree_demo` 输出，可以确认第一阶段已经完成了以下最小可行验证：

1. 原始字符串 key 路径未被破坏。
   `String key point lookups`、`String key full scan` 和 `String key after removals`
   说明当前引入的路径 key 适配与解析逻辑没有破坏 Masstree 原有的插入、点查、扫描和删除能力。

2. 手工构造的 `vector<uint64_t>` 路径 key 已经可以直接进入 Masstree。
   `Vector-style path key point lookups` 说明 `PathKey` 已经能够把路径分量数组稳定编码成 Masstree key，并支持正确命中。

3. 共享前缀的路径分量数组可以被正确区分。
   `Shared-prefix lookup check` 中 `{1,10,100}` 与 `{1,10,101}` 前两级完全相同、最后一级不同，但都能正确查询成功。这表明当前“每 8 字节一个层级”的设计已经能够表达层级路径语义，而不是退化成单个平面 hash。

4. 路径 key 的编码/解码协议是自洽的。
   `Vector-style path key full scan` 说明写入 Masstree 的二进制 key 在 scan 后可以通过 `decode()` 还原成原始组件数组，因此当前编码协议是稳定可逆的。

5. 真实绝对路径字符串已经能走完整解析链路。
   `Absolute path parser point lookups` 说明 `/usr/local/bin/gcc` 这类路径已经可以完成：
   路径字符串 -> 分量切分 -> 分量哈希 -> PathKey -> Masstree 插入/查询。

6. 真实路径的共享目录前缀已经能被保留下来。
   `Parsed shared-prefix check` 中 `/usr/local/bin/gcc` 和 `/usr/local/bin/clang`
   的前三个哈希分量完全一致，只有最后一个分量不同，说明当前解析器已经把目录层级结构映射进 key，而不是把整条路径整体哈希。

7. 真实路径哈希 key 的 scan 结果也是一致的。
   `Absolute path parser full scan` 说明真实路径进入 Masstree 后，扫描结果仍然能反解出正确的哈希组件序列。

因此，当前已经可以明确地说：

- 第一阶段“切断字符串，重塑 key 表达”的核心链路已经跑通。
- 我们已经证明了现有 Masstree 可以通过外部适配层承载 `vector<uint64_t>` 风格的路径 key。
- 我们已经证明了绝对路径字符串可以稳定转换成分层 key，并进入 Masstree。

但同时也要明确当前还没有完成的部分：

- 还没有正式实现 `lookup_file(path)` 和 `creat_file(path, data)` 的文件系统语义包装。
- 还没有定义目录项/文件项/冲突链节点的数据结构。
- 还没有把 value 从简单 `uint64_t` 测试值替换成 inode 指针或 inode 元数据。
- 还没有实现 rename、目录扫描语义和底层 DMB。

### 第二阶段：路径解析与点查实现

在第一阶段完成后，将 POSIX 路径语义接到新 Masstree API 上。优先实现：

- `lookup_file(const std::string& path)`
- `creat_file(const std::string& path, const std::string& data)`

为此需要先实现高效的 Path Parser & Hasher，将绝对路径转换为 `vector<uint64_t>`，再基于该结果调用 Masstree 的 `get`/`put` 接口。

这一阶段的目标是先打通“路径字符串 -> 分层哈希 -> Masstree 路由 -> inode 指针”的主链路。

#### 第二阶段建议的源码修改顺序

基于当前代码状态，第二阶段最合理的推进方式不是立刻大改 Masstree 内核，而是先在现有适配层外面补一个最小的文件系统语义层。建议按下面顺序改。

1. 新增一个最小目录项/文件项值类型。
   当前 `main.cpp` 里表的 `value_type` 还是纯 `uint64_t`，这只适合做 key 测试。
   下一步建议先定义一个轻量结构，例如：
   - `inode_ref`：只保存一个逻辑 inode 编号，或未来的 `block_id + offset`
   - `entry_kind`：区分普通文件、目录、冲突链头等最小类型

   这一层的目标不是一步到位做完整 inode，而是先让 value 从“测试数字”变成“有语义的目录项引用”。

2. 新增一个最小包装类，例如 `lhm_namespace.hh`。
   这个包装类不改 Masstree 内核，只负责：
   - 接收路径字符串
   - 调用 `PathKey::parse_absolute_path()` / `from_absolute_path()`
   - 调用底层 Masstree `get` / `insert`
   - 提供最小接口：
     - `lookup_file(const std::string& path)`
     - `creat_file(const std::string& path, inode_ref value)`

   这样第二阶段的文件系统语义会集中在一个薄封装里，不会污染 `main.cpp`。

3. 明确“目录存在性”与“目标文件项”的最小规则。
   这是第二阶段里最容易被忽略、但必须尽早定下来的语义边界。
   建议先采用最小规则：
   - `creat_file("/a/b/c")` 只在父目录 `/a/b` 已存在时成功
   - 根目录 `/` 视为天然存在
   - 目录本身也先作为一个特殊 entry 存在于同一张表中

   这意味着我们需要开始定义“目录也要有 entry”这一点，而不是只存最终文件。

4. 再补最小的测试驱动。
   当上面两层到位后，`main.cpp` 就可以从“插入测试 key”升级成：
   - 创建目录 `/usr`
   - 创建目录 `/usr/local`
   - 创建文件 `/usr/local/bin/gcc`
   - 对存在和不存在路径做 `lookup`

   这样测试就会从“key 是否可行”升级成“文件系统点查语义是否可行”。

#### 第二阶段里最关键的设计决策

接下来我们需要先一起定下面这个问题，因为它会直接影响源码怎么改：

方案 A：单表方案
- 目录项和文件项都存在同一张 Masstree 表里。
- 目录和文件统一使用 `PathKey(完整路径)`。
- 目录本身作为特殊 entry 类型存在。

优点：
- 实现快，适合尽快验证 `lookup_file` / `creat_file`
- 不需要立即引入额外索引结构

缺点：
- 后续做更强的目录语义和 rename 时，需要再把目录入口语义细化

方案 B：目录入口强化方案
- 在单表基础上，显式引入“目录入口项”和“文件项”两种记录。
- 让目录成为后续 rename、readdir 的正式边界对象。

优点：
- 更接近最终 LHM 目标

缺点：
- 第二阶段实现复杂度会明显上升

当前我更推荐先走方案 A，把第二阶段目标收敛到：

- 路径解析
- 父目录检查
- 基于路径字符串的 `lookup_file` / `creat_file`
- value 从 `uint64_t` 测试值升级为最小 inode 引用

这样我们能更快拿到一个可运行的“命名空间点查原型”，然后再在第三阶段前把目录边界语义强化。

#### 第二阶段当前源码落地

目前已经新增 `lhm_namespace.hh`，并完成了第二阶段的最小语义原型，具体包括：

1. 定义最小 entry 类型 `namespace_entry`
- 使用平凡可拷贝的 POD 结构，兼容 Masstree `leafvalue` 对 `value_type` 的 union 存储要求。
- 当前字段为：
  - `entry_kind kind`
  - `inode_ref ref`
  - `uint8_t name_length`
  - `char name[64]`

其中 `inode_ref` 当前采用：
- `uint32_t block_id`
- `uint32_t offset`

这一步已经把 value 语义从“单个测试编号”推进成了“更接近未来 DMB 地址的引用类型”。
同时也把“目录项名字”直接承载进最小 entry 中，为：
- 哈希冲突时的名字校验
- `readdir` 返回真实名字
提供了第一版数据基础。

2. 定义最小类型系统 `entry_kind`
- `directory`
- `file`
- `conflict_chain`
- `invalid`

当前真正用到的是 `directory` 和 `file`，其余类型是为后续冲突链和扩展语义预留。

3. 新增包装层 `LhmNamespace`
- 内部仍然使用一张 Masstree 表。
- 对外提供更接近文件系统语义的接口：
  - `lookup_entry(path)`
  - `lookup_file(path)`
  - `lookup_directory(path)`
  - `mkdir(path, inode_id)`
  - `creat_file(path, inode_id)`
  - `readdir(path)`
  - `rename_path(old_path, new_path)`

4. 采用“根目录隐式存在”的最小语义
- `/` 不显式存表，但 `lookup_directory("/")` 视为成功。
- `mkdir` 和 `creat_file` 都要求父目录已经存在。
- 同一路径重复创建会失败，避免隐式覆盖已有项。

#### 第二阶段当前 demo 输出所证明的结论

根据当前 `Namespace wrapper checks` 输出，可以确认下面这些语义已经跑通：

1. 父目录检查已经生效
- `creat_file(/usr/local/bin/gcc) without parents -> expected failure`
- 说明包装层不再只是“把 path 转成 key 然后盲插入”，而是已经具备最小目录存在性检查。

2. 目录创建语义已经可用
- `mkdir(/usr)`、`mkdir(/usr/local)`、`mkdir(/usr/local/bin)` 全部成功
- 说明当前单表方案已经能把目录本身作为特殊 entry 记录下来。

3. 文件创建语义已经可用
- 在父目录就绪后，`creat_file(/usr/local/bin/gcc) after mkdir -> success`
- 说明“路径字符串 -> PathKey -> Masstree -> namespace_entry”这条命名空间写路径已经打通。

4. 重复创建保护已经生效
- `creat_file(/usr/local/bin/gcc) duplicate -> expected failure`
- 说明当前原型已经具备最小的存在性检查，不会无意覆盖已有记录。

5. lookup 已经可以按类型区分目录和文件
- `lookup_directory(/usr/local/bin) -> {kind=directory, inode=3}`
- `lookup_file(/usr/local/bin/gcc) -> {kind=file, inode=100}`
- `lookup_file(/usr/local/bin) on directory -> expected failure`

这说明 lookup 不再只是“找到某个 value”，而是已经能在包装层上体现最小文件系统类型语义。

6. 不存在路径能够正确返回失败
- `lookup_file(/usr/local/bin/clang) -> not found`
- 说明当前点查语义已经具备最基本的正确性边界。

7. value 已经升级成 inode 引用语义
- 当前目录和文件项打印出来的结果已经从 `{kind=..., inode=...}` 升级成
  `{kind=..., ref=(block=..., offset=...)}`。
- 说明第二阶段原型已经不再依赖单个测试整数，而是开始靠近最终
  `block_id + offset` 的设计目标。

8. 最小 readdir 目录扫描原型已经形成
- 当前 `readdir(path)` 先通过 scan 全表遍历，再按：
  - 父目录哈希前缀
  - 深度等于父目录深度 + 1
  这两个条件过滤出“直接孩子”。
- 当前已经不再只返回孩子哈希，而是可以直接返回 entry 中保存的真实名字。

根据当前测试，`/usr/local/bin` 目录下已经能扫出：
- `gcc`
- `clang`

它们已经能以真实目录项名字出现在输出中，这说明单表方案已经具备：
- 最小名字承载能力
- 最小目录扫描能力
- 后续冲突校验所需的名字基础

9. 最小哈希冲突链原型已经跑通
- 当前包装层已经支持：如果同一目录下两个不同名字落到同一个最终组件哈希槽位，
  Masstree 表中的该 key 会被提升成一个 `conflict_chain` 标记项。
- 冲突目录项本身当前存放在 `LhmNamespace` 内部维护的冲突桶中。
- lookup 时：
  - 先按哈希 key 命中 Masstree
  - 如果命中的是普通项，则直接比对名字
  - 如果命中的是 `conflict_chain`，则进入冲突桶内按真实名字做二次匹配
- `readdir` 时：
  - 如果扫描到普通项，直接返回
  - 如果扫描到冲突链标记，则把桶中的多个目录项展开返回

当前测试已经证明：
- `collision_alpha` 和 `collision_beta` 可以在同目录、同哈希下同时创建成功
- 两者可以分别被 lookup 成功
- `readdir(/usr/local/bin)` 可以同时返回这两个冲突项

10. 最小 rename 语义已经跑通
- 当前已经新增 `rename_path(old_path, new_path)`，但这不是最终 O(1) 目录 rename，而是一个过渡版“可工作语义实现”。
- 它当前采用：
  - 扫描源子树
  - 根据源根路径和目录项名字重建完整子路径
  - 按深度从浅到深在目标位置重建新子树
  - 再按深度从深到浅删除旧子树

当前测试已经证明：
- 单文件 rename 可以成功：
  - `/usr/local/share/doc -> /usr/local/share/readme`
- 目录 rename 可以成功：
  - `/usr/local/bin -> /usr/local/tools`
- 目录 rename 后：
  - 旧路径查找失败
  - 新路径目录存在
  - 子文件如 `gcc`、`clang` 已经随子树一起迁移
  - `readdir(/usr/local/tools)` 可以看到迁移后的子项

#### 第二阶段当前仍未完成的部分

虽然最小命名空间点查原型已经形成，但还没有完成以下能力：

- 还没有实现自动递归建目录或更完整的 POSIX 创建语义
- 还没有将 `inode_id` 替换成真正的 `block_id + offset`
- 还没有实现目录重命名
- 还没有接入 DMB allocator

其中“还没有将 `inode_id` 替换成真正的 `block_id + offset`”这一项现在已经推进到：
- value 中已经使用 `inode_ref(block_id, offset)` 表示
- 但这个引用仍然是手工构造的占位值，还没有接入真实 DMB 分配器

其中，哈希冲突链现在已经有了最小可运行原型，但还存在明显边界：
- 当前冲突链桶保存在 `LhmNamespace` 包装层内存中，而不是 Masstree 内核或 DMB 中
- 当前冲突链还没有专门处理并发修改
- 当前冲突链测试依赖测试专用“强制最后一级哈希”接口，真实自然碰撞场景还没有做压力验证

其中，rename 现在也有明确的边界：
- 当前 rename 仍然是包装层实现，不是 Masstree 内核级子树重挂
- 当前 rename 的复杂度取决于子树大小，本质上是“扫描 + 重建 + 删除”
- 因此它只用于先验证命名空间语义，不代表最终 LHM 目标中的 O(1) 目录 rename

#### 当前已记录的性能 TODO

在 `create_entry()` 中已经明确记录了一个后续优化 TODO：

- 当前实现为了检查父目录存在性，会先执行一次父目录 lookup，然后在插入目标项时重新从根开始定位。
- 这种“两次查找”在语义上正确，但存在额外开销。
- 后续如果要优化成“查到父目录位置后直接继续向下插入”，大概率需要深入 Masstree 内核，扩展 `tcursor` 或新增命名空间感知的插入接口，以复用第一次查找留下的层级上下文。

- 在 `namespace_entry` 中加入固定长度文件名后，命名空间表的 value 体积显著增大。
- 为了保证最小原型在当前 Masstree 线程池分配器约束下可运行，`namespace_table_params` 暂时从默认宽度调整为了较小宽度。
- 这说明后续如果要继续扩大 entry、加入更多 inode 字段，可能需要：
  - 重新审视 entry 布局
  - 或把名字等可变信息移出 leaf value
  - 或调整 Masstree 节点/分配器策略

- 当前冲突链桶只存在于 `LhmNamespace` 包装层中，主要用于快速验证语义。
- 后续如果要走向更真实的实现，需要决定冲突链最终归属：
  - 是继续作为 value 指向的外部结构
  - 还是纳入 inode / DMB 层统一管理
  - 或进一步和 Masstree 内核的值表示深度结合

- 当前 `rename_path()` 之所以能工作，是因为 entry 中已经保存了每一级目录项的名字，
  从而可以在扫描源子树后重建完整目标路径。
- 但这仍然不是最终方向；后续要做真正的 O(1) 目录 rename，仍需回到最初设计，
  深入 Masstree 内核，把目录入口变成可切换的子树引用，而不是做全子树重写。

#### 内核级 rename 分析结论

围绕“如何把 rename 下沉到内核并实现原子指针切换的常数级目录 rename”，当前已经形成如下判断：

- 真正的 O(1) rename 不能继续建立在包装层的“扫描子树 + 重建路径 + 删除旧项”之上。
- 根本原因不是接口不够，而是当前数据组织还没有把目录提升成可独立搬运的子树入口对象。
- Masstree 现有内核已经具备可复用基础：
  - `leafvalue` 可以保存 `node_base*` 形式的 layer 指针
  - `layer_keylenx` 可以把槽位标识为“下一层子树入口”
  - 读路径已经支持沿 layer 自动下钻
  - `make_new_layer()` 已经展示了“写线程改指针 + 版本变化逼读者重试”的模式
  - `gc_layer()` / `deallocate_rcu()` 已经提供延迟回收底座
- 但要实现目录级 O(1) rename，还必须新增更高层的目录语义：
  - 目录不能再只是普通 value，而应成为真正的 layer 边
  - 目录真实名字和 inode_ref 需要挂在目录子树 root 的元数据上
  - 需要新增“按父目录定位目录边”的专用 cursor / API
  - 需要新增“源父目录删边 + 目标父目录挂边”的双边原子操作

这意味着，后续真正的 rename 内核化重点不在包装层，而在：

- `masstree_struct.hh`
- `masstree_tcursor.hh`
- `masstree_get.hh`
- `masstree_insert.hh`
- `masstree_remove.hh`

也就是说，下一步主线首先应该做的不是继续优化旧版 `rename_path()`，而是先把目录 entry 内核化。

#### 目录 entry 内核化：当前最小推进

这一步已经开始落地，当前采用的是“目录走 layer、文件仍走普通 value”的最小原型：

1. 在 Masstree 内核里新增了两个最小能力
- `tcursor::find_locked_edge()`：
  - 停在当前层命中的槽位上
  - 如果命中的是 layer 边，不再自动下钻
  - 这样上层就能显式观察“父目录中的目录边”
- `tcursor::create_layer()`：
  - 在当前层为一个不存在的精确 key 主动创建空 layer
  - 这解决了“空目录不能只靠后代文件触发存在”的问题

2. 目录 lookup / mkdir 已经改成优先走 layer
- `lookup_directory(path)` 不再依赖目录 value 存在，而是逐级按路径组件哈希查找 layer 边
- `mkdir(path)` 不再向表里写一个普通目录 value，而是在父目录对应 layer root 下主动创建一个新的子 layer
- 目录名字和 inode_ref 目前先保存在 `LhmNamespace` 的 `directory_entries_` 元数据映射中，key 是子 layer root 指针

3. 文件路径暂时保持不变
- 普通文件仍然作为 value 存在于 Masstree 表中
- `creat_file(path)` 和 `lookup_file(path)` 继续复用原有 value 路径
- 但父目录存在性检查已经改为依赖目录 layer 路径

4. 目录创建和文件创建都已经改成“父目录 root 内部局部查找/插入”
- 早期版本在创建目录或文件时，会先用完整路径做一次“目标是否已存在”查询，
  然后再重新定位父目录，存在重复从全局根走查找路径的问题。
- 当前已经把这两条创建路径都改成：
  - 先定位父目录对应的 layer root
  - 再在该 root 内部只用“最后一级组件哈希”检查孩子是否已存在
  - 如果不存在，则直接从该 root 内部完成目录 layer 创建或文件 value 插入
- 这意味着：
  - `mkdir()` 不再为了检查目标目录是否存在而重新从全局根走完整路径
  - `creat_file()` 也不再为了检查目标文件是否存在而重新从全局根走完整路径
- 当前为此新增了两个局部辅助逻辑：
  - `lookup_child_from_parent_root()`：在父目录 root 内部做“哈希 + 名字”局部查找
  - `get_child_value_from_parent_root()`：在父目录 root 内部读取原始 value 槽位，用于冲突链插入前判断
- 这一步的意义是：即使还没有把目录元数据真正挂进 layer root，创建路径本身已经开始利用“目录已被内核化为 layer”这一事实，减少了包装层重复查找开销

4. `readdir` 已经开始兼容两类孩子
- 文件孩子仍然通过全表 `scan` + 前缀过滤得到
- 子目录孩子则通过 `directory_entries_` 中记录的 `parent_root -> child_root` 关系补回

#### 当前测试已经证明的点

`main.cpp` 当前新增的测试已经能够证明：

- `mkdir(/usr)` 成功后，即使 `/usr` 下还没有任何文件，`lookup_directory(/usr)` 也能成功
- `mkdir(/usr/local)`、`mkdir(/usr/local/bin)`、`mkdir(/usr/local/share)` 可以逐级成功
- 这说明“空目录作为真实 layer 入口存在”已经成立，而不是依赖目录 value 或后代文件
- 在这些 layer 目录下继续 `creat_file(/usr/local/bin/gcc)`、`creat_file(/usr/local/bin/clang)`、`creat_file(/usr/local/share/doc)` 都能成功
- `lookup_directory` 和 `lookup_file` 已经能够分别命中目录 layer 和普通文件 value
- `readdir(/usr/local)` 现在可以看到直接孩子目录
- `readdir(/usr/local/bin)` 和 `readdir(/usr/local/share)` 可以看到直接孩子文件

#### 当前边界与 TODO

这一版“目录内核化”仍然只是第一步，明确还存在以下边界：

- 目录元数据目前还保存在 `LhmNamespace::directory_entries_` 中，没有真正挂入 Masstree 子树 root 结构体
- 旧的 `rename_path()` 仍然是包装层实现，还没有切到新的目录 layer 语义
- 哈希冲突链、目录删除、目录级 gc、双父节点原子切边，都还没有迁移到新的目录表示上
- 因此，这一步的意义是“先让目录真正成为 layer 边”，为下一步的内核级 rename 做结构铺垫，而不是已经完成 rename 本身

#### 下一步设计：方案 A 的目录 root 尾随 meta

当前已经明确，目录元数据最终不适合继续长期停留在 `LhmNamespace::directory_entries_` 这类包装层映射里，而应尽量贴近“当前目录子树的 root”。在几种候选方案中，当前主张采用：

- 方案 A：仅目录 root 特殊分配一块尾随 `directory_meta`

这个方案的目标是：

- `layer` 槽位里仍然只保存 `node_base*`
- 不给所有节点都加目录元数据字段
- 只有“当前目录子树的真实 root”会携带一块额外目录元数据
- 当 root 因 split / collapse 发生变化时，目录元数据跟着迁移到新的 root

##### 1. `directory_meta` 结构草图

当前建议的第一版最小结构如下：

```cpp
struct directory_meta {
    inode_ref ref;
    uint64_t component_hash;
    uint8_t name_length;
    uint8_t flags;
    char name[kMaxEntryNameBytes + 1];
};
```

它承载的是“目录对象自身的身份信息”，而不是完整 inode。当前阶段它至少满足：

- `lookup_directory`：可直接从 root 旁边读目录名字与 `inode_ref`
- `readdir`：可直接输出真实目录名字
- rename：后续可在目录 root 上更新目录名字而不改所有后代项
- 调试 / 校验：保留父目录中的组件哈希

##### 2. 内存布局草图

当前建议对目录 root 采用统一的“节点本体 + 尾随 meta”布局。

对 `leaf root`：

```text
+----------------------+
| leaf<P> root node    |
| ...                  |
| iksuf / extra space  |
+----------------------+
| directory_meta       |
+----------------------+
```

对 `internode root`：

```text
+----------------------+
| internode<P> root    |
| ...                  |
+----------------------+
| directory_meta       |
+----------------------+
```

这里的关键约束是：

- `node_base*` 仍然指向节点本体起始地址
- `directory_meta` 不内嵌进所有节点类型
- 只有目录 root 的特殊分配路径才额外多出这段尾随空间

##### 3. `leaf root` 和 `internode root` 的分配设计

当前设计判断如下：

- `leaf` 本来就支持变长分配，因为它已经需要为 `ksuf` 预留额外空间，见：
  - `leaf::make()` [masstree_struct.hh:299]
  - `leaf::allocated_size()` [masstree_struct.hh:320]
- 因此 `leaf root` 最适合扩展出一条新的特殊分配路径，例如：
  - `leaf::make_root_with_meta(...)`

建议 `leaf root` 的分配方式为：

1. 先按现有逻辑计算普通 root 所需大小
2. 再追加 `sizeof(directory_meta)`
3. 继续按 64B 对齐
4. 在尾部 placement-new `directory_meta`

而 `internode` 当前是固定大小分配，见：

- `internode::make()` [masstree_struct.hh:120]

所以仅让 `internode root` 特殊支持尾随 meta，需要新增一条仅 root 使用的专用分配路径，例如：

- `internode::make_root_with_meta(...)`

这里不改变所有 internode 的分配策略，只要求：

- 普通 internode 继续走固定大小 `make()`
- 只有目录 root 在需要时走 `make_root_with_meta()`

##### 4. 访问目录 meta 的统一辅助接口

为了避免后续代码到处手算偏移，当前建议统一增加这类辅助接口：

```cpp
directory_meta* directory_meta_of(node_base<P>* root);
const directory_meta* directory_meta_of(const node_base<P>* root);
bool has_directory_meta(const node_base<P>* root);
size_t root_allocated_size(const node_base<P>* root);
```

这些接口的设计目标是：

- `lookup_directory` 只关心“给定 root，能否拿到目录元数据”
- `readdir` 只关心“给定子目录 root，能否直接读名字和 `inode_ref`”
- split / collapse 只关心“旧 root 上有没有 meta，怎样迁到新 root”
- 回收路径只关心“当前 root 的实际分配大小是多少”

##### 5. root 变化时的迁移规则

方案 A 成败的关键在于：目录元数据属于“当前目录子树的真实 root”，而真实 root 不是静止不变的。

当前至少需要覆盖两类 root 变化：

1. `leaf root -> internode root`
- 触发点在 [masstree_split.hh:218]
- 当前逻辑会在 root 分裂时新建一个 `internode nn`
- 方案 A 下需要扩展成：
  - 如果旧 root 有 `directory_meta`
  - 则新 root `nn` 必须通过 `make_root_with_meta()` 创建
  - 并把旧 root 的 meta 迁移到 `nn`

2. `internode root -> child root`
- 触发点在 [masstree_remove.hh:73]
- 当前逻辑在 `gc_layer()` 中会把冗余 root internode 的唯一 child 提升为新的 root
- 方案 A 下需要扩展成：
  - 如果旧 root internode 有 `directory_meta`
  - 则在提升 child 成 root 前，把 meta 迁移到 child

因此，当前建议增加一个统一迁移辅助接口：

```cpp
void move_directory_meta(node_base<P>* old_root,
                         node_base<P>* new_root,
                         threadinfo& ti);
```

##### 6. 回收路径需要同步覆盖

由于 `directory_meta` 是尾随 root 分配出来的，因此回收设计必须满足：

- `leaf root` 被 `deallocate_rcu()` 时，meta 随 root 一起释放
- `internode root` 被 `deallocate_rcu()` 时，如果它带 meta，也随 root 一起释放
- 不允许出现“meta 单独泄漏”或“old root 已退位但 meta 还停在 old root”这两种情况

因此，这个方案不只是“改分配”，还要求：

- `leaf` 与 `internode` 都能知道自己的实际 root-special 分配大小
- 回收路径按实际分配大小而不是单纯 `sizeof(node)` 释放

##### 7. 当前需要修改的方法清单

如果正式进入方案 A 的代码实现，当前判断至少需要修改以下方法或类。

在 `masstree_struct.hh` 中：

- `node_base<P>`
  - 新增目录 meta 访问辅助接口的声明，至少让外部能统一从 `node_base*` 读取 root meta
- `leaf<P>`
  - 扩展 `make_root()`，或新增 `make_root_with_meta()`
  - 扩展 `allocated_size()` 或对应 meta 定位逻辑
  - 扩展 `deallocate()` / `deallocate_rcu()`，确保目录 root 特殊分配能被正确释放
- `internode<P>`
  - 新增 `make_root_with_meta()`
  - 新增“仅 root special allocation 使用”的分配大小记录能力
  - 扩展 `deallocate()` / `deallocate_rcu()` 或等效释放路径

在 `masstree_split.hh` 中：

- `tcursor<P>::make_split()`
  - root 分裂时，若旧 root 带 `directory_meta`，新建 root internode 必须携带 meta
  - 在旧 root 和新 root 之间执行显式 meta 迁移

在 `masstree_remove.hh` 中：

- `tcursor<P>::gc_layer()`
  - root internode 收缩为 child root 时，执行 meta 迁移
  - empty root 删除时，确保 meta 与 root 一起回收
- 相关 RCU callback 路径
  - 确保 old root 被延迟回收时不会遗留 dangling meta

在 `lhm_namespace.hh` 中：

- `create_directory_from_parsed()`
  - 目录 layer 创建完成后，不再向 `directory_entries_` 写入主元数据
  - 改为直接初始化新 root 尾随 `directory_meta`
- `lookup_entry_from_parsed()`
  - 目录 lookup 改为直接从 root 读取 meta
- `append_directory_children()`
  - 子目录枚举改为围绕 child root 读取 meta，而不是扫 `directory_entries_`
- `directory_entries_`
  - 先降级为过渡对照结构，最后完全删除

##### 8. 当前实现边界与风险判断

方案 A 当前已经明确可行，但风险也很清楚：

- `leaf root` 侧实现相对顺畅，因为 leaf 本来就支持变长分配
- `internode root` 侧是整个方案的难点，因为当前 internode 是固定大小分配
- 方案 A 的真正工作量不在“定义一个 `directory_meta` 结构体”，而在：
  - root-special 分配
  - root 迁移时的 meta 迁移
  - 回收时的正确释放

因此，当前阶段更合理的推进顺序是：

1. 先把方案 A 的数据结构和分配/迁移/回收接口钉住
2. 再优先落地 `leaf root` 侧
3. 最后补齐 `internode root` 侧和 split / gc_layer 路径

在这一步完成之前，`directory_entries_` 仍然是必要的过渡实现，但它已经不再是目标状态。

##### 9. 当前已落地的第一步：`leaf root` 尾随 meta

方案 A 的第一步已经开始实现，当前状态如下：

- 新增了 [directory_meta.hh]
  - 将 `inode_ref`
  - `kMaxEntryNameBytes`
  - `directory_meta`
  - 以及 `make_directory_meta()` / `directory_meta_name()`
  这类目录 root 元数据相关定义抽出成独立头文件，供 Masstree 内核和命名空间层共同使用

- `leaf<P>` 已经支持“root special allocation + 尾随 meta”
  - 在 `leaf` 中新增了 `root_meta_extra64_`
  - 用于记录尾随 `directory_meta` 额外占用的 cache-line 数
  - 新增了 `leaf::make_root_with_meta(...)`
  - 该路径会按“leaf 本体 + 额外尾随 meta”分配一块更大的 root 内存

- `leaf<P>` 已经支持直接访问尾随 meta
  - 新增：
    - `leaf::has_directory_meta()`
    - `leaf::directory_meta() const/non-const`
  - 当前规则是：
    - 如果 `root_meta_extra64_ > 0`
    - 则 `directory_meta` 位于当前 root 分配块的尾部

- `node_base<P>` 已经新增统一访问接口
  - `node_base::has_directory_meta()`
  - `node_base::directory_meta() const/non-const`
  - `node_base::allocated_size()`
  - 当前已经支持：
    - `leaf root`
    - `internode root`

- `tcursor<P>` 已经新增：
  - `create_layer_with_meta(...)`
  - 用于在目录创建时直接生成“带尾随 meta 的 leaf root”

- `LhmNamespace::create_directory_from_parsed()` 已经改成：
  - 目录 layer 创建时直接构造 `directory_meta`
  - 并通过 `create_layer_with_meta()` 初始化新 root

- `lookup_entry_from_parsed()` 已经改成优先从 `root->directory_meta()` 读取目录元数据
  - 若当前 root 还没有内嵌 meta，再回退到 `directory_entries_`

这意味着当前已经验证了：

- 新创建的目录 `leaf root` 可以真正带上一块尾随 `directory_meta`
- 目录 lookup 已经能够直接从 `leaf root` 旁边读出目录名字和 `inode_ref`
- 现有 demo 仍然全部通过，说明这一步没有破坏当前目录 layer / 文件 value 的混合模型

##### 10. 当前仍未完成的部分

虽然 `leaf root` 侧已经落地，但方案 A 现在仍然没有完全闭环：

- `internode root` 现在已经实现了 special allocation
- root split 时，目录 meta 现在已经会复制到新生成的 `internode root`
- `gc_layer()` 收缩 root 时，目录 meta 仍然还不会真正迁移回 child
- 当前 `gc_layer()` 对“带目录 meta 的 root internode”采用保守策略：先禁止折叠，避免错误丢失元数据

换句话说，现在这一步已经把方案 A 的“leaf root + internode root + split 迁移”三部分打通了，但距离“root 变化时 meta 始终跟着当前 root 走”还差 `gc_layer()` 收缩路径的真正迁移实现。

##### 11. 当前已落地的第二步：`internode root` special allocation + split 迁移

这一阶段已经继续向前推进，当前状态如下：

- `internode<P>` 现在新增了：
  - `root_meta_extra64_`
  - `allocated_size()`
  - `has_directory_meta()`
  - `directory_meta() const/non-const`
- 同时新增了：
  - `internode::make_root_with_meta(...)`

这意味着：

- 普通 internode 仍然走原有固定大小 `make()`
- 只有目录 root 在需要时才走 `make_root_with_meta()`
- `directory_meta` 仍然以尾随内存的形式贴在 root 节点之后

- `node_base<P>` 的统一访问接口现在已经同时支持：
  - `leaf root`
  - `internode root`

- 在 `make_split()` 中，当前已经新增：
  - 如果旧 root 带目录 meta，且 split 会生成新的 root internode
  - 则新 root 通过 `internode::make_root_with_meta()` 创建
  - 并把旧 root 的目录 meta 复制到新 root

##### 12. 当前测试已经额外证明的点

`main.cpp` 当前新增了一组“强制目录 root split”的测试：

- 在 `/usr/local` 下继续创建多个子目录：
  - `tmp0`
  - `tmp1`
  - `tmp2`
  - `tmp3`
  - `tmp4`
  - `tmp5`
- 由于 `namespace_table_params` 当前 `leaf_width = 7`
  - `/usr/local` 的目录 root 会从 `leaf root` 长成 `internode root`
- 在 root split 之后：
  - `lookup_directory(/usr/local/tmp5)` 仍然成功
  - `readdir(/usr/local)` 仍然能正确枚举：
    - `bin`
    - `share`
    - `tmp0` ~ `tmp5`

这说明当前已经验证：

- 目录 root split 之后，目录元数据不会因为 root 从 `leaf` 升级为 `internode` 而丢失
- `lookup_directory` 和 `readdir` 已经可以同时兼容 `leaf root` 和 `internode root`

##### 13. `gc_layer()` 当前的目录语义处理

当前采用的策略已经分成两段：

- 如果当前 layer root 是空的 `leaf root`，并且它带有 `directory_meta`
- 则直接保留，不做回收
- 如果待折叠的 root internode 带有 `directory_meta`，并且它的唯一 child 是 `leaf`
- 则 `gc_layer()` 现在会：
  - 复制出一个新的带 meta 的 `leaf root`
  - 用它替换父槽位里的旧 root 指针
  - 让旧的 `internode root` 和旧 `leaf child` 走 RCU 延迟回收
- 如果待折叠的 root internode 带有 `directory_meta`，并且它的唯一 child 仍是 `internode`
- 则 `gc_layer()` 现在同样会：
  - 复制出一个新的带 meta 的 `internode root`
  - 复制旧 child internode 的分隔键与孩子指针
  - 将新 root 下的直接孩子父指针改写为这个新 root
  - 让旧的 `internode root` 与旧 `internode child` 走 RCU 延迟回收

这样做的意义是：

- 不会错误地把目录元数据留在即将被释放的 old root 上
- 在 LHMFS 语义下，带 `directory_meta` 的空 layer 代表一个合法的空目录，而不是可清理的垃圾层

它的代价是：

- 某些空目录对应的空 `leaf root` 会继续常驻，直到显式目录删除语义到位
- `internode child -> 新 root internode` 这条迁移目前主要通过现有 demo 做了回归验证，还没有补专门针对 `gc_layer()` 收缩路径的定向测试

因此，这仍不是最终实现，但已经把 `gc_layer()` 的关键目录语义路径打通了：

- 目录 root 从 `internode root` 收缩回唯一 `leaf child` 时，目录元数据不会丢失
- 目录 root 从 `internode root` 收缩回唯一 `internode child` 时，目录元数据也不会丢失
- 目录对象仍然保持存在

##### 15. 当前新增的定向回归测试与暴露出的边界

`main.cpp` 里新增了一组专门针对 `gc_layer()` 目录 root 收缩路径的定向测试：

- 创建 `/gc`
- 在其下创建 64 个空子目录，强制 `/gc` 的 root 长到 `internode(height=2)`
- 逐步删除这些子目录，并在删除后主动推进两轮 `rcu_quiesce()`
- 用测试辅助接口读取 `/gc` 当前 root 的结构形态

这组测试目前已经确认了两件事：

- 目录 layer edge 的删除路径已经打通，空目录项可以被逐个移除
- 删除到只剩 `/gc/d63` 时，剩余目录 lookup 仍然正确，目录元数据没有丢失

但它同时暴露了一个仍待继续排查的边界：

- 按当前测试结果，`/gc` 的 root 形态在删除后仍停留在 `internode(height=2, size=2)`
- 也就是说，目录项删除虽然生效了，但我们预期中的 root 收缩并没有在这条测试路径上被真正观察到

因此，当前最准确的结论不是“这条路径已经被完整验证”，而是：

- `gc_layer()` 的目录 meta 迁移代码已经补上
- 但“目录 layer edge 删除 -> 触发 root 收缩 -> 观察到形态变化”这条端到端验证还没有完全成立
- 下一步需要继续定位：到底是 `gc_layer()` 没被触发，还是目录 layer 删除路径没有像普通 value 删除那样进入相同的结构压缩流程

##### 14. 当前关于空目录与冗余 root 的语义决策

在原始 Masstree 语义里，空 layer 往往只是字符串分层后的技术性中间层，因此可以在删除后由 `gc_layer()` 回收。

但在当前 LHMFS 语义里，这个判断需要被改写：

- 没有 `directory_meta` 的空 layer：仍可视为技术性空层，后续可以继续按 Masstree 规则清理
- 带 `directory_meta` 的空 layer：表示一个合法的空目录，不能因为“没有孩子”而自动回收

因此，当前实现对“可能冗余的 root internode”和“空目录 root”采用如下原则：

- 目录对象本身优先保留
- 对唯一 child 为 `leaf` 或 `internode` 的 root 收缩，已经允许结构压缩
- 空目录本身仍然保留，不会因为“没有孩子”而自动消失

这也意味着后续 `gc_layer()` 的最终目标不应是“删除空目录”，而应是：

- 保留目录对象
- 在不丢失 `directory_meta` 的前提下，压缩冗余的 root 层级

### 第三阶段：实现 O(1) 范围重命名

这一阶段聚焦目录级子树搬移，包括：

- 根据源路径哈希向量定位子树入口。
- 根据目标路径哈希向量确认新父目录可达。
- 通过原子指针切换实现目录挂接/摘除。
- 结合 RCU/Epoch 回收机制保证并发读下不出现悬挂指针。

这一阶段是整个方案最能体现架构优势、同时实现风险也最高的部分。

### 第四阶段：底层 DMB 存储引擎桩代码

先用大内存模拟 SSD，构建一个最小可运行的 DMBAllocator：

- 预分配大块内存。
- 按 16KB 切分逻辑块。
- 提供 inode 分配、块头标记和 `block_id + offset` 地址编码能力。

这一步的目标不是先解决真实持久化问题，而是让上层 Masstree 路由与下层物理地址模型真正连起来。

## 4. 关键实现难点

在完成源码阅读后，当前最值得重点关注的问题更新为：

- Masstree 原生 key 模型仍然是字符串分段式 `key`，虽然内部使用 `uint64_t ikey`，但它的分层建立仍围绕 suffix/shift 语义展开，因此是否要直接适配 `vector<uint64_t>`，还是复用现有分层 key 机制做包装，仍需仔细设计。
- 子树入口能力已经存在，但它当前是“Masstree layer 入口”，不是“LHM 目录对象”。我们需要决定目录 inode、目录入口、文件入口之间的最小类型系统。
- 普通 value 和 layer 指针已经能区分，但“普通文件项”和“目录项/子树边界项”还没有现成类型，需要在 `value_type` 或上层元数据结构里补齐。
- 并发读下的安全引用切换与 RCU 延迟回收已有成熟基础，但 O(1) rename 需要的是跨父节点的子树重定向，这一操作是否能直接映射为现有 layer 指针更新，还需要专项设计和验证。
- DMB 地址、inode 生命周期、冲突链节点生命周期如何与现有 Masstree 节点回收机制协同，仍然是后续实现中的关键接口问题。

## 5. 哈希冲突处理策略

项目当前采用每级路径分量哈希为 64-bit 指纹的方案。根据当前假设，在同一目录下即便达到千万级文件数，哈希冲突概率仍然较低，大约为百万分之三量级。因此当前设计不引入复杂冲突处理结构，而采用简单链式法解决冲突。

具体理解如下：

- Masstree 中同一个哈希值对应的目录项允许挂接一个小型冲突链。
- 当命中某个 64-bit 哈希值后，再通过 inode 中保存的真实文件名做二次校验。
- 如果名字不一致，则沿冲突链继续比较，直到找到目标文件或确认不存在。

这意味着哈希冲突不会改变主路径路由逻辑，只会在叶子或目录项命中后引入一个局部的链式消解步骤。

#### 当前阶段的实现决策

虽然当前已经做出了一个最小冲突链原型，并验证了“同目录、同哈希、不同名字”可以被区分和返回，但这一能力暂时不作为主线继续推进，原因如下：

- 当前假设下，同一目录千万级文件规模的哈希冲突概率仍然较低。
- 无论是包装层冲突桶，还是更接近最终形态的“叶子内连续重复哈希项”，都会把文件名比对引入读路径。
- 一旦把“命中哈希后继续逐项读名字比较”纳入主线实现，会显著增加 lookup / readdir 的实现复杂度与读取开销。

因此，当前阶段的明确决策是：

- 哈希冲突问题已经识别，并已有最小原型验证其可处理性。
- 但主线实现暂时不继续围绕冲突处理展开。
- 后续工作重心继续放在命名空间主链路，包括：
  - 路径语义
  - 目录项/文件项组织
  - `lookup` / `creat` / `mkdir` / `readdir`
  - inode 引用与 DMB 对接
  - 以及后续的 rename

当前可以把哈希冲突视为“已知但延后解决的问题”，在未来确实需要时，再根据整体读路径开销和工程复杂度选择最终方案。

## 6. 最近一次内核收缩修正

在 `/gc` 定向测试中，目录 edge 真删除已经接入了 Masstree 原生删除链，但仍观察到一个残余 root 形态：

- `root = internode`
- `size = 1`
- `child_[0]` 是空 `leaf`
- `child_[1]` 是唯一有效子树

这一形态的本质是：

- 大量删除后，左侧分支已经被清空。
- 右侧仍保留唯一有效子树。
- 但由于当前节点本身就是带 `directory_meta` 的目录 root，不能直接沿普通非 root internode 的整理逻辑继续压缩。

当前修正策略已经明确并落地到 `gc_layer()`：

- 当 root internode 满足 `size()==1 && child_[0] 为空叶子 && child_[1] 为唯一有效子树` 时，
- 先把 root 归一化成：
  - `child_[0] = old child_[1]`
  - `nkeys_ = 0`
- 也就是把结构从
  - `child0(empty), key0, child1(valid)`
  规整为
  - `child0(valid), nkeys=0`
- 然后复用已有的 “`size()==0` root 收缩 + `directory_meta` 迁移” 路径。

这样做的目的有两个：

- 避免再为 `size()==1` 的残余 root 单独复制一套 `meta` 迁移与 root 替换逻辑。
- 让目录 root 的压缩语义更接近原始 Masstree 对普通 internode 的整理思路，只是在 root + `directory_meta` 这个特例上补齐最后一步归一化。

#### 当前验证结果

这条归一化分支已经实际写入 `gc_layer()`，并重新编译运行了 `/gc` 定向测试，但当前观测结果仍然停在：

- `kind=internode`
- `has_meta=true`
- `height=2`
- `size=1`
- `child0_is_leaf=true`
- `child0_size=0`
- `child1_exists=true`
- `child1_is_leaf=false`
- `child1_size=1`

这说明：

- “如何规整这个残余 root” 的代码已经补上。
- 但当前删除/回收链路下，这个分支还没有真正被命中。
- 因此问题已经进一步收敛为：
  - 不是“缺少 root 归一化逻辑”
  - 而是“谁来在这个时刻再次触发 `gc_layer()`，或者当前链路为什么没有走到这条分支”

后续排查重点应转向：

- 目录 edge 删除后，哪些场景会再次注册/进入 `gc_layer_rcu_callback`
- 为什么在 `/gc` 收缩到残余 `size==1` root 后，没有再发生一次能命中该归一化分支的 `gc_layer()`

## 7. 去除外部 directory_entries

目录元数据现在已经进一步收回到树内：

- `lookup_directory()` / `lookup_entry_from_parsed()` 只要能定位到目录 root，就直接从
  `root->directory_meta()` 还原目录名字与 `inode_ref`。
- `readdir()` 的子目录部分不再依赖外部 `directory_entries_`，而是：
  1. 先定位父目录 root；
  2. 递归遍历这棵目录 layer 子树中的 `leaf`；
  3. 收集每个 `leaf` 里的 layer edge；
  4. 从对应 `child_root->directory_meta()` 恢复真实目录项。
- 目录创建和删除路径也不再维护 `directory_entries_`。

这意味着当前目录语义已经基本形成：

- 目录存在性：由 layer edge + child root 表达
- 目录元数据：由 child root 尾随的 `directory_meta` 表达
- 目录枚举：由父目录 layer 子树中的 edge 集合恢复

因此，外部 `directory_entries_` 已经可以移除。

## 8. 目录 rename 内核化的当前推进

目录 O(1) rename 的实现已经开始从“设计”进入“最小可运行原型”阶段。

当前实现策略是：

- 普通文件 `rename` 仍沿用原有的“扫描/重建/删除”路径。
- 目录 `rename` 开始切到新的 edge 重挂路径。

第一版目录 rename 的执行顺序是：

1. 解析 `old_path` 和 `new_path`，检查：
   - 不是根目录；
   - 不是把目录 rename 到自己子树内部；
   - 目标路径不存在；
   - 新父目录存在。
2. 在 `old_parent_root` 上通过 `find_locked_edge()` 找到旧目录 edge。
3. 拿到其指向的 `subtree_root`，并通过 `subtree_root->directory_meta()` 校验真实目录名。
4. 在 `new_parent_root` 上插入一条新的 layer edge，但不新建目录子树，而是直接挂接已有的 `subtree_root`。
5. 更新 `subtree_root->directory_meta()`：
   - `name = new_name`
   - `component_hash = h(new_name)`
6. 从 `old_parent_root` 上删掉旧的 layer edge。

为此，当前代码新增了一个最小内核原语：

- `tcursor::attach_existing_layer(node_type* layer_root, ...)`

它的语义是：

- 在当前父目录层插入一个新的 layer 槽位；
- 该槽位直接引用一个已有的目录子树 root；
- 不分配新的目录 root，也不复制整棵子树。

这一版还不是最终的“原子双边锁 + 并发读可证明正确”的实现，当前边界是：

- 先追求目录 rename 的 O(1) 子树重挂语义；
- 双父节点锁顺序、严格原子窗口控制、旧 edge 的并发可见性收敛，留到下一步强化。

但这一步的意义已经很明确：

- 目录 rename 不再需要扫描整个子树；
- 子树内部所有后代不再被重写；
- LHM 的目录对象终于开始真正按“edge + subtree root + root meta”整体搬移。

## 9. 当前阶段节点总结

到目前为止，这一版已经可以单独视为一个阶段节点，核心进展如下。

### 9.1 路径与命名空间主链路

- 已完成绝对路径解析：`/a/b/c -> ParsedPath -> vector<uint64_t> -> PathKey`
- 已完成 `lookup_file` / `lookup_directory` / `mkdir` / `creat_file`
- 文件仍以普通 value 表示
- 目录已经以内核 layer 子树表示，不再只是包装层的普通 value

### 9.2 目录对象内核化

- 目录 edge 由父目录中的 layer 槽位表示
- 目录元数据由 `child_root` 尾随的 `directory_meta` 表示
- `leaf root` 和 `internode root` 都支持 special allocation 挂接 `directory_meta`
- root split 时，`directory_meta` 会迁移到新的 root
- 空目录语义已经和 `directory_meta` 绑定，不再把“空 layer”简单当成垃圾层

### 9.3 外部目录表已移除

- `directory_entries_` 已经删除
- `lookup_directory()` 直接从 `child_root->directory_meta()` 恢复目录项
- `readdir()` 的目录孩子部分已经改为纯树内恢复：
  - 遍历父目录 layer 子树中的 leaf
  - 收集其中的 layer edge
  - 再从 `child_root->directory_meta()` 恢复真实目录名与 `inode_ref`

这意味着目录语义现在已经基本闭环在 Masstree 内部：

- 目录存在性：layer edge
- 目录对象元数据：root meta
- 目录枚举：父目录子树中的 edge 集合

### 9.4 只读无锁目录定位

当前又新增了一条更适合作为性能基线前置能力的只读路径：

- `unlocked_tcursor::find_unlocked_edge()`

其语义与 `find_locked_edge()` 一致：

- 命中 layer 时停在父 edge 上
- 不自动下钻
- 不加锁，只依赖版本检查与重试

目前已经切到无锁路径的只读逻辑包括：

- `locate_directory_root()`
- `lookup_child_from_parent_root()`
- `get_child_value_from_parent_root()`

而涉及修改的路径，例如：

- 目录删除
- 目录 rename 的 edge 摘挂

仍然保留 `find_locked_edge()`。

### 9.5 rename 当前状态

- 普通文件 rename：仍然是扫描/重建/删除路径
- 目录 rename：已经有“单线程、常数级子树重挂”的最小原型

也就是说，目录 rename 当前已经能够：

- 在旧父目录摘掉旧 edge
- 在新父目录挂接同一个 `subtree_root`
- 更新 `subtree_root->directory_meta()`
- 不扫描整个子树、不重写所有后代

但它还不是最终的并发安全原子版，后续仍需要补：

- 双父节点锁顺序
- 严格原子窗口控制
- 并发读下的可见性与旧指针回收协议

### 9.6 当前已知但暂挂的问题

- `gc_layer()` 在某个残余 root 形态上的完全收缩还没有打透：
  - `kind=internode`
  - `size=1`
  - `child_[0]` 为空 leaf
  - `child_[1]` 为唯一有效子树
- 该问题当前不阻塞主线功能，因此本阶段先不继续深挖

## 10. 当前阶段的基线测试目标

既然这一版已经可以视为一个阶段节点，下一步先做一组基线测试是合理的。当前最值得先测的是：

- `stat`
  - 对应当前实现里的 `lookup_file` / `lookup_directory`
- `creat`
  - 对应 `creat_file`
- `delete`
  - 对应当前命名空间删除路径
- `ls`
  - 对应 `readdir`

建议这一轮基线测试先关注两类指标：

### 10.1 延迟

- 单次操作平均延迟
- P50 / P95 / P99 延迟

### 10.2 吞吐量

- 单线程吞吐
- 少量多线程吞吐

建议这一轮先把它定位为“阶段性基线测试”，不是最终论文级 benchmark。其主要目的在于：

- 固化当前版本的性能基线
- 为后续“多线程原子 rename”强化提供对照组
- 观察无锁目录读路径是否带来明显收益

## 10.3 当前基准程序落地

当前已经补上一个独立的 `benchmark.cpp`，不再复用 `main.cpp` 的 demo 逻辑。它的设计目标是先给当前阶段打一组可复现的基线数据，主要特点如下：

- 输入数据默认来自 `/mnt/batchtest/filepath/path_kv_ideep.txt`
- 数据按 TSV 解析：
  - 第一列是路径
  - 第二列是 inode JSON
- 当前基准程序用 JSON 中的以下字段做最小转换：
  - `inode` -> `inode_ref.block_id`
  - `perm[0] == 'd'` 判定为目录
  - 其余项判定为普通文件
- 根目录 `"/"` 视为隐式存在，不重复插入

### 10.4 当前 benchmark 的操作定义

- `stat`
  - 对已有路径执行 `lookup_entry`
- `ls`
  - 对已有目录执行 `readdir`
- `create`
  - 当前先测普通文件创建
  - 选择“父目录已存在、但尚未插入”的文件路径执行 `creat_file`
- `delete`
  - 删除刚刚成功创建的同一批文件
  - 当前先不把目录删除纳入基线 benchmark

这样设计的原因是：

- 文件 `create/delete` 更稳定，不受“目录必须为空”约束影响
- 可以保证 `delete` 的目标一定是当前 benchmark 自己刚创建出来的合法路径

### 10.5 当前 benchmark 输出

当前基准程序会输出一份 summary CSV，字段包括：

- `operation`
- `threads`
- `requested_ops`
- `attempted_ops`
- `success`
- `failure`
- `elapsed_seconds`
- `throughput_ops_per_sec`
- `avg_latency_us`

同时还会把以下数据准备阶段信息一并写进 CSV：

- `load_limit`
- `loaded_records`
- `skipped_bad_lines`
- `skipped_long_component`
- `skipped_missing_parent`
- `skipped_duplicate`
- `input_path`

### 10.6 画图脚本准备

当前已经同时补上一个最小 `plot_benchmark.py`，用于直接读取 benchmark 的 summary CSV，并生成两张图：

- 吞吐量柱状图
- 平均延迟柱状图

当前先画：

- `throughput_ops_per_sec`
- `avg_latency_us`

后续如果需要扩展：

- P50 / P95 / P99
- 多组线程数 sweep
- 多组 load_limit sweep

可以继续沿用这套 CSV -> Python 绘图链路扩展。

### 10.7 当前 benchmark 的实现取舍

当前 `create/delete` 基准没有继续沿着 84GB 输入文件向后顺序寻找候选，而是：

- 先按 `load_limit` 预加载一批真实目录树
- 再基于“已加载目录集合”生成一批新的合成文件路径
- 对这些合成文件执行 `create`
- 再对同一批文件执行 `delete`

这样做的原因是：

- 避免为了找 create 候选把超大输入文件继续扫到很后面
- 保证 `create` 的父目录一定存在
- 保证 `delete` 的目标就是 benchmark 自己刚创建的合法路径

这意味着当前 benchmark 对 `stat` / `ls` 使用真实数据集路径，
对 `create/delete` 使用“真实目录 + 合成文件名”的混合基线。

### 10.8 已完成的 smoke 验证

当前已经验证过一组最小 smoke 参数可以完整跑通：

- `--load-limit 100`
- `--stat-ops 20`
- `--ls-ops 10`
- `--create-ops 10`
- `--threads 1`

并成功产出：

- summary CSV
- 吞吐量柱状图
- 平均延迟柱状图

这说明当前 benchmark 链路已经打通：

- 数据集解析
- 命名空间预加载
- `stat/create/delete/ls` 四类操作测量
- CSV 输出
- Python 画图

## 12. 原始 Masstree 与 LHM 的独立对比测试线

为了避免把两套不同代码树的 Masstree 头文件直接塞进同一个翻译单元，当前已经把“原始 Masstree 基线”和“LHM 语义层”拆成两个独立可执行：

- `mass_backup_test.cpp`
  - 基于 `masstree-beta_backup`
  - 把路径编码成哈希分量串，直接按 KV 方式测试
- `lhm_test.cpp`
  - 基于当前 `MasstreeLHM`
  - 通过 `LhmNamespace` 测试文件系统语义接口

这两个程序都通过同一份共享工具头：

- `path_benchmark_common.hh`

统一了：

- 输入 TSV 解析
- 路径采样
- 延迟统计
- 报告输出格式

### 12.1 当前比较口径

两条测试线都支持以下操作：

- `stat`
- `ls`
- `create`
- `delete`
- `rename`

但要注意两条线的语义并不完全一致：

- `mass_backup_test`
  - `stat/create/delete` 是纯 KV 语义
  - `ls` 用的是“按目录前缀做 range scan”的近似基线
  - `rename` 用的是“按路径前缀搬移所有受影响 key”的 KV 级近似基线
- `lhm_test`
  - 走 `LhmNamespace`
  - `stat` 对应 `lookup_entry`
  - `ls` 对应 `readdir`
  - `create/delete` 对应文件系统路径接口
  - `rename` 对应当前目录级 O(1) 子树重挂原型

因此这组测试更适合回答：

- “LHM 在 Masstree 基础上引入文件系统语义后，相对裸 KV 基线增加了多少开销”

而不是直接回答：

- “LHM 与 RocksDB-backed 各类模式谁绝对更快”

### 12.2 当前 smoke 结果

已经用同一组小规模参数完成 smoke：

- `--max-entries 100`
- `--num-stat-tests 20`
- `--num-ls-tests 10`
- `--num-create-tests 10`
- `--num-rename-tests 10`

当前结果显示：

- `mass_backup_test`
  - `stat` 平均约 `12.4 us`
  - `ls` 平均约 `61.2 us`
  - `create` 平均约 `6.3 us`
  - `delete` 平均约 `5.9 us`
  - `rename` 平均约 `598 us`
- `lhm_test`
  - `stat` 平均约 `28.3 us`
  - `ls` 平均约 `393 us`
  - `create` 平均约 `15.8 us`
  - `delete` 平均约 `16.4 us`
  - `rename` 平均约 `66.5 us`

这组 smoke 至少说明两点：

- LHM 的 `rename` 在当前目录级 O(1) 原型下，已经明显优于 KV 基线式的“前缀搬移”
- 但 `stat/ls/create/delete` 相对裸 Masstree KV 基线仍然存在明显语义成本，这正是后续优化需要重点量化和解释的部分

## 11. 一句话总结

LHM 的本质是：把 Masstree 改造成一个“路径分量哈希驱动的层级命名空间索引”，再配合一个“目录局部性感知的块式 inode 存储引擎”，从而同时获得高并发点查、友好的目录扫描能力，以及目录级 O(1) rename 的潜力。

## 13. 2026-04-09：`readdir/ls` 路径改为“当前层扫描”

针对 `ls` 延迟异常高的问题，当前已经把 `LhmNamespace::readdir()` 从“全表扫描 + 前缀过滤”改为“仅扫描当前目录 layer root 的孩子项”。

### 13.1 旧路径（已替换）

旧实现里，`readdir()` 会：

- 先定位目标目录 root；
- 然后执行 `table_.scan(lcdf::Str(""), false, ...)` 全表扫描；
- 对每条记录 `PathKey::decode()`，再按前缀与深度过滤“直接孩子”；
- 再额外遍历目录 layer 子树补目录孩子；
- 最后做一次排序。

这条路径在大规模数据下会把 `ls` 复杂度放大到接近全局规模，导致延迟远高于预期。

### 13.2 新路径（当前实现）

当前 `readdir()` 已改为：

- 只定位目标目录对应的 `directory_root`；
- 仅在这棵目录层内按键序（从左到右）遍历叶子槽位；
- 叶子槽位命中普通 value：直接作为文件项返回；
- 命中 `conflict_chain`：在冲突桶内展开同哈希文件项；
- 命中 layer edge：只恢复当前子目录项（读取 `child_root->directory_meta()`），不下钻子目录；
- 不再执行全表 `scan("")`，也不再做额外排序。

这条路径满足：

- `ls` 只列“当前目录直接孩子（文件 + 子目录）”；
- 遇到目录项不递归下钻；
- 按当前层键序从左到右扫描。

### 13.3 本地基准对比（iwide, 同参数）

基准参数：

- `--input /mnt/batchtest/filepath/path_kv_iwide.txt`
- `--max-entries 100000`
- `--num-stat-tests 10000`
- `--num-ls-tests 1000`
- `--num-create-tests 5000`
- `--num-rename-tests 1000`

`ls` 延迟变化：

- 旧版 LHM（全表扫描）：`avg_latency_us = 94390.5`
- 新版 LHM（当前层扫描）：`avg_latency_us = 39.0543`

即 `ls` 平均延迟下降约 `2416x`。

补充验证（stat-only）：

- 新版 LHM `stat avg = 11.6656 us`
- 原生 Mass 基线 `stat avg = 8.38747 us`

说明这次改动主要命中 `ls` 热点，没有引入明显的 `stat` 路径退化。

## 14. 2026-04-09：`stat/create/delete` 再次收紧 probe 路线

在 `ls` 改成“当前层扫描”之后，下一轮优化聚焦在 `stat/create/delete` 的热路径上。目标不是省略文件系统语义，而是把“重复 probe”收紧成“单次父目录定位 + 单次本层槽位判定”。

### 14.1 这次保留的语义

以下检查仍然保留，没有为了压延迟而拿掉：

- `stat` 仍然区分：不存在 / 文件 / 目录；
- `create` 仍要求父目录存在且目标路径不存在；
- `delete` 仍要求目标存在；
- `delete` 遇到目录时仍要求目录为空；
- 同哈希不同名的文件冲突仍然通过 `conflict_chain` 处理。

### 14.2 这次收紧掉的重复开销

#### `stat`

- `entry_name_equals()` 改为 `length + memcmp`，不再构造临时 `std::string`；
- 单组件 key 改成栈上 8-byte 编码，不再为 `PathKey({one_hash})` 分配临时 `vector/string`；
- 父目录 root 的最后一级孩子查找统一走 `lookup_child_slot_from_parent_root()`。

#### `create`

旧路径里，文件创建会先：

- `lookup_child_from_parent_root()` 判断是否存在；
- 再 `get_child_value_from_parent_root()` 读一次槽位；
- 最后 `find_insert()` 才真正插入。

现在改成：

- 先定位父目录 root；
- 再用一次 `lookup_child_slot_from_parent_root()` 判定目标槽位是：不存在 / value / layer；
- `value` 路径直接复用已有槽位进入 `conflict_chain` 逻辑；
- `不存在` 时才执行 `find_insert()`。

也就是说，`create` 从“本层两次 probe + 一次插入”收紧成了“本层一次 probe + 一次必要插入”。

#### `delete`

旧路径里，删除会先 `lookup_entry_from_parsed()` 做一次存在性/类型判断，然后再走一遍真正删除路径。

现在改成：

- 先定位父目录 root；
- 对最后一级组件做一次 `find_locked_edge()`；
- 命中目录 edge：直接检查 `directory_meta` 名字和目录是否为空，然后删除 layer edge；
- 命中普通 value：直接判定名字和类型后删除；
- 命中 `conflict_chain`：只删除桶内匹配项，必要时把链头收缩回单 entry。

这样 `delete` 不再需要“先查一次、再删一次”的双路径。

### 14.3 预期效果

这次优化主要影响：

- `stat`：降低名字比对和单组件 key 构造的常数项；
- `create`：减少本层重复 probe；
- `delete`：减少一次完整的预检查查找，同时补齐冲突链精确删除。

因此预期结果是：

- `stat/create/delete` 会继续向原始 Masstree 基线靠近；
- 但仍然不会退化成“裸 KV 单次 get/put/remove”那样薄，因为文件系统语义检查本身仍然保留。

### 14.4 本地基准结果（iwide, 100k load）

为了把 `stat` 和 `create/delete` 的影响拆开，当前分别跑了两组局部基准。

#### `stat-only`

参数：

- `--num-stat-tests 10000`
- `--num-ls-tests 0`
- `--num-create-tests 0`
- `--num-rename-tests 0`

结果：

- 原始 Mass 基线：`stat avg = 8.64352 us`
- 优化后 LHM：`stat avg = 9.51319 us`

对比此前同规模 LHM `stat avg ≈ 11.6656 us`，说明这轮热路径收紧后，`stat` 已经进一步逼近原始 Masstree。

#### `create/delete-only`

参数：

- `--num-stat-tests 0`
- `--num-ls-tests 0`
- `--num-create-tests 5000`
- `--num-rename-tests 0`

结果：

- 原始 Mass 基线：
  - `create avg = 8.01012 us`
  - `delete avg = 7.82115 us`
- 优化后 LHM：
  - `create avg = 9.33076 us`
  - `delete avg = 9.0642 us`

对比优化前的同规模 LHM（`create ≈ 11.52 us`, `delete ≈ 12.46 us`），这次改动已经把 `create/delete` 明显往下压了一截。

补充：如果把 `stat + create/delete` 混在同一轮里跑，局部扰动会更大，因此当前文档更采用拆分 workload 的结果来描述这轮优化收益。


## 15. 2026-04-09：目录感知持久化草案（最小版本）

在完成内存内命名空间原型与 `stat/create/delete/ls/rename` 热路径收紧之后，下一阶段的目标是把 LHM 推到 SSD 上，形成真正可与 RocksDB-based InfiniFS / SingularFS 映射方式对比的持久化元数据层。

这一阶段不追求一次做成完整文件系统，而是先定义：

- 最小持久化对象格式；
- 每目录 16KB 内存缓冲；
- 稳定目录块（stable directory blocks）与目录缓冲（directory buffer）的分层；
- 为未来 crash recovery 预留字段，但暂不实现恢复流程。

### 15.1 设计目标

- 保留当前 LHM 的“父目录 root + 最后一级组件”命名空间路由方式；
- 把 inode / 目录项真正落到 SSD 上；
- 避免 `create/delete` 每次都读目录块、改目录块、再整块写回；
- 利用目录局部性，用目录级缓冲吸收高频小更新；
- 保留 checkpoint / version / checksum 等恢复字段，但暂不做恢复实现。

### 15.2 总体分层

系统分成四层：

1. `LhmNamespace / Masstree`
   - 负责路径路由；
   - 定位父目录 root 和最后一级孩子；
   - 不直接承担 SSD 布局管理。

2. `PersistentMetadataStore`
   - 管理 SSD 上的稳定元数据对象；
   - 负责 inode、stable directory blocks、superblock、checkpoint、allocator state 的读写。

3. `DirectoryBufferCache`
   - 只为活跃目录维护内存缓冲；
   - 默认每目录 `16KB`；
   - 优先吸收 `create/delete/rename` 产生的目录项增删改。

4. `FlushManager`
   - 当目录缓冲满、淘汰或显式同步时，把目录缓冲批量刷成新的 stable directory blocks；
   - 更新目录 inode 的 stable root 指针与 version 字段。

### 15.3 最小持久化对象

当前先定义以下对象：

- `superblock`
  - 固定设备入口；
  - 记录块大小、最新 checkpoint 位置、flags。

- `checkpoint`
  - 记录最近一次提交点；
  - 保存 root directory inode ref、allocator state ref、sequence number；
  - 当前只保留字段，不实现 replay/recovery。

- `inode`
  - 文件或目录的持久身份；
  - 建议固定大小版本，便于直接按 slot 定位；
  - 目录 inode 指向 stable directory blocks 链头。

- `stable directory block`
  - SSD 上某个目录最近一次 flush 后的稳定目录项镜像；
  - 每块保存若干当前目录的直接孩子；
  - 多块可以组成链表或 extent。

- `allocator state`
  - 记录块分配状态；
  - 第一版可以非常简单，只支持 append-only 分配与少量 free list 字段预留。

### 15.4 目录缓冲（Directory Buffer）

目录缓冲是本方案的核心。每个活跃目录在内存中可维护一个 `16KB` 缓冲区，用于保存该目录最近的增删改，而不是每次修改都同步读改写 SSD 上的稳定目录块。

目录缓冲建议至少维护：

- `dir_inode_ref`
- `dir_inode_id`
- `base_stable_version`
- `last_access_epoch`
- `last_flush_epoch`
- `used_bytes`
- `recent_read_hits`
- `recent_write_hits`
- `dirty`
- `flushing`
- `deltas`

其中 `deltas` 中每项至少记录：

- 操作类型：`insert / erase`
- `component_hash`
- `name`
- `entry_kind`
- `inode_ref`

### 15.5 活跃目录判断

不是所有目录都长期保留 16KB 缓冲。只有活跃目录才分配缓冲，避免宽目录树下的内存浪费。

建议第一版采用轻量策略：

- 发生 `create/delete/rename` 的目录，立即升温并分配缓冲；
- `ls/stat` 连续命中次数超过阈值（如 4 次）的目录，也可升温；
- `dirty` 目录始终视为活跃；
- 淘汰时优先回收 `dirty == false` 且最近最久未访问的目录（LRU）。

### 15.6 Stable + Buffer 合并视图

`ls/stat/create/delete` 看到的目录状态不是单独来自 SSD stable blocks，也不是单独来自内存 buffer，而是二者合并后的视图：

1. 先读取该目录当前 stable directory blocks，得到稳定目录项集合；
2. 再按目录缓冲中的 `deltas` 覆盖：
   - `insert`：插入或更新对应名字；
   - `erase`：删除对应名字；
3. 输出当前目录的最新直接孩子视图。

这样可以保证：

- `ls` 能列出最新状态；
- `create` 的存在性判断可以看到尚未 flush 的最新目录项；
- `delete/rename` 也能正确处理尚未落盘的目录更新。

### 15.7 各语义的写路径

#### `stat(path)`

- LHM 定位父目录 root 和最后一级组件；
- 读取父目录的 stable + buffer merge 视图，确认目标可见；
- 通过 `inode_ref` 读取 inode；
- 不写盘。

#### `ls(path)`

- 读取目录 inode；
- 加载 stable directory blocks；
- 如果目录有 active buffer，则叠加 deltas；
- 返回当前目录直接孩子；
- 不写盘。

#### `create(path)`

- 定位父目录；
- 在 merged view 上确认目标不存在；
- 分配新 inode，并把 inode 持久化；
- 向父目录 buffer 追加一条 `insert` delta；
- 更新 LHM 内存索引；
- 若该目录 buffer 超过 16KB，则触发 flush。

#### `mkdir(path)`

- 定位父目录；
- 分配目录 inode；
- 初始化空目录状态；
- 向父目录 buffer 追加目录 `insert` delta；
- 更新 LHM 内存索引；
- 必要时 flush 父目录。

#### `delete(path)` / `rmdir(path)`

- 定位父目录；
- 在 merged view 上确认目标存在；
- 文件删除：向父目录 buffer 追加 `erase` delta；
- 目录删除：先确认目标目录 merged view 为空，再追加 `erase` delta；
- 更新 LHM 内存索引；
- 目标 inode 可先标记 tombstone，后续再考虑回收；
- 必要时 flush。

#### `rename(old, new)`

- 文件 rename：
  - old parent buffer 追加 `erase(old)`；
  - new parent buffer 追加 `insert(new, same inode_ref)`；
  - inode 本体不移动。

- 目录 rename：
  - old parent buffer 追加 `erase(old)`；
  - new parent buffer 追加 `insert(new, same dir inode_ref)`；
  - 目录自身 inode / meta 中的 parent / name 字段更新；
  - 子树不重写。

### 15.8 Flush 路径

当目录缓冲被刷盘时：

1. 读取该目录当前 stable directory blocks；
2. 构造稳定目录项集合；
3. 应用当前 buffer deltas，得到新的最终目录镜像；
4. 将镜像重新编码为一个或多个新的 stable directory blocks；
5. 顺序写入 SSD；
6. 更新目录 inode 中的 stable root 指针与 `stable_version`；
7. 清空 buffer，标记 `dirty = false`。

这是一种目录级 copy-on-write：

- 高频小更新先在内存中吸收；
- 刷盘时批量摊平写放大；
- 稳定目录状态始终以完整目录镜像形式保存在 SSD 上。

### 15.9 崩溃恢复的当前边界

当前阶段仅要求：

- 在 `superblock` / `checkpoint` / `inode` / `dir_block` 中预留恢复所需字段；
- 写路径尽量按未来可恢复的方式组织（如保留 `seq/version/checksum`）；
- 暂不实现 checkpoint replay、namespace rebuild、orphan cleanup 等恢复流程。

也就是说，这一阶段先完成“持久化对象布局 + 目录缓冲 + flush 机制”，恢复逻辑在后续阶段再补齐。


## 16. 2026-04-09：持久化布局定稿 v1（inode 不存名字）

在进一步讨论后，当前持久化设计做出以下收敛决定：

- `metadata block size = 16KB`
- `directory buffer size = 16KB`
- `inode size = 256B`
- `inode 不存文件名`
- `名字只属于 directory entry`

这意味着：

- inode 表示对象本体（identity + metadata）；
- directory entry 表示“父目录如何命名并引用该对象”；
- `rename` 只改目录映射，不改 inode 本体；
- `ls` 直接从目录块读取名字，不需要额外回 inode 取名。

### 16.1 设计原则

本版明确采用：

- `name in directory entry`
- `object metadata in inode`

这样做的直接收益是：

- 语义更清晰：名字属于目录项，而不是对象本体；
- `rename` 更干净：只更新 old/new 父目录项；
- inode 更稳定：名字变化不再导致 inode 内容变化；
- 空间更高效：`256B inode` 的空间可以留给元数据、block refs、version、checksum 与预留字段。

### 16.2 16KB 物理元数据块

当前版本直接采用 `16KB` 作为 metadata block 大小。原因是：

- 与每目录 `16KB` buffer 大小一致，flush 逻辑最直接；
- 目录更新可以更好地摊平读改写放大；
- 更适合目录感知元数据写入；
- 当前阶段是研究原型，优先追求实现简单和目录局部性。

同时接受以下 tradeoff：

- 对只读单个 inode 的场景，`16KB` 物理块会比更小块粒度有额外读放大；
- 小目录若单独占用一个稳定目录块，会有一定空间浪费；
- 后续如需更细粒度控制，再回头评估 `4KB` 或混合布局。

### 16.3 通用块头

每个 `16KB` metadata block 先预留一段统一块头，建议按 `64B` 组织，包含：

- `magic`：4B
- `block_type`：2B
- `layout_version`：2B
- `block_id`：4B
- `payload_bytes`：4B
- `checksum`：4B
- `sequence / version`：8B
- `owner_inode_id`：8B
- `next_block_id`：4B
- `flags`：4B
- `reserved`：补齐到 64B

### 16.4 inode 结构（256B）

当前版 inode 不再存名字，建议固定为 `256B`，字段重点放在对象身份、尺寸、时间戳、block refs 与未来恢复字段上。

建议字段组成：

- `inode_id`：8B
- `parent_inode_id`：8B
- `type`：1B
- `flags`：1B
- `mode`：2B
- `link_count`：4B
- `size`：8B
- `ctime_ns`：8B
- `mtime_ns`：8B
- `primary_ref`：8B
- `aux_ref`：8B
- `stable_version`：8B
- `generation`：8B
- `checksum`：4B
- `tombstone_epoch`：8B
- `reserved`：补齐到 `256B`

这里：

- 文件 inode：`primary_ref/aux_ref` 以后可指向文件数据元信息；
- 目录 inode：`primary_ref` 指向 stable directory blocks 链头；
- `parent_inode_id` 当前主要为目录 rename 和未来恢复留钩子；
- `reserved` 留给后续权限、uid/gid、更多 block refs、recovery 字段扩展。

### 16.5 inode block 布局

如果每个 inode 固定 `256B`，则一个 `16KB` inode block 很适合做定长 slot 阵列。

建议布局：

- 通用块头：`64B`
- inode-block 局部头 / bitmap / reserved：`192B`
- inode slots：`63 * 256B = 16128B`

总计：

- `64B + 192B + 16128B = 16384B`

也就是说：

- 一个 `16KB inode block` 可容纳 `63` 个 inode。

### 16.6 directory entry 结构

由于 inode 不再存名字，目录块必须承担全部名字存储。当前建议 directory entry 至少包含：

- `component_hash`：8B
- `inode_ref.block_id`：4B
- `inode_ref.offset`：4B
- `entry_kind`：1B
- `name_length`：1B
- `flags/reserved`：2B
- `name[]`：变长

固定头大小约 `20B`，建议按 `24B` 对齐。于是单条目录项大小大约为：

- `24B + name_length`

举例：

- 若平均文件名 `32B`，则单目录项约 `56B`；
- 若平均文件名 `64B`，则单目录项约 `88B`；
- 若文件名接近 `128B`，则单目录项约 `152B`。

### 16.7 stable directory block 布局

建议 stable directory block 也使用 `16KB`：

- 通用块头：`64B`
- directory-block 局部头：`64B`
- entry payload：`16256B`

directory-block 局部头可包含：

- `dir_inode_id`：8B
- `entry_count`：4B
- `used_bytes`：4B
- `base_version`：8B
- `prev_or_next_block_id`：4B
- `delta_count_or_flags`：4B
- `reserved`：补齐到 `64B`

这样每个稳定目录块可以直接装当前目录的一批直接孩子项。

### 16.8 容量估算

以 `16256B` entry payload 粗估：

- 若单目录项平均 `56B`，每块约可容纳 `290` 项；
- 若单目录项平均 `88B`，每块约可容纳 `184` 项；
- 若单目录项平均 `152B`，每块约可容纳 `107` 项。

这意味着：

- 中小目录通常可落在 1 个稳定目录块内；
- 宽目录则自然扩展为多块链或 extent。

### 16.9 语义影响

这个定稿版对文件系统语义有几项明确影响：

- `stat`：通过目录项找到 inode 后，只读 inode 元数据，不再依赖 inode 中的名字；
- `ls`：直接从 stable directory blocks + directory buffer 合并视图中输出名字；
- `create/delete`：只修改目录映射与 inode 引用关系；
- `rename`：只更新 old/new 父目录中的目录项；inode 本体保持稳定；
- 未来若支持 hard link，这个布局也更自然。

### 16.10 实现起点

基于当前定稿版，后续实现阶段建议优先新增：

- `metadata_layout.hh`
  - 定义 block type、通用块头、inode、directory entry、directory block 头；

- `persistent_store.hh/.cc`
  - 提供按 `inode_ref` 读写 inode、读写稳定目录块、分配 metadata block 的最小接口；

- `directory_buffer.hh/.cc`
  - 实现活跃目录的 `16KB` buffer、delta 追加、merge view 与 flush 触发。

实现顺序建议：

1. 先把对象格式与 block 分配器搭起来；
2. 再打通 `create + ls`；
3. 然后接 `stat`；
4. 再接 `delete`；
5. 最后接 `rename`。

