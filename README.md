# OS-Jackfruit  
## Supervised Multi-Container Runtime with Memory Monitoring

## Project Overview

OS-Jackfruit is an Operating Systems project that demonstrates a lightweight multi-container runtime developed in C on Linux. The project includes:

- User-space container runtime (`engine.c`)
- Kernel-space memory monitor (`monitor.c`)
- Workload generators (`cpu_hog`, `memory_hog`, `io_pulse`)
- Logging and container management commands

This project simulates container lifecycle management similar to Docker at a simplified educational level.

---

# Objectives

- Understand Linux process management
- Learn namespaces and container basics
- Implement IPC using UNIX domain sockets
- Develop producer-consumer logging model using threads
- Build Linux kernel module for memory monitoring
- Enforce soft and hard memory limits
- Practice systems programming in C

---

# Technologies Used

- C Programming Language
- Linux / Ubuntu / WSL2
- POSIX Threads (`pthread`)
- UNIX Domain Socket IPC
- Linux Kernel Module
- Git & GitHub
- GCC Compiler
- Makefile

---

# Project Structure

```text
OS-Jackfruit/
│── README.md
│── boilerplate/
│   ├── engine.c
│   ├── monitor.c
│   ├── monitor_ioctl.h
│   ├── Makefile
│   ├── cpu_hog.c
│   ├── memory_hog.c
│   ├── io_pulse.c
│   └── logs/O
