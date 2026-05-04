#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// Path to the .monitor_pid file
const char *pid_file = "./.monitor_pid";

void handle_sigint(int sig) {
    printf("Caught SIGINT (signal %d). Exiting gracefully...\n", sig);

    if (remove(pid_file) == 0) {
        printf("Deleted %s successfully.\n", pid_file);
    } else {
        perror("ERROR: Failed to delete .monitor_pid file");
    }

    exit(0);
}

void handle_sigusr1(int sig) {
    printf("Received SIGUSR1: A new report has been added.\n");
}

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGUSR1, handle_sigusr1);

    pid_t pid = getpid();

    FILE *file = fopen(pid_file, "w");
    if (file == NULL) {
        perror("ERROR: Failed to open .monitor_pid file");
        return 1;
    }

    fprintf(file, "%d\n", pid);
    fclose(file);

    printf("Process ID %d written to %s\n", pid, pid_file);

    while (1) {
        pause();  // Wait for signals
    }

    return 0;
}