#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        fprintf(2, "Usage: clear - no arguments required\n");
        exit(1);
    }

    printf("\033[H\033[2J");
    exit(0);    
}