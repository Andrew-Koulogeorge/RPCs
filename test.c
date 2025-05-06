/**
 * Writing simple test cases for the 3 functions needed for checkpoint 2
 * Make sure before running this file to test our interposed functions that you 
 * tell the linker how to assign symbols to these functions and not the linux OS functions
*/

#include <fcntl.h> 
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "../include/dirtree.h" 

#define MAXMSGLEN 1024

int main(){
    // int fd;
    
    // printf("TEST1: open and close a file\n");
    
    // fd = open("./foo", O_RDWR); 
    // close(fd);

    // printf("TEST2: create a file with open, write to it, and close it \n");
    
    // fd = open("./foobar", O_RDWR|O_CREAT, 0666); 
    // char msg[] = "Code works!\n";
    // write(fd, msg, strlen(msg));
    // close(fd);

    // printf("TEST3: open a file, read, lseek, read again \n");

    // fd = open("./foo", O_RDWR); 
    // char buffer[MAXMSGLEN];
    // read(fd, buffer, MAXMSGLEN);
    // printf("Read this from the file: %s\n", buffer);

    // lseek(fd, 1, SEEK_SET); // should read from the file 1 byte afterwards
    // read(fd, buffer, MAXMSGLEN);
    // printf("Read this from the file after calling lseek: %s\n", buffer);
    // close(fd);
    
    // printf("TEST4: getdirtree\n");
    // getdirtree("/afs/andrew.cmu.edu/usr8/akouloge/private/15440/15440-p1/interpose");
    // getdirtree("/afs/andrew.cmu.edu/usr8/akouloge/private/15440/15440-p1/test_root");
    // getdirtree("/afs/andrew.cmu.edu/usr8/akouloge/private/15440");

    // printf("TEST5: stat\n");    

    // struct stat file_stat;
    // printf("size of stat struct:%ld\n", sizeof(struct stat));
    // stat("./foo", &file_stat);
    // printf("Device ID: %ld\n", (long)file_stat.st_dev); // ID of device containing file
    // printf("Inode number: %ld\n", (long)file_stat.st_ino); // Inode number
    // printf("File mode: %o\n", file_stat.st_mode); // File type and permissions (octal)
    // printf("Number of hard links: %ld\n", (long)file_stat.st_nlink); // Number of hard links
    // printf("User ID of owner: %ld\n", (long)file_stat.st_uid); // User ID of owner
    // printf("Group ID of owner: %ld\n", (long)file_stat.st_gid); // Group ID of owner
    // printf("Device ID (if special file): %ld\n", (long)file_stat.st_rdev); // Device ID (if special file)
    // printf("File size: %ld bytes\n", (long)file_stat.st_size); // Total size in bytes
    // printf("Block size: %ld bytes\n", (long)file_stat.st_blksize);
    // printf("Allocated blocks: %ld\n", (long)file_stat.st_blocks);

    printf("TEST 6: getdirentries\n");
    int fd = open("/afs/andrew.cmu.edu/usr8/akouloge/private/15440/15440-p1", O_RDONLY); 
    off_t basep = 0;
    char buf[100];
    ssize_t num_bytes_read = getdirentries(fd, buf, 100, &basep);
    exit(0);
}

// is this a problem with close or with doing more then one call to the sever? --> Making more then one connections
