#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define NFORK 10
#define IO 5

int main()
{
    int n, pid;
    int wtime, rtime;
    int twtime = 0, trtime = 0;

    #ifdef RR
        printf("Round Robin Scheduling\n");
    #endif

    #ifdef FCFS
        printf("First Come First Serve Scheduling\n");
    #endif

    #ifdef LBS
        printf("Lottery Based Scheduling\n");
    #endif
    
    #ifdef PBS
        printf("Priority Based Scheduling\n");
    #endif

    #ifdef MLFQ
        printf("Multi Level Feedback Queue Scheduling\n");
    #endif

/*
    #ifdef FCFS
    pid = fork();

    if (pid < 0)
        printf("fork error\n");

    if (pid == 0)
    {
        // FCFS scheduling cannot execute other processes
        // Due to non-preemptive nature of FCFS
        printf("Infinte loop child\n");
        for (;;); // infinite loop
        exit(0);
    }

    else
    {
    #endif
*/

    #ifdef LBS
    settickets(10);
    #endif

    for (n = 0; n < NFORK; n++)
    {
        pid = fork();
            
        if (pid < 0)
            break;

        if (pid == 0)
        {
#ifndef FCFS
            if (n < IO)
                sleep(200); // IO bound processes

            else
            {
#endif
                #ifdef LBS
                
                if (n == 7)
                    settickets(100); // Will only matter for LBS, set higher tickets for IO bound processes
                
                else
                    settickets(1);

                sleep(200); // Make the process a combo of CPU and IO bound
                #endif

                for (volatile int i = 0; i < 1000000000; i++)
                {
                } // CPU bound process
#ifndef FCFS
            }
#endif
            printf("Process %d finished\n", n);
            exit(0);
        }

        else
        {
#ifdef PBS
                set_priority(80, pid); // Will only matter for PBS, set lower priority for IO bound processes
#endif
        }
    }

    for (; n > 0; n--)
    {
        if (waitx(0, &rtime, &wtime) >= 0)
        {
            trtime += rtime;
            twtime += wtime;
        }
    }
    printf("Average rtime %d,  wtime %d\n", trtime / NFORK, twtime / NFORK);

    exit(0);
}