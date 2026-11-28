#include "kernel/mm/address_space.h"
#include "config/memory.h"

#define PAGE_PRESENT 0x1u
#define PAGE_RW      0x2u
#define PAGE_USER    0x4u
/* PTE bit 9 是 x86 的软件可用位；COW 页同时清掉 RW，写入才会触发 #PF。 */
#define PAGE_COW     0x200u
#define USER_RW      (PAGE_PRESENT | PAGE_RW | PAGE_USER)
#define USER_RO      (PAGE_PRESENT | PAGE_USER)
#define PAGE_MASK    0xFFFFF000u
#define PAGE_OFFSET  0x00000FFFu
#define PT_ENTRIES   1024u

#define PHYS_PAGES ((AEVUM_PHYS_POOL_END - AEVUM_PHYS_POOL_BASE) / AEVUM_PAGE_SIZE)
#define PHYS_WORDS ((PHYS_PAGES + 31u) / 32u)

struct address_space_vma {
    u32 start;
    u32 end;
    u32 flags;
    u32 kind;
};

struct address_space {
    int used;
    int user;
    u32 id;
    u32 pd;
    u32 pt;
    u32 brk;
    u32 user_pages;
    u32 cow_pages;
    int vma_count;
    struct address_space_vma vmas[AEVUM_USER_VMA_MAX];
};

static struct address_space spaces[AEVUM_AS_MAX];
static struct address_space kernel_space = {
    1, 0, 0, AEVUM_KERNEL_PD, AEVUM_KERNEL_PT, 0, 0, 0, 0, {{0, 0, 0, 0}}
};
static struct address_space *active_space;
static u32 next_id;
static u32 phys_bitmap[PHYS_WORDS];
static u16 phys_ref[PHYS_PAGES];
static u32 phys_used_pages;
static u32 cow_break_total;
static u32 cow_fork_total;
static u32 cow_shared_total;
static u32 cow_last_asid;
static u32 cow_last_old;
static u32 cow_last_new;

static void zero_page(u32 phys)
{
    u32 *p = (u32 *)phys;
    int i;
    for (i = 0; i < 1024; i++) p[i] = 0;
}

static void copy_page(u32 dst, u32 src)
{
    u32 *d = (u32 *)dst;
    const u32 *s = (const u32 *)src;
    int i;
    for (i = 0; i < 1024; i++) d[i] = s[i];
}

static void write_cr3(u32 phys)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys) : "memory");
}

static void invlpg(u32 va)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

/* 物理页池：位图唯一分配/释放入口；地址落在 0..4MB 恒等映射内。 */
static u32 phys_alloc_page(void)
{
    u32 i;
    for (i = 0; i < PHYS_PAGES; i++) {
        u32 word = i >> 5;
        u32 bit = 1u << (i & 31u);
        if (!(phys_bitmap[word] & bit)) {
            phys_bitmap[word] |= bit;
            phys_ref[i] = 1;
            phys_used_pages++;
            return AEVUM_PHYS_POOL_BASE + i * AEVUM_PAGE_SIZE;
        }
    }
    return 0;
}

static void phys_free_page(u32 phys)
{
    u32 i;
    if (!phys || (phys & PAGE_OFFSET)) return;
    if (phys < AEVUM_PHYS_POOL_BASE || phys >= AEVUM_PHYS_POOL_END) return;
    i = (phys - AEVUM_PHYS_POOL_BASE) / AEVUM_PAGE_SIZE;
    if (phys_bitmap[i >> 5] & (1u << (i & 31u))) {
        phys_bitmap[i >> 5] &= ~(1u << (i & 31u));
        phys_ref[i] = 0;
        if (phys_used_pages) phys_used_pages--;
    }
}

/* COW 共享只加引用；最后一个引用释放时才真正归还位图。 */
static void phys_share_page(u32 phys)
{
    u32 i;
    if (!phys || (phys & PAGE_OFFSET)) return;
    if (phys < AEVUM_PHYS_POOL_BASE || phys >= AEVUM_PHYS_POOL_END) return;
    i = (phys - AEVUM_PHYS_POOL_BASE) / AEVUM_PAGE_SIZE;
    if (phys_bitmap[i >> 5] & (1u << (i & 31u))) {
        if (phys_ref[i] < 0xFFFFu) phys_ref[i]++;
    }
}

static void phys_put_page(u32 phys)
{
    u32 i;
    if (!phys || (phys & PAGE_OFFSET)) return;
    if (phys < AEVUM_PHYS_POOL_BASE || phys >= AEVUM_PHYS_POOL_END) return;
    i = (phys - AEVUM_PHYS_POOL_BASE) / AEVUM_PAGE_SIZE;
    if (!(phys_bitmap[i >> 5] & (1u << (i & 31u)))) return;
    if (phys_ref[i] > 1) {
        phys_ref[i]--;
        return;
    }
    phys_bitmap[i >> 5] &= ~(1u << (i & 31u));
    phys_ref[i] = 0;
    if (phys_used_pages) phys_used_pages--;
}

static u32 pt_index(u32 va)
{
    return (va - AEVUM_USER_VA_BASE) / AEVUM_PAGE_SIZE;
}

static u32 pt_phys(const struct address_space *space, u32 index)
{
    u32 pte;
    if (!space || !space->used || !space->user || index >= PT_ENTRIES) return 0;
    pte = ((const u32 *)space->pt)[index];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) return 0;
    return pte & PAGE_MASK;
}

static struct address_space_vma *vma_find(struct address_space *space, u32 va)
{
    int i;
    if (!space) return 0;
    for (i = 0; i < space->vma_count; i++) {
        if (va >= space->vmas[i].start && va < space->vmas[i].end)
            return &space->vmas[i];
    }
    return 0;
}

static struct address_space_vma *vma_find_heap(struct address_space *space)
{
    int i;
    if (!space) return 0;
    for (i = 0; i < space->vma_count; i++) {
        if (space->vmas[i].kind == ADDRESS_SPACE_VMA_HEAP)
            return &space->vmas[i];
    }
    return 0;
}

static void vma_add(struct address_space *space, u32 start, u32 end,
                    u32 flags, u32 kind)
{
    if (!space || space->vma_count >= AEVUM_USER_VMA_MAX) return;
    space->vmas[space->vma_count].start = start;
    space->vmas[space->vma_count].end = end;
    space->vmas[space->vma_count].flags = flags;
    space->vmas[space->vma_count].kind = kind;
    space->vma_count++;
}

static void vma_init(struct address_space *space)
{
    space->vma_count = 0;
    vma_add(space, AEVUM_USER_CODE_VA, AEVUM_USER_CODE_VA + AEVUM_PAGE_SIZE,
            ADDRESS_SPACE_VMA_READ | ADDRESS_SPACE_VMA_EXEC,
            ADDRESS_SPACE_VMA_CODE);
    vma_add(space, AEVUM_USER_DATA_VA, AEVUM_USER_DATA_VA + AEVUM_PAGE_SIZE,
            ADDRESS_SPACE_VMA_READ | ADDRESS_SPACE_VMA_WRITE,
            ADDRESS_SPACE_VMA_DATA);
    vma_add(space, AEVUM_USER_GUARD_VA, AEVUM_USER_GUARD_VA + AEVUM_PAGE_SIZE,
            0, ADDRESS_SPACE_VMA_GUARD);
    vma_add(space, AEVUM_USER_STACK_VA, AEVUM_USER_STACK_VA + AEVUM_PAGE_SIZE,
            ADDRESS_SPACE_VMA_READ | ADDRESS_SPACE_VMA_WRITE,
            ADDRESS_SPACE_VMA_STACK);
    vma_add(space, AEVUM_USER_CONTROL_VA, AEVUM_USER_CONTROL_VA + AEVUM_PAGE_SIZE,
            ADDRESS_SPACE_VMA_READ, ADDRESS_SPACE_VMA_CONTROL);
    vma_add(space, AEVUM_USER_HEAP_VA, space->brk,
            ADDRESS_SPACE_VMA_READ | ADDRESS_SPACE_VMA_WRITE,
            ADDRESS_SPACE_VMA_HEAP);
}

/* PTE 唯一写入口：只在这里写页表，并负责 TLB 刷新。 */
static void map_pte(struct address_space *space, u32 va, u32 phys, u32 flags)
{
    ((u32 *)space->pt)[pt_index(va)] = (phys & PAGE_MASK) | flags;
    invlpg(va);
}

static void unmap_pte(struct address_space *space, u32 va)
{
    ((u32 *)space->pt)[pt_index(va)] = 0;
    invlpg(va);
}

static int map_zero_page(struct address_space *space, u32 va, u32 flags)
{
    u32 phys = phys_alloc_page();
    if (!phys) return 0;
    zero_page(phys);
    map_pte(space, va, phys, flags);
    space->user_pages++;
    return 1;
}

/*
 * COW 唯一拆分点：复制共享物理页、按 VMA 恢复写权限、释放旧引用。
 * control 页对用户只读，但内核 set_stop 需要写，因此拆分后仍返回 RO 映射。
 */
static int cow_break_page(struct address_space *space, u32 va)
{
    u32 page_va = va & PAGE_MASK;
    u32 index = pt_index(page_va);
    u32 pte = ((u32 *)space->pt)[index];
    u32 old_phys;
    u32 new_phys;
    u32 flags;
    struct address_space_vma *vma;

    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_COW)) return 1;
    old_phys = pte & PAGE_MASK;
    vma = vma_find(space, page_va);
    if (!vma || !(vma->flags & ADDRESS_SPACE_VMA_READ)) return 0;

    new_phys = phys_alloc_page();
    if (!new_phys) return 0;
    copy_page(new_phys, old_phys);

    flags = PAGE_PRESENT | PAGE_USER;
    if (vma->flags & ADDRESS_SPACE_VMA_WRITE) flags |= PAGE_RW;
    map_pte(space, page_va, new_phys, flags);
    phys_put_page(old_phys);
    if (space->cow_pages) space->cow_pages--;

    cow_break_total++;
    cow_last_asid = space->id;
    cow_last_old = old_phys;
    cow_last_new = new_phys;
    return 1;
}

static void free_space_pages(u32 *pages, int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (pages[i]) phys_free_page(pages[i]);
}

void address_space_boot(void)
{
    u32 i;
    active_space = &kernel_space;
    next_id = 0;
    phys_used_pages = 0;
    cow_break_total = 0;
    cow_fork_total = 0;
    cow_shared_total = 0;
    cow_last_asid = 0;
    cow_last_old = 0;
    cow_last_new = 0;
    for (i = 0; i < PHYS_WORDS; i++) phys_bitmap[i] = 0;
    for (i = 0; i < PHYS_PAGES; i++) phys_ref[i] = 0;
}

struct address_space *address_space_kernel(void)
{
    return &kernel_space;
}

struct address_space *address_space_create_user(const u8 *code, u32 code_size)
{
    struct address_space *space = 0;
    u32 pages[6];
    u32 *pd;
    int i;

    if (!code || !code_size || code_size > AEVUM_PAGE_SIZE) return 0;
    for (i = 0; i < AEVUM_AS_MAX; i++) {
        if (!spaces[i].used) {
            space = &spaces[i];
            break;
        }
    }
    if (!space) return 0;

    for (i = 0; i < 6; i++) pages[i] = 0;
    for (i = 0; i < 6; i++) {
        pages[i] = phys_alloc_page();
        if (!pages[i]) {
            free_space_pages(pages, 6);
            return 0;
        }
    }

    space->used = 1;
    space->user = 1;
    space->id = ++next_id;
    space->pd = pages[0];
    space->pt = pages[1];
    space->brk = AEVUM_USER_HEAP_VA;
    space->user_pages = 4;
    space->cow_pages = 0;
    space->vma_count = 0;

    zero_page(space->pd);
    zero_page(space->pt);
    zero_page(pages[2]);
    zero_page(pages[3]);
    zero_page(pages[4]);
    zero_page(pages[5]);
    copy_page(pages[2], (u32)code);

    pd = (u32 *)space->pd;
    pd[0] = AEVUM_KERNEL_PT | PAGE_PRESENT | PAGE_RW;
    pd[1] = space->pt | USER_RW;
    /* P13.2：共享 0xFEC00000..0xFF000000 的 MMIO 页表，内核态可访问。 */
    pd[AEVUM_KERNEL_MMIO_PDE] = AEVUM_KERNEL_MMIO_PT | PAGE_PRESENT | PAGE_RW;

    map_pte(space, AEVUM_USER_CODE_VA, pages[2], USER_RO);
    map_pte(space, AEVUM_USER_DATA_VA, pages[3], USER_RW);
    map_pte(space, AEVUM_USER_STACK_VA, pages[4], USER_RW);
    map_pte(space, AEVUM_USER_CONTROL_VA, pages[5], USER_RO);
    vma_init(space);
    return space;
}

void address_space_release(struct address_space *space)
{
    int i;
    if (!space || space == &kernel_space || !space->used) return;

    if (space->pt) {
        for (i = 0; i < (int)PT_ENTRIES; i++) {
            u32 pte = ((u32 *)space->pt)[i];
            if ((pte & PAGE_PRESENT) && (pte & PAGE_USER))
                phys_put_page(pte & PAGE_MASK);
            ((u32 *)space->pt)[i] = 0;
        }
    }
    phys_put_page(space->pt);
    phys_put_page(space->pd);

    space->used = 0;
    space->user = 0;
    space->id = 0;
    space->pd = 0;
    space->pt = 0;
    space->brk = 0;
    space->user_pages = 0;
    space->cow_pages = 0;
    space->vma_count = 0;
}

/*
 * fork 的地址空间：新 PD/PT，所有可写用户页（data/stack/heap，以及内核可写的
 * control 页）改成 COW 共享；code 只读页直接共享。P2.2 不再复制任何用户页。
 */
struct address_space *address_space_clone(const struct address_space *src)
{
    struct address_space *space = 0;
    struct address_space *parent = (struct address_space *)src;
    struct address_space_vma *heap;
    u32 *pd;
    int i;

    if (!src || !src->used || !src->user) return 0;
    for (i = 0; i < AEVUM_AS_MAX; i++) {
        if (!spaces[i].used) {
            space = &spaces[i];
            break;
        }
    }
    if (!space) return 0;

    space->used = 1;
    space->user = 1;
    space->id = ++next_id;
    space->pd = phys_alloc_page();
    space->pt = phys_alloc_page();
    space->brk = src->brk;
    space->user_pages = 0;
    space->cow_pages = 0;
    space->vma_count = 0;
    if (!space->pd || !space->pt) {
        address_space_release(space);
        return 0;
    }

    zero_page(space->pd);
    zero_page(space->pt);
    pd = (u32 *)space->pd;
    pd[0] = AEVUM_KERNEL_PT | PAGE_PRESENT | PAGE_RW;
    pd[1] = space->pt | USER_RW;
    /* P13.2：共享 0xFEC00000..0xFF000000 的 MMIO 页表，内核态可访问。 */
    pd[AEVUM_KERNEL_MMIO_PDE] = AEVUM_KERNEL_MMIO_PT | PAGE_PRESENT | PAGE_RW;

    for (i = 0; i < (int)PT_ENTRIES; i++) {
        u32 pte = ((const u32 *)src->pt)[i];
        u32 phys;
        u32 va;
        u32 flags;
        u32 vma_kind = 0;
        int share_writable;
        struct address_space_vma *vma;

        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) continue;
        phys = pte & PAGE_MASK;
        va = AEVUM_USER_VA_BASE + (u32)i * AEVUM_PAGE_SIZE;
        vma = vma_find(parent, va);
        if (vma) vma_kind = vma->kind;
        /* control 对用户只读，但内核会写停止字，也必须走 COW。 */
        /* 已经是 COW 的页第二次 fork 时必须保留 COW 位，不能退化成只读页。 */
        share_writable = (pte & (PAGE_RW | PAGE_COW)) != 0 ||
                         vma_kind == ADDRESS_SPACE_VMA_CONTROL;

        flags = pte & (PAGE_PRESENT | PAGE_USER | PAGE_COW);
        if (share_writable) flags |= PAGE_COW;

        phys_share_page(phys);
        if (share_writable) {
            if (!(pte & PAGE_COW)) parent->cow_pages++;
            space->cow_pages++;
            map_pte(parent, va, phys, flags);
        }
        map_pte(space, va, phys, flags);
        space->user_pages++;
    }

    for (i = 0; i < src->vma_count && i < AEVUM_USER_VMA_MAX; i++)
        space->vmas[i] = src->vmas[i];
    space->vma_count = src->vma_count;
    /* 收尾按 kind 定位 HEAP VMA 并把 end 对齐到本进程 brk，
     * 不依赖“HEAP 恰好排在 vmas[] 最后”这一追加式布局约定。 */
    heap = vma_find_heap(space);
    if (heap) heap->end = space->brk;
    cow_fork_total++;
    cow_shared_total += space->user_pages;
    return space;
}

u32 address_space_brk(const struct address_space *space)
{
    return space ? space->brk : 0;
}

int address_space_set_brk(struct address_space *space, u32 new_brk)
{
    struct address_space_vma *heap;
    u32 old;
    u32 va;

    if (!space || !space->used || !space->user) return -1;
    if (new_brk > AEVUM_USER_HEAP_MAX) return -1;
    new_brk = (new_brk + AEVUM_PAGE_SIZE - 1u) & ~(AEVUM_PAGE_SIZE - 1u);
    if (new_brk < AEVUM_USER_HEAP_VA) return -1;

    old = space->brk;
    if (new_brk > old) {
        for (va = old; va < new_brk; va += AEVUM_PAGE_SIZE) {
            if (!map_zero_page(space, va, USER_RW)) {
                /* 回滚本次已建立的页，brk 保持不变。 */
                u32 rb;
                for (rb = old; rb < va; rb += AEVUM_PAGE_SIZE) {
                    u32 pte = ((u32 *)space->pt)[pt_index(rb)];
                    if ((pte & PAGE_PRESENT) && (pte & PAGE_USER)) {
                        if ((pte & PAGE_COW) && space->cow_pages) space->cow_pages--;
                        phys_put_page(pte & PAGE_MASK);
                        unmap_pte(space, rb);
                        if (space->user_pages) space->user_pages--;
                    }
                }
                return -1;
            }
        }
    } else if (new_brk < old) {
        for (va = new_brk; va < old; va += AEVUM_PAGE_SIZE) {
            u32 pte = ((u32 *)space->pt)[pt_index(va)];
            if ((pte & PAGE_PRESENT) && (pte & PAGE_USER)) {
                if ((pte & PAGE_COW) && space->cow_pages) space->cow_pages--;
                phys_put_page(pte & PAGE_MASK);
                unmap_pte(space, va);
                if (space->user_pages) space->user_pages--;
            }
        }
    }
    space->brk = new_brk;
    heap = vma_find_heap(space);
    if (heap) heap->end = new_brk;
    return 0;
}

int address_space_vma_count(const struct address_space *space)
{
    return (space && space->used && space->user) ? space->vma_count : 0;
}

int address_space_vma_snapshot(const struct address_space *space, int slot,
                               struct address_space_vma_info *out)
{
    if (!space || !space->used || !space->user || !out ||
        slot < 0 || slot >= space->vma_count)
        return 0;
    out->start = space->vmas[slot].start;
    out->end = space->vmas[slot].end;
    out->flags = space->vmas[slot].flags;
    out->kind = space->vmas[slot].kind;
    return 1;
}

u32 address_space_user_pages(const struct address_space *space)
{
    return (space && space->used && space->user) ? space->user_pages : 0;
}

u32 address_space_phys_total(void)
{
    return PHYS_PAGES;
}

u32 address_space_phys_used(void)
{
    return phys_used_pages;
}

int address_space_slot_used(void)
{
    int i, n = 0;

    for (i = 0; i < AEVUM_AS_MAX; i++)
        if (spaces[i].used && spaces[i].user) n++;
    return n;
}

int address_space_slot_capacity(void)
{
    return AEVUM_AS_MAX;
}

/*
 * 只把两类用户缺页修好：
 * 1) heap VMA 内、brk 以下尚未 present 的页，零填；
 * 2) present + write 的 COW 页，复制后按 VMA 恢复权限。
 * guard、RO 页和非写保护引起的 present 错误永不在这里修复。
 */
int address_space_handle_user_fault(struct address_space *space, u32 va, u32 err)
{
    struct address_space_vma *vma;
    u32 page_va;
    u32 index;
    u32 pte;

    if (!space || !space->used || !space->user) return 0;
    if (va < AEVUM_USER_VA_BASE || va >= AEVUM_USER_HEAP_MAX) return 0;
    page_va = va & PAGE_MASK;
    vma = vma_find(space, page_va);
    if (!vma) return 0;

    if (err & PAGE_PRESENT) {
        if (!(err & PAGE_RW) || !(vma->flags & ADDRESS_SPACE_VMA_WRITE))
            return 0;
        index = pt_index(page_va);
        pte = ((u32 *)space->pt)[index];
        if (!(pte & PAGE_COW)) return 0;
        return cow_break_page(space, page_va);
    }

    if (vma->kind != ADDRESS_SPACE_VMA_HEAP) return 0;
    if (page_va >= space->brk) return 0;
    index = pt_index(page_va);
    pte = ((u32 *)space->pt)[index];
    if (pte & PAGE_PRESENT) return 0;
    return map_zero_page(space, page_va, USER_RW);
}

int address_space_user_range_ok(const struct address_space *space, u32 va,
                                u32 len, int write)
{
    u32 last, first, i;

    if (!space || !space->used || !space->user || !len) return 0;
    if (va < AEVUM_USER_VA_BASE) return 0;
    if (va > 0xFFFFFFFFu - (len - 1u)) return 0;
    last = va + len - 1u;
    if (last >= AEVUM_USER_HEAP_MAX) return 0;
    first = pt_index(va);
    last = pt_index(last);
    for (i = first; i <= last; i++) {
        u32 page_va = AEVUM_USER_VA_BASE + i * AEVUM_PAGE_SIZE;
        struct address_space_vma *vma = vma_find((struct address_space *)space, page_va);
        u32 pte = ((const u32 *)space->pt)[i];
        if (!vma || !(vma->flags & ADDRESS_SPACE_VMA_READ)) return 0;
        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) return 0;
        if (write && !(vma->flags & ADDRESS_SPACE_VMA_WRITE)) return 0;
        if (write && !(pte & PAGE_RW) && !(pte & PAGE_COW)) return 0;
    }
    return 1;
}

static int ensure_heap_page(struct address_space *space, u32 va)
{
    u32 index;
    u32 pte;
    struct address_space_vma *vma;

    index = pt_index(va);
    pte = ((u32 *)space->pt)[index];
    if (pte & PAGE_PRESENT) return 1;
    vma = vma_find(space, va & PAGE_MASK);
    if (!vma || vma->kind != ADDRESS_SPACE_VMA_HEAP) return 0;
    if ((va & PAGE_MASK) >= space->brk) return 0;
    return map_zero_page(space, va & PAGE_MASK, USER_RW);
}

static int user_translate(struct address_space *space, u32 va, int write,
                          u32 *phys_out)
{
    u32 index, pte, phys;
    struct address_space_vma *vma;

    if (!space || !space->used || !space->user || !phys_out) return 0;
    if (va < AEVUM_USER_VA_BASE || va >= AEVUM_USER_HEAP_MAX) return 0;
    vma = vma_find(space, va & PAGE_MASK);
    if (!vma) return 0;
    index = pt_index(va);
    pte = ((u32 *)space->pt)[index];
    if (!(pte & PAGE_PRESENT)) {
        if (!ensure_heap_page(space, va)) return 0;
        pte = ((u32 *)space->pt)[index];
    }
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) return 0;
    if (!(vma->flags & ADDRESS_SPACE_VMA_READ)) return 0;
    if (write && !(vma->flags & ADDRESS_SPACE_VMA_WRITE)) return 0;
    if (write && (pte & PAGE_COW)) {
        if (!cow_break_page(space, va & PAGE_MASK)) return 0;
        pte = ((u32 *)space->pt)[index];
    }
    if (write && !(pte & PAGE_RW)) return 0;
    phys = (pte & PAGE_MASK) + (va & PAGE_OFFSET);
    *phys_out = phys;
    return 1;
}

int address_space_copyin(const struct address_space *space, void *dst,
                         u32 user_va, u32 len)
{
    u8 *out = (u8 *)dst;
    u32 i;
    if (!dst || !len) return -1;
    for (i = 0; i < len; i++) {
        u32 phys;
        if (!user_translate((struct address_space *)space, user_va + i, 0, &phys))
            return -1;
        out[i] = *(volatile u8 *)phys;
    }
    return 0;
}

int address_space_copyout(struct address_space *space, u32 user_va,
                          const void *src, u32 len)
{
    const u8 *in = (const u8 *)src;
    u32 i;
    if (!src || !len) return -1;
    for (i = 0; i < len; i++) {
        u32 phys;
        if (!user_translate(space, user_va + i, 1, &phys)) return -1;
        *(volatile u8 *)phys = in[i];
    }
    return 0;
}

u32 address_space_user_read32(const struct address_space *space, u32 va)
{
    u32 value = 0;
    if (address_space_copyin(space, &value, va, 4) != 0) return 0;
    return value;
}

int address_space_user_write32(struct address_space *space, u32 va, u32 value)
{
    return address_space_copyout(space, va, &value, 4) == 0;
}

void address_space_switch(struct address_space *space)
{
    if (!space || space == active_space) return;
    active_space = space;
    write_cr3(space->pd);
}

u32 address_space_cr3(const struct address_space *space)
{
    return space ? space->pd : 0;
}

u32 address_space_id(const struct address_space *space)
{
    return space ? space->id : 0;
}

u32 address_space_code_phys(const struct address_space *space)
{
    return pt_phys(space, pt_index(AEVUM_USER_CODE_VA));
}

u32 address_space_data_phys(const struct address_space *space)
{
    return pt_phys(space, pt_index(AEVUM_USER_DATA_VA));
}

u32 address_space_counter(const struct address_space *space)
{
    u32 phys = address_space_data_phys(space);
    return phys ? *(volatile u32 *)phys : 0;
}

u32 address_space_cow_pages(const struct address_space *space)
{
    return (space && space->used && space->user) ? space->cow_pages : 0;
}

void address_space_cow_snapshot(struct address_space_cow_info *out)
{
    int i;
    if (!out) return;
    out->live = 0;
    for (i = 0; i < AEVUM_AS_MAX; i++) {
        if (spaces[i].used && spaces[i].user)
            out->live += spaces[i].cow_pages;
    }
    out->breaks = cow_break_total;
    out->forks = cow_fork_total;
    out->shared = cow_shared_total;
    out->last_asid = cow_last_asid;
    out->last_old = cow_last_old;
    out->last_new = cow_last_new;
}

void address_space_set_stop(struct address_space *space, u32 stop)
{
    u32 phys;
    if (!space || !space->user) return;
    /* control 对用户只读；内核写停止字前必须先拆掉 COW 共享。 */
    if (!cow_break_page(space, AEVUM_USER_CONTROL_VA)) return;
    phys = pt_phys(space, pt_index(AEVUM_USER_CONTROL_VA));
    if (phys) *(volatile u32 *)phys = stop;
}

void address_space_set_mode(struct address_space *space, u32 mode)
{
    if (!space || !space->user) return;
    (void)address_space_user_write32(space, AEVUM_USER_DATA_VA + 4u, mode);
}

int address_space_user_active(const struct address_space *space)
{
    return space && space->user && space->used;
}
