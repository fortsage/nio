#define _XOPEN_SOURCE 600
#define _GNU_SOURCE 

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <string.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <signal.h>

#include "common.h"
#include "timing.h"
#include "sysconf.h"


double IOPs, MBps; 
double min_lat, max_lat, avg_lat;
double r_IOPs, r_MBps, r_avg_lat;
double w_IOPs, w_MBps, w_avg_lat;
pthread_mutex_t mutexsum, rmutex, wmutex;
void *status;
pthread_mutex_t *wait_mutex;
pthread_attr_t attr;
pthread_t *numthreads;
int exit_status = 0;
struct dev_opts iodev;
struct thread_opts *topts;
int r_err, w_err;

int main (int argc, char *argv[])
{
	int i, tnum, l_th_cpu;
	struct worker_opts io_workers;
	cpu_set_t cpuset;
	struct sys_info sys_config;

	memset(&io_workers, 0, sizeof(io_workers));
	init_defaults(&iodev);
	r_err = 0;
	w_err = 0;
	
	parse_args(argc, argv, &iodev);
	worker_setup(&io_workers, &iodev);
	sys_conf(&sys_config);

	dbg_printf(1, ":: io mode: %c, R workers: %d, W workers: %d\n", GET_IO_MODE(iodev.mode), io_workers.rd_workers, io_workers.wr_workers);
	dbg_printf(1, ":: io type: %c, S threads: %d, R threads: %d\n", GET_IO_TYPE(iodev.type), io_workers.seq_threads, io_workers.rnd_threads);
	dbg_printf(1, "** spawn %d sequential io type / read io mode workers\n", io_workers.seq_rd);
	dbg_printf(1, "** spawn %d sequential io type / write io mode workers\n", io_workers.seq_wr);
	dbg_printf(1, "** spawn %d random io type / read io mode workers\n", io_workers.rnd_rd);
	dbg_printf(1, "** spawn %d random io type / write io mode workers\n", io_workers.rnd_wr);
	
	if (iodev.use_dio) {
		if ((iodev.fd = open(iodev.devpath, O_RDWR|O_DIRECT)) < 0) {
		fprintf(stdout, "Unable to open device: %s\n", iodev.devpath);
		perror("open");
		exit(1);
		}
	} else {
		if ((iodev.fd = open(iodev.devpath, O_RDWR)) < 0) {
		fprintf(stdout, "Unable to open device: %s\n", iodev.devpath);
		perror("open");
		exit(1);
		}
	}
		
	if (((iodev.buf = mmap(NULL, iodev.blksize, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)) == MAP_FAILED) \
		|| (mlock(iodev.buf, iodev.blksize) == -1)) {
		perror("mmap");
		perror("mlock");
		exit(1);
	}

	topts = (struct thread_opts *) malloc (sizeof(struct thread_opts) * iodev.nthreads);
	memset(iodev.buf, 0, iodev.blksize);

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigterm_handler);
	signal(SIGKILL, sigkill_handler);

	numthreads = (pthread_t *) malloc(sizeof(pthread_t) * iodev.nthreads);
	wait_mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t) * iodev.nthreads);
	
	pthread_mutex_init(&mutexsum, NULL);
	pthread_mutex_init(&rmutex, NULL);
	pthread_mutex_init(&wmutex, NULL);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	
	l_th_cpu = 0;

  	for (tnum = 0; tnum < iodev.nthreads; tnum++)  {
		CPU_ZERO(&cpuset);

		if (l_th_cpu < sys_config.num_cpus) {
			CPU_SET(l_th_cpu, &cpuset);
			l_th_cpu++;
		} else {
			l_th_cpu = 0;
			CPU_SET(l_th_cpu, &cpuset);
			l_th_cpu++;
		}

 		topts[tnum].thread_id = tnum;
		topts[tnum].opts = &iodev;
		topts[tnum].rand_seed = (tnum + 1000); /* pre-fixed random seed */
	
		worker_alloc(&io_workers, &topts[tnum]);
   		pthread_create(&numthreads[tnum], &attr, io_thread, (void *) &topts[tnum]); 
		pthread_setaffinity_np(numthreads[tnum], sizeof(cpu_set_t), &cpuset);
	}

	pthread_attr_destroy(&attr);

	for(i = 0; i < iodev.nthreads; i++) {
		pthread_join(numthreads[i], &status);
		usleep (100 * MSECS);
  	}

	fprintf(stdout, "dev: %s | n_threads: %02d | mode: %c | type: %c | blksize: %d (B) | iops: %.02f | MB/s: %.02f | svc_time: %.02f (ms)\n",
		iodev.devpath, 
		iodev.nthreads, 
		(iodev.mode) ? ((iodev.mode == N_WRITE) ? 'W' : 'M'): 'R', 
		(iodev.type) ? ((iodev.type == RANDOM) ? 'R' : 'M') : 'S',	
		iodev.blksize, 
		IOPs,
		MBps,
		(avg_lat/MSECS)/iodev.nthreads);

	if (iodev.verbose) 	
		fprintf(stdout, "[READ] IO/s: %.02f, MB/s %.02f, Errors: %d\n[WRITE] IO/s: %.02f, MB/s %.02f, Errors: %d\n",
			r_IOPs, r_MBps, r_err, 
			w_IOPs, w_MBps, w_err);	

	cleanup(&mutexsum, &iodev);
	cleanup(&wmutex, NULL);
	cleanup(&rmutex, NULL); 
	pthread_exit(NULL);

	return 0;
}   