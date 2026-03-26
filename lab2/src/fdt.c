#include "fdt.h"
#include "string.h"
#include <stddef.h>

static inline uint32_t bswap32(uint32_t x) {
    /* 反轉位元組順序（Byte Swap）*/
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static inline const void* align_up(const void* ptr, size_t align) {
    return (const void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static int node_name_matches(const char* node_name, const char* path_part) {
    size_t path_len = str_len(path_part);

    /* 路徑若明確帶有 unit address，就必須完整吻合。 */
    if (str_chr(path_part, '@') != NULL) {
        return str_cmp(node_name, path_part) == 0;
    }

    /* 否則允許 "memory" 這類路徑匹配到 "memory@80000000"。 */
    return str_ncmp(node_name, path_part, path_len) == 0 &&
           (node_name[path_len] == '\0' || node_name[path_len] == '@');
}

static int path_finished(const char* path) {
    while (*path == '/') path++;
    return *path == '\0';
}

int fdt_path_offset(const void* fdt, const char* path) {
    if (!fdt || !path || !*path) return -1;

    /* 將 void* 轉成 header 結構指標 */
    const struct fdt_header* header = (const struct fdt_header*)fdt;
    /* 檢查 Magic Number 是否正確 */
    if (bswap32(header->magic) != 0xd00dfeed) {
        return -1;
    }

    /* 找 structure block 的起始點 */
    uint32_t off_struct = bswap32(header->off_dt_struct);
    uint32_t size_struct = bswap32(header->size_dt_struct);
    const char* struct_ptr = (const char*)fdt + off_struct;
    const char* struct_end = struct_ptr + size_struct;

    if (path[0] == '/' && path[1] == '\0') {
        return (int)off_struct;
    }

    const char* path_cursor = path;
    int depth = -1; // 目前正在掃描的深度(每遇到一個FDT_BEGIN_NODE就往下一層)
    int matched_depth = 0; // 目前路徑比對成功了幾層
    const char* path_stack[64] = {0}; // 路徑回溯用的堆疊
    int advanced_stack[64] = {0}; // 記錄某一層是不是有成功把 path_cursor 往前推進
    char segment[64];

    while (struct_ptr + sizeof(uint32_t) <= struct_end) {
        const char *token_start = struct_ptr;
        uint32_t token = bswap32(*(const uint32_t*) struct_ptr);
        if (token == FDT_BEGIN_NODE) {
            struct_ptr += 4;
            {
                const char *node_name = struct_ptr;
                size_t name_len = str_len(node_name);

                if (depth == -1) {
                    if (name_len != 0) {
                        return -1;
                    }
                    depth = 0;
                } else {
                    if ((size_t)depth >= sizeof(path_stack) / sizeof(path_stack[0])) {
                        return -1;
                    }
                    depth++;
                    advanced_stack[depth] = 0;

                    if (matched_depth == depth - 1) {
                        const char* next_path = path_cursor;

                        if (str_parser(&next_path, segment) > 0 &&
                            node_name_matches(node_name, segment)) {
                            path_stack[depth] = path_cursor;
                            path_cursor = next_path;
                            matched_depth++;
                            advanced_stack[depth] = 1;
                        }
                    }

                    if (advanced_stack[depth] && path_finished(path_cursor)) {
                        return (int)(token_start - (const char*)fdt);
                    }

                    if (name_len >= sizeof(segment)) {
                        return -1;
                    }
                }

                struct_ptr += name_len + 1; // +1 是因為空字元
                struct_ptr = align_up(struct_ptr, 4);
            }
        }
        else if (token == FDT_END_NODE) {
            struct_ptr += 4;
            if (depth > 0) {
                if (advanced_stack[depth]) {
                    matched_depth--;
                    path_cursor = path_stack[depth];
                }
                depth--;
            } else if (depth == 0) {
                depth = -1;
            } else {
                return -1;
            }
        }
        else if (token == FDT_NOP) {
            struct_ptr += 4;
        }
        else if (token == FDT_END) {
            return -1;
        }
        else if (token == FDT_PROP) {
            struct_ptr += 4;
            if (struct_ptr + 2 * sizeof(uint32_t) > struct_end) {
                return -1;
            }
            uint32_t len = bswap32(*(const uint32_t*) struct_ptr);
            struct_ptr += 4;
            struct_ptr += 4;
            struct_ptr += len;
            if (struct_ptr > struct_end) {
                return -1;
            }
            struct_ptr = (const char*)align_up(struct_ptr, 4);
        }
        else {
            return -1;
        }
    }

    return -1;
}

int str_parser(const char** path, char *output){
    const char *p = *path;

    while(*p == '/') p++;
    if(*p == '\0') return 0;

    const char *start = p;

    while(*p && *p != '/') p++;
    int len = p - start;
    
    mem_cpy(output, start, len);
    output[len] = '\0';

    *path = p;

    return len;
}

const void* fdt_getprop(const void* fdt, int nodeoffset, const char* name, int* lenp) {
    const struct fdt_header* header = (const struct fdt_header*)fdt;
    const char* string_ptr = (const char*)fdt + bswap32(header->off_dt_strings);
    const char* string_end = string_ptr + bswap32(header->size_dt_strings);
    const char* struct_ptr = (const char*)fdt + nodeoffset;
    const char* struct_end = (const char*)fdt + bswap32(header->off_dt_struct) +
                             bswap32(header->size_dt_struct);

    if (lenp) *lenp = -1; // 用來把屬性長度傳回去的輸出參數指標
    if (!fdt || !name) return NULL;

    // 先跳過節點的 BEGIN_NODE token 和名稱
    if (struct_ptr + sizeof(uint32_t) > struct_end) return NULL;
    uint32_t token = bswap32(*(const uint32_t*)struct_ptr);
    if (token != FDT_BEGIN_NODE) return NULL;
    
    struct_ptr += 4;
    struct_ptr += str_len(struct_ptr) + 1;
    struct_ptr = align_up(struct_ptr, 4);

    // 搜尋該節點的屬性
    while (struct_ptr + sizeof(uint32_t) <= struct_end) {
        token = bswap32(*(const uint32_t*)struct_ptr);
        
        if (token == FDT_PROP) {
            struct_ptr += 4;
            if (struct_ptr + 2 * sizeof(uint32_t) > struct_end) return NULL;
            uint32_t len = bswap32(*(const uint32_t*)struct_ptr);
            struct_ptr += 4;
            uint32_t nameoff = bswap32(*(const uint32_t*)struct_ptr);
            struct_ptr += 4;
            if (struct_ptr + len > struct_end) return NULL;
            if (string_ptr + nameoff >= string_end) return NULL;

            if (str_cmp(string_ptr + nameoff, name) == 0) {
                if (lenp) *lenp = (int)len;
                return (const void*)struct_ptr; 
            }
 
            struct_ptr += len;
            struct_ptr = align_up(struct_ptr, 4);
        } 
        else if (token == FDT_NOP) {
            struct_ptr += 4;
        } 
        else {
            break;
        }
    }
    return NULL;
}
