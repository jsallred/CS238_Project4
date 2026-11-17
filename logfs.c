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

/* superblock lives in device block 0, log offset x is at device offset x+B */
#define SB_MAGIC ( ((uint64_t)0x4c4f4746 << 32) | (uint64_t)0x53303031 )

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
    int error;
    int open_ok; /* set once logfs_open fully succeeded */
    
};

/* Called from logfs_open, before the worker exists, so no locking needed.
   wbuf is used as a scratch buffer (it is block aligned, as O_DIRECT needs). */
static int sb_load(struct logfs *logfs){
    uint64_t magic, head, size, wbuf_size;

    size = device_size(logfs->D);
    wbuf_size = WCACHE_BLOCKS * logfs->B;

    if (device_read(logfs->D, logfs->wbuf, 0, logfs->B)) {
        TRACE("cannot read superblock");
        return -1;
    }
    memcpy(&magic, logfs->wbuf, sizeof (magic));
    memcpy(&head, logfs->wbuf + sizeof (magic), sizeof (head));
    memset(logfs->wbuf, 0, (size_t)logfs->B);

    if (SB_MAGIC != magic) {
        return 0; /* fresh device: head = tail = 0 */
    }
    if (head > (size - logfs->B)) {
        TRACE("corrupt superblock");
        return -1;
    }

    /* restore: everything below tail is on the device, the partial last
       block (if any) must be loaded back into the ring */
    logfs->head = head;
    logfs->tail = head - (head % logfs->B);
    if (logfs->head != logfs->tail) {
        if (device_read(logfs->D,
                        logfs->wbuf + (logfs->tail % wbuf_size),
                        logfs->tail + logfs->B,
                        logfs->B)) {
            TRACE("cannot read partial block");
            return -1;
        }
    }
    return 0;
}

/* Called from logfs_close after the worker has been joined. */
static void sb_store(struct logfs *logfs){
    uint64_t magic, head;

    magic = SB_MAGIC;
    head = logfs->head;
    memset(logfs->wbuf, 0, (size_t)logfs->B);
    memcpy(logfs->wbuf, &magic, sizeof (magic));
    memcpy(logfs->wbuf + sizeof (magic), &head, sizeof (head));
    if (device_write(logfs->D, logfs->wbuf, 0, logfs->B)) {
        TRACE("cannot write superblock");
    }
}

static void *worker_engine(void *arg){
    struct logfs *logfs = (struct logfs *)arg;
    uint64_t wbuf_data_size, T, wbuf_size;

    wbuf_size = (WCACHE_BLOCKS * logfs->B);
    pthread_mutex_lock(&logfs->mutex);

    while (1) { /* Consumer Loops, waiting if data not available or flushing if done */
        wbuf_data_size = logfs->head - logfs->tail;
        if (wbuf_data_size < logfs->B) {
            /* Less than a full block in the buffer, check if we should flush, otherwise wait for producer */
            if (logfs->done) {
                break;
            }
            pthread_cond_wait(&logfs->data_avail, &logfs->mutex);
            continue;
        }
        T = logfs->tail % wbuf_size;
        /* Writes just one block to the device */
        if (device_write(logfs->D, logfs->wbuf + T, logfs->tail + logfs->B, logfs->B) != 0) {
            TRACE("device write failure in logfs consumer");
            logfs->error = 1;
            pthread_cond_signal(&logfs->space_avail);
            pthread_mutex_unlock(&logfs->mutex);
            return NULL;
        } 
        logfs->tail += logfs->B;
        pthread_cond_signal(&logfs->space_avail);
    }
    
    /* We've exited the while loop, check if there's less than a block remaining in wbuf to flush */
    wbuf_data_size = logfs->head - logfs->tail;
    if (0 != wbuf_data_size) {
        T = logfs->tail % wbuf_size;
        memset(logfs->wbuf + T + wbuf_data_size, 0, logfs->B - wbuf_data_size);
        if (device_write(logfs->D, logfs->wbuf + T, logfs->tail + logfs->B, logfs->B) != 0) {
            TRACE("flush failure in logfs consumer");
            logfs->error = 1;
            pthread_cond_signal(&logfs->space_avail);
            pthread_mutex_unlock(&logfs->mutex);
            return NULL;
        } 
    }

    pthread_mutex_unlock(&logfs->mutex);
    return NULL;
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

    /* Restore state from the superblock, if there is one */
    if (sb_load(logfs)) {
        logfs_close(logfs);
        TRACE("logfs cannot load superblock");
        return NULL;
    }
    logfs->open_ok = 1;

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

        /* Worker is finished and has flushed, record where the log ends */
        if (logfs->open_ok && !logfs->error) {
            sb_store(logfs);
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

int logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len) {
    uint64_t blk, skip_data, wbuf_index, wbuf_size, copy_size, offi;
    char *out;

    assert ( logfs );
    assert ( !len || buf );
    if (0 == len) {
        return 0;
    }

    pthread_mutex_lock(&logfs->mutex);

    /* Check the requested read fits inside allocated data */
    if (off + len > logfs->head) {
        pthread_mutex_unlock(&logfs->mutex);
        TRACE("requested read beyond allocated data");
        return -1;
    }

    out = (char *)buf;
    wbuf_size = WCACHE_BLOCKS * logfs->B;
    
    while (len) {
        /* Get the block aligned start for our read */
        blk = off - (off % logfs->B); 
        skip_data = (off % logfs->B);

        /* Check if we are reading from buffer and not device */
        if (blk >= logfs->tail) {
            wbuf_index = off % wbuf_size;
            /* Potentially split memcpy()s if it wraps around wbuf */
            copy_size = MIN(len, wbuf_size - wbuf_index);
            memcpy(out, logfs->wbuf + wbuf_index, (size_t)copy_size);
            out += copy_size;
            if (copy_size < len){
                memcpy(out, logfs->wbuf, (size_t)(len - copy_size));
                out += (len - copy_size);
            }
            off += len;
            len = 0;
        }
        /* Blocks will reference cache or be pulled from device */
        else {
            offi = (blk / logfs->B) % RCACHE_BLOCKS;

            /* Cache Miss - fill cache from device read*/
            if ( (!logfs->rc_valid[offi]) || (logfs->rc_offs[offi] != blk)) {
                if (device_read(logfs->D, logfs->rbuf + (logfs->B * offi), blk + logfs->B, logfs->B) != 0) {
                    pthread_mutex_unlock(&logfs->mutex);
                    TRACE("device read failure logfs");
                    return -1;
                }
                logfs->rc_valid[offi] = 1;
                logfs->rc_offs[offi] = blk;
            }
            /* Update out from cache */
            copy_size = MIN(len, logfs->B - skip_data);
            memcpy(out, logfs->rbuf + (logfs->B * offi) + skip_data, (size_t)copy_size);
            len -= copy_size;
            off += copy_size;
            out += copy_size;
            
        }
    }
    pthread_mutex_unlock(&logfs->mutex);
    return 0;
}

int logfs_append(struct logfs *logfs, const void *buf, uint64_t len) {
    uint64_t free_space, H, wbuf_size, first_copy_size, chunk_size;
    const char *new_data_buf;

    assert( logfs );
    assert( !len || buf );
    
    new_data_buf = (const char *)buf;
    wbuf_size = (WCACHE_BLOCKS * logfs->B);

    pthread_mutex_lock(&logfs->mutex);

    /* check if the requested write will fit into remaining device capacity */
    if ((logfs->head + len) > (device_size(logfs->D) + logfs->B)) {
        pthread_mutex_unlock(&logfs->mutex);
        TRACE("device full in logfs_append");
        return -1;
    } 

    while (len){
        /* Avoid deadlock by defining an accurate chunk size */
        chunk_size = MIN(len, (WCACHE_BLOCKS - 1) * logfs->B);

        /* Ensure wbuf has enough free space to fit the full append, if not wait on space_avail signal */
        here: 
            if (logfs->error) {
                pthread_mutex_unlock(&logfs->mutex);
                TRACE("worker failed in logfs_append");
                return -1;
            }
            free_space = (wbuf_size) - (logfs->head - logfs->tail); /* capacity - space occupied */
            if (chunk_size > free_space){
                pthread_cond_wait(&logfs->space_avail, &logfs->mutex);
                goto here;
            }

        /* Check for a wrap around which will split into 2 memcpy()s */
        H = logfs->head % wbuf_size;
        first_copy_size = MIN(chunk_size, wbuf_size - H);
        memcpy(logfs->wbuf + H, new_data_buf, (size_t)first_copy_size);
        if (first_copy_size < chunk_size){
            memcpy(logfs->wbuf, new_data_buf + first_copy_size, (size_t)(chunk_size - first_copy_size));
        }
        logfs->head += chunk_size;
        len -= chunk_size;
        new_data_buf += chunk_size;
        pthread_cond_signal(&logfs->data_avail);
    }
    pthread_mutex_unlock(&logfs->mutex);
    return 0;
}

uint64_t logfs_size(struct logfs *logfs){
    uint64_t head;

    assert( logfs );
    pthread_mutex_lock(&logfs->mutex);
    head = logfs->head;
    pthread_mutex_unlock(&logfs->mutex);
    return head;
}

