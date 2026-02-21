#  ProcX – Multi-Instance Process Manager

ProcX is a **Linux-based process management tool** developed as part of an *Operating Systems and Design* term project.
It allows users to start, list, and terminate programs while keeping multiple ProcX instances synchronized across different terminals using **POSIX Inter-Process Communication (IPC)** mechanisms.

---

##  Features

* Run programs in **attached** or **detached** mode
* Global shared process table accessible from all ProcX instances
* List running processes with detailed information (PID, command, mode, owner, runtime)
* Terminate processes by PID using `SIGTERM`
* Real-time **cross-terminal synchronization**
* Automatic detection of process termination
* Clean shutdown and IPC resource management

---

##  Core Operating System Concepts

This project demonstrates practical use of fundamental OS concepts:

* Process creation and replacement (`fork`, `execvp`)
* Session management (`setsid`)
* POSIX shared memory (`shm_open`, `mmap`)
* Synchronization using named semaphores
* Event broadcasting via POSIX message queues
* Multithreading with `pthread`
* Zombie process prevention (`waitpid`, `SIGCHLD`)

---

##  System Architecture

ProcX consists of the following components:

* **Main Thread**
  Menu-driven user interface (run, list, terminate, exit)

* **Shared Process Table**
  Stored in shared memory and protected by a named semaphore

* **Monitor Thread**
  Periodically checks process status and removes terminated entries

* **IPC Listener Thread**
  Receives and prints notifications from other ProcX instances

* **Event Bus**
  POSIX message queue used to broadcast process start and termination events

---

##  Build & Run

### Compile

```bash
make
```

### Run

```bash
./procx
```

---

##  Usage Menu

```
1. Run a new program
2. List running programs
3. Terminate a program
0. Exit
```

---

##  Testing Summary

The project was tested under multiple scenarios including:

* Single-instance process execution
* Multi-terminal synchronization
* Attached vs detached process behavior
* Manual termination using PID
* Automatic termination detection by the monitor thread

All test cases behaved as expected and confirmed correct synchronization and cleanup.

---

##  Limitations

* Fixed-size shared process table (maximum 50 processes)
* Simple whitespace-based command parsing
* Advanced shell features (pipes, redirection, quoting) are not supported

---

##  Conclusion

ProcX successfully demonstrates core operating system mechanisms through a functional and synchronized multi-instance process manager.
The implementation meets all project requirements and validates correct usage of POSIX IPC, multithreading, and process control in Linux.

---

 *This project was developed for educational purposes.*
