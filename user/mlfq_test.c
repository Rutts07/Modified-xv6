#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define NFORK 5

int main()
{
    #ifdef MLFQ
    int n, pid;
    int wtime, rtime;

    #ifdef MLFQ
        printf("Multi Level Feedback Queue Scheduling\n");
    #endif

    for (n = 0; n < NFORK; n++)
    {
        pid = fork();
            
        if (pid < 0)
            break;

        if (pid == 0)
        {
            for (volatile int i = 0; i < 1000000000; i++) {};
            printf("Process %d finished\n", n);

            exit(0);
        }
    }

    for (; n > 0; n--)
        waitx(0, &rtime, &wtime);

    exit(0);
    #endif

    printf("Works only MLFQ Scheduling\n");
    exit(0);
}