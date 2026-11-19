# CS 238P Project 4

Key/value file system on a raw block device, using direct I/O. The database keeps its state when it is closed and opened again. Build it on Linux (we used an Ubuntu VM).

Travis Tran, travitt1, 91922527
Joseph Allred, jsallred, 52776720

## Setup

The program needs a block device. Make a loop device backed by a file:

```bash
truncate -s 64M disk.img
sudo losetup -f --show disk.img
```

The second command prints the device name, such as `/dev/loop0`. The loop device goes away when the machine reboots, so run the second command again after a restart. The file `disk.img` stays.

## Demo

```bash
make
sudo ./cs238 /dev/loop0
```

Use the device name printed by `losetup`. The program runs six tests, and every line should say `[PASS]`: `basic_logic`, `heavy_rewrite`, `read_write_single`, `read_write_small`, `read_write_large`, `persist`. The run takes about 1 second.

## How it works

`logfs.c` is an append-only log on the block device. Writes go into a 32-block ring buffer, and a background thread writes full blocks to the device. Reads of data already on the device go through a 256-block read cache. Reads of data still in the ring buffer are copied from the ring. All I/O is block aligned and uses `O_DIRECT`, so the kernel buffers are not used.

## Store and restore

The database keeps its state when it is closed and opened again.

On close, `logfs` saves the length of the log in a superblock (device block 0). The log itself starts at block 1. On open, `logfs` restores that length and reloads the last partial block, so new appends continue where the last session stopped. Then `kvdb` replays the log to rebuild the index, `size`, and `waste`.

The state is saved on a clean close only.

## Tests

`main.c` wipes block 0 before each test, so every test starts with an empty database. The `persist` test inserts, updates, and removes keys, then closes and reopens the database three times. It checks that:

- `size` and `waste` are restored
- every value is restored, and removed keys stay removed
- new keys can be added after a reopen
- old and new keys are both there after a second reopen

## Clean

```bash
make clean
```

To detach the loop device when you are done:

```bash
sudo losetup -d /dev/loop0
```
