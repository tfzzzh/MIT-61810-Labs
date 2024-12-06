/*
reads lines from standard input, then runs commands for each line
 $ echo hello too | xargs echo bye
   bye hello too
*/
#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

char inputs[512]; 
int len;

// locate position of char c after start when not found 
// strlen is returned
// we assume that the str not contains ' '
int locate(char * str, int start, char c)
{
    while (str[start] != '\0' && str[start] != c) start += 1;
    return start;
}

char * get_substr(char *str, int start, int end)
{
    if (start > end) return 0;

    int len = end - start + 1;
    char * substr = (char *) malloc(len + 1);
    memcpy(substr, str+start, len);
    substr[len] = '\0';
    return substr;
}

// assumption argc_before_io <= MAXARG
void envoke_task(
    char * command, // shall be argv[1]
    char ** args, // pas argv
    int argc_before_io,
    int remain_arg_start,
    int remain_arg_end
) {
    char * argv_to_pass[MAXARG];
    memset(argv_to_pass, 0, MAXARG);

    for (int i=0; i < argc_before_io; ++i) {
        argv_to_pass[i] = args[i+1]; 
    }
    argv_to_pass[argc_before_io] = get_substr(inputs, remain_arg_start, remain_arg_end);

    // envoke subtask
    int pid = fork();
    // parent
    if (pid > 0) {
        // main task, wait for child to complete
        wait((int *) 0);
    }
    // child
    else if (pid == 0) {
        // use exec to execute the command
        int status = exec(command, argv_to_pass);
        if (status < 0) {
            fprintf(2, "execute command error %d\n", pid);
            exit(1);
        }
        exit(0);
    }
    // error
    else {
        fprintf(2, "fork error with code %d\n", pid);
        exit(1);
    }
}

int main(int argc, char* argv[])
{
    if(argc <= 1){
        fprintf(2, "xargs: usage xargs command [args]\n");
        exit(1);
    }

    // read data into inputs
    int len = 0, read_bytes = 0;
    while ( (read_bytes = read(0, inputs+len, sizeof(inputs)-len) ) != 0)
    {   
        len += read_bytes;
    }

    // command to long
    if (len >= 512) {
        fprintf(2, "xargs: input shall less than 512\n");
        exit(1);
    }

    if (inputs[len] != '\0') {
        fprintf(2, "inputs not end with 0\n");
        exit(1);
    }

    // printf("inputs: %s\n", inputs);

    int start = 0;
    // int num_envoke = 0;
    while (start < len) {
        int pos = locate(inputs, start, '\n');
        // now the parameter is at [start, pos-1]
        envoke_task(argv[1], argv, argc-1, start, pos-1);

        start = pos + 1;
        // num_envoke += 1;
    }
    // printf("total envoke: %d\n", num_envoke);

    exit(0);
}