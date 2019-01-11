/*
 * Copyright (C) 2010 ARM Limited. All rights reserved.
 *
 * Portions of this code have been modified from the original.
 * These modifications are:
 *    * includes
 *    * enums
 *    * gralloc_device_open()
 *    * gralloc_register_buffer()
 *    * gralloc_unregister_buffer()
 *    * gralloc_lock()
 *    * gralloc_unlock()
 *    * gralloc_module_methods
 *    * HAL_MODULE_INFO_SYM
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <cutils/log.h>
#include <cutils/atomic.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <fcntl.h>

#include <gralloc1-adapter.h>

#include "gralloc_priv.h"
#include "alloc_device.h"
#include "framebuffer_device.h"

#include "ump.h"
#include "ump_ref_drv.h"
#include "secion.h"
#include "s5p_fimc.h"
#include "exynos_mem.h"
static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sMapLock = PTHREAD_MUTEX_INITIALIZER;

static int s_ump_is_open = 0;
static int gMemfd = 0;
#define PFX_NODE_MEM   "/dev/exynos-mem"

/* we need this for now because pmem cannot mmap at an offset */
#define PMEM_HACK   1
#ifdef USE_PARTIAL_FLUSH
static pthread_mutex_t s_rect_lock = PTHREAD_MUTEX_INITIALIZER;
extern int count_rect(int secure_id);
static struct private_handle_rect *rect_list;

private_handle_rect *find_rect(int secure_id)
{
    private_handle_rect *psRect;

    for (psRect = rect_list; psRect; psRect = psRect->next)
        if (psRect->handle == secure_id)
            break;
    if (!psRect)
        return NULL;

    return psRect;
}

void insert_rect_first(private_handle_rect *new_rect) {
    int secure_id = new_rect->handle;
    private_handle_rect *psRect = NULL;
    private_handle_rect *psFRect = NULL;

    ALOGD_IF(debug_level > 0, "%s secure_id=%d",__func__, secure_id);

    pthread_mutex_lock(&s_rect_lock);
    if (rect_list == NULL) {
        rect_list = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        rect_list->next = new_rect;
    } else {
        for (psRect = rect_list; psRect; psRect = psRect->next) {
            if (psRect->handle == secure_id) {
                  // Inserts rect before existing
                  psFRect->next = new_rect;
                  new_rect->next = psRect;
                  pthread_mutex_unlock(&s_rect_lock);
                  return;
            }
            psFRect = psRect;
        }
        // No match found, just append it
        psFRect->next = new_rect;
    }
    pthread_mutex_unlock(&s_rect_lock);
}

void insert_rect_last(private_handle_rect *new_rect) {
    int secure_id = new_rect->handle;
    private_handle_rect *psRect = NULL;
    private_handle_rect *psFRect = NULL;
    private_handle_rect *psMatchRect = NULL;


    ALOGD_IF(debug_level > 0, "%s secure_id=%d",__func__,secure_id);

    pthread_mutex_lock(&s_rect_lock);
    if (rect_list == NULL) {
        rect_list = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        rect_list->next = new_rect;
    } else {
        for (psRect = rect_list; psRect; psRect = psRect->next) {
            if (psRect->handle == secure_id) {
                psMatchRect = psRect;
            } else if (psMatchRect) {
                psMatchRect->next = new_rect;
                new_rect->next = psRect;
                pthread_mutex_unlock(&s_rect_lock);
                return;
            }
            psFRect = psRect;
        }
        // No match found, just append it
        psFRect->next = new_rect;
    }
    pthread_mutex_unlock(&s_rect_lock);
}

private_handle_rect *find_last_rect(int secure_id)
{
    private_handle_rect *psRect;
    private_handle_rect *psFRect;

    if (rect_list == NULL) {
        rect_list = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        return rect_list;
    }

    for (psRect = rect_list; psRect; psRect = psRect->next) {
        if (psRect->handle == secure_id)
            return psFRect;
        psFRect = psRect;
    }
    return psFRect;
}

int count_rect(int secure_id) {
    private_handle_rect *psRect;
    private_handle_rect *next;

    int count = 0;
    pthread_mutex_lock(&s_rect_lock);
    for (psRect = rect_list; psRect; psRect = psRect->next) {
        next = psRect->next;
        if (next && next->handle == secure_id) {
            count++;
        }
    }

    pthread_mutex_unlock(&s_rect_lock);
    return count;
}

void dump_rect() {
    private_handle_rect *psRect;
    private_handle_rect *next;

    pthread_mutex_lock(&s_rect_lock);
    for (psRect = rect_list; psRect; psRect = psRect->next) {
        ALOGD_IF(debug_partial_flush > 0, "%s:PARTIAL_FLUSH handle/ump_id:%d w:%d h:%d stride:%d, psRect:%08x", __func__, psRect->handle, psRect->w, psRect->h, psRect->stride, psRect);
        next = psRect->next;
    }

    pthread_mutex_unlock(&s_rect_lock);
}

int release_rect(int secure_id)
{
    private_handle_rect *psRect;
    private_handle_rect *psTRect;

    for (psRect = rect_list; psRect; psRect = psRect->next) {
        if (psRect->next) {
            if (psRect->next->handle == secure_id) {
                if (psRect->next->next)
                    psTRect = psRect->next->next;
                else
                    psTRect = NULL;

                free(psRect->next);
                psRect->next = psTRect;
                return 1;
            }
        }
    }

    return 0;
}
#endif

static int gralloc_map(gralloc_module_t const* module __unused,
        buffer_handle_t handle, void** vaddr)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    if (!(hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            size_t size = FIMC1_RESERVED_SIZE * 1024;
            void *mappedAddress = mmap(0, size,
                    PROT_READ|PROT_WRITE, MAP_SHARED, gMemfd, (hnd->paddr - hnd->offset));
            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                return -errno;
            }
            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            size_t size = hnd->size;
            hnd->ion_client = ion_client_create();
            void *mappedAddress = ion_map(hnd->fd, size, 0);

            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not ion_map %s fd(%d)", strerror(errno), hnd->fd);
                return -errno;
            }

            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        } else {
            size_t size = hnd->size;
#if PMEM_HACK
            size += hnd->offset;
#endif
            void *mappedAddress = mmap(0, size,
                    PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd, 0);
            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                return -errno;
            }
            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        }
    }
    *vaddr = (void*)hnd->base;
    return 0;
}

static int gralloc_unmap(gralloc_module_t const* module __unused,
        buffer_handle_t handle)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    if (!(hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            void* base = (void*)(intptr_t(hnd->base) - hnd->offset);
            size_t size = FIMC1_RESERVED_SIZE * 1024;
            if (munmap(base, size) < 0)
                ALOGE("Could not unmap %s", strerror(errno));
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            void* base = (void*)hnd->base;
            size_t size = hnd->size;
            if (ion_unmap(base, size) < 0)
                ALOGE("Could not ion_unmap %s", strerror(errno));
            ion_client_destroy(hnd->ion_client);
        } else {
            void* base = (void*)hnd->base;
            size_t size = hnd->size;
#if PMEM_HACK
            base = (void*)(intptr_t(base) - hnd->offset);
            size += hnd->offset;
#endif
            if (munmap(base, size) < 0)
                ALOGE("Could not unmap %s", strerror(errno));
        }
    }
    hnd->base = 0;
    return 0;
}

static int gralloc_device_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    int status = -EINVAL;

#ifdef ADVERTISE_GRALLOC1
    if (!strcmp(name, GRALLOC_HARDWARE_MODULE_ID)) {
        return gralloc1_adapter_device_open(module, name, device);
    }
#endif

    if (!strcmp(name, GRALLOC_HARDWARE_GPU0))
        status = alloc_device_open(module, name, device);
    else if (!strcmp(name, GRALLOC_HARDWARE_FB0))
        status = framebuffer_device_open(module, name, device);

    return status;
}

static int gralloc_register_buffer(gralloc_module_t const* module, buffer_handle_t handle)
{
    int err = 0;
    int retval = -EINVAL;
    void *vaddr;
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Registering invalid buffer, returning error");
        return -EINVAL;
    }

    /* if this handle was created in this process, then we keep it as is. */
    private_handle_t* hnd = (private_handle_t*)handle;

    ALOGD_IF(debug_level > 1, "%s: ump_id:%d", __func__, hnd->ump_id);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
        return 0;
    }

#ifdef USE_PARTIAL_FLUSH
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
        ALOGD_IF(debug_partial_flush > 0,
            "%s: PARTIAL_FLUSH ump_id:%d === BEGIN === ump_mem_handle:%08x flags=%x usage=%x count:%d backingstore:%d",
            __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
        if (debug_partial_flush > 0)
            dump_rect();

        private_handle_rect *psRect;
        psRect = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        psRect->handle = (int)hnd->ump_id;
        psRect->stride = (int)hnd->stride;
        ALOGD_IF(debug_partial_flush > 0,
            "%s: PARTIAL_FLUSH ump_id:%d === insert_rect_last === ump_mem_handle:%08x flags=%x usage=%x count:%d backingstore:%d",
            __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
        insert_rect_last(psRect);
        if (debug_partial_flush > 0)
            dump_rect();
        ALOGD_IF(debug_partial_flush > 0,
            "%s: PARTIAL_FLUSH ump_id:%d === END === ump_mem_handle:%08x flags=%x usage=%x count:%d backingstore:%d",
            __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
    }
#endif

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
        err = gralloc_map(module, handle, &vaddr);

    pthread_mutex_lock(&s_map_lock);

    if (!s_ump_is_open) {
        ump_result res = ump_open(); /* TODO: Fix a ump_close() somewhere??? */
        if (res != UMP_OK) {
            pthread_mutex_unlock(&s_map_lock);
            ALOGE("Failed to open UMP library");
            return retval;
        }
        s_ump_is_open = 1;
    }

    hnd->pid = getpid();

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
	ALOGD_IF(debug_level > 1, "%s: ump_id:%d ump_mem_handle:%08x", __func__, hnd->ump_id, hnd->ump_mem_handle);

        hnd->ump_mem_handle = (int)ump_handle_create_from_secure_id(hnd->ump_id);
#ifdef USE_PARTIAL_FLUSH
        ALOGD_IF(debug_partial_flush > 0, "%s: PARTIAL_FLUSH ump_id:%d ump_mem_handle:%08x flags=%x usage=%x count:%d backing_store:%d", __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
#endif

        if (UMP_INVALID_MEMORY_HANDLE != (ump_handle)hnd->ump_mem_handle) {
            hnd->base = (int)ump_mapped_pointer_get((ump_handle)hnd->ump_mem_handle);
            if (0 != hnd->base) {
                hnd->lockState = private_handle_t::LOCK_STATE_MAPPED;
                hnd->writeOwner = 0;
                hnd->lockState = 0;

                pthread_mutex_unlock(&s_map_lock);
                return 0;
            } else {
                ALOGE("Failed to map UMP handle");
            }

            ump_reference_release((ump_handle)hnd->ump_mem_handle);
        } else {
            ALOGE("Failed to create UMP handle");
        }
    } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_PMEM) {
        pthread_mutex_unlock(&s_map_lock);
        return 0;
    } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
        void* vaddr = NULL;

        if (gMemfd == 0) {
            gMemfd = open(PFX_NODE_MEM, O_RDWR);
            if (gMemfd < 0) {
                ALOGE("%s:: %s exynos-mem open error\n", __func__, PFX_NODE_MEM);
                return false;
            }
        }

        gralloc_map(module, handle, &vaddr);
        pthread_mutex_unlock(&s_map_lock);
        return 0;
    } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
        hnd->ump_mem_handle = (int)ump_handle_create_from_secure_id(hnd->ump_id);
        if (UMP_INVALID_MEMORY_HANDLE != (ump_handle)hnd->ump_mem_handle) {
            vaddr = (void*)ump_mapped_pointer_get((ump_handle)hnd->ump_mem_handle);
            if (0 != vaddr) {
                hnd->lockState = private_handle_t::LOCK_STATE_MAPPED;
                hnd->writeOwner = 0;
                hnd->lockState = 0;

                pthread_mutex_unlock(&s_map_lock);
                return 0;
            } else {
                ALOGE("Failed to map UMP handle");
            }
            ump_reference_release((ump_handle)hnd->ump_mem_handle);
        } else {
            ALOGE("Failed to create UMP handle");
        }
    } else {
        ALOGE("registering non-UMP buffer not supported");
    }

    if (hnd->flags & private_handle_t::PRIV_FLAGS_GRAPHICBUFFER) {
        ALOGD_IF(debug_level > 0, "ump_id:%d %s: GraphicBuffer (ump_id:%d): ump_mem_handle:%08x (ump_reference_release)", hnd->ump_id, __func__, hnd->ump_id, hnd->ump_mem_handle);
        ump_reference_release((ump_handle)hnd->ump_mem_handle);
    }

    pthread_mutex_unlock(&s_map_lock);
    return retval;
}

static int unregister_buffer(private_handle_t* hnd)
{
	if (private_handle_t::validate(hnd) < 0) {
        ALOGE("%s Unregistering invalid buffer, returning error", __func__);
        return -EINVAL;
    }

    ALOGD_IF(debug_level > 1, "%s: ump_id:%d", __func__, hnd->ump_id);
#ifdef USE_PARTIAL_FLUSH
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
        ALOGD_IF(debug_partial_flush > 0,
            "%s: PARTIAL_FLUSH ump_id:%d === BEGIN === ump_mem_handle:%08x flags=%x usage=%x count:%d backingstore:%d",
            __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
        if (debug_partial_flush > 0)
            dump_rect();

        if (!release_rect((int)hnd->ump_id))
            ALOGE("%s: PARTIAL_FLUSH ump_id:%d, release error", __func__, (int)hnd->ump_id);

        if (debug_partial_flush > 0)
            dump_rect();
        ALOGD_IF(debug_partial_flush > 0,
            "%s: PARTIAL_FLUSH ump_id:%d === END === ump_mem_handle:%08x flags=%x usage=%x count:%d backingstore:%d",
            __func__, hnd->ump_id, hnd->ump_mem_handle, hnd->flags, hnd->usage, count_rect(hnd->ump_id), hnd->backing_store);
    }
#endif
    ALOGE_IF(hnd->lockState & private_handle_t::LOCK_STATE_READ_MASK,
            "[unregister] handle %p still locked (state=%08x)", hnd, hnd->lockState);

    /* never unmap buffers that were not registered in this process */
    if (hnd->pid == getpid()) {
        pthread_mutex_lock(&s_map_lock);
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
	    if (UMP_INVALID_MEMORY_HANDLE != (ump_handle)hnd->ump_mem_handle) {
	        ALOGD_IF(debug_level > 1, "%s: ump_id:%d ump_mem_handle:%08x", __func__, hnd->ump_id, hnd->ump_mem_handle);
	        ump_mapped_pointer_release((ump_handle)hnd->ump_mem_handle);
	        hnd->base = 0;
	        ump_reference_release((ump_handle)hnd->ump_mem_handle);
	        hnd->ump_mem_handle = (int)UMP_INVALID_MEMORY_HANDLE;
	        hnd->lockState  = 0;
	        hnd->writeOwner = 0;
	    } else {
                ALOGD_IF(debug_level > 1, "%s: ump_id:%d SKIPPED ump_mem_handle:%08x", __func__, hnd->ump_id, hnd->ump_mem_handle);
            }
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            if(hnd->base != 0)
                gralloc_unmap(NULL /* module */, (buffer_handle_t)hnd);

            pthread_mutex_unlock(&s_map_lock);
            if (0 < gMemfd) {
                close(gMemfd);
                gMemfd = 0;
            }
            return 0;
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            ump_mapped_pointer_release((ump_handle)hnd->ump_mem_handle);
            ump_reference_release((ump_handle)hnd->ump_mem_handle);
            if (hnd->base)
                gralloc_unmap(NULL /* module */, (buffer_handle_t)hnd);

            hnd->base = 0;
            hnd->ump_mem_handle = (int)UMP_INVALID_MEMORY_HANDLE;
            hnd->lockState  = 0;
            hnd->writeOwner = 0;
        } else {
            ALOGE("unregistering non-UMP buffer not supported");
        }

        pthread_mutex_unlock(&s_map_lock);
    }

    return 0;
}

void* gralloc_unregister_buffer_thread(void *data) {
    private_handle_t* hnd = (private_handle_t*)data;

    ALOGD_IF(debug_level > 1, "%s: ump_id:%d START", __func__, hnd->ump_id);
    usleep(1000000); // 1000ms
    unregister_buffer(hnd);
    ALOGD_IF(debug_level > 1, "%s: ump_id:%d END", __func__, hnd->ump_id);
    delete hnd;
    return NULL;
}

static private_handle_t* clone_private_handle(private_handle_t* hnd) {
    private_handle_t* result = new private_handle_t(
        hnd->flags,
        hnd->size,
        hnd->base,
        hnd->lockState,
        (ump_secure_id)hnd->ump_id,
        (ump_handle)hnd->ump_mem_handle,
        hnd->fd,
        hnd->offset,
        hnd->paddr);
    result->magic = hnd->magic;
    result->base = hnd->base;
    result->writeOwner = hnd->writeOwner;
    result->pid = hnd->pid;
    result->format = hnd->format;
    result->usage = hnd->usage;
    result->width = hnd->width;
    result->height = hnd->height;
    result->bpp = hnd->bpp;
    result->stride = hnd->stride;
    result->uoffset = hnd->uoffset;
    result->voffset = hnd->voffset;
    result->ion_client = hnd->ion_client;
    result->backing_store = hnd->backing_store;
    result->producer_usage = hnd->producer_usage;
    result->consumer_usage = hnd->consumer_usage;
    return result;
}

static int gralloc_unregister_buffer(gralloc_module_t const* module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("unregistering invalid buffer, returning error");
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

    ALOGD_IF(debug_level > 1, "%s: ump_id:%d", __func__, hnd->ump_id);
    if (hnd->flags & private_handle_t::PRIV_FLAGS_GRAPHICBUFFER) {
        pthread_attr_t thread_attr;
        pthread_t unreg_buffer_thread;

        pthread_attr_init(&thread_attr);
        pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&unreg_buffer_thread, &thread_attr,
        gralloc_unregister_buffer_thread, (void *) clone_private_handle(hnd));
        if (rc < 0) {
            ALOGE("%s: Unable to create thread", __func__);
            return -1;
        }
        return 0;
    }

    return unregister_buffer(hnd);
}

static int gralloc_lock(gralloc_module_t const* module __unused, buffer_handle_t handle,
                        int usage, int l __unused, int t __unused, int w __unused,
                        int h __unused, void** vaddr)
{
    int err = 0;
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Locking invalid buffer, returning error");
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

#ifdef SAMSUNG_EXYNOS_CACHE_UMP
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
#ifdef USE_PARTIAL_FLUSH
        private_handle_rect *psRect;
        psRect = find_rect((int)hnd->ump_id);
        psRect->l = l;
        psRect->t = t;
        psRect->w = w;
        psRect->h= h;
        psRect->locked = 1;
#endif
    }
#endif
    if (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))
        *vaddr = (void*)hnd->base;

    if (usage & GRALLOC_USAGE_YUV_ADDR) {
        // Create pointer to 3 pointers for YUV addresses
        void** pAddr = (void **) malloc(3 * sizeof(void *));
        pAddr[0] = (void*)hnd->base;
        pAddr[1] = (void*)(hnd->base + hnd->uoffset);
        pAddr[2] = (void*)(hnd->base + hnd->uoffset + hnd->voffset);
        *vaddr = pAddr;
    }
    return err;
}

static int gralloc_unlock(gralloc_module_t const* module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Unlocking invalid buffer, returning error");
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

#ifdef SAMSUNG_EXYNOS_CACHE_UMP
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
#ifdef USE_PARTIAL_FLUSH
        private_handle_rect *psRect;
        psRect = find_rect((int)hnd->ump_id);
        ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, UMP_MSYNC_CLEAN,
                (void *)(hnd->base + (psRect->stride * psRect->t)), psRect->stride * psRect->h );
        return 0;
#endif
        ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, UMP_MSYNC_CLEAN_AND_INVALIDATE, NULL, 0);
    }
#endif
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
        ion_msync(hnd->ion_client, hnd->fd, (ION_MSYNC_FLAGS) (IMSYNC_DEV_TO_RW | IMSYNC_SYNC_FOR_DEV), hnd->size, hnd->offset);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
        int ret;
        exynos_mem_flush_range mem;
        mem.start = hnd->paddr;
        mem.length = hnd->size;

        ret = ioctl(gMemfd, EXYNOS_MEM_PADDR_CACHE_FLUSH, &mem);
        if (ret < 0) {
            ALOGE("Error in exynos-mem : EXYNOS_MEM_PADDR_CACHE_FLUSH (%d)\n", ret);
            return false;
        }
    }

    return 0;
}

static int gralloc_perform(struct gralloc_module_t const* module,
                    int operation, ... )
{
    int res = -EINVAL;
    va_list args;
    if(!module)
        return res;

    va_start(args, operation);
    switch (operation) {
        case GRALLOC1_ADAPTER_PERFORM_GET_REAL_MODULE_API_VERSION_MINOR:
            {
                auto outMinorVersion = va_arg(args, int*);
                *outMinorVersion = 0;
                ALOGV("%s: GRALLOC1_ADAPTER_PERFORM_GET_REAL_MODULE_API_VERSION_MINOR %d",
                    __func__, *outMinorVersion);
            } break;
        case GRALLOC1_ADAPTER_PERFORM_SET_USAGES:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto producerUsage = va_arg(args, uint64_t);
                auto consumerUsage = va_arg(args, uint64_t);
                hnd->producer_usage = producerUsage;
                hnd->consumer_usage = consumerUsage;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_SET_USAGES p:0x%08x c:0x%08x", __func__,
                    hnd, producerUsage, consumerUsage);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_DIMENSIONS:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outWidth = va_arg(args, int*);
                auto outHeight = va_arg(args, int*);
                *outWidth = hnd->width;
                *outHeight = hnd->height;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_DIMENSIONS %d x %d", __func__,
                    hnd, *outWidth, *outHeight);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_FORMAT:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outFormat = va_arg(args, int*);
                *outFormat = hnd->format;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_FORMAT %d", __func__,
                    hnd, *outFormat);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_PRODUCER_USAGE:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outUsage = va_arg(args, uint64_t*);
                *outUsage = hnd->producer_usage;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_PRODUCER_USAGE 0x%08x", __func__,
                    hnd, hnd->producer_usage);
            } break;
        case GRALLOC1_ADAPTER_PERFORM_GET_CONSUMER_USAGE:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outUsage = va_arg(args, uint64_t*);
                *outUsage = hnd->consumer_usage;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_CONSUMER_USAGE 0x%08x", __func__,
                    hnd, hnd->consumer_usage);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_BACKING_STORE:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outBackingStore = va_arg(args, uint64_t*);
                *outBackingStore = hnd->backing_store;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_BACKING_STORE %llu", __func__,
                    hnd, *outBackingStore);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_NUM_FLEX_PLANES:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outNumFlexPlanes = va_arg(args, int*);

                (void) hnd;
                // for simpilicity
                *outNumFlexPlanes = 4;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_NUM_FLEX_PLANES %d", __func__,
                    hnd, *outNumFlexPlanes);
            } break;

        case GRALLOC1_ADAPTER_PERFORM_GET_STRIDE:
            {
                auto hnd =  va_arg(args, private_handle_t*);
                auto outStride = va_arg(args, int*);
                *outStride = hnd->width;
                ALOGV("%s: (%p) GRALLOC1_ADAPTER_PERFORM_GET_STRIDE %d", __func__,
                    hnd, *outStride);
            } break;
        default:
            ALOGE("%s: NOT IMPLEMENTED %d", __func__, operation);
            break;
    }
    va_end(args);
    return res;
}

static int gralloc_getphys(gralloc_module_t const* module, buffer_handle_t handle, void** paddr)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    paddr[0] = (void*)hnd->paddr;
    paddr[1] = (void*)(hnd->paddr + hnd->uoffset);
    paddr[2] = (void*)(hnd->paddr + hnd->uoffset + hnd->voffset);
    return 0;
}

/* There is one global instance of the module */
static struct hw_module_methods_t gralloc_module_methods =
{
    open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM =
{
    base:
    {
        common:
        {
            tag: HARDWARE_MODULE_TAG,
#ifdef ADVERTISE_GRALLOC1
            version_major: GRALLOC1_ADAPTER_MODULE_API_VERSION_1_0,
#else
            version_major: 1,
#endif
            version_minor: 0,
            id: GRALLOC_HARDWARE_MODULE_ID,
            name: "Graphics Memory Allocator Module",
            author: "ARM Ltd.",
            methods: &gralloc_module_methods,
            dso: NULL,
        },
        registerBuffer: gralloc_register_buffer,
        unregisterBuffer: gralloc_unregister_buffer,
        lock: gralloc_lock,
        unlock: gralloc_unlock,
//        getphys: gralloc_getphys,
        perform: gralloc_perform,
    },
    framebuffer: NULL,
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: NULL,
};
