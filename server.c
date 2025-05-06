/**
 * @author Andrew Koulogeorge
 * CMU 15440: Distributed Systems
 * Server side code to handle requests from multiple clients using an interposed RPC library
*/

#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <fcntl.h> 
#include <signal.h>
#include <dirent.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include "../include/dirtree.h"

#define MAXMSGLEN 100 	// temporary storage size for reading bytes from TCP connection
#define FD_OFFSET 10000 // used to manage fd translation between client and server
#define OPCODE_BYTE 1 	// each serialized data packet contains a byte indicating which RPC to call
#define NULL_TERM 1 	// include \0 when copying strings to a buffer

/* Opcodes to identify RPCs to call on server side */
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

int recursive_encode(struct dirtreenode *node, void *packet, int idx);
void *encode_dirtreenode(struct dirtreenode *root_node, size_t payload_size);
size_t count_tree_size(struct dirtreenode *node);

/* Wrapper function for malloc that automatically handles errors */
void *xMalloc(size_t nbytes){
	void *ptr = malloc(nbytes);
	if(ptr == NULL){
		fprintf(stderr, "Malloc failed...\n");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

int main(int argc, char**argv) {
	signal(SIGCHLD, SIG_IGN); // signal to the OS to reap the child 

	char buf[MAXMSGLEN+1];
	char *serverport;
	unsigned short port;
	int sockfd, sessfd, rv;
	struct sockaddr_in srv, cli;
	socklen_t sa_size;
	
	// Get environment variable indicating the port of the server
	serverport = getenv("serverport15440");
	if (serverport) port = (unsigned short)atoi(serverport);
	else port=15440;
	
	// Create socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);	// TCP/IP socket
	if (sockfd<0) err(1, 0);					// in case of error
	
	// setup address structure to indicate server port
	memset(&srv, 0, sizeof(srv));				// clear it first
	srv.sin_family = AF_INET;					// IP family
	srv.sin_addr.s_addr = htonl(INADDR_ANY);	// don't care IP address
	srv.sin_port = htons(port);					// server port

	// bind to our port
	rv = bind(sockfd, (struct sockaddr*)&srv, sizeof(struct sockaddr));
	if (rv<0) err(1,0);
	
	// start listening for connections
	rv = listen(sockfd, 5);
	if (rv<0) err(1,0);
	
	
	while(1){ // main server loop; forks off clients to handle RPCs
		// wait for next client, get session socket
		sa_size = sizeof(struct sockaddr_in);
		sessfd = accept(sockfd, (struct sockaddr *)&cli, &sa_size);
		if (sessfd<0) err(1,0);

		pid_t pid = fork(); // fork off a child process here to handle the request from the client
		if( pid == 0){
			close(sockfd); // child does not need listening socket
			while(1) 	   // loop to handle several RPCs from users program
			{
				/* Init Variables to recive packet from user */
				char *packet = NULL;
				long num_bytes_length = 0;
				long num_bytes_payload = 0;
				bool found_length = false;
				size_t idx = 0;
				size_t total_bytes_expected = 0;

				/* Read in Packet */
				while ( (rv=recv(sessfd, buf, MAXMSGLEN, 0)) > 0) 
				{
					if(!found_length){// allocate sufficent memory to read in packet
						char *endbyte;
						num_bytes_length = strlen(buf) + 1; 			// number of bytes the string length takes up; ex -> "15/0" = 3 bytes
						num_bytes_payload = strtol(buf, &endbyte, 10);  
						assert(*endbyte == '\0'); 						// ensure that string being parsed is null termianted
						total_bytes_expected = num_bytes_length + (size_t)num_bytes_payload;
						packet = (char *)xMalloc(total_bytes_expected);  // allocate memory to parse incoming packet
						found_length = true; 						    // only run this code once
					}
					
					memcpy(packet + idx, buf, rv); 
					idx += rv;
					if(idx == total_bytes_expected) break; // break once we have gotten all of the bytes we are expecting from user
				}
				if(rv <= 0){								  // terminate worker when connection ends
					close(sessfd);
					exit(0);
				}
				
				/* Decode Packet */
				idx = num_bytes_length; 				// resert index past length information
				unsigned char opcode = *(packet + idx); // extract opcode from packet 
				idx++; // read opcode			

				/* Variable setup for opcode*/
				size_t return_packet_size = 0; // number of bytes in the return packet
				char *return_packet = NULL;
				void *buffer = NULL; 
				int fd = 0;
				size_t count = 0, buf_length = 0, pathname_length = 0; 
				char *pathname = NULL; 

							/* Based on opcode, handle RPC and send return packet back to client */

				switch (opcode) {
					case open_opcode:							
						pathname_length = num_bytes_payload; // backout how long pathname is based on length of other arguments
						pathname_length--; // subtract 1 for opcode

						/* decode arguments */
						unsigned char num_args = *(packet + idx); idx++;  			// num_args (1 byte)
						pathname_length--;
						int flags = *((int*)(packet + idx)); idx += sizeof(flags);	// flags (int; 4 bytes)
						pathname_length -= sizeof(flags);
						
						mode_t mode = 0; 											// mode (mode_t=unsigned short; 2 bytes)
						if(num_args == 3){
							mode = *((short*)(packet + idx));
							idx += sizeof(mode);
							pathname_length -= sizeof(mode);
						}
						
						pathname = xMalloc(pathname_length);
						memcpy(pathname, packet + idx, pathname_length);
						idx = 0; // reset pointer

						int open_fd = open(pathname, flags, mode); // execute RPC
						
						if(open_fd >= 0) // if open failed, then dont give it a new offset
							open_fd += FD_OFFSET;
						
						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(open_fd) + sizeof(errno);
						return_packet = xMalloc(return_packet_size);

						*(int*)(return_packet + idx) = open_fd;
						idx += sizeof(open_fd);
						*(int*)(return_packet + idx) = errno;
						idx += sizeof(errno);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						free(pathname);
						break;
					case write_opcode:
						/* decode arguments */
						fd = *((int*)(packet + idx)); idx += sizeof(fd); 	// fd (int; 4 bytes)
						fd -= FD_OFFSET;

						count = *((size_t*)(packet + idx)); idx += sizeof(count); // count (size; 8 bytes)

						buf_length = count; // allocate memory for variable length variable (payload minus other args)
						buffer = xMalloc(buf_length);
						memcpy(buffer, packet + idx, buf_length);
						
						idx = 0; // reset pointer
						ssize_t write_return_val = write(fd, buffer, count); // execute RPC

						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(write_return_val) + sizeof(errno);
						return_packet = xMalloc(return_packet_size);
						*(ssize_t*)(return_packet + idx) = write_return_val; idx += sizeof(write_return_val);
						*(int*)(return_packet + idx) = errno; idx += sizeof(errno);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						free(buffer);
						break;
				
					case close_opcode:
						/* decode argument */
						fd = *((int*)(packet + idx)); 	// fd (int; 4 bytes)
						fd -= FD_OFFSET;
						
						idx = 0; // reset pointer
						int close_return_val = close(fd); // execute RPC

						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(close_return_val) + sizeof(errno);
						return_packet = xMalloc(return_packet_size);

						*(int*)(return_packet + idx) = close_return_val;
						idx += sizeof(close_return_val);
						*(int*)(return_packet + idx) = errno;
						idx += sizeof(errno);

						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						break;
					case read_opcode:
						/* decode arguments */
						fd = *((int*)(packet + idx)); 	// fd (int; 4 bytes)
						fd -= FD_OFFSET;
						idx += sizeof(fd);

						count = *((size_t*)(packet + idx)); // count (size; 8 bytes)
						idx += sizeof(count);
						
						idx = 0; // reset pointer
						
						// allocate buffer to read from file
						buffer = xMalloc(count); // reading at most count bytes from file
						ssize_t read_return_val = read(fd, buffer, count); // execute RPC

						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(read_return_val) + sizeof(errno) + count;
						return_packet = xMalloc(return_packet_size);

						*(ssize_t*)(return_packet + idx) = read_return_val;
						idx += sizeof(read_return_val);
						*(int*)(return_packet + idx) = errno;
						idx += sizeof(errno);
						memcpy(return_packet + idx, buffer, count);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(buffer);
						free(return_packet);
						break;
					case lseek_opcode:
						/* decode argument */
						fd = *((int*)(packet + idx)); fd -= FD_OFFSET; // fd (int; 4 bytes)
						idx += sizeof(fd);
						
						off_t offset = *((off_t*)(packet + idx)); idx += sizeof(offset);
						int whence = *((int*)(packet + idx)); idx += sizeof(whence);	// fd (int; 4 bytes)

						idx = 0; // reset pointer
						off_t lseek_return_val = lseek(fd, offset, whence); // execute RPC

						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(lseek_return_val) + sizeof(errno);
						return_packet = xMalloc(return_packet_size);

						*(off_t*)(return_packet + idx) = lseek_return_val; idx += sizeof(lseek_return_val);
						*(int*)(return_packet + idx) = errno; idx += sizeof(errno);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						break;
					case stat_opcode:
						size_t pathname_length = num_bytes_payload - OPCODE_BYTE;
						pathname = xMalloc(pathname_length);
						memcpy(pathname, packet + idx, pathname_length);

						struct stat file_stat;
						int stat_returnval = stat(pathname, &file_stat); // call RPC with pathname and fresh stat struct
						
						// traverse the entire tree to count the total number of bytes we are going to need to encode it
						return_packet_size = 2*sizeof(int) + sizeof(struct stat);
						return_packet = (char*)xMalloc(return_packet_size);
						idx = 0; // reset pointer
						
						memcpy(return_packet + idx, &stat_returnval, sizeof(int)); idx += sizeof(int); // write return val
						memcpy(return_packet + idx, &errno, sizeof(int)); idx += sizeof(errno); // write errno
						
							/* Populate the the stat data structure */
						memcpy(return_packet + idx, &(file_stat.st_dev), sizeof(file_stat.st_dev)); idx += sizeof(file_stat.st_dev);
						memcpy(return_packet + idx,&(file_stat.st_ino), sizeof(file_stat.st_ino)); idx += sizeof(file_stat.st_ino);
						memcpy(return_packet + idx,&(file_stat.st_mode), sizeof(file_stat.st_mode)); idx += sizeof(file_stat.st_mode);
						memcpy(return_packet + idx,&(file_stat.st_nlink), sizeof(file_stat.st_nlink)); idx += sizeof(file_stat.st_nlink);
						memcpy(return_packet + idx,&(file_stat.st_uid), sizeof(file_stat.st_uid)); idx += sizeof(file_stat.st_uid);
						memcpy(return_packet + idx,&(file_stat.st_gid), sizeof(file_stat.st_gid)); idx += sizeof(file_stat.st_gid);
						memcpy(return_packet + idx, &(file_stat.st_rdev),sizeof(file_stat.st_rdev)); idx += sizeof(file_stat.st_rdev);
						memcpy(return_packet + idx,&(file_stat.st_size), sizeof(file_stat.st_size)); idx += sizeof(file_stat.st_size);
						memcpy(return_packet + idx,&(file_stat.st_blksize), sizeof(file_stat.st_blksize)); idx += sizeof(file_stat.st_blksize);
						memcpy(return_packet + idx,&(file_stat.st_blocks), sizeof(file_stat.st_blocks)); idx += sizeof(file_stat.st_blocks);
												
						send(sessfd, return_packet, return_packet_size, 0); 
						free(pathname);
						free(return_packet);
						break;						
					case getdirentries_opcode:
						/* decode arguments */
						fd = *((int*)(packet + idx)); idx += sizeof(fd); // fd (int; 4 bytes)
						fd -= FD_OFFSET; 
						size_t nbytes = *((size_t*)(packet + idx)); idx += sizeof(nbytes);// count (size; 8 bytes)
						off_t basep = *((off_t*)(packet + idx)); idx += sizeof(basep);// offset (size; 8 bytes)
						
						// allocate buffer to read from file
						char *new_buffer = (char*)xMalloc(nbytes); // reading at most nbytes bytes from file
						ssize_t getdirentries_return_val = getdirentries(fd, new_buffer, nbytes, &basep); // execute RPC

						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(getdirentries_return_val) + sizeof(errno) + sizeof(basep) + nbytes;
						return_packet = (char*)xMalloc(return_packet_size);

						idx = 0; // reset pointer

						*(ssize_t*)(return_packet + idx) = getdirentries_return_val; idx += sizeof(getdirentries_return_val);
						*(int*)(return_packet + idx) = errno; idx += sizeof(errno);
						*(off_t*)(return_packet + idx) = basep; idx += sizeof(off_t);
						memcpy(return_packet + idx, new_buffer, nbytes);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(new_buffer);
						free(return_packet);
						break;

					case getdirtree_opcode:				
						// allocate memory for variable length variable (payload minus other args)
						size_t path_length = num_bytes_payload - OPCODE_BYTE;
						char *path = xMalloc(path_length);
						memcpy(path, packet + idx, path_length);
						
						idx = 0; // reset pointer

						struct dirtreenode *root_node = getdirtree(path); // execute RPC

						// traverse the entire tree to count the total number of bytes we are going to need to encode it
						size_t payload_size = count_tree_size(root_node) + sizeof(errno);
						size_t return_packet_size = payload_size + sizeof(payload_size);
						return_packet = encode_dirtreenode(root_node, payload_size);

						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						free(path);
						freedirtree(root_node);
						break;				
					case unlink_opcode:
						
						pathname_length = num_bytes_payload; // backout how long pathname is based on length of other arguments
						pathname_length--; 					 // subtract 1 for opcode
						pathname = xMalloc(pathname_length);
						memcpy(pathname, packet + idx, pathname_length);
						
						int unlink_status = unlink(pathname); // execute RPC
					
						/* package return value and errno into a packet */ 
						return_packet_size = sizeof(unlink_status) + sizeof(errno);
						return_packet = xMalloc(return_packet_size);
						
						idx = 0; // reset pointer
						*(int*)(return_packet + idx) = unlink_status; idx += sizeof(unlink_status);
						*(int*)(return_packet + idx) = errno; idx += sizeof(errno);
						
						send(sessfd, return_packet, return_packet_size, 0); 
						free(return_packet);
						free(pathname);
						break;								
					}
				free(packet);
			}
		}
		else{
			close(sessfd); // parent closes connection with client after child process has been forked
		}
	}
	close(sockfd);
	return 0;
}

/* Function that takes in the root note of the directory tree and returns a byte sequence packet encoding the tree */
void *encode_dirtreenode(struct dirtreenode *root_node, size_t payload_size){
	void *packet = xMalloc(payload_size + sizeof(size_t)); 				 // allocate all bytes we need
	memcpy(packet, &payload_size, sizeof(size_t));
	memcpy((char*)packet + sizeof(size_t), &errno, sizeof(errno));
	recursive_encode(root_node, packet, sizeof(size_t) + sizeof(errno)); // traverse the entire tree again to encode it; start 8 bytes in
	return packet;
}

/* Helper function that recursively encodes all the nodes in the dirtree into a byte packet */
int recursive_encode(struct dirtreenode *node, void *packet, int idx){
	int name_length = strlen(node->name) + NULL_TERM; 													   // encode name length including nul terminator
	memcpy(packet+idx, &name_length, sizeof(name_length));idx += sizeof(name_length);
	memcpy(packet+idx, node->name, name_length); idx += name_length; 									   // encode node name
	memcpy(packet+idx, &(node->num_subdirs), sizeof(node->num_subdirs)); idx += sizeof(node->num_subdirs); // encode number of children

	// loop over number of children and recurse
	for(int i = 0; i < node->num_subdirs; i++)
		idx = recursive_encode(*(node->subdirs + i), packet, idx); // returning updated location in packet with idx
	return idx;
}

/* Helper function that traverses over the tree and computes how many total bytes we are going to need to send dirtree to client */
size_t count_tree_size(struct dirtreenode *node){
	size_t total_bytes = sizeof(int) + sizeof(node->num_subdirs); // 8 bytes for (size_of_name) and (num_of_subdirectories)
	total_bytes += strlen(node->name) + NULL_TERM; 				  // length of name including /0
	for(int i = 0; i < node->num_subdirs; i++) 					  // compute size of children nodes
		total_bytes += count_tree_size(*(node->subdirs + i));
	return total_bytes;
}
