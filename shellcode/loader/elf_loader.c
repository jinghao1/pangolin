#include "elf_loader.h"
#include <z_std.h>
#include <z_log.h>
#include <z_syscall.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <elf.h>
#include <limits.h>

#define STACK_ALIGN     16

#define PROGRAM         0
#define INTERPRETER     1

#define AV_PATH         "/proc/self/auxv"

#ifndef PAGE_SIZE
#define PAGE_SIZE       0x1000
#endif

#define ROUND_PG(x)     (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x)     ((x) & ~(PAGE_SIZE - 1))

#if __i386__ || __arm__

#define BUFFER_SIZE     512

#define STAT            stat64
#define Z_STAT          z_stat64

#define Elf_Ehdr        Elf32_Ehdr
#define Elf_Phdr        Elf32_Phdr
#define Elf_auxv_t      Elf32_auxv_t
#define ELF_CLASS       ELFCLASS32

#elif __x86_64__ || __aarch64__

#define BUFFER_SIZE     832

#define STAT            stat
#define Z_STAT          z_stat

#define Elf_Ehdr        Elf64_Ehdr
#define Elf_Phdr        Elf64_Phdr
#define Elf_auxv_t      Elf64_auxv_t
#define ELF_CLASS       ELFCLASS64

#endif

int load_segments(char *buffer, int fd, elf_image_t *image) {
    Elf_Ehdr *ehdr = (Elf_Ehdr *)buffer;
    Elf_Phdr *phdr = (Elf_Phdr *)(buffer + ehdr->e_phoff);

    unsigned long minVA = -1;
    unsigned long maxVA = 0;

    for (Elf_Phdr *i = phdr; i < &phdr[ehdr->e_phnum]; i++) {
        if (i->p_type != PT_LOAD)
            continue;

        if (i->p_vaddr < minVA)
            minVA = i->p_vaddr;

        if (i->p_vaddr + i->p_memsz > maxVA)
            maxVA = i->p_vaddr + i->p_memsz;
    }

    minVA = TRUNC_PG(minVA);
    maxVA = ROUND_PG(maxVA);

    int dyn = ehdr->e_type == ET_DYN;

    void *base = Z_RESULT_V(z_mmap(
            dyn ? NULL : (void *)minVA,
            maxVA - minVA,
            PROT_NONE,
            (dyn ? 0 : MAP_FIXED) | MAP_PRIVATE | MAP_DENYWRITE,
            fd,
            0));

    if (base == MAP_FAILED) {
        LOG("mmap failed");
        return -1;
    }

    LOG("segment base: 0x%lx[0x%lx]", base, maxVA - minVA);

    for (Elf_Phdr *i = phdr; i < &phdr[ehdr->e_phnum]; i++) {
        if (i->p_type != PT_LOAD)
            continue;

        if ((i->p_align & (PAGE_SIZE - 1)) != 0) {
            z_munmap(base, maxVA - minVA);
            return -1;
        }

        if (((i->p_vaddr - i->p_offset) & (i->p_align - 1)) != 0) {
            z_munmap(base, maxVA - minVA);
            return -1;
        }

        unsigned long offset = i->p_vaddr & (PAGE_SIZE - 1);
        unsigned long start = (unsigned long)base + TRUNC_PG(i->p_vaddr) - minVA;
        unsigned long size = ROUND_PG(i->p_memsz + offset);
        unsigned long filesize = ROUND_PG(i->p_filesz + offset);

        LOG("segment: 0x%lx[0x%lx]", start, size);

        unsigned int flags = i->p_flags;
        int protection = (flags & PF_R ? PROT_READ : 0) | (flags & PF_W ? PROT_WRITE : 0) | (flags & PF_X ? PROT_EXEC : 0);

        if (Z_RESULT_V(z_mmap(
                (void *)start,
                size,
                protection,
                MAP_FIXED | MAP_PRIVATE | MAP_DENYWRITE,
                fd,
                i->p_offset - offset)) == MAP_FAILED) {
            z_munmap(base, maxVA - minVA);
            return -1;
        }

        if (i->p_memsz == i->p_filesz)
            continue;

        z_memset((char *)start + offset + i->p_filesz, 0, filesize - i->p_filesz - offset);

        if (size == filesize)
            continue;

        if (Z_RESULT_V(z_mmap(
                (char *)start + filesize,
                size - filesize,
                protection,
                MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0)) == MAP_FAILED) {
            z_munmap(base, maxVA - minVA);
            return -1;
        }
    }

    image->base = (unsigned long)base;
    image->minVA = minVA;
    image->maxVA = maxVA;

    return 0;
}

int elf_check(Elf_Ehdr *ehdr) {
    if (
            ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
            ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
            ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
            ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        LOG("elf magic error");
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELF_CLASS || ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        LOG("elf class error");
        return -1;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        LOG("elf type error");
        return -1;
    }

    return 0;
}

int elf_map(const char *path, elf_context_t *ctx) {
    struct STAT sb;
    z_memset(&sb, 0, sizeof(sb));

    if (Z_RESULT_V(Z_STAT(path, &sb)) < 0) {
        LOG("stat file failed: %s", path);
        return -1;
    }

    if ((sb.st_mode & S_IFREG) == 0) {
        LOG("file type invalid: %s", path);
        return -1;
    }

    int fd = Z_RESULT_V(z_open(path, O_RDONLY, 0));

    if (fd < 0) {
        LOG("open file failed: %s", path);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    z_memset(buffer, 0, sizeof(buffer));

    if (Z_RESULT_V(z_read(fd, buffer, BUFFER_SIZE)) < 0) {
        LOG("read file failed: %s", path);
        z_close(fd);
        return -1;
    }

    if (elf_check((Elf_Ehdr *)buffer) < 0) {
        z_close(fd);
        return -1;
    }

    Elf_Ehdr *ehdr = (Elf_Ehdr *)buffer;
    Elf_Phdr *phdr = (Elf_Phdr *)(buffer + ehdr->e_phoff);

    for (Elf_Phdr *i = phdr; i < &phdr[ehdr->e_phnum]; i++) {
        if (i->p_type == PT_INTERP) {
            char interpreter[PATH_MAX];
            z_memset(interpreter, 0, sizeof(interpreter));

            if (Z_RESULT_V(z_lseek(fd, i->p_offset, SEEK_SET)) < 0) {
                z_close(fd);
                return -1;
            }

            if (Z_RESULT_V(z_read(fd, interpreter, i->p_filesz)) != i->p_filesz) {
                z_close(fd);
                return -1;
            }

            if (elf_map(interpreter, ctx + 1) < 0) {
                z_close(fd);
                return -1;
            }

            break;
        }
    }

    LOG("mapping %s", path);

    elf_image_t image;
    z_memset(&image, 0, sizeof(image));

    if (load_segments(buffer, fd, &image)) {
        z_close(fd);
        return -1;
    }

    ctx->base = image.base;
    ctx->entry = image.base + ehdr->e_entry - image.minVA;
    ctx->header = image.base + ehdr->e_phoff;
    ctx->header_num = ehdr->e_phnum;
    ctx->header_size = ehdr->e_phentsize;

    z_close(fd);

    return 0;
}

int elf_loader(loader_payload_t *payload) {
    int argc = 0;

    char *argv[PAYLOAD_MAX_ARG];
    char *env[PAYLOAD_MAX_ENV];

    z_memset(argv, 0, sizeof(argv));
    z_memset(env, 0, sizeof(env));

    if (!z_strlen(payload->argv)) {
        LOG("empty argv");
        return -1;
    }

    argv[argc++] = payload->argv;

    for (char *i = payload->argv; *i && argc < PAYLOAD_MAX_ARG; i++) {
        if (*i == *PAYLOAD_DELIMITER) {
            *i = 0;
            argv[argc++] = i + 1;
        }
    }

    int count = 0;
    char e_quit[64];

    snprintf(e_quit, sizeof(e_quit), "QUIT=%p", quit_p());
    env[count++] = e_quit;

    if (z_strlen(payload->env)) {
        env[count++] = payload->env;

        for (char *i = payload->env; *i && count < PAYLOAD_MAX_ENV; i++) {
            if (*i == *PAYLOAD_DELIMITER) {
                *i = 0;
                env[count++] = i + 1;
            }
        }
    }

    for (int i = 0; i < argc; i++)
        LOG("arg[%d] %s", i, argv[i]);

    for (char **e = env; *e != NULL; e++)
        LOG("env %s", *e);

    const char *path = argv[0];

    elf_context_t context[2];
    z_memset(context, 0, sizeof(context));

    if (elf_map(path, context) < 0) {
        LOG("elf mapping failed: %s", path);
        return -1;
    }

    int fd = Z_RESULT_V(z_open(AV_PATH, O_RDONLY, 0));

    if (fd < 0) {
        LOG("open failed: %s", AV_PATH);
        return -1;
    }

    char av[1024];
    z_memset(av, 0, sizeof(av));

    ssize_t length = Z_RESULT_V(z_read(fd, av, sizeof(av)));

    if (length == -1) {
        z_close(fd);
        return -1;
    }

    z_close(fd);

    for (Elf_auxv_t *i = (Elf_auxv_t *)av; i->a_type != AT_NULL; i++) {
        switch (i->a_type) {
            case AT_PHDR:
                i->a_un.a_val = context[PROGRAM].header;
                break;

            case AT_PHENT:
                i->a_un.a_val = context[PROGRAM].header_size;
                break;

            case AT_PHNUM:
                i->a_un.a_val = context[PROGRAM].header_num;
                break;

            case AT_BASE:
                i->a_un.a_val = context[INTERPRETER].base ? context[INTERPRETER].base : 0;
                break;

            case AT_ENTRY:
                i->a_un.a_val = context[PROGRAM].entry;
                break;

            case AT_EXECFN:
                i->a_un.a_val = (unsigned long)path;
                break;
        }
    }

    unsigned char buffer[4096];
    z_memset(buffer, 0, sizeof(buffer));

    unsigned long entry = context[INTERPRETER].entry ? context[INTERPRETER].entry : context[PROGRAM].entry;

    unsigned char *stack = (unsigned char *)(((unsigned long)buffer + STACK_ALIGN - 1) & ~(STACK_ALIGN - 1));
    unsigned long *p = (unsigned long *)stack;

    *(int *)p++ = argc;

    for (int i = 0; i < argc; i++)
        *(char **)p++ = argv[i];

    *(char **)p++ = NULL;

    for (char ** i = env; *i; i++)
        *(char **)p++ = *i;

    *(char **)p++ = NULL;

    z_memcpy(p, av, length);

#ifdef __i386__
    asm volatile("mov %0, %%esp; xor %%edx, %%edx; jmp *%1;" :: "r"(stack), "a"(entry));
#elif __x86_64__
    asm volatile("mov %0, %%rsp; xor %%rdx, %%rdx; jmp *%1;" :: "r"(stack), "a"(entry));
#elif __arm__
    asm volatile("mov %%sp, %0; mov %%r0, #0; bx %[func];" :: "r"(stack), [func] "r"(entry));
#elif __aarch64__
    asm volatile("mov sp, %[stack]; mov w0, #0; br %[func];" :: [stack] "r"(stack), [func] "r"(entry));
#endif

    return 0;
}