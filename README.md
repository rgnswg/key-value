# Simple Key-Value Store

This is a personal hobby/educational project I built to test my c skills. It definitely needs some heavy refactoring, but it serves as a great playground for learning how database storage engines function under the hood.

## Project Overview

The project consists of a lightweight Key-Value storage engine inspired by log-structured architectures. It uses an **Append-Only Log** for persistence and indexes data in RAM using a custom Hash Table.

## Features

* **Append-Only Log**: All `SET` and `DEL` operations are written sequentially at the end of the file in constant time $O(1)$, avoiding random disk writes.
* **In-Memory Bootstrapping**: Upon startup, the program scans the entire database file sequentially to build the Hash Table index in RAM.
* **Dynamic Table Sizing**: The initial size of the Hash Table during bootstrap varies dynamically based on the last recorded packaging density (load factor) saved in the file header.
* **Collision Resolution**: Hash Table collisions are handled using traditional chained linked lists per bucket.
* **File Compaction**: Includes a basic compaction function that purges stale data, overwritten key history, and deleted keys (*Tombstones*), rewriting a fresh, optimized database file.
* **Packed Binary Header**: Features a strict 64-byte file header to validate basic metadata and consistency across sessions.

## Compilation

To compile the project, run:

```bash
gcc -o main main.c
