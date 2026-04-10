#ifndef MASSTREELHM_DIRECTORY_META_HH
#define MASSTREELHM_DIRECTORY_META_HH

#include <stdint.h>
#include <string.h>
#include <string>
#include <string_view>

namespace MasstreeLHM {

// 当前目录项和目录 root 元数据都复用同一份最小名字长度约束。
static constexpr size_t kMaxEntryNameBytes = 63;

// inode_ref 模拟未来 DMB 中的物理地址引用。
// 当前阶段仍然使用 block_id + offset 作为最小占位表示。
struct inode_ref {
    uint32_t block_id;
    uint32_t offset;
};

inline inode_ref make_inode_ref(uint32_t block_id, uint32_t offset) {
    inode_ref ref;
    ref.block_id = block_id;
    ref.offset = offset;
    return ref;
}

// directory_meta 是方案 A 中“目录 root 尾随元数据”的最小结构。
// 它描述的是目录对象自身身份，而不是目录中的某个普通叶子项。
struct directory_meta {
    inode_ref ref;
    uint64_t component_hash;
    uint8_t name_length;
    uint8_t flags;
    char name[kMaxEntryNameBytes + 1];
};

inline directory_meta make_directory_meta(inode_ref ref, uint64_t component_hash,
                                          const std::string& name) {
    directory_meta meta;
    meta.ref = ref;
    meta.component_hash = component_hash;
    meta.name_length = static_cast<uint8_t>(name.size());
    meta.flags = 0;
    memset(meta.name, 0, sizeof(meta.name));
    memcpy(meta.name, name.data(), name.size());
    return meta;
}

inline std::string directory_meta_name(const directory_meta& meta) {
    return std::string(meta.name, meta.name + meta.name_length);
}

inline bool directory_meta_name_equals(const directory_meta& meta, std::string_view name) {
    const size_t n = static_cast<size_t>(meta.name_length);
    return n == name.size() && (n == 0 || memcmp(meta.name, name.data(), n) == 0);
}

}  // namespace MasstreeLHM

#endif
