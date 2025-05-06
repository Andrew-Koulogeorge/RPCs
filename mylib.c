/**
 * @author Andrew Koulogeorge
 * CMU 15440: Distributed Systems
 * Interposition Library that implements common linux functions as Remote Procedure Calls
 * 
 * The interposed functions are identical to those in the standard C library. Indeed,
 * the server code physically executing the commands remotly are calling exactly those functions. As a
 * result, please see the corresponding man7 pages (https://man7.org) for details about the functions
 * arguments, return values, ect
*/

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <assert.h>
#include <stdbool.h>
#include "../include/dirtree.h" 

#define MAXMSGLEN 100 	// temporary storage size for reading bytes from TCP connection
#define FD_OFFSET 10000 // used to manage fd translation between client and server
#define OPCODE_BYTE 1 	// each serialized data packet contains a byte indicating which RPC to call
#define NUMARGS_BYTE 1 	// open has variable number of args
#define NULL_TERM 1 	// include \0 when copying strings to a buffer

int sockfd; // global socket to communicate with server during lifetime

/* Helper functions for managing getdirtree function */
size_t compute_packet_size(size_t num_args, size_t *args_length, size_t args_num_bytes);
struct dirtreenode *decode_dirtree(void *return_packet);
int recursive_decode(struct dirtreenode *node, void *packet, int idx);
void recursive_free(struct dirtreenode *node);

/* Declare function pointers with the same prototype as the original function */
int (*orig_open)(const char *pathname, int flags, ...);  // mode_t mode is needed when flags includes O_CREAT; variable num of args
int (*orig_close)(int fd); 
ssize_t (*orig_read)(int fd, void *buf, size_t count);
ssize_t (*orig_write)(int fd, const void *buf, size_t count);
off_t (*orig_lseek)(int fd, off_t offset, int whence);
int (*orig_stat)(const char *pathname, struct stat *statbuf);
ssize_t (*orig_getdirentries)(int fd, char *buf, size_t nbytes, off_t *basep);
struct dirtreenode* (*orig_getdirtree)(char *path);
void* (*orig_freedirtree)(struct dirtreenode *treenode);
int (*orig_unlink)(const char *pathname);

/* op-codes for each function are encoded in the packet informs which function is being called by the client*/
enum OPCODES {
	open_opcode = 0,
	write_opcode = 1,
	close_opcode = 2,
	read_opcode = 3,
	lseek_opcode = 4,
	stat_opcode = 5, 
	getdirentries_opcode = 6,
	getdirtree_opcode = 7,
	unlink_opcode = 8
};

/* Wrapper function for malloc that automatically handles errors */
void *xMalloc(size_t nbytes){
	void *ptr = malloc(nbytes);
	if(ptr == NULL){
		fprintf(stderr, "Malloc failed...\n");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

					/* INTERPOSED RPC LIB */

int open(const char *pathname, int flags, ...) {
	fprintf(stderr, "mylib: open called for path %s\n", pathname);

	char *packet; 							// packet to send over network
	char length_buffer[MAXMSGLEN];  		// buffer to write length of packet into
	size_t num_args = 2; 					// lower bound on number of args
	
	/* Size of args for packet */
	size_t flags_length = sizeof(flags);
	size_t m_length = 0;
	size_t pathname_length = strlen(pathname) + NULL_TERM; // include the null terminator

	mode_t m=0;
	if (flags & O_CREAT) { 
		va_list a;
		va_start(a, flags); 				// tells va_start where to look for variable length args
		m = va_arg(a, mode_t);
		va_end(a);
		num_args++;
		m_length = sizeof(m);	
	}
	
	/* Prepare Packet */
	size_t num_bytes_payload = OPCODE_BYTE + NUMARGS_BYTE + flags_length + m_length + pathname_length; // number of bytes needed to encode payload
	sprintf(length_buffer, "%zu", num_bytes_payload); // write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; // number of bytes needed to store the length string at start (include \0)
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); 

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, open_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 			// 1; byte for opcode
	memcpy(packet + idx, &num_args, NUMARGS_BYTE); idx += NUMARGS_BYTE; 			// 2; byte for numa args (only need this in read bc variable args)
	memcpy(packet + idx, &flags, flags_length); idx += flags_length;				// 3; write in flag variable
	if(num_args == 3){
		memcpy(packet + idx, &m, m_length); idx += m_length; 						// 3.5; if flag present, write in m variable	
	}
	memcpy(packet + idx, pathname, pathname_length); idx += pathname_length; 		// 4; write in pathname 

	send(sockfd, packet, packet_size, 0); 						// send packet using (global) socket connection
	free(packet);
	
	/* Read Return Packet */
	size_t return_size = sizeof(int) + sizeof(errno); 			// open return value and errno both ints
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; 							// buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0) // reading bytes in from tcp stream
	{
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}

	/* Decode Args*/
	idx = 0; 
	int return_val = *((int*)(return_packet + idx)); idx += sizeof(int);

	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}

	fprintf(stderr,"Error description from errno: %s\n\n", strerror(errno)); // handle the errno
	free(return_packet);
	return return_val; // return RPC's return value
}

ssize_t write(int fd, const void *buf, size_t count){
	fprintf(stderr, "mylib: write called for %ld bytes from file %d\n", count, fd);
	if(fd == sockfd){ // do not let the user do anything with libs connection to server
		errno = EBADF;
		return -1;
	}

	if(fd < FD_OFFSET){ // pass the call to normal lib if we see that the fd passed in was not given by server
		return orig_write(fd, buf, count);
	} 
	char *packet; 					// packet to send over network
	char length_buffer[MAXMSGLEN]; 	// buffer to write length of packet into
	
	/* Size of args for packet */
	size_t fd_length = sizeof(fd);
	size_t count_length = sizeof(count);
	size_t buf_length = count; 		// writes up to count bytes

	/* Prepare Packet */
	size_t num_bytes_payload = OPCODE_BYTE + fd_length + count_length + buf_length; 
	sprintf(length_buffer, "%zu", num_bytes_payload); 
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; 
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); 

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, write_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 			// 1; byte for opcode
	memcpy(packet + idx, &fd, fd_length); idx += fd_length; 			 			// 2; fd
	memcpy(packet + idx, &count, count_length); idx += count_length; 				// 3; write in count
	memcpy(packet + idx, buf, buf_length); idx += buf_length; 						// 4; write buffer

	/* Send Packet to Sever */
	send(sockfd, packet, packet_size, 0); 
	free(packet);

	/* Receive Incoming Packet */
	size_t return_size = sizeof(int) + sizeof(size_t); // write returns size_t; errno is type int
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; 					// buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}
	
	/* Decode Incoming Packet */
	idx = 0;
	ssize_t return_val = *((ssize_t*)(return_packet + idx)); idx += sizeof(return_val);
	
	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}

	free(return_packet);
	return return_val; // return RPC's return value
}

int close(int fd){
	fprintf(stderr, "mylib: close called for file:  %d\n", fd);
	if(fd == sockfd){ // dont let the user do anything with connection to server
		errno = EBADF;
		return -1;
	}

	if(fd < FD_OFFSET){ // pass the call to normal lib
		return orig_close(fd);
	} 

	char *packet; 					// packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Size of args for packet */
	size_t fd_length = sizeof(fd);

	/* Prepare Packet */
	size_t num_bytes_payload = OPCODE_BYTE + fd_length;
	sprintf(length_buffer, "%zu", num_bytes_payload); // write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; // number of bytes needed to store the length at start (include \0)
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); // allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); 	idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, close_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 			 // 1; byte for opcode
	memcpy(packet + idx, &fd, fd_length); idx += fd_length; 						 // 2; write in fd 

	/* Send Packet to Sever */
	send(sockfd, packet, packet_size, 0); 
	free(packet);

	/* Receive Incoming Packet */
	size_t return_size = sizeof(int) + sizeof(int); // write returns size_t; errno is type int
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}

	/* Decode Incoming Packet */
	idx = 0;
	size_t return_val = *((int*)(return_packet + idx)); idx += sizeof(size_t);
	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}

	free(return_packet);
	return return_val; // return RPC's return value	
}

ssize_t read(int fd, void *buf, size_t count){
	fprintf(stderr, "mylib: read called for %ld bytes from file %d\n", count, fd);
	if(fd == sockfd){ // dont let the user do anything with connection to server
		errno = EBADF;
		return -1;
	}

	if(fd < FD_OFFSET){ // pass the call to normal lib
		return orig_read(fd, buf, count);
	} 

	char *packet; 					// packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Size of args for packet */
	size_t fd_length = sizeof(fd);
	size_t count_length = sizeof(count);

	/* Prepare Packet */
	size_t num_bytes_payload = OPCODE_BYTE + fd_length + count_length; 	// number of bytes needed encode payload 
	sprintf(length_buffer, "%zu", num_bytes_payload); 					// write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; 		// number of bytes needed to store the length at start (include \0)
	packet = (char *)xMalloc(num_bytes_payload + num_bytes_length); 		// allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, read_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 			// 1; byte for opcode
	memcpy(packet + idx, &fd, fd_length); idx += fd_length; 						// 2; write in fd 
	memcpy(packet + idx, &count, count_length); idx += count_length; 		        // 3; write in count

	/* Send Packet to Sever */
	send(sockfd, packet, num_bytes_length + num_bytes_payload, 0); 
	free(packet);

	/* Receive Incoming Packet */
	size_t return_size = sizeof(int) + sizeof(size_t) + count; // write returns size_t; errno is type int
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}
	
	/* Decode Incoming Packet */
	idx = 0;
	ssize_t return_val = *((ssize_t*)(return_packet + idx)); idx += sizeof(return_val);

	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}
	idx += sizeof(errno); 					 // move pointer along no matter if error present as to read buf
	memcpy(buf, return_packet + idx, count); // count is the number of bytes that we are reading in from the file

	free(return_packet);
	return return_val; // return RPC's return value
}

off_t lseek(int fd, off_t offset, int whence){
	fprintf(stderr, "mylib: lseek called with an offset of %ld from file %d with mode of %d\n", offset, fd, whence);
	if(fd == sockfd){ // dont let the user do anything with connection to server
		errno = EBADF;
		return -1;
	}

	if(fd < FD_OFFSET){ // pass the call to normal lib
		return orig_lseek(fd, offset, whence);
	} 

	char *packet; 				   // packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Compute number of bytes required for each argument */ 
	size_t fd_length = sizeof(fd);
	size_t offset_length = sizeof(offset);
	size_t whence_length = sizeof(whence);

	/* Prepare Packet */
	size_t num_bytes_payload = OPCODE_BYTE + fd_length + offset_length + whence_length;
	sprintf(length_buffer, "%zu", num_bytes_payload); 			 // write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; // number of bytes needed to store the length at start (include \0)
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); 						 // allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, lseek_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; // 1; byte for opcode
	memcpy(packet + idx, &fd, fd_length); idx += fd_length; 			 // 2; write in fd 
	memcpy(packet + idx, &offset, fd_length); idx += offset_length; 	 // 3; write in offset
	memcpy(packet + idx, &whence, fd_length); idx += whence_length; 	 // 4; write in whence

	/* Send Packet to Sever */
	send(sockfd, packet, packet_size, 0); 
	free(packet);
	

	/* Receive Incoming Packet */
	size_t return_size = sizeof(off_t) + sizeof(int); // write returns size_t; errno is type int
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}
	/* Decode Incoming Packet */
	idx = 0;
	size_t return_val = *((int*)(return_packet + idx)); idx += sizeof(off_t);
	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}

	free(return_packet);
	return return_val; // return RPC's return value	
}

int stat(const char *pathname, struct stat *statbuf){
	fprintf(stderr, "mylib: stat called for path %s\n", pathname);

	char *packet; // packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Number of bytes required for each argument */ 
	size_t pathname_length = strlen(pathname) + 1; // include the null terminator
	
	size_t num_bytes_payload = OPCODE_BYTE + pathname_length; // number of bytes needed encode payload
	sprintf(length_buffer, "%zu", num_bytes_payload); // write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; // number of bytes needed to store the length at start (include \0)
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); // allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, stat_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 			// 1; byte for opcode
	memcpy(packet + idx, pathname, pathname_length); idx += pathname_length; 		// 2; write in pathname (\0 included)

	send(sockfd, packet, packet_size, 0); // send packet using (global) socket connection

	/* Read Return Packet */
	size_t return_size = sizeof(int) + sizeof(int) + sizeof(struct stat); // return value, errno, and stat struct
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}

	/* Decode Args */
	idx = 0;
	int return_val = *((int*)(return_packet + idx)); idx += sizeof(int);
	if(return_val == -1){ // if RPC failed, update errno
		errno = *((int*)(return_packet + idx)); 
	}
	idx += sizeof(int);
	
	/* Populate the stat data structure */
	memcpy(&(statbuf->st_dev), (return_packet + idx), sizeof(statbuf->st_dev)); idx += sizeof(statbuf->st_dev);
	memcpy(&(statbuf->st_ino), (return_packet + idx), sizeof(statbuf->st_ino)); idx += sizeof(statbuf->st_ino);
	memcpy(&(statbuf->st_mode), (return_packet + idx), sizeof(statbuf->st_mode)); idx += sizeof(statbuf->st_mode);
	memcpy(&(statbuf->st_nlink), (return_packet + idx), sizeof(statbuf->st_nlink)); idx += sizeof(statbuf->st_nlink);
	memcpy(&(statbuf->st_uid), (return_packet + idx), sizeof(statbuf->st_uid)); idx += sizeof(statbuf->st_uid);
	memcpy(&(statbuf->st_gid), (return_packet + idx), sizeof(statbuf->st_gid)); idx += sizeof(statbuf->st_gid);
	memcpy(&(statbuf->st_rdev), (return_packet + idx), sizeof(statbuf->st_rdev)); idx += sizeof(statbuf->st_rdev);
	memcpy(&(statbuf->st_size), (return_packet + idx), sizeof(statbuf->st_size)); idx += sizeof(statbuf->st_size);
	memcpy(&(statbuf->st_blksize), (return_packet + idx), sizeof(statbuf->st_blksize)); idx += sizeof(statbuf->st_blksize);
	memcpy(&(statbuf->st_blocks), (return_packet + idx), sizeof(statbuf->st_blocks)); idx += sizeof(statbuf->st_blocks);

	free(return_packet);
	return return_val; // return RPC's return value
}

ssize_t getdirentries(int fd, char *buf, size_t nbytes, off_t *basep){
	fprintf(stderr, "mylib: getdirentries called to read %ld bytes from fd %d\n", nbytes, fd);
	if(fd == sockfd){ // dont let the user do anything with connection to server
		errno = EBADF;
		return -1;
	}

	if(fd < FD_OFFSET){ // pass the call to normal lib
		return orig_getdirentries(fd, buf, nbytes, basep);
	} 
	char *packet; // packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Number of bytes required for each argument */ 
	size_t fd_length = sizeof(fd);
	size_t nbytes_length = sizeof(nbytes);
	size_t basep_length = sizeof(off_t); // important point; we are sending the value of basep over the network

	/* Allocate Packet */
	size_t num_bytes_payload = OPCODE_BYTE + fd_length + nbytes_length + basep_length;  // number of bytes needed encode payload 
	sprintf(length_buffer, "%zu", num_bytes_payload); 									// write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; 						// number of bytes needed to store the length at start (include \0)
	size_t packet_size = num_bytes_payload + num_bytes_length;
	packet = (char *)xMalloc(packet_size); 												// allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, getdirentries_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 	// 1; byte for opcode
	memcpy(packet + idx, &fd, sizeof(fd)); idx += sizeof(fd); 						// 2; fd to read from
	memcpy(packet + idx, &nbytes, sizeof(nbytes)); idx += sizeof(nbytes); 			// 3; number of bytes to read
	memcpy(packet + idx, basep, sizeof(off_t)); idx += sizeof(off_t); 				// 4; base pointer value

	/* Send Packet to Sever */
	send(sockfd, packet, packet_size, 0); 
	free(packet);

	/* Receive Incoming Packet */
	size_t return_size = sizeof(ssize_t) + sizeof(int) + nbytes + sizeof(off_t); // errno, num bytes read, actual data read, updated offset
	char *return_packet = xMalloc(return_size);
	char return_buffer[MAXMSGLEN]; 												 // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}

	/* Decode Incoming Packet */
	idx = 0;
	ssize_t return_val = *((ssize_t*)(return_packet + idx)); idx += sizeof(return_val); // return val = num bytes read
	if(return_val == -1){ 
		errno = *((int*)(return_packet + idx)); // if RPC failed, update errno
	}
	idx += sizeof(errno); 																// update index even if return valid
	*basep = *((off_t*)(return_packet + idx)); idx += sizeof(off_t); 					// write updated basep
	memcpy(buf, (return_packet + idx), nbytes); 										// count is the number of bytes that we are reading in from the file

	free(return_packet);
	return return_val; // return RPC's return value	
}

int unlink(const char *pathname){
	fprintf(stderr, "mylib: unlink called with pathname %s\n", pathname);
	
	char *packet; 				   // packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Number of bytes required for each argument */ 
	size_t pathname_length = strlen(pathname) + 1; // include the null terminator
	
	size_t num_bytes_payload = OPCODE_BYTE + pathname_length; 			// number of bytes needed encode payload
	sprintf(length_buffer, "%zu", num_bytes_payload); 					// write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; 		// number of bytes needed to store the length at start (include \0)
	packet = (char *)xMalloc(num_bytes_payload + num_bytes_length); 		// allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, unlink_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; // 1; byte for opcode
	memcpy(packet + idx, pathname, pathname_length); idx += pathname_length; // 2; write in pathname (\0 included)
	
	send(sockfd, packet, num_bytes_length + num_bytes_payload, 0); // send packet using (global) socket connection
	free(packet);
	
	/* Read Return Packet */
	size_t return_size = 2*sizeof(int); // open return value and errno both ints
	char *return_packet = xMalloc(return_size);
	char return_buffer[return_size]; // buffer to write length of packet into
	int rv;
	size_t total_bytes_rec = 0;
	while ( (rv=recv(sockfd, return_buffer, MAXMSGLEN, 0)) > 0){
		memcpy(return_packet+total_bytes_rec, return_buffer, rv);
		total_bytes_rec += rv;
		if(total_bytes_rec == return_size) break;
	}

	/* Decode Args*/
	idx = 0; 
	int unlink_return_status = *((int*)(return_packet + idx)); idx += sizeof(int);
	
	if(unlink_return_status == -1){ // only update the errno when the return val is -1
		errno = *((int*)(return_packet + idx)); 
		idx += sizeof(errno);
	}

	free(return_packet);
	return unlink_return_status; // return RPC's return value
}


struct dirtreenode* getdirtree(const char *path){
	fprintf(stderr, "mylib: getdirtree called for path %s\n", path);
	
	char *packet; // packet to send over network
	char length_buffer[MAXMSGLEN]; // buffer to write length of packet into
	
	/* Number of bytes required for each argument */ 
	size_t path_length = strlen(path) + 1; // include the null terminator

	size_t num_bytes_payload = OPCODE_BYTE + path_length; 			// number of bytes needed encode payload
	sprintf(length_buffer, "%zu", num_bytes_payload); 				// write number of bytes into a buffer as a string; decodable by server
	size_t num_bytes_length = strlen(length_buffer) + NULL_TERM; 	// number of bytes needed to store the length at start (include \0)
	packet = (char *)xMalloc(num_bytes_payload + num_bytes_length);  // allocate all bytes needed for packet

	/* Write Packet */
	size_t idx = 0; 
	memcpy(packet + idx, length_buffer, num_bytes_length); 	idx += num_bytes_length; // 0; total bytes sent
	memset(packet + idx, getdirtree_opcode, OPCODE_BYTE); idx += OPCODE_BYTE; 		 // 1; byte for opcode
	memcpy(packet + idx, path, path_length); idx += path_length; 					 // 2; write in pathname (\0 included)
		
	send(sockfd, packet, num_bytes_length + num_bytes_payload, 0); // send packet using (global) socket connection
	free(packet);
	
	/* Read Return Packet */
	char *return_packet = NULL;
	num_bytes_payload = 0;
	size_t total_bytes_expected = 0;
	bool found_length = false;
	idx = 0;
	int rv;

	/* Read in Packet */
	while ( (rv=recv(sockfd, length_buffer, MAXMSGLEN, 0)) > 0) // ASSUMING THIS WILL NOT SHORT SO MUCH THAT LENGTH DONT COME IN
	{
		if(!found_length){// allocate sufficent memory to read in packet
			num_bytes_payload = *((size_t *)length_buffer);
			total_bytes_expected = sizeof(size_t) + num_bytes_payload;
			return_packet = (char *)xMalloc(total_bytes_expected); // allocate memory to parse incoming packet
			found_length = true; 
		}
		memcpy(return_packet + idx, length_buffer, rv); 
		idx += rv;
		if(idx == total_bytes_expected) break;
	}

	errno = *(int*)((char*)return_packet + sizeof(size_t)); // set errno from packet
	struct dirtreenode *root_node = decode_dirtree(return_packet);
	
	free(return_packet);
	return root_node; // return RPC's return value 
}

/* Helper function to construct directory tree from binary packet sent from server */
struct dirtreenode *decode_dirtree(void *packet){
	struct dirtreenode *root_node = xMalloc(sizeof(struct dirtreenode)); 
	recursive_decode(root_node, packet, sizeof(size_t) + sizeof(errno));
	return root_node;
}

/* Helper function to traverse the encoded dirtree and recursively allocate memory for each node */
int recursive_decode(struct dirtreenode *node, void *packet, int idx){
	
	int name_length = *((int*)((char *)packet + idx)); idx += sizeof(int); 		// extract name length
	node->name = xMalloc(name_length); 											// allocate name and write into it
	memcpy(node->name, (char *)packet + idx, name_length); idx += name_length;
																				// write number of children into node
	memcpy(&(node->num_subdirs), (char *)packet + idx, sizeof(node->num_subdirs)); idx += sizeof(node->num_subdirs);
	node->subdirs = xMalloc(node->num_subdirs*sizeof(void*)); 					// allocate memory for children pointers

	for(int i = 0; i < node->num_subdirs; i++)				 // loop over number of children and recurse
	{
		struct dirtreenode *child_node = xMalloc(sizeof(struct dirtreenode));
		node->subdirs[i] = child_node; 					 	// inclde this node as a child to the parent
		idx = recursive_decode(child_node, packet, idx); 	// returning updated location in packet with idx
	}
	return idx;
}

/**
 * @brief Client can handle the freeing of directory tree memory locally; dont need to implement an RPC 
 * NOTE: We have to implement a custom function to free all the memory allocated by our local dirtree because
 * the manner in which we allocated memory during the dirtree's construction may be different from the shared
 * object files implementation
*/
void freedirtree(struct dirtreenode *treenode){
	fprintf(stderr, "mylib: freedirtree called\n");
	if(treenode == NULL){
		fprintf(stderr, "tried to free a null pointed treenode\n");	
		return;
	}
	recursive_free(treenode);
}

/* Helper function to free memory in same way that we allocated it*/
void recursive_free(struct dirtreenode *node){
	for(int i = 0; i < node->num_subdirs; i++) // loop over number of children and recurse
		recursive_free(node->subdirs[i]);
	free(node->name);
	free(node->subdirs);
	free(node);
}


/* This function is automatically called when program is started */
void _init(void) {
	// set function pointer orig_open to point to the original open function
	// returns a pointer to the old function in the linking table ? 
	fprintf(stderr, "Init mylib\n");
	orig_open = dlsym(RTLD_NEXT, "open"); 
	orig_close = dlsym(RTLD_NEXT, "close");
	orig_read = dlsym(RTLD_NEXT, "read");
	orig_write = dlsym(RTLD_NEXT, "write");
	orig_lseek = dlsym(RTLD_NEXT, "lseek");
	orig_stat = dlsym(RTLD_NEXT, "stat");
	orig_getdirentries = dlsym(RTLD_NEXT, "getdirentries");
	orig_getdirtree = dlsym(RTLD_NEXT, "getdirtree");
	orig_freedirtree = dlsym(RTLD_NEXT, "freedirtree");
	orig_unlink = dlsym(RTLD_NEXT, "unlink");

	// set up the connection with the server; store stocket in global var
	char *serverip;
	char *serverport;
	unsigned short port;
	int rv;
	struct sockaddr_in srv;
	
	// Get environment variable indicating the ip address of the server
	serverip = getenv("server15440");
	if (serverip){
		fprintf(stderr, "Got environment variable server15640: %s\n", serverip);
	} 
	else {
		fprintf(stderr, "Environment variable server15640 not found.  Using 127.0.0.1\n");
		serverip = "127.0.0.1";
	}
	
	// Get environment variable indicating the port of the server
	serverport = getenv("serverport15440");
	if (serverport){
		fprintf(stderr, "Got environment variable serverport15440: %s\n", serverport);
	} 
	else {
		fprintf(stderr, "Environment variable serverport15440 not found.  Using 15440\n");
		serverport = "15440";
	}
	port = (unsigned short)atoi(serverport);
	
	// Create socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);	// TCP/IP socket
	if (sockfd<0) err(1, 0);			// in case of error
	
	// setup address structure to point to server
	memset(&srv, 0, sizeof(srv));			// clear it first
	srv.sin_family = AF_INET;			// IP family
	srv.sin_addr.s_addr = inet_addr(serverip);	// IP address of server
	srv.sin_port = htons(port);			// server port

	// actually connect to the server
	rv = connect(sockfd, (struct sockaddr*)&srv, sizeof(struct sockaddr));
	if (rv<0) err(1,0);	
}

/* Code that closes this processes connection with the server when it terminates */
void _fini(void){
	orig_close(sockfd);
} 