/*
 ============================================================================
 Name        : mtail-f.c
 Author      : Irfan Mohammad
 Version     :
 Copyright   : GPL
 Description : Memory mapped files are usually filled with NULL '\0' character
               It is difficult to follow changes to the file, when it is used
               as a syslog or log-rotated.
               mtail-f provides the ability to follow a file which is filled
               with NULL chars, and actual text appears with time


               Other features include:
                   1. Follow multiple files
                   2. Follow pattern of filename - using glob
                   3. Follow until a pid exists - only on POSIX systems
                   4. Print only specified number of last lines
                   5. Follow names and not inode-
                   	   	   in case log is resized/rotated

               Press ctrl-c to stop.

               exit codes:
                   0 on success
                   1 if file(s) were not opened for reading


 ============================================================================
 */

#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>

/* chunks of data to read at a time from files */
#define BUF_CHUNK_SIZE 4096

/* longest cmdline arg size */
#define MAX_ARG_SIZE 1024

/* wrapper to control verbose debug logs sent to stderr used with -v option */
#define dbg_printf(format, ...) do {                                            \
	if(debug) { fprintf (stderr, format, __VA_ARGS__); } else {}              \
} while(0)

/*
 * parameters used to control the behavior of mtail
 *
 * */
typedef struct mtail_params_ {
	int num_lines; /* mtail-f -n <> print only last n lines*/
	int watch_pid; /* follow until this pid is alive */
	int delay_seconds; /* interval before files are inspected for changes */
	bool verbose; /* debug flag to print details to stderr */
	bool quiet; /* don't output file name headers */
	bool lines_from_start; /* read from start rather than end (taif -n +10) */
	char regex[MAX_ARG_SIZE]; /* Work with files mathing regex*/
	char **files; /* names of the files to follow */
	int num_files;
	char delim; /* default delim is \n, specify delimiter to read */
	char end_marker; /* mmap file is usually NULL filled */
} mtail_params_t;

/*
 * array backed ring buffer to hold the last n lines of the file
 * for printing with -n option
 */

typedef struct ring_buffer_ {
	int latest_index; /* index in the buffer, where last line was stored */
	int capacity; /* number of lines to hold in buffer */
	int size; /* current lines in buffer */
	char **line; /* actual lines held in an array */
} ring_buffer_t;

typedef struct file_data_ {
	FILE *fp; /* ptr to the open file */
	ring_buffer_t rb; /* buffer to store the tail -n data */
	bool end_reached; /* to indicate that current tail -n condition is met
	 	 	 	 	   * and printing can happen
	 	 	 	 	   */
	char delim;       /* delimiter to use for this file for reading */
} file_data_t;

int debug = false;

/*
 * print the usage of this utility
 * */
void
print_usage (int argc, char **argv) {
	/* TODO elaborate */
	fprintf(stderr, "Usage:"
			"    %s filename # filename to follow\n", argv[0]);
}

bool glob_files (char *regex, glob_t* pglob) {
	int ret;
	int i;
	ret = glob(regex, 0, NULL, pglob);
	if (ret == 0) {
		for (i=0;i<pglob->gl_pathc;i++) {
			dbg_printf("%s: %d: %s\n", __FUNCTION__, i+1, pglob->gl_pathv[i]);
		}
		return true;
	} else {
		globfree(pglob);
		return false;
	}
}
/*
 * Close all files in case of error, or if we are finished
 * */
void
close_files (file_data_t *file_data_array, int num_files) {
	while (num_files-->0) {
		fclose(file_data_array[num_files].fp);
		file_data_array[num_files].fp = NULL;
	}
}
/*
 * Attempt to open all the files,
 * if any file fails to open, close all files opened thus far, and return false
 * else return true
 *
 * */
bool
open_files (char *filenames[], int num_files, file_data_t *file_data_array) {
	int i;
	for (i=0;i<num_files;i++) {
		if (file_data_array[i].fp) {
			/* File is already open */
			/* TODO check if file exists, has been rotated/deleted? */
			continue;
		}
		file_data_array[i].fp = fopen(filenames[i], "r");
		if (!file_data_array[i].fp) {
			dbg_printf("Error opening %s for reading: %s\n",
						filenames[i], strerror(errno));
			close_files(file_data_array, i);
			return false;
		}
	}
	return true;
}

/*
 * if size has reached capacity, replace oldest with newest line.
 * else insert the line into ring buffer.
 * */
void
enqueue (ring_buffer_t *rb, char *line, int len) {
	int old_len;
	int index;
	rb->latest_index++;
	if (rb->latest_index>=rb->capacity) {
		rb->latest_index = 0;
	}
	index = rb->latest_index;
	if (!rb->line[index]) {
		rb->line[index] = malloc(sizeof(char)*len);
	} else {
		old_len = sizeof(rb->line[index]);
		if (old_len<len) {
			free(rb->line[index]);
			rb->line[index] = malloc(sizeof(char)*len);
		}
	}
	line[len-1]='\0'; /* Ensure that line is null terminated */
	strncpy(rb->line[index], line, len);
	if (rb->size<rb->capacity) {
		rb->size++;
	}
}

/*
 * print the lines in buffer for "tail -n <>" functionality
 */
void
print_ring_buffer(ring_buffer_t *rb) {
	int i;
	int oldest_index = rb->latest_index+1;
	if (!rb->line) {
		/* Nothing to print */
		return;
	}
	for (i=0;i<rb->size;i++) {
		if (oldest_index >= rb->size) {
			oldest_index = 0;
		}
		fprintf(stdout,"%s",rb->line[oldest_index]);
		free(rb->line[oldest_index]);
		rb->line[oldest_index]=NULL;
		oldest_index++;
	}
	fflush(stdout);
	free(rb->line);
	rb->line=NULL;
}

void
ring_buffer_init(ring_buffer_t *rb, int capacity) {
	memset(rb, 0, sizeof(ring_buffer_t));
	rb->capacity=capacity;
	rb->latest_index = capacity;
	rb->line = malloc(sizeof(char*)*capacity);
	memset(rb->line,0,sizeof(char*)*capacity);
}

/* check if we need to stop tailing based on params
 * is watched_pid alive?
 * */
bool
stop_conditions_met (mtail_params_t *params) {
	if (params->watch_pid!=0) {
#ifdef _POSIX_VERSION
		dbg_printf("Checking pid:%d is alive\n", params->watch_pid);
		kill(params->watch_pid,0);
		if (errno == ESRCH) {

			return true;
		}
#else
		fprintf(stderr, "pid check unsupported on non-posix systems\n");
		return true;
#endif
	}
	return false;
}
/*
 * Parse and validate the cmdline options
 * */
bool
parse_opts (int argc, char *argv[], mtail_params_t *params) {
    int opt;
    glob_t pglob;
	if (argc<2) {
		print_usage(argc, argv);
		return EXIT_FAILURE;
	}
	memset(params,0,sizeof(mtail_params_t));

	/* initialize defaults */
	params->num_lines = 10;
	params->delim = '\n';
	params->delay_seconds = 1;

	/* write a better string */
	while ((opt = getopt(argc, argv, "n:s:vp:qr:d:")) != -1) {
		dbg_printf("opt:%c optarg:%s\n", opt, optarg);
		switch (opt) {
		case 'n':
			if (optarg[0]=='+') {
				params->lines_from_start = true;
				optarg++;
			}
			params->num_lines = atoi(optarg);
			break;
		case 's':
			params->delay_seconds = atoi(optarg);
			break;
		case 'v':
			params->verbose = true;
			debug = true;
			break;
		case 'p':
			params->watch_pid = atoi(optarg);
			break;
		case 'q':
			params->quiet = true;
			break;
		case 'r':
			strncpy(params->regex, optarg, MAX_ARG_SIZE);
			params->regex[MAX_ARG_SIZE-1]='\0';
			glob_files(params->regex, &pglob);
			break;
		case 'd':
			params->delim = optarg[0];
			break;
		case 'x':
			params->end_marker = optarg[0];
			break;
		default:
			break;
		}
	}
	if (params->regex[0]=='\0') {
		params->files = &argv[optind];
		params->num_files = argc - optind;
	} else if (pglob.gl_pathc!=0) {
		params->files = pglob.gl_pathv;
		params->num_files = pglob.gl_pathc;
	} else {
		fprintf(stderr, "glob: %s Input files not found\n", params->regex);
	}
	dbg_printf("argv[%d] = %s\n", optind, argv[optind]);
	return true;
}

/*
 * Helper function to determine first index of end_marker in given char *buf
 * */
int
find_end_index (char *buf, char end_marker, int limit) {
	int i=0;
	while(buf[i]!=end_marker && i<limit ) {
		i++;
	}
	if (buf[i]==end_marker) {
		return i;
	} else {
		return -1;
	}
}

bool
print_file_content (mtail_params_t *param_args) {
	FILE* fp = NULL;
	int i = 0;
	size_t chunk_size = BUF_CHUNK_SIZE;
	char *buf = NULL;
	bool print_file_name;
	int read_chars = 0;
	int last_file_printed = -1;
	int move_by = 0;
	file_data_t *f_array =
			malloc(sizeof(file_data_t)*param_args->num_files);

	/* Initialize data structures */
	for (i=0;i<param_args->num_files;i++) {
		ring_buffer_init(&f_array[i].rb, param_args->num_lines);
		/* end is reached if we are not looking for last n lines */
		f_array[i].end_reached = (param_args->num_lines == 0);
		f_array[i].fp = NULL;
		f_array[i].delim = param_args->delim;
	}

	/* Need to print file names when we are tailing more than 1 file
	 * and if quiet mode is disabled
	 * */
	bool need_to_print_file_name = (!param_args->quiet) &&
									(param_args->num_files>1);

	while (true) {
		if (!open_files(param_args->files, param_args->num_files, f_array)) {
			/* Could not open the given files, retry */
			break;
		}
		for (i=0; i<param_args->num_files; i++) { /* for each file */
			fp = f_array[i].fp; /* Get the file pointer */
			print_file_name = need_to_print_file_name;
			/* getdelim is problematic with huge files fix this */
			while((read_chars =
					getdelim(&buf, &chunk_size, f_array[i].delim, fp))>0) {
				/* something new was read */
				dbg_printf("%s: %d chars read, cursor at %ld, errno:%s\n",
						param_args->files[i], read_chars, ftell(fp),
						strerror(errno));
				/* print, if we read anything other than just end_marker */
				if (buf[0]!=param_args->end_marker) {
					if (print_file_name && (last_file_printed != i)) {
						/* Header for a different file was printed */
						fprintf(stdout, "\n==> %s <==\n", param_args->files[i]);
						last_file_printed = i;
						print_file_name = false;
					}
					if (f_array[i].end_reached) {
						/* print the actual content */
						fprintf(stdout, "%s", buf);
						fflush(stdout);
					} else {
						/* mtail-f -n specified, enqueue content */
						enqueue(&f_array[i].rb,buf,read_chars+1);
					}
				}

				/* Check if end_marker was found */
				if (buf[read_chars-1]==param_args->end_marker) {
					if (!f_array[i].end_reached) {
						print_ring_buffer(&f_array[i].rb);
						f_array[i].end_reached = true;
						f_array[i].delim = param_args->end_marker;
					}
					/* move cursor to first occurrence of end_marker */
					move_by = read_chars-find_end_index(buf,
											param_args->end_marker, read_chars);


					fseek(fp,
							-move_by,
							SEEK_CUR);
					dbg_printf("%s: end_marker ASCII:%d found, cursor at %ld\n",
							param_args->files[i], (int)(param_args->end_marker),
							ftell(fp));
					break; /* so that we pause before we retry reading */
				}
			}
			dbg_printf("%s: %d chars read, cursor at %ld, errno:%s\n",
					param_args->files[i], read_chars, ftell(fp),
					strerror(errno));
			fseek(fp, 0, SEEK_CUR);
		}
		/* wait before retrying */
		sleep(param_args->delay_seconds);
		if (stop_conditions_met(param_args)) {
			break;
		}
	}
	close_files(f_array, param_args->num_files);
	if (buf) {
		free(buf);
	}
	return true;
}

int main (int argc, char *argv[]) {
	mtail_params_t param_args;
	setvbuf(stderr,NULL,_IONBF,0);

	parse_opts(argc, argv, &param_args);
	print_file_content(&param_args);

	return EXIT_SUCCESS;
}
