#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc < 2) 
    {
        fprintf(2, "Usage: strace mask command [args] - atleast 3 arguments required\n");
        exit(1);
    }

    // check if the mask is an integer
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
        fprintf(2, "Usage: strace mask command [args] - mask must be an integer\n");
        exit(1);
    }

    int trace_mask = atoi(argv[1]);
    trace(trace_mask);

    // copy the rest of the arguments to the new argv
    // 0-strace 1-[trace_mask] 2-[command] 3-[args]
    char **args = malloc((argc - 1) * sizeof(char*));

    for (int i = 0; i < argc - 2; i++)
        args[i] = argv[i + 2];

    args[argc - 2] = 0;
    exec(args[0], args);

    fprintf(2, "exec %s failed\n", args[0]);
    exit(0);
}