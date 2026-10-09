# keel's filesystem format

The definition is `ondisk/keel/ondisk/ondisk.sysl` (module `keel.ondisk`), compiled into both the
kernel and `tools/mkfs`; this page describes it. Every number is little-endian.

## Blocks

A disk is 512-byte sectors; the filesystem works in **4 KiB blocks** of 8 sectors, block `b` being
sectors `8b` to `8b + 7`. The test image is 1024 blocks (4 MiB).

| block | what |
|---|---|
| 0 | the boot block: never used by the filesystem (milestone 7's disk test reads sector 1 and writes sector 2 here) |
| 1 | the superblock, at offset 0 |
| 2 … | the block bitmap: one bit per block, 32,768 blocks per bitmap block |
| `inode_start` … | the inode table: 64 inodes per block |
| `data_start` … `blocks - 1` | data: file contents, directories, indirect blocks |

## Superblock (48 bytes)

| offset | field | |
|---|---|---|
| 0 | `magic: u32` | `0x6c65656b`, the bytes "keel" |
| 4 | `version: u32` | 2 (1 had no owners; the kernel refuses a version-1 disk) |
| 8 | `block_size: u32` | 4096 |
| 12 | `blocks: u32` | the whole filesystem, boot block included; at most the disk's size |
| 16 | `bitmap_start: u32` | 2 |
| 20 | `bitmap_blocks: u32` | enough for `blocks` bits |
| 24 | `inode_start: u32` | `bitmap_start + bitmap_blocks` |
| 28 | `inode_blocks: u32` | |
| 32 | `inodes: u32` | `inode_blocks * 64`, inode 0 included |
| 36 | `data_start: u32` | `inode_start + inode_blocks` |
| 40 | `root: u32` | 1 |
| 44 | `reserved: u32` | 0 |

`keel.ondisk.valid` checks all of these relations before the kernel mounts a disk.

## Bitmap

Bit `b % 8` of byte `(b % 32768) / 8` of bitmap block `bitmap_start + b / 32768` is set while block
`b` is in use. Every block below `data_start` is marked; a data block is marked exactly while some
inode holds it.

## Inodes (64 bytes)

Inode `i` is at offset `(i % 64) * 64` of block `inode_start + i / 64`. Inode 0 is never used, so a
directory entry naming inode 0 is a free slot; the root directory is inode 1.

| offset | field | |
|---|---|---|
| 0 | `kind: u16` | 0 free, 1 file, 2 directory |
| 2 | `mode: u16` | POSIX's `st_mode`: the type (`0o100000` file, `0o040000` directory, agreeing with `kind`), set-user-ID (`0o4000`), and `rwx` for the owner, the group and other (`0o777`) |
| 4 | `links: u16` | directory entries naming it: 1 for a file; 2 + its subdirectories for a directory |
| 6 | `uid: u16` | the owner; 0 is root |
| 8 | `size: u64` | bytes |
| 16 | `direct: [10]u32` | the blocks holding bytes 0 to 40 KiB |
| 56 | `indirect: u32` | a block of 1024 more block numbers (`u32`), bytes 40 KiB to 4136 KiB |
| 60 | `gid: u16` | the group |
| 62 | `reserved: u16` | |

The kernel enforces the mode (Unix's rules: uid 0 passes; otherwise the owner's bits if the process owns
the inode, else the group's if its gid is the inode's, else other's). A new file is `0o644` and a new
directory `0o755`, owned by the process that made it. Search (`x`) is needed on every directory a path
walks through.

A block number of 0 means none. **A file has no holes**: every block up to its size is allocated (a
write past the end allocates zeroed blocks for the gap), and no block pointer past its size is set.
The indirect block is held only by a file of more than 10 blocks.

## Directories

A directory is a file of **32-byte entries**, 128 to a block; its size is a multiple of 32. Its first
two entries are `.` (itself) and `..` (its parent; the root's is itself).

| offset | field | |
|---|---|---|
| 0 | `ino: u32` | 0: a free slot |
| 4 | `len: u8` | bytes of `name` in use, 1 to 24 |
| 5 | `kind: u8` | the inode's kind, so a listing need not read every inode |
| 6 | `reserved: u16` | |
| 8 | `name: [24]u8` | not terminated; any byte but `/` |

A removed entry becomes a free slot, reused before the directory grows; directories never shrink.

## What `mkfs check` holds an image to

The superblock is valid; inode 0 is free and the root is a directory; every inode in use has type bits in its mode that agree with its kind, and no
mode bits outside `0o4777` below them; every directory reached from
the root has `.` naming itself and `..` naming its parent, no holes, and entries naming only inodes in
use, of the kind the entry says, each directory reached once; every inode in use is named, exactly
`links` times, and a directory's `links` is 2 + its subdirectories; every block a file holds is a data
block, held by one inode only, present up to its size and absent past it; and the bitmap marks exactly
the blocks below `data_start` and the held ones.
