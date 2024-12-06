#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char buff1[512];
char buff2[64];

// search file in the path, assumption: path is a dir
void find(char * path, char * name, char * target) {
    int fd;
    if((fd = open(path, O_RDONLY)) < 0){
        fprintf(2, "find: cannot open %s\n", path);
        exit(1);
    }

    struct stat st;
    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        exit(1);
    }

    // when current path is a file compare name with taget
    if (st.type == T_FILE) {
        if (strcmp(name, target) == 0) {
            printf("%s\n", path);
        }

        close(fd);
        return;
    }

    // if (st.type != T_DIR) {
    //     fprintf(2, "find: %s is not a directory\n", path);
    //     close(fd);
    //     exit(1);
    // }
    // for device nothing shall done
    if (st.type == T_DEVICE) {
        close(fd);
        return;
    }

    // assumption path is large enough
    struct dirent de;
    int path_len = strlen(path);
    while(read(fd, &de, sizeof(de)) == sizeof(de))
    {
        if (strcmp(de.name, ".") == 0) continue;
        if (strcmp(de.name, "..") == 0) continue;
        if (de.inum == 0) continue;

        // resursive search 
        strcpy(path+path_len, "/");
        strcpy(path+path_len+1, de.name);
        find(path, de.name, target);

        path[path_len] = '\0';
    }

    close(fd);
}


int
main(int argc, char *argv[])
{

  if(argc != 3){
    fprintf(2, "usage: find directory filename \n");
    exit(1);
  }

  // copy directory into buffer
  strcpy(buff1, argv[1]);
  strcpy(buff2, argv[2]);
  find(buff1, "", buff2);

  exit(0);
}
