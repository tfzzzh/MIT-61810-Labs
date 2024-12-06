#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


typedef struct Record {
    // process to send a number
    int child_pid;
    // prime to print for current process
    int filter_num;
    // reading from its parents
    // when read_pipe == -1 => at main process
    // the numnber shall range from 2 to n
    int read_pipe;
    int recv_num;
    // sending to its child
    int write_pipe;
    int send_num;

} Record;

// memory handler, when a task create, so is the record.
// each process shall handle the record create by it
Record * create_record() {
    return (Record*) malloc(sizeof(Record));
}

void delete_record(Record **record) {
    free(*record);
    *record = 0;
}

void init_record(Record *record) {
    record->child_pid = -1;
    record->filter_num = 0;
    record->read_pipe = -1;
    record->write_pipe = -1;
}


// get next number to filter from its parent
// void get_next_number(Record *record) {
//     int read_byte = read(record->read_pipe, &(record->recv_num), sizeof(int));
//     if (read_byte == 0) {
//         close(record->write_pipe);
//         return;
//     }


// }

// void print_number(Record *record, int num) {
//     printf("prime %d\n", num);
//     record->filter_num = num;
// }
void create_sub_task(Record * record, int sub_filter_num);
void check_and_push(Record * record, int num) {
    // when subtask is null create a new subtask using num as flter
    if (record->write_pipe == -1) {
        create_sub_task(record, num);
    }
    // else push num to the pip of the subtask
    else {
        record->send_num = num;
        write(record->write_pipe, &(record->send_num), sizeof(int));
    }
}

void task(Record *record) {
    // print the filter num
    printf("prime %d\n", record->filter_num);

    // loop over get next number and push it to child
    // int arr[150];
    // int len = 0, x = -1;
    int x;
    while (read(record->read_pipe, &record->recv_num, sizeof(record->recv_num)) != 0) {
        x = record->recv_num;
        if (x % record->filter_num == 0) continue;
        // varr[len++] = x;
        check_and_push(record, x);
    }

    // when main task end close the pipe
    close(record->read_pipe);
    // printf("close pipe %d\n", record->read_pipe);

    // for (int i=0; i < len; ++i) {
    //     check_and_push(record, arr[i]);
    // }

    // if (len > 0) {
    //     // printf("close pipe %d\n", record->write_pipe);
    //     close(record->write_pipe);
    // }

    // wait for child to complete
    if (record->child_pid != -1) {
        close(record->write_pipe);
        wait((int *) 0);
    }
    // destroy record
    // delete_record(&record);
}

void main_task() {
    Record * record = create_record();
    init_record(record);

    // print 2
    record->filter_num = 2;
    printf("prime %d\n", record->filter_num);
    
    for (int x = record->filter_num+1; x <= 280; ++x) {
        if (x % record->filter_num == 0) continue;
        check_and_push(record, x);
    }
    
    close(record->write_pipe);
    if (record->child_pid != -1) {
        wait((int *) 0);
    }

    delete_record(&record);
}

// create a subprocess to filter remaining numbers, the main task only
// need to the port to sent msg to the child, 
void create_sub_task(Record * record, int sub_filter_num) {
    int ports[2];
    int status = pipe(ports);
    // printf("open pipe %d, %d\n", ports[0], ports[1]);
    if (status != 0) {
        fprintf(2, "open pipe with error code %d\n", status);
        exit(1);
    }

    // fork child process
    int pid = fork();
    // parent
    if (pid > 0) {
        // main task, close read port, kepp write port to subtask
        // printf("pid=%d\n", pid);
        close(ports[0]);
        record->write_pipe = ports[1];
        record->child_pid = pid;
    }
    // child
    else if (pid == 0) {
        // subtask, close write port, keep read port to the main task
        // destory record of the main task since it is in child process
        close(ports[1]);
        // close port in record
        // if (record->read_pipe != -1)
        //     close(record->read_pipe);
        // delete_record(&record);
        if (record->read_pipe != -1)
            close(record->read_pipe);
        // record = create_record();
        init_record(record);
        record->read_pipe = ports[0];
        record->filter_num = sub_filter_num;
        // if (getpid() == 14) printf("into task, filter_num = %d\n", record->filter_num);
        task(record);
        // delete_record(&record); // task done
        exit(0);
    }
    // error
    else {
        fprintf(2, "fork error with code %d\n", pid);
        exit(1);
    }
}


// void fork_until_broke() {
    
// }

int main(int argc, char* argv[])
{
    main_task();
    // create pipe until broke
    // int ports[2];
    // int status = pipe(ports);
    // while (status == 0) {
    //     printf("open pipe %d, %d\n", ports[0], ports[1]);
    //     status = pipe(ports);
    // }
    // fprintf(2, "open pipe with error code %d\n", status);
    exit(0);
}