#include "fdt.h"
#include "string.h"
#include "uart.h"
#include <stddef.h>
/* Internal */
static inline uint32_t bswap32(uint32_t x)
{
    /* 反轉位元組順序（Byte Swap）*/
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static inline const void *align_up(const void *ptr, size_t align)
{ /* 位元對齊 */
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static int node_name_matches(const char *node_name, const char *path_part)
{
    size_t path_len = str_len(path_part);

    /* 路徑若明確帶有 unit address，就必須完整吻合。 */
    if (str_chr(path_part, '@') != NULL)
    {
        return str_cmp(node_name, path_part) == 0;
    }

    /* 否則允許 "memory" 這類路徑匹配到 "memory@80000000"。 */
    return str_ncmp(node_name, path_part, path_len) == 0 &&
           (node_name[path_len] == '\0' || node_name[path_len] == '@');
}

static int str_parser(const char **path, char *output)
{
    const char *p = *path;

    while (*p == '/')
        p++;
    if (*p == '\0')
        return 0;

    const char *start = p;

    while (*p && *p != '/')
        p++;
    int len = p - start;

    mem_cpy(output, start, len);
    output[len] = '\0';

    *path = p;

    return len;
}

static int path_finished(const char *path)
{
    while (*path == '/')
        path++;
    return *path == '\0';
}

static int get_root_cells(const void *fdt, int *address_cells, int *size_cells)
{
    int root;
    int len = 0;
    const void *prop;

    if (!fdt || !address_cells || !size_cells)
    {
        return -1;
    }

    root = fdt_path_offset(fdt, "/");
    if (root < 0)
    {
        return -1;
    }

    *address_cells = 2; // 位址用 2 個 32 bits 表示
    *size_cells = 2;    // 大小用 2 個 32 bits 表示

    prop = fdt_getprop(fdt, root, "#address-cells", &len);
    if (prop && len >= 4)
    {
        *address_cells = (int)bswap32(*(const uint32_t *)prop);
    }

    prop = fdt_getprop(fdt, root, "#size-cells", &len);
    if (prop && len >= 4)
    {
        *size_cells = (int)bswap32(*(const uint32_t *)prop);
    }

    if (*address_cells <= 0 || *address_cells > 2 ||
        *size_cells <= 0 || *size_cells > 2)
    {
        return -1;
    }

    return 0;
}

static unsigned long read_be_cells(const uint32_t *cells, int count)
{
    /*
        它會從 cells 開始，讀 count 個 uint32_t
        每個 cell 都先做 bswap32()，因為 FDT 裡是 big-endian
        然後把多個 32-bit cell 拼成一個 unsigned long
    */
    /* 讀 big-endian */
    unsigned long value = 0;
    int i;

    for (i = 0; i < count; i++)
    {
        value = (value << 32) | (unsigned long)bswap32(cells[i]);
    }

    return value;
}

/*
 * 取得指定 node 解析 reg 時要用的 cell 設定
 * 多數情況下會先沿用父層傳進來的預設值，
 * 若 node 自己有宣告 #address-cells / #size-cells，則以該值覆蓋。
 */
static int get_node_cells(const void *fdt,
                          int nodeoffset,
                          int default_addr_cells,
                          int default_size_cells,
                          int *address_cells,
                          int *size_cells)
{
    int len = 0;
    const void *prop;

    /* 呼叫者必須提供輸出位置，才能把最後解析出的 cell 數寫回去。 */
    if (!address_cells || !size_cells)
    {
        return -1;
    }

    /* 先採用上一層傳下來的預設值，若本 node 沒覆寫就沿用它。 */
    *address_cells = default_addr_cells;
    *size_cells = default_size_cells;

    /* 若 node 自己定義 #address-cells，就覆蓋預設值。 */
    prop = fdt_getprop(fdt, nodeoffset, "#address-cells", &len);
    if (prop && len >= 4)
    {
        *address_cells = (int)bswap32(*(const uint32_t *)prop);
    }

    /* 若 node 自己定義 #size-cells，就覆蓋預設值。 */
    prop = fdt_getprop(fdt, nodeoffset, "#size-cells", &len);
    if (prop && len >= 4)
    {
        *size_cells = (int)bswap32(*(const uint32_t *)prop);
    }

    /* 這份 parser 只支援 1 或 2 個 32-bit cells 的 address/size 編碼。 */
    if (*address_cells <= 0 || *address_cells > 2 ||
        *size_cells <= 0 || *size_cells > 2)
    {
        return -1;
    }

    return 0;
}


static int append_reg_regions(const void *reg_prop,
                              int len,
                              int addr_cells,
                              int size_cells,
                              struct fdt_memory_region *regions,
                              int max_regions,
                              int count)
{
    int tuple_cells = addr_cells + size_cells;
    int tuples;
    int i;
    const uint32_t *cells = (const uint32_t *)reg_prop;

    if (!reg_prop || !regions || max_regions <= 0 || tuple_cells <= 0)
    {
        return count;
    }

    /* 把 reg property 中的一組組 (start, size) 直接展平成 region array。 */
    tuples = len / (tuple_cells * (int)sizeof(uint32_t)); // 計算 reg 裡有幾組區段
    for (i = 0; i < tuples && count < max_regions; i++) 
    {
        unsigned long start = read_be_cells(cells, addr_cells);
        unsigned long size = read_be_cells(cells + addr_cells, size_cells);

        if (size != 0)
        {
            regions[count].start = start;
            regions[count].size = size;
            count++;
        }

        cells += tuple_cells; // 這組 tuple 已經讀完了，下一輪要移到下一組
    }

    return count; // 回傳的是 regions 共有幾筆有效資料
}
/* External */

int fdt_path_offset(const void *fdt, const char *path)
{
    if (!fdt || !path || !*path)
        return -1;

    /* 將 void* 轉成 header 結構指標 */
    const struct fdt_header *header = (const struct fdt_header *)fdt;
    /* 檢查 Magic Number 是否正確 */
    if (bswap32(header->magic) != 0xd00dfeed)
    {
        return -1;
    }

    /* 找 structure block 的起始點 */
    uint32_t off_struct = bswap32(header->off_dt_struct);
    uint32_t size_struct = bswap32(header->size_dt_struct);
    const char *struct_ptr = (const char *)fdt + off_struct;
    const char *struct_end = struct_ptr + size_struct;

    if (path[0] == '/' && path[1] == '\0')
    {
        return (int)off_struct;
    }

    const char *path_cursor = path;
    int depth = -1;                   // 目前正在掃描的深度(每遇到一個FDT_BEGIN_NODE就往下一層)
    int matched_depth = 0;            // 目前路徑比對成功了幾層
    const char *path_stack[64] = {0}; // 路徑回溯用的堆疊
    int advanced_stack[64] = {0};     // 記錄某一層是不是有成功把 path_cursor 往前推進
    char segment[64];

    while (struct_ptr + sizeof(uint32_t) <= struct_end)
    {
        const char *token_start = struct_ptr;
        uint32_t token = bswap32(*(const uint32_t *)struct_ptr);
        if (token == FDT_BEGIN_NODE)
        {
            struct_ptr += 4;
            {
                const char *node_name = struct_ptr;
                size_t name_len = str_len(node_name);

                if (depth == -1)
                {
                    if (name_len != 0)
                    {
                        return -1;
                    }
                    depth = 0;
                }
                else
                {
                    if ((size_t)depth >= sizeof(path_stack) / sizeof(path_stack[0]))
                    {
                        return -1;
                    }
                    depth++;
                    advanced_stack[depth] = 0;

                    if (matched_depth == depth - 1)
                    {
                        const char *next_path = path_cursor;

                        if (str_parser(&next_path, segment) > 0 &&
                            node_name_matches(node_name, segment))
                        {
                            path_stack[depth] = path_cursor;
                            path_cursor = next_path;
                            matched_depth++;
                            advanced_stack[depth] = 1;
                        }
                    }

                    if (advanced_stack[depth] && path_finished(path_cursor))
                    {
                        return (int)(token_start - (const char *)fdt);
                    }

                    if (name_len >= sizeof(segment))
                    {
                        return -1;
                    }
                }

                struct_ptr += name_len + 1; // +1 是因為空字元
                struct_ptr = align_up(struct_ptr, 4);
            }
        }
        else if (token == FDT_END_NODE)
        {
            struct_ptr += 4;
            if (depth > 0)
            {
                if (advanced_stack[depth])
                {
                    matched_depth--;
                    path_cursor = path_stack[depth];
                }
                depth--;
            }
            else if (depth == 0)
            {
                depth = -1;
            }
            else
            {
                return -1;
            }
        }
        else if (token == FDT_NOP)
        {
            struct_ptr += 4;
        }
        else if (token == FDT_END)
        {
            return -1;
        }
        else if (token == FDT_PROP)
        {
            struct_ptr += 4;
            if (struct_ptr + 2 * sizeof(uint32_t) > struct_end)
            {
                return -1;
            }
            uint32_t len = bswap32(*(const uint32_t *)struct_ptr);
            struct_ptr += 4;
            struct_ptr += 4;
            struct_ptr += len;
            if (struct_ptr > struct_end)
            {
                return -1;
            }
            struct_ptr = (const char *)align_up(struct_ptr, 4);
        }
        else
        {
            return -1;
        }
    }

    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp)
{
    const struct fdt_header *header = (const struct fdt_header *)fdt;
    const char *string_ptr = (const char *)fdt + bswap32(header->off_dt_strings);
    const char *string_end = string_ptr + bswap32(header->size_dt_strings);
    const char *struct_ptr = (const char *)fdt + nodeoffset;
    const char *struct_end = (const char *)fdt + bswap32(header->off_dt_struct) +
                             bswap32(header->size_dt_struct);

    if (lenp)
        *lenp = -1; // 用來把屬性長度傳回去的輸出參數指標
    if (!fdt || !name)
        return NULL;

    // 先跳過節點的 BEGIN_NODE token 和名稱
    if (struct_ptr + sizeof(uint32_t) > struct_end)
        return NULL;
    uint32_t token = bswap32(*(const uint32_t *)struct_ptr);
    if (token != FDT_BEGIN_NODE)
        return NULL;

    struct_ptr += 4;
    struct_ptr += str_len(struct_ptr) + 1;
    struct_ptr = align_up(struct_ptr, 4);

    // 搜尋該節點的屬性
    while (struct_ptr + sizeof(uint32_t) <= struct_end)
    {
        token = bswap32(*(const uint32_t *)struct_ptr);

        if (token == FDT_PROP)
        {
            struct_ptr += 4;
            if (struct_ptr + 2 * sizeof(uint32_t) > struct_end)
                return NULL;
            uint32_t len = bswap32(*(const uint32_t *)struct_ptr);
            struct_ptr += 4;
            uint32_t nameoff = bswap32(*(const uint32_t *)struct_ptr);
            struct_ptr += 4;
            if (struct_ptr + len > struct_end)
                return NULL;
            if (string_ptr + nameoff >= string_end)
                return NULL;

            if (str_cmp(string_ptr + nameoff, name) == 0)
            {
                if (lenp)
                    *lenp = (int)len;
                return (const void *)struct_ptr;
            }

            struct_ptr += len;
            struct_ptr = align_up(struct_ptr, 4);
        }
        else if (token == FDT_NOP)
        {
            struct_ptr += 4;
        }
        else
        {
            break;
        }
    }
    return NULL;
}

int fdt_read_prop_addr(const void *prop, int len, unsigned long *out)
{
    const uint32_t *cells = (const uint32_t *)prop;

    if (!prop || !out)
    {
        return -1;
    }

    if (len >= 8)
    {
        *out = ((unsigned long)bswap32(cells[0]) << 32) | (unsigned long)bswap32(cells[1]);
        return 0;
    }

    if (len >= 4)
    {
        *out = (unsigned long)bswap32(cells[0]);
        return 0;
    }
    return -1;
}

unsigned long fdt_totalsize(const void *fdt)
{
    const struct fdt_header *header = (const struct fdt_header *)fdt;

    /* DTB blob 本身也佔用實體記憶體，因此 allocator 需要保留 totalsize 範圍。 */
    if (!fdt || bswap32(header->magic) != 0xd00dfeed)
    {
        return 0;
    }

    return (unsigned long)bswap32(header->totalsize);
}

unsigned long fdt_get_initrd_start_or_default(const void *fdt,
                                              unsigned long fallback)
{
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;

    if (fdt_get_initrd_range(fdt, &initrd_start, &initrd_end) < 0)
    {
        return fallback;
    }

    if (initrd_end <= initrd_start)
    {
        return fallback;
    }

    return initrd_start;
}

int fdt_get_memory_range(const void *fdt, unsigned long *base, unsigned long *size)
{
    int mem_offset;     // memory node 在 FDT structure block 裡的 offset
    int len = 0;        // 存 reg property 的長度，單位是 byte
    int addr_cells = 0; // 表示一個 address 佔幾個 cell
    int size_cells = 0; // 表示一個 size 佔幾個 cell
    int tuple_cells;    // 存一組 (address, size) 總共需要幾個 cell

    const uint32_t *reg;

    if (!fdt || !base || !size)
        return -1;

    if (get_root_cells(fdt, &addr_cells, &size_cells) < 0)
        return -1;

    mem_offset = fdt_path_offset(fdt, "/memory");
    if (mem_offset < 0)
        return -1;

    reg = (const uint32_t *)fdt_getprop(fdt, mem_offset, "reg", &len);
    tuple_cells = addr_cells + size_cells;
    if (!reg || len < tuple_cells * 4)
        return -1;

    /* 想像 reg 的排列： reg = [ address cell 0 ][ address cell 1 ][ size cell 0 ][ size cell 1 ] */
    *base = read_be_cells(reg, addr_cells);
    *size = read_be_cells(reg + addr_cells, size_cells);

    if (*size == 0)
        return -1;
    return 0;
}

int fdt_get_reserved_memory_regions(const void *fdt,
                                    struct fdt_memory_region *regions,
                                    int max_regions)
{
    const struct fdt_header *header = (const struct fdt_header *)fdt;
    const char *struct_ptr;
    const char *struct_end;
    int reserved_offset;
    int root_addr_cells = 0;
    int root_size_cells = 0;
    int addr_cells = 0;
    int size_cells = 0;
    int depth = 0; // the depth of /reserved-memory
    int count = 0; // the number of regions

    if (!fdt || !regions || max_regions <= 0)
    {
        return -1;
    }

    /*
     * 收集 /reserved-memory 的第一層子節點中，
     * 具有固定 reg property 的保留區塊。
     */
    if (get_root_cells(fdt, &root_addr_cells, &root_size_cells) < 0)
    {
        return -1;
    }

    reserved_offset = fdt_path_offset(fdt, "/reserved-memory");
    if (reserved_offset < 0)
    {
        return 0;
    }

    if (get_node_cells(fdt,
                       reserved_offset,
                       root_addr_cells,
                       root_size_cells,
                       &addr_cells,
                       &size_cells) < 0)
    {
        return -1;
    }

    struct_end = (const char *)fdt + bswap32(header->off_dt_struct) +
                 bswap32(header->size_dt_struct);
    
    struct_ptr = (const char *)fdt + reserved_offset;
    uint32_t token = bswap32(*(const uint32_t *)struct_ptr);
    if(token != FDT_BEGIN_NODE) return -1;
    struct_ptr += 4; // token
    struct_ptr += str_len(struct_ptr) + 1; // node name
    struct_ptr = (const char *)align_up(struct_ptr, 4);
    if(!struct_ptr || struct_ptr > struct_end) return -1;

    while (struct_ptr + sizeof(uint32_t) <= struct_end)
    {
        const char *token_start = struct_ptr;
        uint32_t token = bswap32(*(const uint32_t *)struct_ptr);

        if (token == FDT_PROP)
        {
            uint32_t len;

            struct_ptr += 4; // token
            if (struct_ptr + 2 * sizeof(uint32_t) > struct_end) // len(4)+offset(4)
            {
                return -1;
            }

            len = bswap32(*(const uint32_t *)struct_ptr);
            struct_ptr += 8;
            struct_ptr += len;
            if (struct_ptr > struct_end)
            {
                return -1;
            }
            struct_ptr = (const char *)align_up(struct_ptr, 4);
            continue;
        }

        if (token == FDT_NOP)
        {
            struct_ptr += 4;
            continue;
        }

        if (token == FDT_BEGIN_NODE)
        {
            int len = 0;
            const void *reg_prop;

            depth++; // go into sub-node
            struct_ptr += 4;
            struct_ptr += str_len(struct_ptr) + 1;
            struct_ptr = (const char *)align_up(struct_ptr, 4);
            
            if (!struct_ptr)
            {
                return -1;
            }

            if (depth == 1)
            {
                reg_prop = fdt_getprop(fdt,
                                       (int)(token_start - (const char *)fdt),
                                       "reg",
                                       &len);
                count = append_reg_regions(reg_prop,
                                           len,
                                           addr_cells,
                                           size_cells,
                                           regions,
                                           max_regions,
                                           count);
            }
            continue;
        }

        if (token == FDT_END_NODE)
        {
            struct_ptr += 4;
            if (depth == 0)
            {
                break;
            }

            depth--;
            continue;
        }

        if (token == FDT_END)
        {
            break;
        }

        return -1;
    }

    return count;
}

int fdt_get_initrd_range(const void *fdt, unsigned long *start, unsigned long *end)
{
    int chosen_offset;
    int start_len;
    int end_len;

    const void *start_prop;
    const void *end_prop;

    if (!fdt || !start || !end)
        return -1;

    chosen_offset = fdt_path_offset(fdt, "/chosen");
    if (chosen_offset < 0)
        return -1;

    start_prop = fdt_getprop(fdt, chosen_offset, "linux,initrd-start", &start_len);
    end_prop = fdt_getprop(fdt, chosen_offset, "linux,initrd-end", &end_len);
    if (!start_prop || !end_prop)
        return -1;

    if (fdt_read_prop_addr(start_prop, start_len, start) < 0 ||
        fdt_read_prop_addr(end_prop, end_len, end) < 0)
    {
        uart_puts("err");
        return -1;
    }

    if (*start == 0 || *end < *start)
        return -1;

    return 0;
}
