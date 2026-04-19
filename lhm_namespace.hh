#ifndef MASSTREELHM_LHM_NAMESPACE_HH
#define MASSTREELHM_LHM_NAMESPACE_HH

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kvthread.hh"
#include "directory_meta.hh"
#include "masstree.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "path_key.hh"

namespace MasstreeLHM {

// entry_kind 描述同一张命名空间表中的最小记录类型。
// 当前阶段先只区分目录和普通文件，为后续 rename、冲突链和 inode
// 外存指针预留扩展空间。
enum class entry_kind : uint8_t {
    invalid = 0,
    directory = 1,
    file = 2,
    conflict_chain = 3
};

struct namespace_entry {
    entry_kind kind;
    inode_ref ref;
    uint8_t name_length;
    char name[kMaxEntryNameBytes + 1];
};

// readdir_record 描述一次最小目录扫描结果。
// 当前阶段已经可以同时返回：
// 1. 完整哈希路径
// 2. 当前目录下的直接孩子哈希
// 3. 真实目录项名字
// 4. 对应 entry
struct readdir_record {
    std::vector<uint64_t> full_hashes;
    uint64_t child_component_hash;
    std::string child_name;
    namespace_entry entry;
};

struct subtree_record {
    std::vector<uint64_t> full_hashes;
    namespace_entry entry;
};

struct directory_root_debug_info {
    bool found;
    bool is_leaf;
    bool has_meta;
    uint32_t height;
    int size;
    bool child0_is_leaf;
    bool child1_exists;
    bool child1_is_leaf;
    int child0_size;
    int child1_size;
};

struct lookup_probe_stats {
    uint32_t directory_levels_walked = 0;
    uint32_t child_slot_lookups = 0;
};

inline std::string entry_name(const namespace_entry& entry) {
    return std::string(entry.name, entry.name + entry.name_length);
}

inline std::string_view entry_name_view(const namespace_entry& entry) {
    return std::string_view(entry.name, entry.name_length);
}

inline bool entry_name_fits(const std::string& name) {
    return name.size() <= kMaxEntryNameBytes;
}

inline namespace_entry make_namespace_entry(entry_kind kind, inode_ref ref,
                                            const std::string& name) {
    namespace_entry entry;
    entry.kind = kind;
    entry.ref = ref;
    entry.name_length = static_cast<uint8_t>(name.size());
    memset(entry.name, 0, sizeof(entry.name));
    memcpy(entry.name, name.data(), name.size());
    return entry;
}

inline bool entry_is_valid(const namespace_entry& entry) {
    return entry.kind != entry_kind::invalid;
}

inline bool entry_is_directory(const namespace_entry& entry) {
    return entry.kind == entry_kind::directory;
}

inline bool entry_is_file(const namespace_entry& entry) {
    return entry.kind == entry_kind::file;
}

inline bool entry_is_conflict_chain(const namespace_entry& entry) {
    return entry.kind == entry_kind::conflict_chain;
}

inline bool entry_name_equals(const namespace_entry& entry, std::string_view name) {
    const size_t n = static_cast<size_t>(entry.name_length);
    return n == name.size() && (n == 0 || memcmp(entry.name, name.data(), n) == 0);
}

inline const char* entry_kind_name(entry_kind kind) {
    switch (kind) {
    case entry_kind::directory:
        return "directory";
    case entry_kind::file:
        return "file";
    case entry_kind::conflict_chain:
        return "conflict_chain";
    default:
        return "invalid";
    }
}

// 命名空间表的 value 比简单 uint64_t 大得多，尤其在加入固定长度文件名后，
// 如果仍使用默认宽度，leaf 节点尺寸会超过当前线程池分配器允许的上限。
// 因此这里先把宽度调小，保证“最小原型 + 文件名承载”能够正常运行。
struct namespace_table_params : Masstree::nodeparams<7, 7> {
    using value_type = namespace_entry;
    using value_print_type = Masstree::value_print<value_type>;
    using threadinfo_type = ::threadinfo;
};

// LhmNamespace 是第二阶段的薄包装层：
// 负责把“路径字符串 -> PathKey -> Masstree 操作”这条链路收敛成
// 更接近文件系统语义的接口。
class LhmNamespace {
  public:
    using table_type = Masstree::basic_table<namespace_table_params>;
    using unlocked_cursor_type = typename table_type::unlocked_cursor_type;
    using cursor_type = Masstree::tcursor<namespace_table_params>;
    using value_type = namespace_entry;

    void initialize(threadinfo& ti) {
        table_.initialize(ti);
    }

    void destroy(threadinfo& ti) {
        table_.destroy(ti);
    }

    // lookup_entry 返回任意类型的命名空间项，根目录 "/" 被视为隐式存在。
    bool lookup_entry(const std::string& path, value_type& out, threadinfo& ti) const {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        return lookup_entry_from_parsed(parsed, out, ti);
    }

    // lookup_file 只在目标路径存在且类型为普通文件时返回成功。
    bool lookup_file(const std::string& path, value_type& out, threadinfo& ti) const {
        if (!lookup_entry(path, out, ti)) {
            return false;
        }
        return entry_is_file(out);
    }

    // lookup_directory 只在目标路径存在且类型为目录时返回成功。
    bool lookup_directory(const std::string& path, value_type& out, threadinfo& ti) const {
        if (!lookup_entry(path, out, ti)) {
            return false;
        }
        return entry_is_directory(out);
    }

    // mkdir 采用最小语义：父目录必须已存在，目标路径必须尚不存在。
    bool mkdir(const std::string& path, inode_ref ref, threadinfo& ti) {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        return create_directory_from_parsed(parsed, ref, ti);
    }

    // creat_file 与 mkdir 类似，也要求父目录存在且目标路径尚不存在。
    bool creat_file(const std::string& path, inode_ref ref, threadinfo& ti) {
        return create_entry(path, entry_kind::file, ref, ti);
    }

    // 仅用于最小冲突链测试：强制将最后一级路径分量替换成指定哈希值，
    // 从而稳定构造“同目录、同哈希、不同名字”的冲突场景。
    bool creat_file_with_forced_last_hash_for_test(const std::string& path, inode_ref ref,
                                                   uint64_t forced_hash, threadinfo& ti) {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        if (parsed.hashes.empty()) {
            return false;
        }
        parsed.hashes.back() = forced_hash;
        return create_entry_from_parsed(parsed, entry_kind::file, ref, ti);
    }

    bool lookup_file_with_forced_last_hash_for_test(const std::string& path, value_type& out,
                                                    uint64_t forced_hash, threadinfo& ti) const {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        if (parsed.hashes.empty()) {
            return false;
        }
        parsed.hashes.back() = forced_hash;
        if (!lookup_entry_from_parsed(parsed, out, ti)) {
            return false;
        }
        return entry_is_file(out);
    }

    // readdir 只扫描“当前目录对应 layer root”这一层的孩子项。
    // 扫描顺序按 Masstree 键序从左到右；命中目录 edge 时只读取目录元数据，
    // 不下钻到子目录内部。
    bool lookup_inode(const std::string& path, inode_ref& out, threadinfo& ti) const {
        value_type entry;
        if (!lookup_entry(path, entry, ti)) {
            return false;
        }
        out = entry.ref;
        return true;
    }

    bool lookup_inode_from_parsed(const ParsedPath& parsed, inode_ref& out, threadinfo& ti) const {
        return lookup_inode_from_parsed(parsed, out, ti, nullptr);
    }

    bool lookup_inode_from_parsed(const ParsedPath& parsed, inode_ref& out, threadinfo& ti,
                                  lookup_probe_stats* stats) const {
        value_type entry;
        if (!lookup_entry_from_parsed(parsed, entry, ti, stats)) {
            return false;
        }
        out = entry.ref;
        return true;
    }

    std::vector<readdir_record> readdir(const std::string& path, threadinfo& ti) const {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        std::vector<readdir_record> results;
        const node_type* directory_root = nullptr;

        if (parsed.normalized_path != "/") {
            value_type dir_entry;
            if (!lookup_directory(parsed.normalized_path, dir_entry, ti)) {
                return results;
            }
            if (!locate_directory_root(parsed, directory_root, ti)) {
                return results;
            }
        } else {
            directory_root = table_.root();
        }

        append_directory_children(directory_root, parsed.hashes, results);
        return results;
    }

    // 当前 rename 是单表原型上的“可工作语义实现”，不是最终 O(1) 版本。
    // 它通过扫描源子树、重建目标路径并逐项重插入/删除来完成：
    // - 普通文件 rename：移动一个 entry
    // - 目录 rename：递归移动整棵子树
    // 后续真正的 O(1) 目录 rename 仍需要依赖 Masstree 内核中的子树入口切换。
    bool rename_path(const std::string& old_path, const std::string& new_path, threadinfo& ti) {
        ParsedPath old_parsed = PathKey::parse_absolute_path(old_path);
        ParsedPath new_parsed = PathKey::parse_absolute_path(new_path);

        if (old_parsed.normalized_path == "/" || new_parsed.normalized_path == "/") {
            return false;
        }
        if (old_parsed.normalized_path == new_parsed.normalized_path) {
            return true;
        }
        if (is_prefix_path(old_parsed.normalized_path, new_parsed.normalized_path)) {
            return false;
        }

        value_type source_entry;
        if (!lookup_entry_from_parsed(old_parsed, source_entry, ti)) {
            return false;
        }

        if (entry_is_directory(source_entry)) {
            return rename_directory_o1_from_parsed(old_parsed, new_parsed, ti);
        }

        value_type target_entry;
        if (lookup_entry_from_parsed(new_parsed, target_entry, ti)) {
            return false;
        }

        value_type new_parent;
        if (!lookup_directory(parent_path(new_parsed.normalized_path), new_parent, ti)) {
            return false;
        }

        std::vector<subtree_record> subtree = scan_subtree(old_parsed, ti);
        if (subtree.empty()) {
            return false;
        }

        std::map<std::string, namespace_entry> source_path_map;
        for (const subtree_record& record : subtree) {
            source_path_map.emplace(hash_path_key(record.full_hashes), record.entry);
        }

        std::vector<rename_plan_item> plan;
        plan.reserve(subtree.size());
        for (const subtree_record& record : subtree) {
            std::string source_path = reconstruct_path_under_root(
                old_parsed.normalized_path, old_parsed.hashes, record.full_hashes, source_path_map);
            if (source_path.empty()) {
                return false;
            }

            std::string destination_path = rewrite_destination_path(
                source_path, old_parsed.normalized_path, new_parsed.normalized_path);
            if (destination_path.empty()) {
                return false;
            }

            ParsedPath destination_parsed = PathKey::parse_absolute_path(destination_path);
            value_type existing;
            if (lookup_entry_from_parsed(destination_parsed, existing, ti)) {
                return false;
            }

            rename_plan_item item;
            item.source_path = std::move(source_path);
            item.destination_path = std::move(destination_path);
            item.entry = record.entry;
            item.depth = destination_parsed.hashes.size();
            plan.push_back(std::move(item));
        }

        std::sort(plan.begin(), plan.end(),
                  [](const rename_plan_item& a, const rename_plan_item& b) {
                      if (a.depth != b.depth) {
                          return a.depth < b.depth;
                      }
                      return a.destination_path < b.destination_path;
                  });

        for (const rename_plan_item& item : plan) {
            if (!create_entry(item.destination_path, item.entry.kind, item.entry.ref, ti)) {
                return false;
            }
        }

        std::sort(plan.begin(), plan.end(),
                  [](const rename_plan_item& a, const rename_plan_item& b) {
                      if (a.depth != b.depth) {
                          return a.depth > b.depth;
                      }
                      return a.source_path > b.source_path;
                  });

        for (const rename_plan_item& item : plan) {
            if (!remove_entry(item.source_path, ti)) {
                return false;
            }
        }

        return true;
    }

    // 测试辅助：最小删除接口，复用当前命名空间层内部的 remove_entry。
    bool remove_path_for_test(const std::string& path, threadinfo& ti) {
        return remove_entry(path, ti);
    }

    // 测试辅助：读取某个目录当前 root 的结构形态，
    // 用来验证 gc_layer() 是否真的把 root internode 收缩到了 child root。
    bool debug_directory_root_info(const std::string& path,
                                   directory_root_debug_info& out,
                                   threadinfo& ti) const {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        const node_type* root = nullptr;
        if (parsed.normalized_path == "/") {
            root = table_.root();
        } else if (!locate_directory_root(parsed, root, ti)) {
            out = directory_root_debug_info{false, false, false, 0, 0, false,
                                            false, false, -1, -1};
            return false;
        }

        out.found = true;
        out.is_leaf = root->isleaf();
        out.has_meta = root->has_directory_meta();
        out.child0_is_leaf = false;
        out.child1_exists = false;
        out.child1_is_leaf = false;
        out.child0_size = -1;
        out.child1_size = -1;
        if (root->isleaf()) {
            const typename node_type::leaf_type* lf =
                static_cast<const typename node_type::leaf_type*>(root);
            out.height = 0;
            out.size = lf->size();
        } else {
            const typename node_type::internode_type* in =
                static_cast<const typename node_type::internode_type*>(root);
            out.height = in->height_;
            out.size = in->size();
            out.child0_is_leaf = in->child_[0] ? in->child_[0]->isleaf() : false;
            if (in->child_[0]) {
                out.child0_size = in->child_[0]->isleaf()
                    ? static_cast<const typename node_type::leaf_type*>(in->child_[0])->size()
                    : static_cast<const typename node_type::internode_type*>(in->child_[0])->size();
            }
            if (in->nkeys_ > 0 && in->child_[1]) {
                out.child1_exists = true;
                out.child1_is_leaf = in->child_[1]->isleaf();
                out.child1_size = in->child_[1]->isleaf()
                    ? static_cast<const typename node_type::leaf_type*>(in->child_[1])->size()
                    : static_cast<const typename node_type::internode_type*>(in->child_[1])->size();
            }
        }
        return true;
    }

  private:
    using node_type = typename table_type::node_type;

    struct conflict_bucket {
        std::vector<namespace_entry> entries;
    };

    struct rename_plan_item {
        std::string source_path;
        std::string destination_path;
        namespace_entry entry;
        size_t depth;
    };

    struct directory_edge_lookup {
        bool found = false;
        node_type* child_root = nullptr;
        value_type entry{};
    };

    struct child_slot_lookup {
        bool found = false;
        bool is_layer = false;
        node_type* child_root = nullptr;
        value_type value{};
    };

    struct readdir_scanner {
        const std::vector<uint64_t>* parent_hashes;
        std::vector<readdir_record>* out;
        const std::unordered_map<uint32_t, conflict_bucket>* conflict_buckets;

        template <typename SS, typename K>
        void visit_leaf(const SS&, const K&, threadinfo&) {
        }

        bool visit_value(lcdf::Str key, value_type value, threadinfo&) {
            std::vector<uint64_t> decoded = PathKey::decode(key);
            if (decoded.size() != parent_hashes->size() + 1) {
                return true;
            }
            for (size_t i = 0; i < parent_hashes->size(); ++i) {
                if (decoded[i] != (*parent_hashes)[i]) {
                    return true;
                }
            }

            if (!entry_is_conflict_chain(value)) {
                readdir_record record;
                record.child_component_hash = decoded.back();
                record.child_name = entry_name(value);
                record.full_hashes = decoded;
                record.entry = value;
                out->push_back(std::move(record));
                return true;
            }

            auto it = conflict_buckets->find(value.ref.block_id);
            if (it == conflict_buckets->end()) {
                return true;
            }
            for (const namespace_entry& chain_entry : it->second.entries) {
                readdir_record record;
                record.child_component_hash = decoded.back();
                record.child_name = entry_name(chain_entry);
                record.full_hashes = decoded;
                record.entry = chain_entry;
                out->push_back(std::move(record));
            }
            return true;
        }
    };

    struct subtree_scanner {
        const std::vector<uint64_t>* root_hashes;
        std::vector<subtree_record>* out;
        const std::unordered_map<uint32_t, conflict_bucket>* conflict_buckets;

        template <typename SS, typename K>
        void visit_leaf(const SS&, const K&, threadinfo&) {
        }

        bool visit_value(lcdf::Str key, value_type value, threadinfo&) {
            std::vector<uint64_t> decoded = PathKey::decode(key);
            if (decoded.size() < root_hashes->size()) {
                return true;
            }
            for (size_t i = 0; i < root_hashes->size(); ++i) {
                if (decoded[i] != (*root_hashes)[i]) {
                    return true;
                }
            }

            if (!entry_is_conflict_chain(value)) {
                subtree_record record;
                record.full_hashes = std::move(decoded);
                record.entry = value;
                out->push_back(std::move(record));
                return true;
            }

            auto it = conflict_buckets->find(value.ref.block_id);
            if (it == conflict_buckets->end()) {
                return true;
            }
            for (const namespace_entry& chain_entry : it->second.entries) {
                subtree_record record;
                record.full_hashes = decoded;
                record.entry = chain_entry;
                out->push_back(std::move(record));
            }
            return true;
        }
    };

    table_type table_;
    std::unordered_map<uint32_t, conflict_bucket> conflict_buckets_;
    uint32_t next_conflict_bucket_id_ = 1;

    struct single_component_key {
        char bytes[sizeof(uint64_t)];

        lcdf::Str as_str() const {
            return lcdf::Str(bytes, sizeof(bytes));
        }
    };

    static single_component_key make_single_component_key(uint64_t component_hash) {
        single_component_key key;
        uint64_t be = host_to_net_order(component_hash);
        memcpy(key.bytes, &be, sizeof(be));
        return key;
    }

    static node_type* canonicalize_directory_root(node_type* root) {
        while (true) {
            node_type* next = root->maybe_parent();
            if (next == root) {
                return root;
            }
            root = next;
        }
    }

    static const node_type* canonicalize_directory_root(const node_type* root) {
        while (true) {
            const node_type* next = root->maybe_parent();
            if (next == root) {
                return root;
            }
            root = next;
        }
    }

    bool directory_root_has_children(const node_type* node) const {
        if (node->isleaf()) {
            const typename node_type::leaf_type* leaf =
                static_cast<const typename node_type::leaf_type*>(node);
            return leaf->size() != 0;
        }

        const typename node_type::internode_type* in =
            static_cast<const typename node_type::internode_type*>(node);
        for (int i = 0; i != in->size() + 1; ++i) {
            if (in->child_[i] && directory_root_has_children(in->child_[i])) {
                return true;
            }
        }
        return false;
    }

    static std::string parent_path(const std::string& normalized_path) {
        if (normalized_path == "/") {
            return "/";
        }

        size_t pos = normalized_path.find_last_of('/');
        if (pos == 0) {
            return "/";
        }
        return normalized_path.substr(0, pos);
    }

    static bool is_prefix_path(const std::string& prefix, const std::string& path) {
        if (prefix == "/") {
            return true;
        }
        if (path.size() < prefix.size()) {
            return false;
        }
        if (path.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        return path.size() == prefix.size() || path[prefix.size()] == '/';
    }

    static std::string hash_path_key(const std::vector<uint64_t>& hashes) {
        std::string key;
        key.reserve(hashes.size() * sizeof(uint64_t));
        for (uint64_t h : hashes) {
            key.append(reinterpret_cast<const char*>(&h), sizeof(h));
        }
        return key;
    }

    static std::string rewrite_destination_path(const std::string& source_path,
                                                const std::string& source_root,
                                                const std::string& destination_root) {
        if (source_path == source_root) {
            return destination_root;
        }
        if (!is_prefix_path(source_root, source_path)) {
            return std::string();
        }
        return destination_root + source_path.substr(source_root.size());
    }

    static std::string reconstruct_path_under_root(
        const std::string& root_path,
        const std::vector<uint64_t>& root_hashes,
        const std::vector<uint64_t>& full_hashes,
        const std::map<std::string, namespace_entry>& source_path_map) {
        if (full_hashes.size() < root_hashes.size()) {
            return std::string();
        }
        for (size_t i = 0; i < root_hashes.size(); ++i) {
            if (full_hashes[i] != root_hashes[i]) {
                return std::string();
            }
        }

        std::string path = root_path;
        for (size_t i = root_hashes.size(); i < full_hashes.size(); ++i) {
            std::vector<uint64_t> prefix_hashes(full_hashes.begin(), full_hashes.begin() + i + 1);
            auto it = source_path_map.find(hash_path_key(prefix_hashes));
            if (it == source_path_map.end()) {
                return std::string();
            }
            path.push_back('/');
            path += entry_name(it->second);
        }
        return path;
    }

    bool create_entry(const std::string& path, entry_kind kind, inode_ref ref, threadinfo& ti) {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        return create_entry_from_parsed(parsed, kind, ref, ti);
    }

    bool rename_directory_o1_from_parsed(const ParsedPath& old_parsed,
                                         const ParsedPath& new_parsed,
                                         threadinfo& ti) {
        if (old_parsed.normalized_path == "/" || new_parsed.normalized_path == "/") {
            return false;
        }
        if (old_parsed.normalized_path == new_parsed.normalized_path) {
            return true;
        }
        if (is_prefix_path(old_parsed.normalized_path, new_parsed.normalized_path)) {
            return false;
        }
        if (new_parsed.components.empty() || !entry_name_fits(new_parsed.components.back())) {
            return false;
        }

        ParsedPath old_parent = old_parsed;
        old_parent.normalized_path = parent_path(old_parsed.normalized_path);
        old_parent.components.pop_back();
        old_parent.hashes.pop_back();

        ParsedPath new_parent = new_parsed;
        new_parent.normalized_path = parent_path(new_parsed.normalized_path);
        new_parent.components.pop_back();
        new_parent.hashes.pop_back();

        const node_type* old_parent_root_const = nullptr;
        if (!locate_directory_root(old_parent, old_parent_root_const, ti)) {
            return false;
        }
        const node_type* new_parent_root_const = nullptr;
        if (!locate_directory_root(new_parent, new_parent_root_const, ti)) {
            return false;
        }

        directory_edge_lookup source_edge;
        if (!lookup_directory_edge_from_parent_root(const_cast<node_type*>(old_parent_root_const),
                                                   old_parsed.components.back(),
                                                   old_parsed.hashes.back(),
                                                   source_edge, ti)) {
            return false;
        }

        value_type existing_target;
        if (lookup_child_from_parent_root(const_cast<node_type*>(new_parent_root_const),
                                          new_parsed.components.back(),
                                          new_parsed.hashes.back(),
                                          existing_target, ti)) {
            return false;
        }

        PathKey target_edge_key({new_parsed.hashes.back()});
        cursor_type attach_cursor(const_cast<node_type*>(new_parent_root_const),
                                  target_edge_key.as_str().data(),
                                  target_edge_key.as_str().length());
        if (!attach_cursor.attach_existing_layer(source_edge.child_root, ti)) {
            return false;
        }

        if (!source_edge.child_root->has_directory_meta()) {
            return false;
        }
        directory_meta* meta = source_edge.child_root->directory_meta();
        if (meta == nullptr) {
            return false;
        }
        *meta = make_directory_meta(meta->ref, new_parsed.hashes.back(),
                                    new_parsed.components.back());

        ParsedPath detach_parent = old_parent;
        PathKey old_edge_key({old_parsed.hashes.back()});
        cursor_type detach_cursor(const_cast<node_type*>(old_parent_root_const),
                                  old_edge_key.as_str().data(),
                                  old_edge_key.as_str().length());
        detach_cursor.find_locked_edge(ti);
        int state = detach_cursor.state();
        if (state >= 0 || !detach_cursor.is_layer()
            || detach_cursor.layer_root() != source_edge.child_root) {
            detach_cursor.finish_read();
            return false;
        }
        if (!detach_cursor.remove_layer_edge(ti)) {
            return false;
        }
        return true;
    }

    bool remove_entry(const std::string& path, threadinfo& ti) {
        ParsedPath parsed = PathKey::parse_absolute_path(path);
        if (parsed.normalized_path == "/" || parsed.components.empty()) {
            return false;
        }

        ParsedPath parent = parsed;
        parent.normalized_path = parent_path(parsed.normalized_path);
        parent.components.pop_back();
        parent.hashes.pop_back();

        const node_type* parent_root_const = nullptr;
        if (!locate_directory_root(parent, parent_root_const, ti)) {
            return false;
        }
        node_type* parent_root = const_cast<node_type*>(parent_root_const);

        single_component_key key = make_single_component_key(parsed.hashes.back());
        cursor_type cursor(parent_root, key.as_str().data(), key.as_str().length());
        cursor.find_locked_edge(ti);
        int state = cursor.state();

        if (state < 0 && cursor.is_layer()) {
            node_type* child_root = canonicalize_directory_root(cursor.layer_root());
            if (!child_root->has_directory_meta()) {
                cursor.finish_read();
                return false;
            }
            const directory_meta* meta = child_root->directory_meta();
            if (meta == nullptr || !directory_meta_name_equals(*meta, parsed.components.back())) {
                cursor.finish_read();
                return false;
            }
            if (directory_root_has_children(child_root)) {
                cursor.finish_read();
                return false;
            }
            return cursor.remove_layer_edge(ti);
        }

        if (state <= 0) {
            cursor.finish_read();
            return false;
        }

        value_type slot_value = cursor.value();
        if (!entry_is_conflict_chain(slot_value)) {
            if (!entry_name_equals(slot_value, parsed.components.back()) || !entry_is_file(slot_value)) {
                cursor.finish_read();
                return false;
            }
            cursor.finish(-1, ti);
            return true;
        }

        auto it = conflict_buckets_.find(slot_value.ref.block_id);
        if (it == conflict_buckets_.end()) {
            cursor.finish_read();
            return false;
        }

        std::vector<namespace_entry>& entries = it->second.entries;
        auto match = std::find_if(entries.begin(), entries.end(),
                                  [&](const namespace_entry& entry) {
                                      return entry_name_equals(entry, parsed.components.back());
                                  });
        if (match == entries.end() || !entry_is_file(*match)) {
            cursor.finish_read();
            return false;
        }

        if (entries.size() == 1) {
            conflict_buckets_.erase(it);
            cursor.finish(-1, ti);
            return true;
        }

        entries.erase(match);
        if (entries.size() == 1) {
            namespace_entry survivor = entries.front();
            conflict_buckets_.erase(it);
            cursor.value() = survivor;
            fence();
        }
        cursor.finish(0, ti);
        return true;
    }

    bool lookup_entry_from_parsed(const ParsedPath& parsed, value_type& out, threadinfo& ti) const {
        return lookup_entry_from_parsed(parsed, out, ti, nullptr);
    }

    bool lookup_entry_from_parsed(const ParsedPath& parsed, value_type& out, threadinfo& ti,
                                  lookup_probe_stats* stats) const {
        if (parsed.normalized_path == "/") {
            out = make_namespace_entry(entry_kind::directory, make_inode_ref(0, 0), "/");
            return true;
        }
        if (parsed.components.empty() || parsed.hashes.empty()) {
            return false;
        }

        // stat/lookup 走单一路线：
        // 1) 先只定位到父目录 root；
        // 2) 再只查最后一级孩子槽位；
        // 3) 命中 layer 就按目录返回，命中 value 就按文件/冲突链返回。
        // 这样避免了旧路径中“先整条路径按目录逐级判断，再走一次 value 查询”的双遍历。
        ParsedPath parent = parsed;
        parent.normalized_path = parent_path(parsed.normalized_path);
        parent.components.pop_back();
        parent.hashes.pop_back();

        const node_type* parent_root_const = nullptr;
        if (!locate_directory_root(parent, parent_root_const, ti, stats)) {
            return false;
        }

        return lookup_child_from_parent_root(const_cast<node_type*>(parent_root_const),
                                             parsed.components.back(),
                                             parsed.hashes.back(),
                                             out, ti, stats);
    }

    bool create_entry_from_parsed(const ParsedPath& parsed, entry_kind kind, inode_ref ref,
                                  threadinfo& ti) {
        if (kind == entry_kind::directory) {
            return create_directory_from_parsed(parsed, ref, ti);
        }
        if (parsed.normalized_path == "/") {
            return false;
        }
        if (parsed.components.empty()) {
            return false;
        }
        if (!entry_name_fits(parsed.components.back())) {
            return false;
        }

        ParsedPath parent = parsed;
        parent.normalized_path = parent_path(parsed.normalized_path);
        parent.components.pop_back();
        parent.hashes.pop_back();

        const node_type* parent_root_const = nullptr;
        if (!locate_directory_root(parent, parent_root_const, ti)) {
            return false;
        }
        node_type* parent_root = const_cast<node_type*>(parent_root_const);

        value_type entry = make_namespace_entry(kind, ref, parsed.components.back());
        single_component_key key = make_single_component_key(parsed.hashes.back());

        child_slot_lookup slot;
        if (lookup_child_slot_from_parent_root(parent_root, parsed.hashes.back(), slot, ti)) {
            if (slot.is_layer) {
                return false;
            }
            return insert_into_conflict_chain(key.as_str(), slot.value, entry, ti);
        }

        cursor_type cursor(parent_root, key.as_str().data(), key.as_str().length());
        bool already_present = cursor.find_insert(ti);
        if (already_present) {
            cursor.finish(0, ti);
            return false;
        }

        cursor.value() = entry;
        fence();
        cursor.finish(1, ti);
        return true;
    }

    bool create_directory_from_parsed(const ParsedPath& parsed, inode_ref ref, threadinfo& ti) {
        if (parsed.normalized_path == "/") {
            return false;
        }
        if (parsed.components.empty()) {
            return false;
        }
        if (!entry_name_fits(parsed.components.back())) {
            return false;
        }

        ParsedPath parent = parsed;
        parent.normalized_path = parent_path(parsed.normalized_path);
        parent.components.pop_back();
        parent.hashes.pop_back();

        const node_type* parent_root_const = nullptr;
        if (!locate_directory_root(parent, parent_root_const, ti)) {
            return false;
        }
        node_type* parent_root = const_cast<node_type*>(parent_root_const);

        child_slot_lookup existing;
        if (lookup_child_slot_from_parent_root(parent_root, parsed.hashes.back(), existing, ti)) {
            return false;
        }

        single_component_key edge_key = make_single_component_key(parsed.hashes.back());
        cursor_type cursor(parent_root, edge_key.as_str().data(), edge_key.as_str().length());
        node_type* directory_root = nullptr;
        directory_meta meta = make_directory_meta(ref, parsed.hashes.back(),
                                                  parsed.components.back());
        if (!cursor.create_layer_with_meta(directory_root, meta, ti)) {
            return false;
        }
        return true;
    }

    std::vector<subtree_record> scan_subtree(const ParsedPath& root, threadinfo& ti) const {
        subtree_scanner scanner;
        std::vector<subtree_record> results;
        scanner.root_hashes = &root.hashes;
        scanner.out = &results;
        scanner.conflict_buckets = &conflict_buckets_;
        table_.scan(lcdf::Str(""), false, scanner, ti);
        return results;
    }

    bool insert_into_conflict_chain(lcdf::Str key, const value_type& slot_value,
                                    const value_type& new_entry, threadinfo& ti) {
        if (entry_is_conflict_chain(slot_value)) {
            auto it = conflict_buckets_.find(slot_value.ref.block_id);
            if (it == conflict_buckets_.end()) {
                return false;
            }
            for (const namespace_entry& chain_entry : it->second.entries) {
                if (entry_name_equals(chain_entry, entry_name_view(new_entry))) {
                    return false;
                }
            }
            it->second.entries.push_back(new_entry);
            return true;
        }

        if (entry_name_equals(slot_value, entry_name_view(new_entry))) {
            return false;
        }

        uint32_t bucket_id = next_conflict_bucket_id_++;
        conflict_bucket bucket;
        bucket.entries.push_back(slot_value);
        bucket.entries.push_back(new_entry);
        conflict_buckets_.emplace(bucket_id, std::move(bucket));

        value_type chain_marker = make_namespace_entry(entry_kind::conflict_chain,
                                                       make_inode_ref(bucket_id, 0), "");
        cursor_type cursor(table_, key);
        bool found = cursor.find_insert(ti);
        if (!found) {
            cursor.finish(0, ti);
            conflict_buckets_.erase(bucket_id);
            return false;
        }
        cursor.value() = chain_marker;
        cursor.finish(0, ti);
        return true;
    }

    // 在父目录对应的 layer root 内，用“最后一级组件哈希 + 真实名字”检查孩子是否已存在。
    // 当前目录和文件都可以复用这条局部查找路径，避免每次都从全局根重新走完整路径。
    bool lookup_child_from_parent_root(node_type* parent_root, const std::string& child_name,
                                       uint64_t child_hash, value_type& out,
                                       threadinfo& ti) const {
        return lookup_child_from_parent_root(parent_root, child_name, child_hash, out, ti, nullptr);
    }

    bool lookup_child_from_parent_root(node_type* parent_root, const std::string& child_name,
                                       uint64_t child_hash, value_type& out,
                                       threadinfo& ti, lookup_probe_stats* stats) const {
        if (stats != nullptr) {
            ++stats->child_slot_lookups;
        }
        child_slot_lookup slot;
        if (!lookup_child_slot_from_parent_root(parent_root, child_hash, slot, ti)) {
            return false;
        }

        if (slot.is_layer) {
            const node_type* child_root = slot.child_root;
            if (child_root->has_directory_meta()) {
                const directory_meta* meta = child_root->directory_meta();
                if (meta != nullptr && directory_meta_name_equals(*meta, child_name)) {
                    out = make_namespace_entry(entry_kind::directory, meta->ref,
                                               directory_meta_name(*meta));
                    return true;
                }
            }
            return false;
        }

        const value_type& raw = slot.value;
        if (!entry_is_conflict_chain(raw)) {
            if (entry_name_equals(raw, child_name)) {
                out = raw;
                return true;
            }
            return false;
        }

        auto it = conflict_buckets_.find(raw.ref.block_id);
        if (it == conflict_buckets_.end()) {
            return false;
        }
        for (const namespace_entry& chain_entry : it->second.entries) {
            if (entry_name_equals(chain_entry, child_name)) {
                out = chain_entry;
                return true;
            }
        }
        return false;
    }

    bool lookup_directory_edge_from_parent_root(node_type* parent_root,
                                                const std::string& child_name,
                                                uint64_t child_hash,
                                                directory_edge_lookup& out,
                                                threadinfo& ti) const {
        single_component_key child_key = make_single_component_key(child_hash);
        cursor_type cursor(parent_root, child_key.as_str().data(), child_key.as_str().length());
        cursor.find_locked_edge(ti);
        int state = cursor.state();
        if (state < 0 && cursor.is_layer()) {
            node_type* child_root = canonicalize_directory_root(cursor.layer_root());
            cursor.finish_read();
            if (!child_root->has_directory_meta()) {
                return false;
            }
            const directory_meta* meta = child_root->directory_meta();
            if (meta == nullptr || !directory_meta_name_equals(*meta, child_name)) {
                return false;
            }
            out.found = true;
            out.child_root = child_root;
            out.entry = make_namespace_entry(entry_kind::directory, meta->ref,
                                             directory_meta_name(*meta));
            return true;
        }
        cursor.finish_read();
        return false;
    }

    bool lookup_child_slot_from_parent_root(node_type* parent_root, uint64_t child_hash,
                                            child_slot_lookup& out, threadinfo& ti) const {
        out = child_slot_lookup{};
        single_component_key child_key = make_single_component_key(child_hash);
        unlocked_cursor_type cursor(parent_root, child_key.as_str().data(),
                                    child_key.as_str().length());
        cursor.find_unlocked_edge(ti);
        int state = cursor.state();
        if (state < 0 && cursor.is_layer()) {
            out.found = true;
            out.is_layer = true;
            out.child_root = canonicalize_directory_root(cursor.layer_root());
            return true;
        }
        if (state <= 0) {
            return false;
        }
        out.found = true;
        out.value = cursor.value();
        return true;
    }

    bool locate_directory_root(const ParsedPath& parsed, const node_type*& out,
                               threadinfo& ti) const {
        return locate_directory_root(parsed, out, ti, nullptr);
    }

    bool locate_directory_root(const ParsedPath& parsed, const node_type*& out,
                               threadinfo& ti, lookup_probe_stats* stats) const {
        out = table_.root();
        if (parsed.normalized_path == "/") {
            return true;
        }

        for (uint64_t component_hash : parsed.hashes) {
            single_component_key edge_key = make_single_component_key(component_hash);
            unlocked_cursor_type cursor(const_cast<node_type*>(out), edge_key.as_str().data(),
                                        edge_key.as_str().length());
            cursor.find_unlocked_edge(ti);
            int state = cursor.state();
            if (state >= 0 || !cursor.is_layer()) {
                return false;
            }
            if (stats != nullptr) {
                ++stats->directory_levels_walked;
            }
            out = canonicalize_directory_root(cursor.layer_root());
        }
        return true;
    }

    void append_directory_children(const node_type* parent_root,
                                   const std::vector<uint64_t>& parent_hashes,
                                   std::vector<readdir_record>& out) const {
        append_directory_children_from_node(parent_root, parent_hashes, out);
    }

    void append_directory_children_from_node(const node_type* node,
                                             const std::vector<uint64_t>& parent_hashes,
                                             std::vector<readdir_record>& out) const {
        if (node->isleaf()) {
            const typename node_type::leaf_type* leaf =
                static_cast<const typename node_type::leaf_type*>(node);
            typename node_type::leaf_type::permuter_type perm = leaf->permutation();
            for (int i = 0; i != leaf->size(); ++i) {
                int p = perm[i];
                uint64_t child_hash = static_cast<uint64_t>(leaf->ikey(p));

                if (!leaf->is_layer(p)) {
                    value_type slot_value = leaf->lv_[p].value();
                    if (!entry_is_conflict_chain(slot_value)) {
                        readdir_record record;
                        record.full_hashes = parent_hashes;
                        record.full_hashes.push_back(child_hash);
                        record.child_component_hash = child_hash;
                        record.child_name = entry_name(slot_value);
                        record.entry = slot_value;
                        out.push_back(std::move(record));
                        continue;
                    }

                    auto it = conflict_buckets_.find(slot_value.ref.block_id);
                    if (it == conflict_buckets_.end()) {
                        continue;
                    }
                    for (const namespace_entry& chain_entry : it->second.entries) {
                        readdir_record record;
                        record.full_hashes = parent_hashes;
                        record.full_hashes.push_back(child_hash);
                        record.child_component_hash = child_hash;
                        record.child_name = entry_name(chain_entry);
                        record.entry = chain_entry;
                        out.push_back(std::move(record));
                    }
                    continue;
                }

                // 命中目录 edge 时只恢复当前目录项，不下钻子目录。
                const node_type* child_root =
                    canonicalize_directory_root(leaf->lv_[p].layer());
                if (!child_root->has_directory_meta()) {
                    continue;
                }
                const directory_meta* meta = child_root->directory_meta();
                if (meta == nullptr) {
                    continue;
                }
                readdir_record record;
                record.full_hashes = parent_hashes;
                record.full_hashes.push_back(meta->component_hash);
                record.child_component_hash = meta->component_hash;
                record.child_name = directory_meta_name(*meta);
                record.entry = make_namespace_entry(entry_kind::directory, meta->ref,
                                                    directory_meta_name(*meta));
                out.push_back(std::move(record));
            }
            return;
        }

        const typename node_type::internode_type* in =
            static_cast<const typename node_type::internode_type*>(node);
        for (int i = 0; i != in->size() + 1; ++i) {
            if (in->child_[i]) {
                append_directory_children_from_node(in->child_[i], parent_hashes, out);
            }
        }
    }
};

}  // namespace MasstreeLHM

namespace Masstree {

template <>
class value_print<MasstreeLHM::namespace_entry> {
  public:
    static void print(MasstreeLHM::namespace_entry value, FILE* f, const char* prefix,
                      int indent, Str key, kvtimestamp_t, char* suffix) {
        std::string name = MasstreeLHM::entry_name(value);
        fprintf(f, "%s%*s%.*s = {kind=%s, name=%s, ref=(block=%" PRIu32 ", offset=%" PRIu32 ")}%s\n",
                prefix, indent, "", key.len, key.s,
                MasstreeLHM::entry_kind_name(value.kind), name.c_str(),
                value.ref.block_id, value.ref.offset, suffix);
    }
};

}  // namespace Masstree

#endif
