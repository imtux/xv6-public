#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;
  char username[50] = { '\0', };
  char password[50] = { '\0', };

  for(;;) 
  {
    printf(1, "[Login]\n");
    printf(1, "Username: ");
    gets(username, sizeof username);
    username[strlen(username) - 1] = 0;
    printf(1, "Password: ");
    gets(password, sizeof password);
    password[strlen(password) - 1] = 0;

    if(verify(username, password) == 0){
      pid = fork();
      if(pid < 0){
        printf(1, "verify: fork failed\n");
        exit();
      }
      if(pid == 0){
        exec("sh", argv);
        printf(1, "verify: exec sh failed\n");
        exit();
      }
      while((wpid = wait()) >= 0 && wpid != pid)
        printf(1, "zombie!\n");
    }
    else{
      printf(2, "Sorry, again.\n");
    }
  }
}