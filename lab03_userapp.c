#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
    int pid = fork();
    while (1){
        if(!pid){
            printf(1, "Child : %d %d\n", getpid(), getLevel());
            yield();
        } 
        else if(pid > 0){
            printf(1, "Parent : %d %d\n", getpid(), getLevel());
            yield();
        }
        else{
            printf(1, "Error\n");
            yield();
        }
    }
    exit();
};