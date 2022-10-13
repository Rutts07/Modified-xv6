#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    #ifndef PBS
    fprintf(2, "usage: setpriority priority pid - scheduler must be priority based\n");
    exit(1);
    #endif

    int priority;
    int pid;
    if (argc < 2)
    {
        fprintf(2, "usage: setpriority priority pid - atleast 3 arguments required\n");
        exit(1);
    }

    // check if the priority is an integer
    int isnumber = 1;
    for (int i = 0; i < strlen(argv[1]); i++)
    {
        if (argv[1][i] < '0' || argv[1][i] > '9')
        {
            isnumber = 0;
            break;
        }
    }

    if (isnumber != 1)
    {
        fprintf(2, "Usage: setpriority priority pid - priority must be an integer\n");
        exit(1);
    }

    priority = atoi(argv[1]);

    // check if the pid is an integer
    isnumber = 1;
    for (int i = 0; i < strlen(argv[2]); i++)
    {
        if (argv[2][i] < '0' || argv[2][i] > '9')
        {
            isnumber = 0;
            break;
        }
    }

    if (isnumber != 1)
    {
        fprintf(2, "Usage: setpriority priority pid - pid must be an integer\n");
        exit(1);
    }

    pid = atoi(argv[2]);

    printf("setpriority: %d\n", set_priority(priority, pid));
    exit(0);
}