/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * logfs.c
 */

#include <pthread.h>
#include "device.h"
#include "logfs.h"

#define WCACHE_BLOCKS 32
#define RCACHE_BLOCKS 256

/**
 * Needs:
 *   pthread_create()
 *   pthread_join()
 *   pthread_mutex_init()
 *   pthread_mutex_destroy()
 *   pthread_mutex_lock()
 *   pthread_mutex_unlock()
 *   pthread_cond_init()
 *   pthread_cond_destroy()
 *   pthread_cond_wait()
 *   pthread_cond_signal()
 */

/* research the above Needed API and design accordingly */

struct logfs {
    struct device *D; /*Device*/
    uint64_t B; /*Block Size*/
    int rc_valid[RCACHE_BLOCKS]; /*Read Cache Valid Bits*/
    uint64_t rc_offs[RCACHE_BLOCKS]; /*Reach Cache Offsets*/
    char *rbuf_, *rbuf; /*Read Buffers, one for malloc & one for align*/
    uint64_t head; /*Device Offset*/
    uint64_t tail; /*Device Offset*/
    char *wbuf_, *wbuf; /*Write Buffer*/
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t data_avail, space_avail;
    int done; /*BOOL*/

    /* Flags to guide primitives on logfs_close() */
    int mutex_ok;
    int data_ok;
    int space_ok;
    int worker_ok;
    
};

static void *worker_engine(void *arg){
    /* struct logfs *logfs = (struct logfs *)arg; */

    return arg;
}

struct logfs *logfs_open(const char *pathname){

    struct logfs *logfs;

    /* Assert our input argument is valid*/
    assert (safe_strlen(pathname));

    /* Malloc space for logfs and flush it with 0s*/
    if (!(logfs = malloc(sizeof (struct logfs)))) {
		TRACE("out of memory opening logfs");
		return NULL;
	}
    memset(logfs, 0, sizeof (struct logfs));
    
    /* Open the device from filename and stick it in our logfs handle*/
    if (!(logfs->D = device_open(pathname))){
        logfs_close(logfs);
        TRACE("logfs cannot open device");
        return NULL;
    }

    /* Get the block size from the open device*/
    if (!(logfs->B = device_block(logfs->D))){
        logfs_close(logfs);
        TRACE("logfs cannot get block size");
        return NULL;
    }

    /* Malloc the Read Cache Blocks*/
    if (!(logfs->rbuf_ = malloc((RCACHE_BLOCKS + 1) * logfs->B))){
        logfs_close(logfs);
        TRACE("cannot malloc rbuf logfs_open");
        return NULL;
    }
    logfs->rbuf = (char *)memory_align(logfs->rbuf_, logfs->B);

    /* Malloc the Write Buffer*/
    if (!(logfs->wbuf_ = malloc((WCACHE_BLOCKS + 1) * logfs->B))){
        logfs_close(logfs);
        TRACE("cannot malloc wbuf logfs_open");
        return NULL;
    }
    logfs->wbuf = (char *)memory_align(logfs->wbuf_, logfs->B);
    memset(logfs->wbuf, 0, logfs->B * WCACHE_BLOCKS);

    /* Initialize our Mutex */
    /* Pass NULL for default arguments */
    if (pthread_mutex_init(&logfs->mutex, NULL) != 0){ 
        logfs_close(logfs);
        TRACE("logfs init mutex");
        return NULL;
    }
    logfs->mutex_ok = 1;

    /* Initialize our Pthread Signals*/
    if (pthread_cond_init(&logfs->data_avail, NULL) != 0){ 
        logfs_close(logfs);
        TRACE("logfs init cond");
        return NULL;
    }
    logfs->data_ok = 1;
    if (pthread_cond_init(&logfs->space_avail, NULL) != 0){ 
        logfs_close(logfs);
        TRACE("logfs init cond");
        return NULL;
    }
    logfs->space_ok = 1;

    /* Create the Background Worker*/
    if (pthread_create(&logfs->worker, NULL, worker_engine, logfs)) {
		logfs_close(logfs);
		TRACE("logfs cannot create worker thread");
		return NULL;
	}
	logfs->worker_ok = 1;

    return logfs;
}

void logfs_close(struct logfs* logfs){
    if (logfs){

        /* Signal the worker thread to wrap up and wait for it to return */
        if(logfs->worker_ok){
            pthread_mutex_lock(&logfs->mutex);
            logfs->done = 1;
            pthread_cond_signal(&logfs->data_avail);
            pthread_mutex_unlock(&logfs->mutex);
            pthread_join(logfs->worker, NULL);
        }

        /* Clean up remaining primitives */
        if(logfs->mutex_ok){
            if (pthread_mutex_destroy(&logfs->mutex)){
                TRACE("mutex destroy logfs close");
            }
        }
        if(logfs->data_ok){
            if (pthread_cond_destroy(&logfs->data_avail)){
                TRACE("data avail destroy logfs close");
            }
        }
        if(logfs->space_ok){
            if (pthread_cond_destroy(&logfs->space_avail)){
                TRACE("space avail destroy logfs close");
            }
        }

        /* Free buffers, close device, and flush with 0s */
        FREE(logfs->wbuf_);
        FREE(logfs->rbuf_);
        device_close(logfs->D);
        memset(logfs, 0, sizeof(struct logfs));
    }

    FREE(logfs);
}

int logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len);

int logfs_append(struct logfs *logfs, const void *buf, uint64_t len);

