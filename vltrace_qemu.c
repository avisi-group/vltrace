#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include "bindings.h"

// Must match kernel module definitions
#define VLTRACER_DEVICE "/dev/vl_tracer"
#define TRACE_FILE      "/tmp/trace_file.txt"
#define IOCTL_START _IO('v', 1)
#define IOCTL_STOP  _IO('v', 2)
#define IOCTL_SET_FD _IOW('v', 3, int)

static int vl_tracer_fd = -1;
static bool ptw_enabled = false;

void handle_arg_vltrace(const char *arg) {
    printf("handle_arg_vltrace called with arg: %s\n", arg);
    
    if (strcmp(arg, "ptw") == 0) {
        ptw_enabled = true;
        printf("PTW enabled flag set\n");
    }
    
    setup_trace_file();
}

bool vltrace_insert_pt_write(void) {
    printf("vltrace_insert_pt_write returning %d\n", ptw_enabled);
    return ptw_enabled;
}

int setup_trace_file(void) {
    char abs_path[PATH_MAX];
    
    printf("setup_trace_file started\n");
    
    if (realpath(TRACE_FILE, abs_path) == NULL) {
        printf("Couldn't resolve absolute path, using default: %s\n", TRACE_FILE);
        strcpy(abs_path, TRACE_FILE);
    } else {
        printf("Resolved absolute path: %s\n", abs_path);
    }
    
    printf("Attempting to open trace file: %s\n", abs_path);
    int fd = open(abs_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "[ERROR] Failed to open trace file: %s (%d: %s)\n", 
                abs_path, errno, strerror(errno));
        return -1;
    }
    printf("Trace file opened successfully with fd %d\n", fd);

    printf("Attempting to open device: %s\n", VLTRACER_DEVICE);
    vl_tracer_fd = open(VLTRACER_DEVICE, O_RDWR);
    if (vl_tracer_fd < 0) {
        fprintf(stderr, "[ERROR] Failed to open device %s: %d (%s)\n", 
                VLTRACER_DEVICE, errno, strerror(errno));
        close(fd);
        return -1;
    }
    printf("Device opened successfully with fd %d\n", vl_tracer_fd);

    printf("Attempting to set trace file fd via ioctl\n");
    if (ioctl(vl_tracer_fd, IOCTL_SET_FD, &fd) < 0) {
        fprintf(stderr, "[ERROR] ioctl(IOCTL_SET_FD) failed: %d (%s)\n",
                errno, strerror(errno));
        close(fd);
        close(vl_tracer_fd);
        return -1;
    }
    printf("Successfully set trace file fd via ioctl\n");

    close(fd);
    printf("setup_trace_file completed successfully\n");
    return 0;
}

void vltrace_start_recording(void) {
    printf("vltrace_start_recording called\n");
    
    printf("Attempting to open device: %s\n", VLTRACER_DEVICE);
    vl_tracer_fd = open(VLTRACER_DEVICE, O_RDWR);
    if (vl_tracer_fd < 0) {
        fprintf(stderr, "[ERROR] Failed to open device in start_recording: %d (%s)\n",
                errno, strerror(errno));
        return;
    }
    printf("Device opened successfully with fd %d\n", vl_tracer_fd);

    printf("Attempting to start tracing via ioctl\n");
    if (ioctl(vl_tracer_fd, IOCTL_START) < 0) {
        fprintf(stderr, "[ERROR] ioctl(IOCTL_START) failed: %d (%s)\n",
                errno, strerror(errno));
        close(vl_tracer_fd);
        vl_tracer_fd = -1;
    } else {
        printf("Tracing started successfully\n");
    }
}

void vltrace_stop_recording(void) {
    printf("vltrace_stop_recording called\n");
    
    if (vl_tracer_fd >= 0) {
        printf("Attempting to stop tracing via ioctl\n");
        if (ioctl(vl_tracer_fd, IOCTL_STOP) < 0) {
            fprintf(stderr, "[ERROR] ioctl(IOCTL_STOP) failed: %d (%s)\n",
                    errno, strerror(errno));
        } else {
            printf("Tracing stopped successfully\n");
        }
        
        printf("Closing device fd %d\n", vl_tracer_fd);
        close(vl_tracer_fd);
        vl_tracer_fd = -1;
    } else {
        printf("No active tracing session to stop\n");
    }
}