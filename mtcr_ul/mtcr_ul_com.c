/*
 *
 * Copyright (c) 2013 Mellanox Technologies Ltd.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *  mtcr_ul.c - Mellanox Hardware Access implementation
 *
 */

//use memory mapped /dev/mem for access
#define CONFIG_ENABLE_MMAP 1
//mmap /dev/mem for memory access (does not work on sparc)
#define CONFIG_USE_DEV_MEM 1
//use pci configuration cycles for access
#define CONFIG_ENABLE_PCICONF 1

#ifndef _XOPEN_SOURCE
#if CONFIG_ENABLE_PCICONF && CONFIG_ENABLE_MMAP
/* For strerror_r */
#define _XOPEN_SOURCE 600
#elif CONFIG_ENABLE_PCICONF
#define _XOPEN_SOURCE 500
#endif
#endif

#if CONFIG_ENABLE_MMAP
#define _FILE_OFFSET_BITS 64
#endif

#define MTCR_MAP_SIZE 0x100000

#include <stdio.h>
#include <dirent.h>
#include <string.h>

#include <unistd.h>

#include <netinet/in.h>
#include <endian.h>
#include <byteswap.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/file.h>

#if CONFIG_ENABLE_MMAP
#include <sys/mman.h>
#include <sys/pci.h>
#include <sys/ioctl.h>
#endif

#include <bit_slice.h>
#include "tools_utils.h"
#include "mtcr_ul_com.h"
#include "mtcr_int_defs.h"
#include "mtcr_ib.h"
#include "packets_layout.h"
#include "mtcr_tools_cif.h"
#include "mtcr_icmd_cif.h"
#ifndef MST_UL
#include "../mtcr_mlnxos.h"
#endif
#ifndef __be32_to_cpu
#define __be32_to_cpu(x) ntohl(x)
#endif
#ifndef __cpu_to_be32
#define __cpu_to_be32(x) htonl(x)
#endif

#if __BYTE_ORDER == __LITTLE_ENDIAN
#ifndef __cpu_to_le32
#define  __cpu_to_le32(x) (x)
#endif
#ifndef __le32_to_cpu
#define  __le32_to_cpu(x) (x)
#endif
#elif __BYTE_ORDER == __BIG_ENDIAN
#ifndef __cpu_to_le32
#define  __cpu_to_le32(x) bswap_32(x)
#endif
#ifndef __le32_to_cpu
#define  __le32_to_cpu(x) bswap_32(x)
#endif
#else
#ifndef __cpu_to_le32
#define  __cpu_to_le32(x) bswap_32(__cpu_to_be32(x))
#endif
#ifndef __le32_to_cpu
#define  __le32_to_cpu(x) __be32_to_cpu(bswap_32(x))
#endif
#endif

#define CX3_SW_ID    4099
#define CX3PRO_SW_ID 4103

/* Forward decl*/
static int get_inband_dev_from_pci(char* inband_dev, char* pci_dev);

int check_force_config(unsigned my_domain, unsigned my_bus, unsigned my_dev, unsigned my_func);
/*
 * Lock file section:
 *
 * in order to support concurrency on both MEMORY/CONFIG_CYCLES access methods
 * there is need for a sync mechanism between mtcr_ul using processes
 *
 * solution: each mfile obj shall contain a file descriptor linked to a unique, device determined file
 *           which will be used as a lock for the critical sections in read/write operations.
 *           these lock files will be created per device (dbdf format) AND per interface (memory/configuration cycles).
 */

#define LOCK_FILE_DIR "/tmp/mstflint_lockfiles"
#define LOCK_FILE_FORMAT "/tmp/mstflint_lockfiles/%04x:%02x:%02x.%x_%s"
// lockfile example : /tmp/mstflint_lockfiles/0000:0b:00.0_config
//                    /tmp/mstflint_lockfiles/0000:0b:00.0_mem
// general format : /tmp/mstflint_lockfiles/<domain:bus:device.function>_<config|mem>
#define CHECK_LOCK(rc) \
    if(rc) {\
        return rc;\
    }

#define MAX_RETRY_CNT 4096
static int _flock_int(int fdlock, int operation)
{
    int cnt = 0;
    if (fdlock == 0) { // in case we failed to create the lock file we ignore the locking mechanism
        return 0;
    }
    do {
        if (flock(fdlock, operation | LOCK_NB) == 0) {
            return 0;
        } else if (errno != EWOULDBLOCK) {
            break; // BAD! lock/free failed
        }
        if ((cnt & 0xf) == 0) { // sleep every 16 retries
            usleep(1);
        }
        cnt++;
    } while (cnt < MAX_RETRY_CNT);
    perror("failed to perform lock operation.");
    return -1;

}

static int _create_lock(mfile* mf, unsigned domain, unsigned bus, unsigned dev, unsigned func)
{
    char fname[64] = { 0 };
    int rc;
    int fd = 0;
    if (!(mf->ul_ctx)) {
        goto cl_clean_up;
    }
    snprintf(fname, 64, LOCK_FILE_FORMAT, domain, bus, dev, func, mf->tp == MST_PCICONF ? "config" : "mem");
    rc = mkdir("/tmp", 0777);
    if (rc && errno != EEXIST) {
        goto cl_clean_up;
    }
    rc = mkdir(LOCK_FILE_DIR, 0777);
    if (rc && errno != EEXIST) {
        goto cl_clean_up;
    }

    fd = open(fname, O_RDONLY | O_CREAT, 0777);
    if (fd < 0) {
        goto cl_clean_up;
    }

    ((ul_ctx_t*)mf->ul_ctx)->fdlock = fd;
    return 0;

cl_clean_up:
    fprintf(stderr, "Warrning: Failed to create lockfile: %s (parallel access not supported)\n", fname);
    return 0;
}
/*End of Lock file section */

static int _extract_dbdf_from_full_name(const char *name, unsigned* domain, unsigned* bus, unsigned* dev,
        unsigned* func)
{
    if (sscanf(name, "/sys/bus/pci/devices/%4x:%2x:%2x.%d/resource0", domain, bus, dev, func) == 4) {
        return 0;
    } else if (sscanf(name, "/sys/bus/pci/devices/%4x:%2x:%2x.%d/config", domain, bus, dev, func) == 4) {
        return 0;
    } else if (sscanf(name, "/proc/bus/pci/%4x:%2x/%2x.%d", domain, bus, dev, func) == 4) {
        return 0;
    } else if (sscanf(name, "/proc/bus/pci/%2x/%2x.%d", bus, dev, func) == 3) {
        *domain = 0;
        return 0;
    }
    // failed to extract dbdf format from name
    errno = EINVAL;
    return -1;
}

static int mtcr_connectx_flush(void *ptr, int fdlock)
{
    u_int32_t value;
    int rc;
    rc = _flock_int(fdlock, LOCK_EX);
    CHECK_LOCK(rc);
    *((u_int32_t *) ((char *) ptr + 0xf0380)) = 0x0;
    do {
        asm volatile ("":::"memory");
        value = __be32_to_cpu(*((u_int32_t * )((char * )ptr + 0xf0380)));
    } while (value);
    rc = _flock_int(fdlock, LOCK_UN);
    CHECK_LOCK(rc)
    return 0;
}

int mread4_ul(mfile *mf, unsigned int offset, u_int32_t *value)
{
    ul_ctx_t* ctx = mf->ul_ctx;
    return ctx->mread4(mf, offset, value);
}

int mwrite4_ul(mfile *mf, unsigned int offset, u_int32_t value)
{
    ul_ctx_t* ctx = mf->ul_ctx;
    return ctx->mwrite4(mf, offset, value);
}

// TODO: Verify change 'data' type from void* to u_in32_t* does not mess up things
static int mread_chunk_as_multi_mread4(mfile *mf, unsigned int offset, u_int32_t* data, int length)
{
    int i;
    if (length % 4) {
        return EINVAL;
    }
    for (i = 0; i < length; i += 4) {
        u_int32_t value;
        if (mread4_ul(mf, offset + i, &value) != 4) {
            return -1;
        }
        memcpy((char*) data + i, &value, 4);
    }
    return length;
}

static int mwrite_chunk_as_multi_mwrite4(mfile *mf, unsigned int offset, u_int32_t* data, int length)
{
    int i;
    if (length % 4) {
        return EINVAL;
    }
    for (i = 0; i < length; i += 4) {
        u_int32_t value;
        memcpy(&value, (char*) data + i, 4);
        if (mwrite4_ul(mf, offset + i, value) != 4) {
            return -1;
        }
    }
    return length;
}

/*
 * Return values:
 * 0:  OK
 * <0: Error
 * 1 : Device does not support memory access
 *
 */
static
int mtcr_check_signature(mfile *mf)
{
    unsigned signature;
    int rc;
    char* connectx_flush = getenv("CONNECTX_FLUSH");
    rc = mread4_ul(mf, 0xF0014, &signature);
    if (rc != 4) {
        if (!errno)
            errno = EIO;
        return -1;
    }

    switch (signature) {
        case 0xbad0cafe: /* secure host mode device id */
            return 0;
        case 0xbadacce5: /* returned upon mapping the UAR bar */
        case 0xffffffff: /* returned when pci mem access is disabled (driver down) */
            return 1;
    }

    if (connectx_flush == NULL || strcmp(connectx_flush, "0")) {
        if ((signature == 0xa00190 || (signature & 0xffff) == 0x1f5 || (signature & 0xffff) == 0x1f7)
                && mf->tp == MST_PCI) {
            ul_ctx_t* ctx = mf->ul_ctx;
            ctx->connectx_flush = 1;
            if (mtcr_connectx_flush(mf->ptr, ctx->fdlock)) {
                return -1;
            }
        }
    }

    return 0;
}

#if CONFIG_ENABLE_MMAP
/*
 * The PCI interface treats multi-function devices as independent
 * devices.  The slot/function address of each device is encoded
 * in a single byte as follows:
 *
 *  7:3 = slot
 *  2:0 = function
 */
#define PCI_DEVFN(slot,func)    ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)     (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)     ((devfn) & 0x07)

static
unsigned long long mtcr_procfs_get_offset(unsigned my_bus, unsigned my_dev, unsigned my_func)
{
    FILE* f;
    unsigned irq;
    unsigned long long base_addr[6], rom_base_addr, size[6], rom_size;

    unsigned bus, dev, func;
    //unsigned vendor_id;
    //unsigned device_id;
    unsigned int cnt;

    unsigned long long offset = (unsigned long long) -1;

    char buf[4048];

    f = fopen("/proc/bus/pci/devices", "r");
    if (!f)
        return offset;

    for (;;)
        if (fgets(buf, sizeof(buf) - 1, f)) {
            unsigned dfn, vend;

            cnt = sscanf(buf, "%x %x %x %llx %llx %llx %llx %llx %llx "
                    "%llx %llx %llx %llx %llx %llx %llx %llx", &dfn, &vend, &irq, &base_addr[0], &base_addr[1],
                    &base_addr[2], &base_addr[3], &base_addr[4], &base_addr[5], &rom_base_addr, &size[0], &size[1],
                    &size[2], &size[3], &size[4], &size[5], &rom_size);
            if (cnt != 9 && cnt != 10 && cnt != 17) {
                fprintf(stderr, "proc: parse error (read only %d items)\n", cnt);
                fprintf(stderr, "the offending line in " "/proc/bus/pci/devices" " is "
                        "\"%.*s\"\n", (int) sizeof(buf), buf);
                goto error;
            }
            bus = dfn >> 8U;
            dev = PCI_SLOT(dfn & 0xff);
            func = PCI_FUNC(dfn & 0xff);
            //vendor_id = vend >> 16U;
            //device_id = vend & 0xffff;

            if (bus == my_bus && dev == my_dev && func == my_func)
                break;
        } else
            goto error;

    if (cnt != 17 || size[1] != 0 || size[0] != MTCR_MAP_SIZE) {
        if (0)
            fprintf(stderr, "proc: unexpected region size values: "
                    "cnt=%d, size[0]=%#llx, size[1]=%#llx\n", cnt, size[0], size[1]);
        if (0)
            fprintf(stderr, "the offending line in " "/proc/bus/pci/devices"
                    " is \"%.*s\"\n", (int) sizeof(buf), buf);
        goto error;
    }

    offset = ((unsigned long long) (base_addr[1]) << 32)
            + ((unsigned long long) (base_addr[0]) & ~(unsigned long long) (0xfffff));

    fclose(f);
    return offset;

error:
    fclose(f);
    errno = ENXIO;
    return offset;
}

static
unsigned long long mtcr_sysfs_get_offset(unsigned domain, unsigned bus, unsigned dev, unsigned func)
{
    unsigned long long start, end, type;
    unsigned long long offset = (unsigned long long) -1;
    FILE *f;
    int cnt;
    char mbuf[] = "/sys/bus/pci/devices/XXXX:XX:XX.X/resource";
    sprintf(mbuf, "/sys/bus/pci/devices/%4.4x:%2.2x:%2.2x.%1.1x/resource", domain, bus, dev, func);

    f = fopen(mbuf, "r");
    if (!f)
        return offset;

    cnt = fscanf(f, "0x%llx 0x%llx 0x%llx", &start, &end, &type);
    if (cnt != 3 || end != start + MTCR_MAP_SIZE - 1) {
        if (0)
            fprintf(stderr, "proc: unexpected region size values: "
                    "cnt=%d, start=%#llx, end=%#llx\n", cnt, start, end);
        goto error;
    }

    fclose(f);
    return start;

error:
    fclose(f);
    errno = ENOENT;
    return offset;
}

//
// PCI MEMORY ACCESS FUNCTIONS
//

static
int mtcr_pcicr_mclose(mfile *mf)
{
    if (mf) {
        if (mf->ptr) {
            munmap(mf->ptr, MTCR_MAP_SIZE);
        }
        if (mf->fd > 0) {
            close(mf->fd);
        }
        if (mf->res_fd > 0) {
            close(mf->res_fd);
        }
    }
    return 0;
}

static
int mtcr_mmap(mfile *mf, const char *name, off_t off, int ioctl_needed)
{
    int err;
    mf->fd = open(name, O_RDWR | O_SYNC);
    if (mf->fd < 0)
        return -1;

    if (ioctl_needed && ioctl(mf->fd, PCIIOC_MMAP_IS_MEM) < 0) {
        err = errno;
        close(mf->fd);
        errno = err;
        return -1;
    }

    mf->ptr = mmap(NULL, MTCR_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mf->fd, off);

    if (!mf->ptr || mf->ptr == MAP_FAILED) {
        err = errno;
        close(mf->fd);
        errno = err;
        return -1;
    }
    return 0;
}

int mtcr_pcicr_mread4(mfile *mf, unsigned int offset, u_int32_t *value)
{
    ul_ctx_t *ctx = mf->ul_ctx;

    if (offset >= MTCR_MAP_SIZE) {
        errno = EINVAL;
        return 0;
    }
    if (ctx->need_flush) {
        if (mtcr_connectx_flush(mf->ptr, ctx->fdlock)) {
            return 0;
        }
        ctx->need_flush = 0;
    }
    *value = __be32_to_cpu(*((u_int32_t * )((char * )mf->ptr + offset)));
    return 4;
}

int mtcr_pcicr_mwrite4(mfile *mf, unsigned int offset, u_int32_t value)
{
    ul_ctx_t *ctx = mf->ul_ctx;

    if (offset >= MTCR_MAP_SIZE) {
        errno = EINVAL;
        return 0;
    }
    *((u_int32_t *) ((char *) mf->ptr + offset)) = __cpu_to_be32(value);
    ctx->need_flush = ctx->connectx_flush;
    return 4;
}

static
int mtcr_pcicr_open(mfile *mf, const char *name, char* conf_name, off_t off, int ioctl_needed)
{
    int rc;
    ul_ctx_t *ctx = mf->ul_ctx;

    mf->tp = MST_PCI;

    ctx->mread4 = mtcr_pcicr_mread4;
    ctx->mwrite4 = mtcr_pcicr_mwrite4;
    ctx->mread4_block = mread_chunk_as_multi_mread4;
    ctx->mwrite4_block = mwrite_chunk_as_multi_mwrite4;
    ctx->mclose = mtcr_pcicr_mclose;

    mf->ptr = NULL;
    mf->fd = -1;
    ctx->connectx_flush = 0;
    ctx->need_flush = 0;

    rc = mtcr_mmap(mf, name, off, ioctl_needed);
    if (rc) {
        goto end;
    }

    rc = mtcr_check_signature(mf);
end:
    if (rc) {
        mtcr_pcicr_mclose(mf);
    } else if (conf_name != NULL) {
        mfile* conf_mf = mopen_ul(conf_name);
        if (conf_mf != NULL) {
            mf->res_fd = conf_mf->fd;
            ul_ctx_t *conf_ctx = conf_mf->ul_ctx;
            mf->res_tp = conf_mf->tp;
            mf->vsec_addr = conf_mf->vsec_addr;
            mf->vsec_supp = conf_mf->vsec_supp;
            mf->address_space = conf_mf->address_space;
            ctx->res_fdlock = conf_ctx->fdlock;
            ctx->res_mread4 = conf_ctx->mread4;
            ctx->res_mwrite4 = conf_ctx->mwrite4;
            ctx->res_mread4_block = conf_ctx->mread4_block;
            ctx->res_mwrite4_block = conf_ctx->mwrite4_block;
            free(conf_mf);
        }
    }

    return rc;
}

//
// PCI CONF ACCESS FUNCTIONS
//

#if CONFIG_ENABLE_PCICONF
/* PCI address space related enum*/
enum {
    PCI_CAP_PTR = 0x34, PCI_HDR_SIZE = 0x40, PCI_EXT_SPACE_ADDR = 0xff,

    PCI_CTRL_OFFSET = 0x4, // for space / semaphore / auto-increment bit
    PCI_COUNTER_OFFSET = 0x8,
    PCI_SEMAPHORE_OFFSET = 0xc,
    PCI_ADDR_OFFSET = 0x10,
    PCI_DATA_OFFSET = 0x14,

    PCI_FLAG_BIT_OFFS = 31,

    PCI_SPACE_BIT_OFFS = 0,
    PCI_SPACE_BIT_LEN = 16,

    PCI_STATUS_BIT_OFFS = 29,
    PCI_STATUS_BIT_LEN = 3,
};

/* Mellanox vendor specific enum */
enum {
    CAP_ID = 0x9, ICMD_DOMAIN = 0x1, CR_SPACE_DOMAIN = 0x2, SEMAPHORE_DOMAIN = 0xa, IFC_MAX_RETRIES = 2048
};

/* PCI operation enum(read or write)*/
enum {
    READ_OP = 0, WRITE_OP = 1,
};

#define READ4_PCI(mf, val_ptr, pci_offs, err_prefix, action_on_fail)    \
    do {                                                                \
        int rc;                                                         \
        int lock_rc;                                                    \
        ul_ctx_t *pci_ctx = mf->ul_ctx;                                 \
        lock_rc = _flock_int(pci_ctx->fdlock, LOCK_EX);                 \
        if (lock_rc) {                                                  \
            perror(err_prefix);                                         \
            action_on_fail;                                             \
        }                                                               \
        rc = pread(mf->fd, val_ptr, 4, pci_offs);                       \
        lock_rc = _flock_int(pci_ctx->fdlock, LOCK_UN);                 \
        if (lock_rc) {                                                  \
            perror(err_prefix);                                         \
            action_on_fail;                                             \
        }                                                               \
        if (rc != 4 ) {                                                 \
            if (rc < 0)                                                 \
                perror(err_prefix);                                     \
            action_on_fail;                                             \
        }                                                               \
        *val_ptr = __le32_to_cpu(*val_ptr);\
    } while (0)

#define WRITE4_PCI(mf, val, pci_offs, err_prefix, action_on_fail)           \
        do {                                                                \
            int rc;                                                         \
            int lock_rc;                                                    \
            u_int32_t val_le;                                               \
            ul_ctx_t *pci_ctx = mf->ul_ctx;                                 \
            val_le = __cpu_to_le32(val);                                    \
            lock_rc = _flock_int(pci_ctx->fdlock, LOCK_EX);                 \
            if (lock_rc) {                                                  \
                perror(err_prefix);                                         \
                action_on_fail;                                             \
            }                                                               \
            rc = pwrite(mf->fd, &val_le, 4, pci_offs);                      \
            lock_rc = _flock_int(pci_ctx->fdlock, LOCK_UN);                 \
            if (lock_rc) {                                                  \
                perror(err_prefix);                                         \
                action_on_fail;                                             \
            }                                                               \
            if (rc != 4 ) {                                                 \
                if (rc < 0)                                                 \
                    perror(err_prefix);                                     \
                action_on_fail;                                         \
            }                                                               \
        } while (0)

int pci_find_capability(mfile* mf, int cap_id)
{
    unsigned offset;
    unsigned char visited[256] = { }; /* Prevent infinite loops */
    unsigned char data[2];
    int ret;
    int lock_ret;
    ul_ctx_t *pci_ctx = mf->ul_ctx;

    // protect against parallel access
    lock_ret = _flock_int(pci_ctx->fdlock, LOCK_EX);
    if (lock_ret) {
        return 0;
    }

    ret = pread(mf->fd, data, 1, PCI_CAP_PTR);

    lock_ret = _flock_int(pci_ctx->fdlock, LOCK_UN);
    if (lock_ret) {
        return 0;
    }

    if (ret != 1) {
        return 0;
    }

    offset = data[0];
    while (1) {
        if (offset < PCI_HDR_SIZE || offset > PCI_EXT_SPACE_ADDR) {
            return 0;
        }

        lock_ret = _flock_int(pci_ctx->fdlock, LOCK_EX);
        if (lock_ret) {
            return 0;
        }

        ret = pread(mf->fd, data, sizeof data, offset);

        lock_ret = _flock_int(pci_ctx->fdlock, LOCK_UN);
        if (lock_ret) {
            return 0;
        }

        if (ret != sizeof data) {
            return 0;
        }

        visited[offset] = 1;

        if (data[0] == cap_id) {
            return offset;
        }

        offset = data[1];

        if (offset > PCI_EXT_SPACE_ADDR) {
            return 0;
        }
        if (visited[offset]) {
            return 0;
        }
    }
    return 0;
}

int mtcr_pciconf_cap9_sem(mfile* mf, int state)
{
    u_int32_t lock_val;
    u_int32_t counter = 0;
    int retries = 0;
    if (!state) { // unlock
        WRITE4_PCI(mf, 0, mf->vsec_addr + PCI_SEMAPHORE_OFFSET, "unlock semaphore", return ME_PCI_WRITE_ERROR);
    } else { // lock
        do {
            if (retries > IFC_MAX_RETRIES) {
                return ME_SEM_LOCKED;
            }
            // read semaphore untill 0x0
            READ4_PCI(mf, &lock_val, mf->vsec_addr + PCI_SEMAPHORE_OFFSET, "read counter", return ME_PCI_READ_ERROR);
            if (lock_val) { //semaphore is taken
                retries++;
                msleep(1); // wait for current op to end
                continue;
            }
            //read ticket
            READ4_PCI(mf, &counter, mf->vsec_addr + PCI_COUNTER_OFFSET, "read counter", return ME_PCI_READ_ERROR);
            //write ticket to semaphore dword
            WRITE4_PCI(mf, counter, mf->vsec_addr + PCI_SEMAPHORE_OFFSET, "write counter to semaphore",
                    return ME_PCI_WRITE_ERROR);
            // read back semaphore make sure ticket == semaphore else repeat
            READ4_PCI(mf, &lock_val, mf->vsec_addr + PCI_SEMAPHORE_OFFSET, "read counter", return ME_PCI_READ_ERROR);
            retries++;
        } while (counter != lock_val);
    }
    return ME_OK;
}

int mtcr_pciconf_wait_on_flag(mfile* mf, u_int8_t expected_val)
{
    int retries = 0;
    u_int32_t flag;
    do {
        if (retries > IFC_MAX_RETRIES) {
            return ME_PCI_IFC_TOUT;
        }
        READ4_PCI(mf, &flag, mf->vsec_addr + PCI_ADDR_OFFSET, "read flag", return ME_PCI_READ_ERROR);
        flag = EXTRACT(flag, PCI_FLAG_BIT_OFFS, 1);
        retries++;
        if ((retries & 0xf) == 0) { // dont sleep always
            msleep(1);
        }
    } while (flag != expected_val);
    return ME_OK;
}

int mtcr_pciconf_set_addr_space(mfile* mf, u_int16_t space)
{
    // read modify write
    u_int32_t val;
    READ4_PCI(mf, &val, mf->vsec_addr + PCI_CTRL_OFFSET, "read domain", return ME_PCI_READ_ERROR);
    val = MERGE(val, space, PCI_SPACE_BIT_OFFS, PCI_SPACE_BIT_LEN);
    WRITE4_PCI(mf, val, mf->vsec_addr + PCI_CTRL_OFFSET, "write domain", return ME_PCI_WRITE_ERROR);
    // read status and make sure space is supported
    READ4_PCI(mf, &val, mf->vsec_addr + PCI_CTRL_OFFSET, "read status", return ME_PCI_READ_ERROR);
    if (EXTRACT(val, PCI_STATUS_BIT_OFFS, PCI_STATUS_BIT_LEN) == 0) {
        return ME_PCI_SPACE_NOT_SUPPORTED;
    }
    return ME_OK;
}

int mtcr_pciconf_rw(mfile *mf, unsigned int offset, u_int32_t* data, int rw)
{
    int rc = ME_OK;
    u_int32_t address = offset;

    //last 2 bits must be zero as we only allow 30 bits addresses
    if (EXTRACT(address, 30, 2)) {
        return ME_BAD_PARAMS;
    }

    address = MERGE(address, (rw ? 1 : 0), PCI_FLAG_BIT_OFFS, 1);
    if (rw == WRITE_OP) {
        // write data
        WRITE4_PCI(mf, *data, mf->vsec_addr + PCI_DATA_OFFSET, "write value", return ME_PCI_WRITE_ERROR);
        // write address
        WRITE4_PCI(mf, address, mf->vsec_addr + PCI_ADDR_OFFSET, "write offset", return ME_PCI_WRITE_ERROR);
        // wait on flag
        rc = mtcr_pciconf_wait_on_flag(mf, 0);
    } else {
        // write address
        WRITE4_PCI(mf, address, mf->vsec_addr + PCI_ADDR_OFFSET, "write offset", return ME_PCI_WRITE_ERROR);
        // wait on flag
        rc = mtcr_pciconf_wait_on_flag(mf, 1);
        // read data
        READ4_PCI(mf, data, mf->vsec_addr + PCI_DATA_OFFSET, "read value", return ME_PCI_READ_ERROR);
    }
    return rc;
}

int mtcr_pciconf_send_pci_cmd_int(mfile *mf, int space, unsigned int offset, u_int32_t* data, int rw)
{
    int rc = ME_OK;

    // take semaphore
    rc = mtcr_pciconf_cap9_sem(mf, 1);
    if (rc) {
        return rc;
    }

    // set address space
    rc = mtcr_pciconf_set_addr_space(mf, space);
    if (rc) {
        goto cleanup;
    }

    // read/write the data
    rc = mtcr_pciconf_rw(mf, offset, data, rw);
    cleanup:
    // clear semaphore
    mtcr_pciconf_cap9_sem(mf, 0);
    return rc;
}

// adrianc: no need to lock the file semaphore if we access cr-space through mellanox vendor specific cap
int mtcr_pciconf_mread4(mfile *mf, unsigned int offset, u_int32_t *value)
{
    int rc;
    rc = mtcr_pciconf_send_pci_cmd_int(mf, mf->address_space, offset, value, READ_OP);
    if (rc) {
        return -1;
    }
    return 4;
}
int mtcr_pciconf_mwrite4(mfile *mf, unsigned int offset, u_int32_t value)
{
    int rc;
    rc = mtcr_pciconf_send_pci_cmd_int(mf, mf->address_space, offset, &value, WRITE_OP);
    if (rc) {
        return -1;
    }
    return 4;
}

static int block_op_pciconf(mfile *mf, unsigned int offset, u_int32_t* data, int length, int rw)
{
    int i;
    int rc = ME_OK;
    int wrote_or_read = length;
    if (length % 4) {
        return -1;
    }
    // lock semaphore and set address space
    rc = mtcr_pciconf_cap9_sem(mf, 1);
    if (rc) {
        return -1;
    }
    // set address space
    rc = mtcr_pciconf_set_addr_space(mf, mf->address_space);
    if (rc) {
        wrote_or_read = -1;
        goto cleanup;
    }

    for (i = 0; i < length; i += 4) {
        if (mtcr_pciconf_rw(mf, offset + i, &(data[(i >> 2)]), rw)) {
            wrote_or_read = i;
            goto cleanup;
        }
    }
    cleanup: mtcr_pciconf_cap9_sem(mf, 0);
    return wrote_or_read;
}

static int mread4_block_pciconf(mfile *mf, unsigned int offset, u_int32_t* data, int length)
{
    return block_op_pciconf(mf, offset, data, length, READ_OP);
}

static int mwrite4_block_pciconf(mfile *mf, unsigned int offset, u_int32_t* data, int length)
{
    return block_op_pciconf(mf, offset, data, length, WRITE_OP);
}

int mtcr_pciconf_mread4_old(mfile *mf, unsigned int offset, u_int32_t *value)
{
    ul_ctx_t *ctx = mf->ul_ctx;
    int rc;
    // adrianc: PCI registers always in le32
    offset = __cpu_to_le32(offset);
    rc = _flock_int(ctx->fdlock, LOCK_EX);
    if (rc) {
        goto pciconf_read_cleanup;
    }
    rc = pwrite(mf->fd, &offset, 4, 22 * 4);
    if (rc < 0) {
        perror("write offset");
        goto pciconf_read_cleanup;
    }
    if (rc != 4) {
        rc = 0;
        goto pciconf_read_cleanup;
    }

    rc = pread(mf->fd, value, 4, 23 * 4);
    if (rc < 0) {
        perror("read value");
        goto pciconf_read_cleanup;
    }
    *value = __le32_to_cpu(*value);
    pciconf_read_cleanup: _flock_int(ctx->fdlock, LOCK_UN);
    return rc;
}

int mtcr_pciconf_mwrite4_old(mfile *mf, unsigned int offset, u_int32_t value)
{
    ul_ctx_t *ctx = mf->ul_ctx;
    int rc;
    offset = __cpu_to_le32(offset);
    rc = _flock_int(ctx->fdlock, LOCK_EX);
    if (rc) {
        goto pciconf_write_cleanup;
    }
    rc = pwrite(mf->fd, &offset, 4, 22 * 4);
    if (rc < 0) {
        perror("write offset");
        goto pciconf_write_cleanup;
    }
    if (rc != 4) {
        rc = 0;
        goto pciconf_write_cleanup;
    }
    value = __cpu_to_le32(value);
    rc = pwrite(mf->fd, &value, 4, 23 * 4);
    if (rc < 0) {
        perror("write value");
        goto pciconf_write_cleanup;
    }
    pciconf_write_cleanup: _flock_int(ctx->fdlock, LOCK_UN);
    return rc;
}

static
int mtcr_pciconf_mclose(mfile *mf)
{
    unsigned int word;
    if (mf) {
        // Adrianc: set address in PCI configuration space to be non-semaphore.
        mread4_ul(mf, 0xf0014, &word);
        if (mf->fd > 0) {
            close(mf->fd);
        }
    }

    return 0;
}

static
int mtcr_pciconf_open(mfile *mf, const char *name)
{
    int err;
    int rc;
    ul_ctx_t *ctx = mf->ul_ctx;
    mf->fd = -1;
    mf->fd = open(name, O_RDWR | O_SYNC);
    if (mf->fd < 0) {
        return -1;
    }

    mf->tp = MST_PCICONF;

    if ((mf->vsec_addr = pci_find_capability(mf, CAP_ID))) {
        // check if the needed spaces are supported
        if (mtcr_pciconf_set_addr_space(mf, ICMD_DOMAIN) || mtcr_pciconf_set_addr_space(mf, SEMAPHORE_DOMAIN)
                || mtcr_pciconf_set_addr_space(mf, CR_SPACE_DOMAIN)) {
            mf->vsec_supp = 0;
        } else {
            mf->vsec_supp = 1;
        }
    }

    if (mf->vsec_supp) {
        mf->address_space = CR_SPACE_DOMAIN;
        ctx->mread4 = mtcr_pciconf_mread4;
        ctx->mwrite4 = mtcr_pciconf_mwrite4;
        ctx->mread4_block = mread4_block_pciconf;
        ctx->mwrite4_block = mwrite4_block_pciconf;
    } else {
        ctx->mread4 = mtcr_pciconf_mread4_old;
        ctx->mwrite4 = mtcr_pciconf_mwrite4_old;
        ctx->mread4_block = mread_chunk_as_multi_mread4;
        ctx->mwrite4_block = mwrite_chunk_as_multi_mwrite4;
    }
    ctx->mclose = mtcr_pciconf_mclose;

    rc = mtcr_check_signature(mf);
    if (rc) {
        rc = -1;
        goto end;
    }

    end: if (rc) {
        err = errno;
        mtcr_pciconf_mclose(mf);
        errno = err;
    }
    return rc;
}
#else
static
int mtcr_pciconf_open(mfile *mf, const char *name)
{
    return -1;
}
#endif

//
// IN-BAND ACCESS FUNCTIONS
//

static
int mtcr_inband_open(mfile* mf, const char* name)
{
#ifndef NO_INBAND
    ul_ctx_t *ctx = mf->ul_ctx;
    mf->tp = MST_IB;
    ctx->mread4 = mib_read4;
    ctx->mwrite4 = mib_write4;
    ctx->mread4_block = mib_readblock;
    ctx->mwrite4_block = mib_writeblock;
    ctx->maccess_reg = mib_acces_reg_mad;
    ctx->mclose = mib_close;
    char* p;
    if ((p = strstr(name, "ibdr-")) != 0 || (p = strstr(name, "iblid-")) != 0 || (p = strstr(name, "lid-")) != 0) {
        return mib_open(p, mf, 0);
    } else {
        return -1;
    }

#else
    (void) name;
    (void) mf;
    errno = ENOSYS;
    return -1;
#endif
}

static MType mtcr_parse_name(const char* name, int *force, unsigned *domain_p, unsigned *bus_p, unsigned *dev_p,
        unsigned *func_p)
{
    unsigned my_domain = 0;
    unsigned my_bus;
    unsigned my_dev;
    unsigned my_func;
    int scnt, r;
    int force_config = 0;
    char config[] = "/config";
    char resource0[] = "/resource0";
    char procbuspci[] = "/proc/bus/pci/";

    unsigned len = strlen(name);
    unsigned tmp;

    if (len >= sizeof config && !strcmp(config, name + len + 1 - sizeof config)) {
        *force = 1;
        return MST_PCICONF;
    }

    if (len >= sizeof resource0 && !strcmp(resource0, name + len + 1 - sizeof resource0)) {
        *force = 1;
        return MST_PCI;
    }

    if (!strncmp(name, "/proc/bus/pci/", sizeof procbuspci - 1)) {
        *force = 1;
        return MST_PCICONF;
    }

    if (sscanf(name, "lid-%x", &tmp) == 1 || sscanf(name, "ibdr-%x", &tmp) == 1 || strstr(name, "lid-") != 0
            || strstr(name, "ibdr-") != 0) {
        *force = 1;
        return MST_IB;
    }

    if (sscanf(name, "mthca%x", &tmp) == 1 || sscanf(name, "mlx4_%x", &tmp) == 1
            || sscanf(name, "mlx5_%x", &tmp) == 1) {
        char mbuf[4048];
        char pbuf[4048];
        char *base;

        r = snprintf(mbuf, sizeof mbuf, "/sys/class/infiniband/%s/device", name);
        if (r <= 0 || r >= (int) sizeof mbuf) {
            fprintf(stderr, "Unable to print device name %s\n", name);
            goto parse_error;
        }

        r = readlink(mbuf, pbuf, sizeof pbuf - 1);
        if (r < 0) {
            perror("read link");
            fprintf(stderr, "Unable to read link %s\n", mbuf);
            return MST_ERROR;
        }
        pbuf[r] = '\0';

        base = basename(pbuf);
        if (!base)
            goto parse_error;
        scnt = sscanf(base, "%x:%x:%x.%x", &my_domain, &my_bus, &my_dev, &my_func);
        if (scnt != 4)
            goto parse_error;
        if (sscanf(name, "mlx5_%x", &tmp) == 1) {
            force_config = 1;
        }
        goto name_parsed;
    }

    scnt = sscanf(name, "%x:%x.%x", &my_bus, &my_dev, &my_func);
    if (scnt == 3) {
        force_config = check_force_config(my_domain, my_bus, my_dev, my_func);
        goto name_parsed;
    }

    scnt = sscanf(name, "%x:%x:%x.%x", &my_domain, &my_bus, &my_dev, &my_func);
    if (scnt == 4) {
        force_config = check_force_config(my_domain, my_bus, my_dev, my_func);
        goto name_parsed;
    }

    scnt = sscanf(name, "pciconf-%x:%x.%x", &my_bus, &my_dev, &my_func);
    if (scnt == 3) {
        force_config = 1;
        goto name_parsed;
    }

    scnt = sscanf(name, "pciconf-%x:%x:%x.%x", &my_domain, &my_bus, &my_dev, &my_func);
    if (scnt == 4) {
        force_config = 1;
        goto name_parsed;
    }

parse_error:
    fprintf(stderr, "Unable to parse device name %s\n", name);
    errno = EINVAL;
    return MST_ERROR;

name_parsed:
    *domain_p = my_domain;
    *bus_p = my_bus;
    *dev_p = my_dev;
    *func_p = my_func;
    *force = 0;
#ifdef __aarch64__
    // on ARM processors MMAP not supported
    (void)force_config;
    return MST_PCICONF;
#else
    if (force_config) {
        return MST_PCICONF;
    }
    return MST_PCI;
#endif
}
#endif

int mread4_block_ul(mfile *mf, unsigned int offset, u_int32_t* data, int byte_len)
{
    ul_ctx_t *ctx = mf->ul_ctx;
    return ctx->mread4_block(mf, offset, data, byte_len);
}

int mwrite4_block_ul(mfile *mf, unsigned int offset, u_int32_t* data, int byte_len)
{
    ul_ctx_t *ctx = mf->ul_ctx;
    return ctx->mwrite4_block(mf, offset, data, byte_len);
}

int msw_reset_ul(mfile *mf)
{
#ifndef NO_INBAND
    switch (mf->tp) {
        case MST_IB:
            return mib_swreset(mf);
        default:
            errno = EPERM;
            return -1;
    }
#else
    (void)mf;
    return -1;
#endif
}

int mhca_reset_ul(mfile *mf)
{
    (void) mf;
    errno = ENOTSUP;
    return -1;
}

static long supported_dev_ids[] = {
        /* CX2 */
        0x6340, 0x634a, 0x6368, 0x6372,
        0x6732, 0x673c, 0x6750, 0x675a,
        0x676e, 0x6746, 0x6764,
        /*****/
        0x1003, //Connect-X3
        0x1007, //Connect-X3Pro
        0x1011, //Connect-IB
        0x1013, //Connect-X4
        0x1015, //Connect-X4Lx
        0x1017, //Connect-X5
        0xc738, //SwitchX
        0xcb20, //Switch-IB
        0xcb84, //Spectrum
        0xcf08, //Switch-IB2
        -1 };

int is_supported_devid(long devid)
{
    int i = 0;
    int ret_val = 0;
    while (supported_dev_ids[i] != -1) {
        if (devid == supported_dev_ids[i]) {
            ret_val = 1;
            break;
        }
        i++;
    }
    return ret_val;
}

int is_supported_device(char* devname)
{

    char fname[64];
    char inbuf[64];
    FILE* f;
    int ret_val = 0;
    sprintf(fname, "/sys/bus/pci/devices/%s/device", devname);
    f = fopen(fname, "r");
    if (f == NULL) {
        //printf("-D- Could not open file: %s\n", fname);
        return 1;
    }
    if (fgets(inbuf, sizeof(inbuf), f)) {
        long devid = strtol(inbuf, NULL, 0);
        ret_val = is_supported_devid(devid);
    }
    fclose(f);
    return ret_val;
}

int mdevices_ul(char *buf, int len, int mask)
{
    return mdevices_v_ul(buf, len, mask, 0);
}

int mdevices_v_ul(char *buf, int len, int mask, int verbosity)
{

#define MDEVS_TAVOR_CR  0x20
#define MLNX_PCI_VENDOR_ID  0x15b3

    FILE* f;
    DIR* d;
    struct dirent *dir;
    int pos = 0;
    int sz;
    int rsz;
    int ndevs = 0;

    if (!(mask & MDEVS_TAVOR_CR)) {
        return 0;
    }

    char inbuf[64];
    char fname[64];

    d = opendir("/sys/bus/pci/devices");
    if (d == NULL) {
        return -2;
    }

    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') {
            continue;
        }
        sz = strlen(dir->d_name);
        if (sz > 2 && strcmp(dir->d_name + sz - 2, ".0") && !verbosity) {
            continue;
        } else if (sz > 4 && strcmp(dir->d_name + sz - 4, "00.0") && !verbosity) {
            // Skip virtual functions
            char physfn[64];
            DIR* physfndir;
            sprintf(physfn, "/sys/bus/pci/devices/%s/physfn", dir->d_name);
            if ((physfndir = opendir(physfn)) != NULL) {
                closedir(physfndir);
                continue;
            }
        }
        sprintf(fname, "/sys/bus/pci/devices/%s/vendor", dir->d_name);
        f = fopen(fname, "r");
        if (f == NULL) {
            ndevs = -2;
            goto cleanup_dir_opened;
        }
        if (fgets(inbuf, sizeof(inbuf), f)) {
            long venid = strtoul(inbuf, NULL, 0);
            if (venid == MLNX_PCI_VENDOR_ID && is_supported_device(dir->d_name)) {
                rsz = sz + 1; //dev name size + place for Null char
                if ((pos + rsz) > len) {
                    ndevs = -1;
                    goto cleanup_file_opened;
                }
                memcpy(&buf[pos], dir->d_name, rsz);
                pos += rsz;
                ndevs++;
            }
        }
        fclose(f);
    }
    closedir(d);

    return ndevs;

cleanup_file_opened:
    fclose(f);
cleanup_dir_opened:
    closedir(d);
    return ndevs;
}

static
int read_pci_config_header(u_int16_t domain, u_int8_t bus, u_int8_t dev, u_int8_t func, u_int8_t data[0x40])
{
    char proc_dev[64];
    sprintf(proc_dev, "/sys/bus/pci/devices/%04x:%02x:%02x.%d/config", domain, bus, dev, func);
    FILE* f = fopen(proc_dev, "r");
    if (!f) {
        //fprintf(stderr, "Failed to open (%s) for reading: %s\n", proc_dev, strerror(errno));
        return 1;
    }
    setvbuf(f, NULL, _IONBF, 0);
    if (fread(data, 0x40, 1, f) != 1) {
        fprintf(stderr, "Failed to read from (%s): %s\n", proc_dev, strerror(errno));
        fclose(f);
        return 1;
    }

    fclose(f);
    return 0;
}

int check_force_config(unsigned my_domain, unsigned my_bus, unsigned my_dev, unsigned my_func)
{
    u_int8_t conf_header[0x40];
    u_int32_t *conf_header_32p = (u_int32_t*) conf_header;
    if (read_pci_config_header(my_domain, my_bus, my_dev, my_func, conf_header)) {
        return 0;
    }
    u_int32_t devid = __le32_to_cpu(conf_header_32p[0]) >> 16;
    if (devid == CX3PRO_SW_ID || devid == CX3_SW_ID) {
        return 0;
    }
    return 1;
}
#define IB_INF "infiniband:"
#define ETH_INF "net:"

static char** get_ib_net_devs(int domain, int bus, int dev, int func, int ib_eth_)
{
    char** ib_net_devs = NULL;
    int i;
    int count = 0;
    int plan_b = 0;
    DIR           *dir;
    struct dirent *dirent;
    char** ib_net_devs_r;
    char sysfs_path[256];
    DIR* physfndir;
    /* Check that the DBDF is not for Virtual function */
    sprintf(sysfs_path, "/sys/bus/pci/devices/%04x:%02x:%02x.%x/physfn", domain, bus, dev, func);
    if ((physfndir = opendir(sysfs_path)) != NULL) {
        closedir(physfndir);
        return NULL;
    }
    if (ib_eth_)
        sprintf(sysfs_path, "/sys/bus/pci/devices/%04x:%02x:%02x.%x/infiniband", domain, bus, dev, func);
    else
        sprintf(sysfs_path, "/sys/bus/pci/devices/%04x:%02x:%02x.%x/net", domain, bus, dev, func);

    if ((dir = opendir(sysfs_path)) == NULL) {
        sprintf(sysfs_path, "/sys/bus/pci/devices/%04x:%02x:%02x.%x", domain, bus, dev, func);
        if ((dir = opendir(sysfs_path)) == NULL) {
            return NULL;
        }
        plan_b = 1;
    }
    while ((dirent = readdir(dir)) != NULL) {
        char *name = dirent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (plan_b) {
            char* p = NULL;
            char* devs_inf = ib_eth_ ? IB_INF : ETH_INF;
            p = strstr(name, devs_inf);
            if (! p) {
                continue;
            }
            name = p + strlen(devs_inf);
        }
        count++;
        ib_net_devs_r = (char**)realloc(ib_net_devs, (count+1)*sizeof(char*));
        if (!ib_net_devs_r) {
            closedir(dir);
            goto mem_error;
        }
        ib_net_devs = ib_net_devs_r;

        ib_net_devs[count - 1] = (char*)malloc(strlen(name) + 1);
        if (!ib_net_devs[count - 1]) {
            closedir(dir);
            goto mem_error;
        }
        strcpy(ib_net_devs[count - 1], name);

        // Set last entry to NULL
        ib_net_devs[count] = NULL;
    }
    closedir(dir);
    return ib_net_devs;

mem_error:
    fprintf(stderr, "Memory allocation failure for ib/net devices\n");
    if (ib_net_devs) {
        for (i = 0; i < count; i++) {
            if (ib_net_devs[i])
                free(ib_net_devs[i]);
        }

        free(ib_net_devs);
    }

    return NULL;
}


dev_info* mdevices_info_ul(int mask, int* len)
{
    return mdevices_info_v_ul(mask, len, 0);
}

dev_info* mdevices_info_v_ul(int mask, int* len, int verbosity)
{
    char* devs = 0;
    char* dev_name;
    int size = 2048;
    int rc;
    int i;

    // Get list of devices
    do {
        if (devs) {
            free(devs);
        }
        size *= 2;
        devs = (char*) malloc(size);
        rc = mdevices_v_ul(devs, size, mask, verbosity);
    } while (rc == -1);

    if (rc <= 0) {
        len = 0;
        if (devs) {
            free(devs);
        }
        return NULL;
    }
    // For each device read
    dev_info* dev_info_arr = (dev_info*) malloc(sizeof(dev_info) * rc);
    memset(dev_info_arr, 0, sizeof(dev_info) * rc);
    dev_name = devs;
    for (i = 0; i < rc; i++) {
        int domain = 0;
        int bus = 0;
        int dev = 0;
        int func = 0;

        dev_info_arr[i].ul_mode = 1;
        dev_info_arr[i].type = (Mdevs) MDEVS_TAVOR_CR;
        u_int8_t conf_header[0x40];
        u_int32_t *conf_header_32p = (u_int32_t*) conf_header;

        // update default device name
        strncpy(dev_info_arr[i].dev_name, dev_name, sizeof(dev_info_arr[i].dev_name) - 1);
        strncpy(dev_info_arr[i].pci.cr_dev, dev_name, sizeof(dev_info_arr[i].pci.cr_dev) - 1);

        // update dbdf
        if (sscanf(dev_name, "%x:%x:%x.%x", &domain, &bus, &dev, &func) != 4) {
            rc = -1;
            len = 0;
            free(dev_info_arr);
            free(devs);
            return NULL;
        }
        dev_info_arr[i].pci.domain = domain;
        dev_info_arr[i].pci.bus = bus;
        dev_info_arr[i].pci.dev = dev;
        dev_info_arr[i].pci.func = func;

        // set pci conf device
        snprintf(dev_info_arr[i].pci.conf_dev, sizeof(dev_info_arr[i].pci.conf_dev),
                "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config", domain, bus, dev, func);


        // Get attached infiniband devices
        dev_info_arr[i].pci.ib_devs  = get_ib_net_devs(domain, bus, dev, func, 1);
        dev_info_arr[i].pci.net_devs = get_ib_net_devs(domain, bus, dev, func, 0);

        // read configuration space header
        if (read_pci_config_header(domain, bus, dev, func, conf_header)) {
            goto next;
        }

        dev_info_arr[i].pci.dev_id = __le32_to_cpu(conf_header_32p[0]) >> 16;
        dev_info_arr[i].pci.vend_id = __le32_to_cpu(conf_header_32p[0]) & 0xffff;
        dev_info_arr[i].pci.class_id = __le32_to_cpu(conf_header_32p[2]) >> 8;
        dev_info_arr[i].pci.subsys_id = __le32_to_cpu(conf_header_32p[11]) >> 16;
        dev_info_arr[i].pci.subsys_vend_id = __le32_to_cpu(conf_header_32p[11]) & 0xffff;



next:
        dev_name += strlen(dev_name) + 1;
    }

    free(devs);
    *len = rc;
    return dev_info_arr;
}


/*
 * This function used to change from running on CONF to CR
 * and vice versa
 */

extern void mpci_change(mfile* mf)
{
    mf->mpci_change(mf);
    return;
}

 void mpci_change_ul(mfile* mf)
{
    if (mf->res_tp == MST_PCICONF) {
        mf->res_tp = MST_PCI;
        mf->tp = MST_PCICONF;
    } else if (mf->res_tp == MST_PCI) {
        mf->res_tp = MST_PCICONF;
        mf->tp = MST_PCI;
    } else {
        return;
    }
    ul_ctx_t* ctx = mf->ul_ctx;
    /***** Switching READ WRITE FUNCS ******/
    f_mread4 tmp_mread4 = ctx->mread4;
    ctx->mread4 = ctx->res_mread4;
    ctx->res_mread4 = tmp_mread4;

    f_mwrite4 tmp_mwrite4 = ctx->mwrite4;
    ctx->mwrite4 = ctx->res_mwrite4;
    ctx->res_mwrite4 = tmp_mwrite4;

    f_mread4_block tmp_mread4_block = ctx->mread4_block;
    ctx->mread4_block = ctx->res_mread4_block;
    ctx->res_mread4_block = tmp_mread4_block;

    f_mwrite4_block tmp_mwrite4_block = ctx->mwrite4_block;
    ctx->mwrite4_block = ctx->res_mwrite4_block;
    ctx->res_mwrite4_block = tmp_mwrite4_block;

    /***** Switching FD LOCKs ******/
    int tmp_lock = ctx->res_fdlock;
    ctx->res_fdlock = ctx->fdlock;
    ctx->fdlock = tmp_lock;
    /***** Switching FDs ******/
    int fd = mf->fd;
    mf->fd = mf->res_fd;
    mf->res_fd = fd;
}


mfile *mopen_ul(const char *name)
{
    mfile *mf;
    off_t offset;
    unsigned domain = 0, bus = 0, dev = 0, func = 0;
    MType dev_type;
    int force;
    char rbuf[] = "/sys/bus/pci/devices/XXXX:XX:XX.X/resource0";
    char cbuf[] = "/sys/bus/pci/devices/XXXX:XX:XX.X/config";
    char pdbuf[] = "/proc/bus/pci/XXXX:XX/XX.X";
    char pbuf[] = "/proc/bus/pci/XX/XX.X";
    char pcidev[] = "XXXX:XX:XX.X";
    int err;
    int rc;

    if (geteuid() != 0) {
        errno = EACCES;
        return NULL;
    }
    mf = (mfile *) malloc(sizeof(mfile));
    if (!mf) {
        return NULL;
    }
    memset(mf, 0, sizeof(mfile));
    mf->ul_ctx = malloc (sizeof(ul_ctx_t));
    if (!(mf->ul_ctx)) {
        goto open_failed;
    }
    memset(mf->ul_ctx, 0, sizeof(ul_ctx_t));
    mf->dev_name = strdup(name);
    if (!mf->dev_name) {
        goto open_failed;
    }
    mf->sock = -1; // we are not opening remotely
    mf->fd = -1;
    mf->res_fd = -1;
    mf->mpci_change = mpci_change_ul;
    dev_type = mtcr_parse_name(name, &force, &domain, &bus, &dev, &func);
    if (dev_type == MST_ERROR) {
        goto open_failed;
    }
    mf->tp = dev_type;
    if (dev_type == MST_PCICONF || dev_type == MST_PCI) {
        // allocate lock to sync between parallel cr space requests (CONF/MEMORY only)
        if (force) {
            // need to extract the dbdf format from the full name
            if (_extract_dbdf_from_full_name(name, &domain, &bus, &dev, &func)) {
                goto open_failed;
            }
        }
        if (_create_lock(mf, domain, bus, dev, func)) {
            goto open_failed;
        }

        sprintf(pcidev, "%4.4x:%2.2x:%2.2x.%1.1x", domain, bus, dev, func);
        if (!is_supported_device(pcidev)) {
            errno = ENOTSUP;
            goto open_failed;
        }
    }

    sprintf(cbuf, "/sys/bus/pci/devices/%4.4x:%2.2x:%2.2x.%1.1x/config", domain, bus, dev, func);

    if (force) {
        switch (dev_type) {
            case MST_PCICONF:
                rc = mtcr_pciconf_open(mf, name);
                break;
            case MST_PCI:
                rc = mtcr_pcicr_open(mf, name, cbuf, 0, 0);
                break;
            case MST_IB:
                rc = mtcr_inband_open(mf, name);
                break;
            default:
                goto open_failed;
        }

        if (0 == rc) {
            return mf;
        }
        goto open_failed;
    }

    if (dev_type == MST_PCICONF)
        goto access_config_forced;

    sprintf(rbuf, "/sys/bus/pci/devices/%4.4x:%2.2x:%2.2x.%1.1x/resource0", domain, bus, dev, func);

    rc = mtcr_pcicr_open(mf, rbuf, cbuf, 0, 0);
    if (rc == 0) {
        return mf;
    } else if (rc == 1) {
        goto access_config_forced;
    }

    /* Following access methods need the resource BAR */
    offset = mtcr_sysfs_get_offset(domain, bus, dev, func);
    if (offset == -1 && !domain)
        offset = mtcr_procfs_get_offset(bus, dev, func);
    if (offset == -1)
        goto access_config_forced;

    sprintf(pdbuf, "/proc/bus/pci/%4.4x:%2.2x/%2.2x.%1.1x", domain, bus, dev, func);
    rc = mtcr_pcicr_open(mf, pdbuf, cbuf, offset, 1);
    if (rc == 0) {
        return mf;
    } else if (rc == 1) {
        goto access_config_forced;
    }

    if (!domain) {
        sprintf(pbuf, "/proc/bus/pci/%2.2x/%2.2x.%1.1x", bus, dev, func);
        rc = mtcr_pcicr_open(mf, pbuf, cbuf, offset, 1);
        if (rc == 0) {
            return mf;
        } else if (rc == 1) {
            goto access_config_forced;
        }
    }

#if CONFIG_USE_DEV_MEM
    /* Non-portable, but helps some systems */
    if (!mtcr_pcicr_open(mf, "/dev/mem", cbuf, offset, 0))
        return mf;
#endif

access_config_forced:
    sprintf(cbuf, "/sys/bus/pci/devices/%4.4x:%2.2x:%2.2x.%1.1x/config", domain, bus, dev, func);
    if (!mtcr_pciconf_open(mf, cbuf))
        return mf;

    sprintf(pdbuf, "/proc/bus/pci/%4.4x:%2.2x/%2.2x.%1.1x", domain, bus, dev, func);
    if (!mtcr_pciconf_open(mf, pdbuf))
        return mf;

    if (!domain) {
        sprintf(pbuf, "/proc/bus/pci/%2.2x/%2.2x.%1.1x", bus, dev, func);
        if (!mtcr_pciconf_open(mf, pdbuf))
            return mf;
    }

open_failed:
    err = errno;
    mclose_ul(mf);
    errno = err;
    return NULL;
}

int mclose_ul(mfile *mf)
{
    if (mf != NULL) {
        ul_ctx_t *ctx = mf->ul_ctx;
        if (ctx) {
            if (ctx->mclose != NULL) {
                // close icmd if if needed
                if (mf->icmd.icmd_opened) {
                    icmd_close(mf);
                }
                ctx->mclose(mf);
            }

            if (ctx->fdlock) {
                close(ctx->fdlock);
            }
            if (ctx->res_fdlock) {
                close(ctx->res_fdlock);
            }
            free(ctx);
        }
        if (mf->dev_name) {
            free(mf->dev_name);
        }
        free(mf);
    }
    return 0;
}

#define IBDR_MAX_NAME_SIZE 128
#define BDF_NAME_SIZE 12
#define DEV_DIR_MAX_SIZE 128
static
int get_inband_dev_from_pci(char* inband_dev, char* pci_dev)
{
    unsigned domain = 0, bus = 0, dev = 0, func = 0;
    int force = 0;
    MType dev_type;
    DIR* d;
    struct dirent *dir;
    char dirname[DEV_DIR_MAX_SIZE], subdirname[DEV_DIR_MAX_SIZE], linkname[DEV_DIR_MAX_SIZE];
    int found = 0;

    dev_type = mtcr_parse_name(pci_dev, &force, &domain, &bus, &dev, &func);

    strcpy(dirname, "/sys/class/infiniband");
    d = opendir(dirname);
    if (d == NULL) {
        errno = ENODEV;
        return -2;
    }

    while ((dir = readdir(d)) != NULL) {
        unsigned curr_domain = 0, curr_bus = 0, curr_dev = 0, curr_func = 0;
        int curr_force = 0, link_size;
        if (dir->d_name[0] == '.') {
            continue;
        }
        sprintf(subdirname, "%s/%s/device", dirname, dir->d_name);
        link_size = readlink(subdirname, linkname, DEV_DIR_MAX_SIZE);
        dev_type = mtcr_parse_name(&linkname[link_size - BDF_NAME_SIZE], &curr_force, &curr_domain, &curr_bus,
                &curr_dev, &curr_func);

        if (domain == curr_domain && bus == curr_bus && dev == curr_dev && func == curr_func) {
            sprintf(inband_dev, "ibdr-0,%s,1", dir->d_name);
            found = 1;
            break;
        }
    }

    closedir(d);
    (void) dev_type; // avoid compiler warrnings
    if (found) {
        return 0;
    } else {
        errno = ENODEV;
        return -1;
    }
}

static
int reopen_pci_as_inband(mfile* mf)
{
    int rc;
    char inband_dev[IBDR_MAX_NAME_SIZE];
    rc = get_inband_dev_from_pci(inband_dev, mf->dev_name);
    if (rc) {
        errno = ENODEV;
        return -1;
    }

    ((ul_ctx_t*)mf->ul_ctx)->mclose(mf);
    free(mf->dev_name);
    mf->dev_name = strdup(inband_dev);

    rc = mtcr_inband_open(mf, inband_dev);
    return rc;
}

int maccess_reg_mad_ul(mfile *mf, u_int8_t *data)
{
    int rc;

    if (mf == NULL || data == NULL) {
        return ME_BAD_PARAMS;
    }

    if (mf->tp != MST_IB) {
        // Close current device and re-open as inband
        rc = reopen_pci_as_inband(mf);
        if (rc) {
            errno = ENODEV; // for compatibility untill we change mtcr to use error code
            return ME_REG_ACCESS_UNKNOWN_ERR;
        }
    }

    return ((ul_ctx_t*)mf->ul_ctx)->maccess_reg(mf, data);
}

static void mtcr_fix_endianness(u_int32_t *buf, int len)
{
    int i;

    for (i = 0; i < (len / 4); ++i) {
        buf[i] = __be32_to_cpu(buf[i]);
    }
}

int mread_buffer_ul(mfile *mf, unsigned int offset, u_int8_t* data, int byte_len)
{
    int rc;
    rc = mread4_block_ul(mf, offset, (u_int32_t*) data, byte_len);
    mtcr_fix_endianness((u_int32_t*) data, byte_len);
    return rc;

}

int mwrite_buffer_ul(mfile *mf, unsigned int offset, u_int8_t* data, int byte_len)
{
    mtcr_fix_endianness((u_int32_t*) data, byte_len);
    return mwrite4_block_ul(mf, offset, (u_int32_t*) data, byte_len);
}

/*
 * Reg Access Section
 */

#define TLV_OPERATION_SIZE 4
#define OP_TLV_SIZE        16
#define REG_TLV_HEADER_LEN 4

enum {
    MAD_CLASS_REG_ACCESS = 1,
};
enum {
    TLV_END = 0, TLV_OPERATION = 1, TLV_DR = 2, TLV_REG = 3, TLV_USER_DATA = 4,
};

#define REGISTER_HEADERS_SIZE 12
#define INBAND_MAX_REG_SIZE 44
#define ICMD_MAX_REG_SIZE (ICMD_MAX_CMD_SIZE - REGISTER_HEADERS_SIZE)
#define TOOLS_HCR_MAX_REG_SIZE (TOOLS_HCR_MAX_MBOX - REGISTER_HEADERS_SIZE)

static int supports_icmd(mfile* mf);
static int supports_tools_cmdif_reg(mfile* mf);
static int init_operation_tlv(struct OperationTlv *operation_tlv, u_int16_t reg_id, u_int8_t method);
static int mreg_send_wrapper(mfile* mf, u_int8_t *data, int r_icmd_size, int w_icmd_size);
static int mreg_send_raw(mfile *mf, u_int16_t reg_id, maccess_reg_method_t method, void *reg_data, u_int32_t reg_size,
        u_int32_t r_size_reg, u_int32_t w_size_reg, int *reg_status);


// maccess_reg: Do a reg_access for the mf device.
// - reg_data is both in and out
// TODO: When the reg operation succeeds but the reg status is != 0,
//       a specific

int maccess_reg_ul(mfile *mf,
        u_int16_t reg_id,
        maccess_reg_method_t reg_method,
        void*     reg_data,
        u_int32_t reg_size,
        u_int32_t r_size_reg,
        u_int32_t w_size_reg,
        int*      reg_status)
{
    int rc;
    if (mf == NULL || reg_data == NULL || reg_status == NULL || reg_size <= 0) {
        return ME_BAD_PARAMS;
    }
    // check register size
    unsigned int max_size = (unsigned int) mget_max_reg_size_ul(mf);
    if (reg_size > (unsigned int) max_size) {
        //reg too big
        return ME_REG_ACCESS_SIZE_EXCCEEDS_LIMIT;
    }
#ifndef MST_UL
    // TODO: add specific checks for each FW access method where needed
    if (mf->flags & MDEVS_MLNX_OS) {
        rc = mos_reg_access_raw(mf, reg_id, reg_method, reg_data, reg_size, reg_status);
    } else if ((mf->flags & (MDEVS_IB | MDEVS_FWCTX)) ||
               (mf->flags != MDEVS_IB && (supports_icmd(mf) || supports_tools_cmdif_reg(mf)))) {
        rc = mreg_send_raw(mf, reg_id, reg_method, reg_data, reg_size, r_size_reg, w_size_reg, reg_status);
    } else {
        return ME_REG_ACCESS_NOT_SUPPORTED;
    }
#else
    if (mf->tp == MST_IB || (supports_icmd(mf) || supports_tools_cmdif_reg(mf)) ) {
        rc = mreg_send_raw(mf, reg_id, reg_method, reg_data, reg_size, r_size_reg, w_size_reg, reg_status);
    } else {
        return ME_REG_ACCESS_NOT_SUPPORTED;
    }
#endif

    if (rc) {
        return rc;
    } else if (*reg_status) {
        switch (*reg_status) {
            case 1:
                return ME_REG_ACCESS_DEV_BUSY;
            case 2:
                return ME_REG_ACCESS_VER_NOT_SUPP;
            case 3:
                return ME_REG_ACCESS_UNKNOWN_TLV;
            case 4:
                return ME_REG_ACCESS_REG_NOT_SUPP;
            case 5:
                return ME_REG_ACCESS_CLASS_NOT_SUPP;
            case 6:
                return ME_REG_ACCESS_METHOD_NOT_SUPP;
            case 7:
                return ME_REG_ACCESS_BAD_PARAM;
            case 8:
                return ME_REG_ACCESS_RES_NOT_AVLBL;
            case 9:
                return ME_REG_ACCESS_MSG_RECPT_ACK;
            case 0x22:
                return ME_REG_ACCESS_CONF_CORRUPT;
            case 0x24:
                return ME_REG_ACCESS_LEN_TOO_SMALL;
            case 0x20:
                return ME_REG_ACCESS_BAD_CONFIG;
            case 0x21:
                return ME_REG_ACCESS_ERASE_EXEEDED;
            case 0x70:
                return ME_REG_ACCESS_INTERNAL_ERROR;
            default:
                return ME_REG_ACCESS_UNKNOWN_ERR;
        }
    }

    return ME_OK;
}

static int init_operation_tlv(struct OperationTlv *operation_tlv, u_int16_t reg_id, u_int8_t method)
{
    memset(operation_tlv, 0, sizeof(*operation_tlv));

    operation_tlv->Type = TLV_OPERATION;
    operation_tlv->class = MAD_CLASS_REG_ACCESS;
    operation_tlv->len = TLV_OPERATION_SIZE;
    operation_tlv->method = method;
    operation_tlv->register_id = reg_id;
    return 0;
}

///////////////////  Function that sends the register via the correct interface ///////////////////////////

static int mreg_send_wrapper(mfile* mf, u_int8_t *data, int r_icmd_size, int w_icmd_size)
{
    int rc;
    if (mf->tp == MST_IB) { //inband access
        rc = maccess_reg_mad(mf, data);
        if (rc) {
            //printf("-E- 2. Access reg mad failed with rc = %#x\n", rc);
            return ME_MAD_SEND_FAILED;
        }
    } else if (supports_icmd(mf)) {
#if defined(MST_UL) && !defined(MST_UL_ICMD)
        if (mf->vsec_supp) { // we support accessing fw via icmd space
            rc = icmd_send_command_int(mf, FLASH_REG_ACCESS, data, w_icmd_size, r_icmd_size, 0);
            if (rc) {
                return rc;
            }
        } else { // send register via inband (maccess_reg_mad will open the device as inband thus consecutives calls will go to the first if)
            rc = maccess_reg_mad(mf, data);
            if (rc) {
                //printf("-E- 2. Access reg mad failed with rc = %#x\n", rc);
                return ME_MAD_SEND_FAILED;
            }
        }
#else
        rc = icmd_send_command_int(mf, FLASH_REG_ACCESS, data, w_icmd_size, r_icmd_size, 0);
        if (rc) {
            return rc;
        }
#endif
    } else if (supports_tools_cmdif_reg(mf)) {
        rc = tools_cmdif_reg_access(mf, data, w_icmd_size, r_icmd_size);
        if (rc) {
            return rc;
        }
    } else {
        return ME_NOT_IMPLEMENTED;
    }
    return ME_OK;
}

static int mreg_send_raw(mfile *mf, u_int16_t reg_id, maccess_reg_method_t method, void *reg_data, u_int32_t reg_size,
        u_int32_t r_size_reg, u_int32_t w_size_reg, int *reg_status)
{
    //printf("-D- reg_id = %d, reg_size = %d, r_size_reg = %d , w_size_reg = %d \n",reg_id,reg_size,r_size_reg,w_size_reg);
    int mad_rc, cmdif_size = 0;
    struct OperationTlv tlv;
    struct reg_tlv tlv_info;
    u_int8_t buffer[1024];

    init_operation_tlv(&(tlv), reg_id, method);
    // Fill Reg TLV
    memset(&tlv_info, 0, sizeof(tlv_info));
    tlv_info.Type = TLV_REG;
    tlv_info.len = (reg_size + REG_TLV_HEADER_LEN) >> 2; // length is in dwords

    // Pack the mad

    cmdif_size += OperationTlv_pack(&tlv, buffer);
    cmdif_size += reg_tlv_pack(&tlv_info, buffer + OP_TLV_SIZE);
    //put the reg itself into the buffer
    memcpy(buffer + OP_TLV_SIZE + REG_TLV_HEADER_LEN, reg_data, reg_size);
    cmdif_size += reg_size;

#ifdef _ENABLE_DEBUG_
    fprintf(stdout, "-I-Tlv's of Data Sent:\n");
    fprintf(stdout, "\tOperation Tlv\n");
    OperationTlv_dump(&tlv, stdout);
    fprintf(stdout, "\tReg Tlv\n");
    reg_tlv_dump(&tlv_info, stdout);
#endif
    // printf("-D- reg_info.len = |%d, OP_TLV: %d, REG_TLV= %d, cmdif_size = %d\n", reg_info.len, OP_TLV_SIZE, REG_RLV_HEADER_LEN, cmdif_size);
    // update r/w_size_reg with the size of op tlv and reg tlv as we need to read/write them as well
    r_size_reg += OP_TLV_SIZE + REG_TLV_HEADER_LEN;
    w_size_reg += OP_TLV_SIZE + REG_TLV_HEADER_LEN;
    //printf("-D- reg_size = %d, r_size_reg = %d , w_size_reg = %d \n",reg_size,r_size_reg,w_size_reg);

    mad_rc = mreg_send_wrapper(mf, buffer, r_size_reg, w_size_reg);
    // Unpack the mad
    OperationTlv_unpack(&tlv, buffer);
    reg_tlv_unpack(&tlv_info, buffer + OP_TLV_SIZE);
    // copy register back from the buffer
    memcpy(reg_data, buffer + OP_TLV_SIZE + REG_TLV_HEADER_LEN, reg_size);

#ifdef _ENABLE_DEBUG_
    fprintf(stdout, "-I-Tlv's of Data Recieved:\n");
    fprintf(stdout, "\tOperation Tlv\n");
    OperationTlv_dump(&tlv, stdout);
    fprintf(stdout, "\tReg Tlv\n");
    reg_tlv_dump(&tlv_info, stdout);
#endif
    // Check the return value
    *reg_status = tlv.status;
    if (mad_rc) {
        return mad_rc;
    }
    return ME_OK;
}

// needed device HW IDs
#define CONNECTX3_PRO_HW_ID 0x1f7
#define CONNECTX2_HW_ID 0x190
#define CONNECTX3_HW_ID 0x1f5
#define SWITCHX_HW_ID 0x245
#define INFINISCALE4_HW_ID 0x1b3

#define HW_ID_ADDR 0xf0014

static int supports_icmd(mfile* mf)
{
    u_int32_t dev_id;

    if (mread4_ul(mf, HW_ID_ADDR, &dev_id) != 4) { // cr might be locked and retured 0xbad0cafe but we dont care we search for device that supports icmd
        return 0;
    }
    switch (dev_id & 0xffff) { // that the hw device id
        case CONNECTX2_HW_ID:
        case CONNECTX3_HW_ID:
        case CONNECTX3_PRO_HW_ID:
        case INFINISCALE4_HW_ID:
        case SWITCHX_HW_ID:
            return 0;
        default:
            break;
    }
    return 1;
}

static int supports_tools_cmdif_reg(mfile* mf)
{
    u_int32_t dev_id;
    if (mread4_ul(mf, HW_ID_ADDR, &dev_id) != 4) { // cr might be locked and retured 0xbad0cafe but we dont care we search for device that supports tools cmdif
        return 0;
    }
    switch (dev_id & 0xffff) { // that the hw device id
        case CONNECTX3_HW_ID: //Cx3
        case CONNECTX3_PRO_HW_ID: // Cx3-pro
            if (tools_cmdif_is_supported(mf) == ME_OK) {
                return 1;
            }
            break;
        default:
            break;
    }
    return 0;
}

int mget_max_reg_size_ul(mfile *mf)
{
    if (mf->acc_reg_params.max_reg_size) {
        return mf->acc_reg_params.max_reg_size;
    } else if (mf->tp == MST_IB) {
        mf->acc_reg_params.max_reg_size = INBAND_MAX_REG_SIZE;
    } else if (supports_icmd(mf)) { // we support icmd and we dont use IB interface -> we use icmd for reg access
        if (mf->vsec_supp) {
            mf->acc_reg_params.max_reg_size = ICMD_MAX_REG_SIZE;
        } else {
            // we send via inband
            mf->acc_reg_params.max_reg_size = INBAND_MAX_REG_SIZE;
        }
    } else if (supports_tools_cmdif_reg(mf)) {
        mf->acc_reg_params.max_reg_size = TOOLS_HCR_MAX_REG_SIZE;
    }
    return mf->acc_reg_params.max_reg_size;
}
