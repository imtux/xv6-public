#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  return buf;
}

void
ls(char *path)
{
  char permission[10] = "-rwxrwx";
  char buf[512], *p;
  int fd;
  uint i;
  struct dirent de;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    printf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    printf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  for(i = 0; i < 6; i++){
    int bit = st.permission & (1 << i);
    if(bit == 0)
      permission[6-i] = '-';
  }

  switch(st.type){
  case T_FILE:
    permission[0] = '-';
    printf(1, "%s %s %d %d %d %s\n", permission, st.owner, st.type, st.ino, st.size, fmtname(path));
    break;

  case T_DIR:
    permission[0] = 'd';
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf(1, "ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf(1, "ls: cannot stat %s\n", buf);
        continue;
      }
      if(st.type == T_DEV){
        printf(1, "%d %d %d %s\n", st.type, st.ino, st.size, fmtname(buf));
      }
      else{
        char childPermission[10] = "drwxrwx";
        childPermission[0] = (st.type == T_DIR) ? 'd' : '-';
        for(i = 0; i < 6; i++){
          int bit = st.permission & (1 << i);
          if(bit == 0)
            childPermission[6-i] = '-';
        }
        printf(1, "%s %s %d %d %d %s\n", childPermission, st.owner, st.type, st.ino, st.size, fmtname(buf));
      }
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit();
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit();
}
