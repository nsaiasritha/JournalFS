#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define JOURNAL "journal.log"
#define TARGET "data.txt"

volatile sig_atomic_t crash_requested = 0;

/* ---------------- SIGNAL HANDLER ---------------- */

void handle_crash(int sig)
{
    (void)sig;
    crash_requested = 1;
}

/* ---------------- JOURNAL MANAGER ---------------- */

int append_journal(int tx,
                   const char *op,
                   const char *file,
                   const char *data)
{
    int fd;

    fd = open(JOURNAL,
              O_WRONLY | O_CREAT | O_APPEND,
              0644);

    if (fd < 0)
    {
        perror("open journal");
        return -1;
    }

    char line[512];

    int n = snprintf(
        line,
        sizeof(line),
        "TX%d LOG %s %s %s\n",
        tx,
        op,
        file,
        data ? data : "-"
    );

    if (write(fd, line, n) != n)
    {
        perror("write journal");
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);

    printf("[JRN] LOG    TX#%d %s %s\n",
           tx, op, file);

    return 0;
}


/* ---------------- COMMIT ---------------- */

int append_commit(int tx)
{
    int fd;

    fd = open(JOURNAL,
              O_WRONLY | O_CREAT | O_APPEND,
              0644);

    if (fd < 0)
    {
        perror("open journal");
        return -1;
    }

    char line[128];

    int n = snprintf(
        line,
        sizeof(line),
        "TX%d COMMIT\n",
        tx
    );

    if (write(fd, line, n) != n)
    {
        perror("write commit");
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);

    printf("[JRN] COMMIT TX#%d\n", tx);

    return 0;
}


/* ---------------- FILESYSTEM OPERATION ---------------- */

int apply_operation(const char *op,
                    const char *file,
                    const char *data)
{
    int flags = O_WRONLY | O_CREAT;

    if (strcmp(op, "create") == 0)
    {
        flags |= O_TRUNC;
    }
    else if (strcmp(op, "write") == 0)
    {
        flags |= O_APPEND;
    }
    else
    {
        printf("Unknown operation\n");
        return -1;
    }

    int fd = open(file, flags, 0644);

    if (fd < 0)
    {
        perror("open target");
        return -1;
    }

    if (data && strcmp(data, "-") != 0)
    {
        size_t len = strlen(data);

        write(fd, data, len);
        write(fd, "\n", 1);
    }

    fsync(fd);
    close(fd);

    printf("[FS ] APPLY  %s %s\n",
           op, file);

    return 0;
}


/* ---------------- RECOVERY ---------------- */

void recover(void)
{
    FILE *fp = fopen(JOURNAL, "r");

    if (!fp)
    {
        printf("[REC] No journal found.\n");
        return;
    }

    struct Tx
    {
        int tx;
        int committed;

        char op[32];
        char file[128];
        char data[256];

    } txs[100];

    int count = 0;

    char line[512];

    while (fgets(line, sizeof(line), fp))
    {
        int tx;

        char op[32];
        char file[128];
        char data[256];

        if (sscanf(
                line,
                "TX%d LOG %31s %127s %255s",
                &tx,
                op,
                file,
                data) == 4)
        {
            if (count < 100)
            {
                txs[count].tx = tx;
                txs[count].committed = 0;

                strcpy(txs[count].op, op);
                strcpy(txs[count].file, file);
                strcpy(txs[count].data, data);

                count++;
            }
        }
        else if (sscanf(
                     line,
                     "TX%d COMMIT",
                     &tx) == 1)
        {
            for (int i = 0; i < count; i++)
            {
                if (txs[i].tx == tx)
                {
                    txs[i].committed = 1;
                }
            }
        }
    }

    fclose(fp);

    int found = 0;

    for (int i = 0; i < count; i++)
    {
        if (!txs[i].committed)
        {
            found = 1;

            printf(
                "[REC] Incomplete TX#%d found. Replaying...\n",
                txs[i].tx
            );

            if (apply_operation(
                    txs[i].op,
                    txs[i].file,
                    txs[i].data) == 0)
            {
                append_commit(txs[i].tx);
            }
        }
    }

    if (!found)
    {
        printf(
            "[REC] No incomplete transactions. Filesystem is consistent.\n"
        );
    }
}


/* ================================================= */
/* CO-1 : BASIC JOURNAL + POSIX FILE I/O             */
/* ================================================= */

void core_demo(void)
{
    printf(
        "JournalFS - CO1 Core / POSIX File I/O\n\n"
    );

    append_journal(
        1,
        "create",
        TARGET,
        "-"
    );

    apply_operation(
        "create",
        TARGET,
        NULL
    );

    append_commit(1);


    append_journal(
        2,
        "write",
        TARGET,
        "Hello_JournalFS"
    );

    apply_operation(
        "write",
        TARGET,
        "Hello_JournalFS"
    );

    append_commit(2);
}


/* ================================================= */
/* CO-2 : PROCESS CREATION AND SYNCHRONIZATION      */
/* ================================================= */

void co2_demo(void)
{
    printf(
        "JournalFS - CO2 Process Control\n\n"
    );

    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        exit(1);
    }


    /* CHILD PROCESS */

    if (pid == 0)
    {
        printf(
            "[CHILD] PID=%d executing transaction...\n",
            getpid()
        );

        append_journal(
            10,
            "write",
            TARGET,
            "CO2_Process_Data"
        );

        apply_operation(
            "write",
            TARGET,
            "CO2_Process_Data"
        );

        append_commit(10);

        printf(
            "[CHILD] Transaction completed.\n"
        );

        fflush(stdout);

        _exit(0);
    }


    /* PARENT PROCESS */

    else
    {
        printf(
            "[PARENT] Child PID=%d created. Waiting...\n",
            pid
        );

        int status;

        waitpid(
            pid,
            &status,
            0
        );

        if (WIFEXITED(status))
        {
            printf(
                "[PARENT] Child exited with status %d.\n",
                WEXITSTATUS(status)
            );
        }
    }
}


/* ================================================= */
/* CO-3 : PIPE + SIGNAL + CRASH SIMULATION          */
/* ================================================= */

void co3_demo(void)
{
    printf(
        "JournalFS - CO3 IPC: Pipe + Signal\n\n"
    );

    int pipefd[2];

    if (pipe(pipefd) < 0)
    {
        perror("pipe");
        exit(1);
    }


    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        exit(1);
    }


    /* ---------------- CHILD ---------------- */

    if (pid == 0)
    {
        close(pipefd[0]);

        signal(
            SIGUSR1,
            handle_crash
        );

        printf(
            "[CHILD] PID=%d logging transaction...\n",
            getpid()
        );

        append_journal(
            20,
            "write",
            TARGET,
            "CO3_IPC_Data"
        );


        /* Send message to parent */

        const char *msg = "LOG_DONE";

        write(
            pipefd[1],
            msg,
            strlen(msg) + 1
        );

        close(pipefd[1]);

        printf(
            "[CHILD] Waiting for parent signal...\n"
        );

        pause();


        /* Signal received */

        if (crash_requested)
        {
            printf(
                "[CHILD] SIGUSR1 received. "
                "Simulating crash before COMMIT.\n"
            );

            fflush(stdout);

            _exit(2);
        }
    }


    /* ---------------- PARENT ---------------- */

    else
    {
        close(pipefd[1]);

        char msg[32] = {0};

        read(
            pipefd[0],
            msg,
            sizeof(msg)
        );

        close(pipefd[0]);

        printf(
            "[PARENT] Pipe message: %s\n",
            msg
        );


        printf(
            "[PARENT] Sending SIGUSR1 to child PID=%d...\n",
            pid
        );

        kill(
            pid,
            SIGUSR1
        );


        int status;

        waitpid(
            pid,
            &status,
            0
        );


        if (WIFEXITED(status))
        {
            printf(
                "[PARENT] Child exited with status %d.\n",
                WEXITSTATUS(status)
            );
        }


        printf(
            "\n[REC] Starting recovery...\n"
        );

        recover();
    }
}


/* ================================================= */
/* MAIN                                             */
/* ================================================= */

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf(
            "Usage: %s core | co2 | co3 | recover\n",
            argv[0]
        );

        return 1;
    }


    if (strcmp(argv[1], "core") == 0)
    {
        core_demo();
    }

    else if (strcmp(argv[1], "co2") == 0)
    {
        co2_demo();
    }

    else if (strcmp(argv[1], "co3") == 0)
    {
        co3_demo();
    }

    else if (strcmp(argv[1], "recover") == 0)
    {
        recover();
    }

    else
    {
        printf(
            "Unknown mode.\n"
        );

        return 1;
    }

    return 0;
}
