# task_lister

A Linux kernel module that exposes process information through a `/proc` interface.
This is an operating-systems project for learning kernel task traversal, `seq_file`,
module parameters, and procfs file operations.

## Features

- Flat process listing with PID, parent PID, state, RSS, and CPU time
- Parent-child process tree
- Runtime filtering by task state
- Aggregate task statistics
- Clean procfs teardown when the module is unloaded

The module creates:

```text
/proc/task_lister/tasks
/proc/task_lister/tree
/proc/task_lister/filter
/proc/task_lister/stats
```

## Requirements

- Linux kernel headers for the target kernel
- `make`, GCC, and standard kernel-module build tools
- Root access for loading, unloading, or installing the module

Test this module in a disposable virtual machine. Kernel modules execute with
full kernel privileges and this project is not intended for production systems.

## Build

Build against the currently running kernel:

```sh
make
```

To target a different kernel build tree:

```sh
make KDIR=/path/to/kernel/build
```

Inspect the resulting module:

```sh
make info
```

Clean generated Kbuild files:

```sh
make clean
```

## Use

Load and unload manually:

```sh
sudo insmod task_lister.ko
cat /proc/task_lister/tasks
cat /proc/task_lister/tree
cat /proc/task_lister/stats
sudo rmmod task_lister
```

Filter the task list by state:

```sh
cat /proc/task_lister/filter
echo R | sudo tee /proc/task_lister/filter  # running
echo S | sudo tee /proc/task_lister/filter  # sleeping
echo - | sudo tee /proc/task_lister/filter  # clear filter
```

Supported states are `R`, `S`, `D`, `Z`, `T`, `t`, and `X`.

The filter can also be set when loading the module:

```sh
sudo insmod task_lister.ko filter_state=R
```

## VM smoke test

After building the module inside the VM, run:

```sh
sudo bash test_task_lister.sh
```

The script loads the module, checks all procfs entries, exercises listing,
tree, statistics, and filtering, then unloads the module and verifies cleanup.

## License

The source is licensed under GPL-2.0-only, as declared in
[`task_lister.c`](task_lister.c).
