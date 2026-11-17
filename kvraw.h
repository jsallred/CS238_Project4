/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * kvraw.h
 */

#ifndef _KVRAW_H_
#define _KVRAW_H_

#include "system.h"

struct kvraw;

struct kvraw *kvraw_open(const char *pathname);

void kvraw_close(struct kvraw *kvraw);

int kvraw_lookup(struct kvraw *kvraw,
		 void *key,
		 uint64_t *key_len, /* in/out */
		 void *val,
		 uint64_t *val_len, /* in/out */
		 uint64_t *off);    /* in/out */

int kvraw_append(struct kvraw *kvraw,
		 const void *key,
		 uint64_t key_len,
		 const void *val,
		 uint64_t val_len,
		 uint64_t *off);

uint64_t kvraw_size(const struct kvraw *kvraw);

/* Reads the record at *off. Copies up to *key_len bytes of its key into key,
   then reports the record's real key_len and val_len. On return *off is the
   offset of the NEXT record in the log (not the chain pointer). */
int kvraw_scan(struct kvraw *kvraw,
	       void *key,
	       uint64_t *key_len, /* in/out */
	       uint64_t *val_len, /* out */
	       uint64_t *off);    /* in/out */

#endif /* _KVRAW_H_ */
