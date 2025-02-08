# City Emergency Dispatch Simulation - FreeRTOS Project

## Overview

This project is a **FreeRTOS-based emergency dispatch simulation**, designed to simulate real-time event handling for police, fire, and ambulance emergency services. The system leverages **FreeRTOS tasks, queues, semaphores, and timers** to efficiently handle event prioritization and resource management.

## **Build System and Dependencies**

This project uses a **Makefile-based build system** and relies on the **FreeRTOS kernel and FreeRTOS-Plus components**.

### **Important Note: Ensure Correct FreeRTOS Directory Paths**

Several variables in the Makefile reference **external FreeRTOS source files**. These paths must be set correctly to an **untouched FreeRTOS repository clone**. Ensure you have cloned FreeRTOS into the correct location, and that the variables in the Makefile correctly reflect its directory structure.

#### **Relevant Makefile Variables:**

- `FREERTOS_DIR_REL` → Path to **FreeRTOS Kernel Source**
- `FREERTOS_DIR` → Absolute path to **FreeRTOS**
- `FREERTOS_PLUS_DIR_REL` → Path to **FreeRTOS-Plus** extensions
- `FREERTOS_PLUS_DIR` → Absolute path to **FreeRTOS-Plus**

Ensure that **no modifications are made to the FreeRTOS source files**, as the project relies on their original structure.

## **Building the Project**

To build the project, simply run:

```sh
make
```

This will compile all necessary source files and generate the `posix_demo` binary in the `build/` directory.

### **Cleaning the Build**

To remove all compiled objects and binaries:

```sh
make clean
```

## **Compilation Flags and Configuration Options**

The Makefile includes various compilation flags and feature toggles:

### **Tracing and Coverage Testing**

- **Enable Tracing:** `projENABLE_TRACING=1` (default)\
  To disable: `make NO_TRACING=1`
- **Enable Coverage Testing:** `projCOVERAGE_TEST=1`\
  Adds extra debug options and error handling.

### **Optimization and Debugging**

- **Profiling Mode:** Use `make PROFILE=1` to enable profiling (`-pg`).
- **Address Sanitization:** Use `make SANITIZE_ADDRESS=1` for memory checking.
- **Leak Sanitization:** Use `make SANITIZE_LEAK=1` for memory leak detection.

## **Project Features**

- **Real-time event scheduling** using FreeRTOS queues and semaphores.
- **Unit dispatch management** with priority-based handling.
- **Fault management system** for unhandled emergencies.
- **Tracing and debugging** support for performance analysis.

## **Profiling with gprof**

If profiling is enabled, generate profiling reports with:

```sh
make profile
```

This will create `prof_flat.txt` and `prof_call_graph.txt` in the `build/` directory.

## **License**

This project is based on FreeRTOS, which is distributed under the MIT License.

## **Contact**

For any questions or issues, please refer to the FreeRTOS documentation or open an issue in your project's repository.
