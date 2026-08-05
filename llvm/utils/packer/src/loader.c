/*
 * ---------------------------------------------------------------------------
 * NOTE: This file was created and/or refined with the assistance of AI tools
 *       (e.g., ChatGPT). The author has reviewed and validated the content,
 *       and remains fully responsible for its accuracy and completeness.
 * ---------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stddef.h>

#define STACK_SIZE   (128 * 1024)    // 128 KiB

static uint8_t *heap;
static size_t hp;

void* malloc(size_t n) {
    if (hp + n > (1 << 20)) return 0;  // Set Maximum 1 MiB
    void* p = &heap[hp];
    hp += n;
    return p;
}
void free(void* p) { (void)p; }
void* realloc(void* p, size_t n) { (void)p; return malloc(n); }

void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* D = d;
    const uint8_t* S = s;
    while (n--) *D++ = *S++;
    return d;
}

void* memset(void* d, int c, size_t n) {
    uint8_t* D = d;
    while (n--) *D++ = (uint8_t)c;
    return d;
}
// --- 放在前面：一个“原子跳转”函数 ---
__attribute__((noreturn, noinline, optimize("O0")))
static void jump_with_a0_minus1(uint32_t entry, uint32_t stack_top)
{
    // 把 entry 预先固定到 a1，确保 jalr 用 a1 而不是 a0
    register uint32_t target __asm__("a1") = entry;

    __asm__ volatile(
        "mv sp, %0\n"       // ① 切到我们映射的干净栈
        "li a0, -1\n"       // ② a0 = -1（告诉 payload：不要切栈）
        "jalr x0, a1, 0\n"  // ③ 直接跳到 entry（不返回）
        :
        : "r"(stack_top), "r"(target)
        : "memory", "a0"    // 声明 clobber a0；a1 作为输入已绑定
    );
    __builtin_unreachable();
}

static void* sys_mmap(void* addr, unsigned len, unsigned prot, unsigned flags, int fd, unsigned off) {
    register long a0 __asm__("a0") = (long)addr,
                  a1 __asm__("a1") = len,
                  a2 __asm__("a2") = prot,
                  a3 __asm__("a3") = flags,
                  a4 __asm__("a4") = fd,
                  a5 __asm__("a5") = off,
                  n  __asm__("a7") = 222;
    __asm__ volatile("ecall" : "+r"(a0)
                             : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(n)
                             : "memory");
    return (void*)a0;
}

static long sys_mprotect(void* addr, unsigned len, unsigned prot) {
    register long a0 __asm__("a0") = (long)addr,
                  a1 __asm__("a1") = len,
                  a2 __asm__("a2") = prot,
                  n  __asm__("a7") = 226;
    __asm__ volatile("ecall" : "+r"(a0)
                             : "r"(a1), "r"(a2), "r"(n)
                             : "memory");
    return a0;
}

#define TINFL_DECOMPRESS_MEM_TO_MEM
#define TINFL_IMPLEMENTATION
#include "tinfl.c"

#define MAGIC 0x52565058u
struct SegHdr { uint32_t v, f, m, flg, aln, off; };
static int prot(uint32_t f) {
    return ((f & 4) ? 1 : 0)
         | ((f & 2) ? 2 : 0)
         | ((f & 1) ? 4 : 0);
}

extern unsigned char _binary_hdr_bin_start[];
extern unsigned int  _binary_hdr_bin_len;
extern unsigned char _binary_payload_bin_start[];
extern unsigned int  _binary_payload_bin_len;

void _start(void) {
    heap = sys_mmap(NULL, 1 << 20,
                    /*prot=*/ 1|2,        // PROT_READ|PROT_WRITE
                    /*flags=*/ 0x22,      // MAP_PRIVATE|MAP_ANONYMOUS
                    -1, 0);
    hp = 0;

    uint8_t* hdr = _binary_hdr_bin_start;
    if (*(uint32_t*)hdr != MAGIC) while (1);
    hdr += 4;
    uint32_t entry = *(uint32_t*)hdr; hdr += 4;
    uint32_t phnum = *(uint32_t*)hdr; hdr += 4;

    struct SegHdr* ph = (struct SegHdr*)hdr;
    uint32_t max_end = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t e = ph[i].v + ph[i].m;
        if (e > max_end) max_end = e;
    }

    // 解压到 heap+0x1000
    uint8_t* tmp = heap + 0x1000;
    size_t decompressed_size = tinfl_decompress_mem_to_mem(
        tmp, 64 * 1024 * 1024,
        _binary_payload_bin_start, _binary_payload_bin_len,
        TINFL_FLAG_PARSE_ZLIB_HEADER
    );
    if (decompressed_size == 0) {
        while (1);
    }

    // 按原始 PT_LOAD 映射各段
    for (uint32_t i = 0; i < phnum; i++) {
        struct SegHdr s = ph[i];
        uint32_t pg = 0x1000;
        uint32_t mapS = s.v & ~(pg - 1);
        uint32_t mapL = ((s.v + s.m + pg - 1) & ~(pg - 1)) - mapS;
        int current_prot = prot(s.flg) | 0x2; // add PROT_WRITE
        sys_mmap((void*)mapS, mapL,
                current_prot,
                0x20 | 0x02 | 0x10,  // MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS
                -1, 0);
        memcpy((void*)s.v, tmp + s.off, s.f);
        memset((void*)(s.v + s.f), 0, s.m - s.f);
        if (!(s.flg & 2))
            sys_mprotect((void*)mapS, mapL, prot(s.flg));
    }

    // 2) stack：让内核自己选一块合法可写区
    void* stack_base = sys_mmap(NULL, STACK_SIZE,
                                /*prot=*/ 1|2,      // PROT_READ|PROT_WRITE
                                /*flags=*/ 0x22,    // MAP_PRIVATE|MAP_ANONYMOUS
                                -1, 0);
    if (stack_base == (void*)-1) {
        while (1);
    }
    uint32_t stack_top = (((uint32_t)stack_base) + STACK_SIZE) & ~0xF;

    // （可选但非常建议）在把代码段 memcpy 完、mprotect 之前做 I-cache 刷新：
    // 见你文件里注释掉的 sys_icache_flush，实现放开即可，再 mprotect 成 RX。
    
    // 原子地：切栈 + a0=-1 + 跳转
    jump_with_a0_minus1(entry, stack_top);
}
