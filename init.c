/*
 * This file contains job initialization and setup functions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fio.h"
#include "parse.h"
#include "smalloc.h"
#include "filehash.h"
#include "verify.h"
#include "profile.h"
#include "server.h"

#include "lib/getopt.h"

#if FIO_PATCH > 0
static char fio_version_string[] =	__fio_stringify(FIO_MAJOR) "."	\
					__fio_stringify(FIO_MINOR) "."	\
					__fio_stringify(FIO_PATCH);
#else
static char fio_version_string[] =	__fio_stringify(FIO_MAJOR) "."	\
					__fio_stringify(FIO_MINOR);
#endif

#define FIO_RANDSEED		(0xb1899bedUL)

static char **ini_file;
static int max_jobs = FIO_MAX_JOBS;
static int dump_cmdline;

static struct thread_data def_thread;
struct thread_data *threads = NULL;

int exitall_on_terminate = 0;
int terse_output = 0;
int eta_print;
unsigned long long mlock_size = 0;
FILE *f_out = NULL;
FILE *f_err = NULL;
char **job_sections = NULL;
int nr_job_sections = 0;
char *exec_profile = NULL;
int warnings_fatal = 0;
int terse_version = 2;
int is_backend = 0;
int nr_clients = 0;
int log_syslog = 0;

int write_bw_log = 0;
int read_only = 0;

static int write_lat_log;

static int prev_group_jobs;

unsigned long fio_debug = 0;
unsigned int fio_debug_jobno = -1;
unsigned int *fio_debug_jobp = NULL;

static char cmd_optstr[256];

/*
 * Command line options. These will contain the above, plus a few
 * extra that only pertain to fio itself and not jobs.
 */
static struct option l_opts[FIO_NR_OPTIONS] = {
	{
		.name		= (char *) "output",
		.has_arg	= required_argument,
		.val		= 'o',
	},
	{
		.name		= (char *) "timeout",
		.has_arg	= required_argument,
		.val		= 't',
	},
	{
		.name		= (char *) "latency-log",
		.has_arg	= required_argument,
		.val		= 'l',
	},
	{
		.name		= (char *) "bandwidth-log",
		.has_arg	= required_argument,
		.val		= 'b',
	},
	{
		.name		= (char *) "minimal",
		.has_arg	= optional_argument,
		.val		= 'm',
	},
	{
		.name		= (char *) "version",
		.has_arg	= no_argument,
		.val		= 'v',
	},
	{
		.name		= (char *) "help",
		.has_arg	= no_argument,
		.val		= 'h',
	},
	{
		.name		= (char *) "cmdhelp",
		.has_arg	= optional_argument,
		.val		= 'c',
	},
	{
		.name		= (char *) "showcmd",
		.has_arg	= no_argument,
		.val		= 's',
	},
	{
		.name		= (char *) "readonly",
		.has_arg	= no_argument,
		.val		= 'r',
	},
	{
		.name		= (char *) "eta",
		.has_arg	= required_argument,
		.val		= 'e',
	},
	{
		.name		= (char *) "debug",
		.has_arg	= required_argument,
		.val		= 'd',
	},
	{
		.name		= (char *) "section",
		.has_arg	= required_argument,
		.val		= 'x',
	},
	{
		.name		= (char *) "alloc-size",
		.has_arg	= required_argument,
		.val		= 'a',
	},
	{
		.name		= (char *) "profile",
		.has_arg	= required_argument,
		.val		= 'p',
	},
	{
		.name		= (char *) "warnings-fatal",
		.has_arg	= no_argument,
		.val		= 'w',
	},
	{
		.name		= (char *) "max-jobs",
		.has_arg	= required_argument,
		.val		= 'j',
	},
	{
		.name		= (char *) "terse-version",
		.has_arg	= required_argument,
		.val		= 'V',
	},
	{
		.name		= (char *) "server",
		.has_arg	= no_argument,
		.val		= 'S',
	},
	{	.name		= (char *) "daemonize",
		.has_arg	= no_argument,
		.val		= 'D',
	},
	{
		.name		= (char *) "net-port",
		.has_arg	= required_argument,
		.val		= 'P',
	},
	{
		.name		= (char *) "client",
		.has_arg	= required_argument,
		.val		= 'C',
	},
	{
		.name		= NULL,
	},
};

static void free_shm(void)
{
	struct shmid_ds sbuf;

	if (threads) {
		void *tp = threads;

		threads = NULL;
		file_hash_exit();
		fio_debug_jobp = NULL;
		shmdt(tp);
		shmctl(shm_id, IPC_RMID, &sbuf);
	}

	scleanup();
}

/*
 * The thread area is shared between the main process and the job
 * threads/processes. So setup a shared memory segment that will hold
 * all the job info. We use the end of the region for keeping track of
 * open files across jobs, for file sharing.
 */
static int setup_thread_area(void)
{
	void *hash;

	if (threads)
		return 0;

	/*
	 * 1024 is too much on some machines, scale max_jobs if
	 * we get a failure that looks like too large a shm segment
	 */
	do {
		size_t size = max_jobs * sizeof(struct thread_data);

		size += file_hash_size;
		size += sizeof(unsigned int);

		shm_id = shmget(0, size, IPC_CREAT | 0600);
		if (shm_id != -1)
			break;
		if (errno != EINVAL) {
			perror("shmget");
			break;
		}

		max_jobs >>= 1;
	} while (max_jobs);

	if (shm_id == -1)
		return 1;

	threads = shmat(shm_id, NULL, 0);
	if (threads == (void *) -1) {
		perror("shmat");
		return 1;
	}

	memset(threads, 0, max_jobs * sizeof(struct thread_data));
	hash = (void *) threads + max_jobs * sizeof(struct thread_data);
	fio_debug_jobp = (void *) hash + file_hash_size;
	*fio_debug_jobp = -1;
	file_hash_init(hash);
	return 0;
}

/*
 * Return a free job structure.
 */
static struct thread_data *get_new_job(int global, struct thread_data *parent)
{
	struct thread_data *td;

	if (global)
		return &def_thread;
	if (setup_thread_area()) {
		log_err("error: failed to setup shm segment\n");
		return NULL;
	}
	if (thread_number >= max_jobs) {
		log_err("error: maximum number of jobs (%d) reached.\n",
				max_jobs);
		return NULL;
	}

	td = &threads[thread_number++];
	*td = *parent;

	td->o.uid = td->o.gid = -1U;

	dup_files(td, parent);
	options_mem_dupe(td);

	profile_add_hooks(td);

	td->thread_number = thread_number;
	return td;
}

static void put_job(struct thread_data *td)
{
	if (td == &def_thread)
		return;

	profile_td_exit(td);

	if (td->error)
		log_info("fio: %s\n", td->verror);

	memset(&threads[td->thread_number - 1], 0, sizeof(*td));
	thread_number--;
}

static int __setup_rate(struct thread_data *td, enum fio_ddir ddir)
{
	unsigned int bs = td->o.min_bs[ddir];
	unsigned long long bytes_per_sec;

	assert(ddir_rw(ddir));

	if (td->o.rate[ddir])
		bytes_per_sec = td->o.rate[ddir];
	else
		bytes_per_sec = td->o.rate_iops[ddir] * bs;

	if (!bytes_per_sec) {
		log_err("rate lower than supported\n");
		return -1;
	}

	td->rate_nsec_cycle[ddir] = 1000000000ULL / bytes_per_sec;
	td->rate_pending_usleep[ddir] = 0;
	return 0;
}

static int setup_rate(struct thread_data *td)
{
	int ret = 0;

	if (td->o.rate[DDIR_READ] || td->o.rate_iops[DDIR_READ])
		ret = __setup_rate(td, DDIR_READ);
	if (td->o.rate[DDIR_WRITE] || td->o.rate_iops[DDIR_WRITE])
		ret |= __setup_rate(td, DDIR_WRITE);

	return ret;
}

static int fixed_block_size(struct thread_options *o)
{
	return o->min_bs[DDIR_READ] == o->max_bs[DDIR_READ] &&
		o->min_bs[DDIR_WRITE] == o->max_bs[DDIR_WRITE] &&
		o->min_bs[DDIR_READ] == o->min_bs[DDIR_WRITE];
}

/*
 * Lazy way of fixing up options that depend on each other. We could also
 * define option callback handlers, but this is easier.
 */
static int fixup_options(struct thread_data *td)
{
	struct thread_options *o = &td->o;
	int ret = 0;

#ifndef FIO_HAVE_PSHARED_MUTEX
	if (!o->use_thread) {
		log_info("fio: this platform does not support process shared"
			 " mutexes, forcing use of threads. Use the 'thread'"
			 " option to get rid of this warning.\n");
		o->use_thread = 1;
		ret = warnings_fatal;
	}
#endif

	if (o->write_iolog_file && o->read_iolog_file) {
		log_err("fio: read iolog overrides write_iolog\n");
		free(o->write_iolog_file);
		o->write_iolog_file = NULL;
		ret = warnings_fatal;
	}

	/*
	 * only really works for sequential io for now, and with 1 file
	 */
	if (o->zone_size && td_random(td) && o->open_files == 1)
		o->zone_size = 0;

	/*
	 * Reads can do overwrites, we always need to pre-create the file
	 */
	if (td_read(td) || td_rw(td))
		o->overwrite = 1;

	if (!o->min_bs[DDIR_READ])
		o->min_bs[DDIR_READ] = o->bs[DDIR_READ];
	if (!o->max_bs[DDIR_READ])
		o->max_bs[DDIR_READ] = o->bs[DDIR_READ];
	if (!o->min_bs[DDIR_WRITE])
		o->min_bs[DDIR_WRITE] = o->bs[DDIR_WRITE];
	if (!o->max_bs[DDIR_WRITE])
		o->max_bs[DDIR_WRITE] = o->bs[DDIR_WRITE];

	o->rw_min_bs = min(o->min_bs[DDIR_READ], o->min_bs[DDIR_WRITE]);

	/*
	 * For random IO, allow blockalign offset other than min_bs.
	 */
	if (!o->ba[DDIR_READ] || !td_random(td))
		o->ba[DDIR_READ] = o->min_bs[DDIR_READ];
	if (!o->ba[DDIR_WRITE] || !td_random(td))
		o->ba[DDIR_WRITE] = o->min_bs[DDIR_WRITE];

	if ((o->ba[DDIR_READ] != o->min_bs[DDIR_READ] ||
	    o->ba[DDIR_WRITE] != o->min_bs[DDIR_WRITE]) &&
	    !o->norandommap) {
		log_err("fio: Any use of blockalign= turns off randommap\n");
		o->norandommap = 1;
		ret = warnings_fatal;
	}

	if (!o->file_size_high)
		o->file_size_high = o->file_size_low;

	if (o->norandommap && o->verify != VERIFY_NONE
	    && !fixed_block_size(o))  {
		log_err("fio: norandommap given for variable block sizes, "
			"verify disabled\n");
		o->verify = VERIFY_NONE;
		ret = warnings_fatal;
	}
	if (o->bs_unaligned && (o->odirect || td->io_ops->flags & FIO_RAWIO))
		log_err("fio: bs_unaligned may not work with raw io\n");

	/*
	 * thinktime_spin must be less than thinktime
	 */
	if (o->thinktime_spin > o->thinktime)
		o->thinktime_spin = o->thinktime;

	/*
	 * The low water mark cannot be bigger than the iodepth
	 */
	if (o->iodepth_low > o->iodepth || !o->iodepth_low) {
		/*
		 * syslet work around - if the workload is sequential,
		 * we want to let the queue drain all the way down to
		 * avoid seeking between async threads
		 */
		if (!strcmp(td->io_ops->name, "syslet-rw") && !td_random(td))
			o->iodepth_low = 1;
		else
			o->iodepth_low = o->iodepth;
	}

	/*
	 * If batch number isn't set, default to the same as iodepth
	 */
	if (o->iodepth_batch > o->iodepth || !o->iodepth_batch)
		o->iodepth_batch = o->iodepth;

	if (o->nr_files > td->files_index)
		o->nr_files = td->files_index;

	if (o->open_files > o->nr_files || !o->open_files)
		o->open_files = o->nr_files;

	if (((o->rate[0] + o->rate[1]) && (o->rate_iops[0] + o->rate_iops[1]))||
	    ((o->ratemin[0] + o->ratemin[1]) && (o->rate_iops_min[0] +
		o->rate_iops_min[1]))) {
		log_err("fio: rate and rate_iops are mutually exclusive\n");
		ret = 1;
	}
	if ((o->rate[0] < o->ratemin[0]) || (o->rate[1] < o->ratemin[1]) ||
	    (o->rate_iops[0] < o->rate_iops_min[0]) ||
	    (o->rate_iops[1] < o->rate_iops_min[1])) {
		log_err("fio: minimum rate exceeds rate\n");
		ret = 1;
	}

	if (!o->timeout && o->time_based) {
		log_err("fio: time_based requires a runtime/timeout setting\n");
		o->time_based = 0;
		ret = warnings_fatal;
	}

	if (o->fill_device && !o->size)
		o->size = -1ULL;

	if (o->verify != VERIFY_NONE) {
		if (td_write(td) && o->do_verify && o->numjobs > 1) {
			log_info("Multiple writers may overwrite blocks that "
				"belong to other jobs. This can cause "
				"verification failures.\n");
			ret = warnings_fatal;
		}

		o->refill_buffers = 1;
		if (o->max_bs[DDIR_WRITE] != o->min_bs[DDIR_WRITE] &&
		    !o->verify_interval)
			o->verify_interval = o->min_bs[DDIR_WRITE];
	}

	if (o->pre_read) {
		o->invalidate_cache = 0;
		if (td->io_ops->flags & FIO_PIPEIO) {
			log_info("fio: cannot pre-read files with an IO engine"
				 " that isn't seekable. Pre-read disabled.\n");
			ret = warnings_fatal;
		}
	}

#ifndef FIO_HAVE_FDATASYNC
	if (o->fdatasync_blocks) {
		log_info("fio: this platform does not support fdatasync()"
			 " falling back to using fsync().  Use the 'fsync'"
			 " option instead of 'fdatasync' to get rid of"
			 " this warning\n");
		o->fsync_blocks = o->fdatasync_blocks;
		o->fdatasync_blocks = 0;
		ret = warnings_fatal;
	}
#endif

	return ret;
}

/*
 * This function leaks the buffer
 */
static char *to_kmg(unsigned int val)
{
	char *buf = malloc(32);
	char post[] = { 0, 'K', 'M', 'G', 'P', 'E', 0 };
	char *p = post;

	do {
		if (val & 1023)
			break;

		val >>= 10;
		p++;
	} while (*p);

	snprintf(buf, 31, "%u%c", val, *p);
	return buf;
}

/* External engines are specified by "external:name.o") */
static const char *get_engine_name(const char *str)
{
	char *p = strstr(str, ":");

	if (!p)
		return str;

	p++;
	strip_blank_front(&p);
	strip_blank_end(p);
	return p;
}

static int exists_and_not_file(const char *filename)
{
	struct stat sb;

	if (lstat(filename, &sb) == -1)
		return 0;

	/* \\.\ is the device namespace in Windows, where every file
	 * is a device node */
	if (S_ISREG(sb.st_mode) && strncmp(filename, "\\\\.\\", 4) != 0)
		return 0;

	return 1;
}

static void td_fill_rand_seeds_os(struct thread_data *td)
{
	os_random_seed(td->rand_seeds[0], &td->bsrange_state);
	os_random_seed(td->rand_seeds[1], &td->verify_state);
	os_random_seed(td->rand_seeds[2], &td->rwmix_state);

	if (td->o.file_service_type == FIO_FSERVICE_RANDOM)
		os_random_seed(td->rand_seeds[3], &td->next_file_state);

	os_random_seed(td->rand_seeds[5], &td->file_size_state);
	os_random_seed(td->rand_seeds[6], &td->trim_state);

	if (!td_random(td))
		return;

	if (td->o.rand_repeatable)
		td->rand_seeds[4] = FIO_RANDSEED * td->thread_number;

	os_random_seed(td->rand_seeds[4], &td->random_state);
}

static void td_fill_rand_seeds_internal(struct thread_data *td)
{
	init_rand_seed(&td->__bsrange_state, td->rand_seeds[0]);
	init_rand_seed(&td->__verify_state, td->rand_seeds[1]);
	init_rand_seed(&td->__rwmix_state, td->rand_seeds[2]);

	if (td->o.file_service_type == FIO_FSERVICE_RANDOM)
		init_rand_seed(&td->__next_file_state, td->rand_seeds[3]);

	init_rand_seed(&td->__file_size_state, td->rand_seeds[5]);
	init_rand_seed(&td->__trim_state, td->rand_seeds[6]);

	if (!td_random(td))
		return;

	if (td->o.rand_repeatable)
		td->rand_seeds[4] = FIO_RANDSEED * td->thread_number;

	init_rand_seed(&td->__random_state, td->rand_seeds[4]);
}

void td_fill_rand_seeds(struct thread_data *td)
{
	if (td->o.use_os_rand)
		td_fill_rand_seeds_os(td);
	else
		td_fill_rand_seeds_internal(td);

	init_rand_seed(&td->buf_state, td->rand_seeds[7]);
}

/*
 * Initialize the various random states we need (random io, block size ranges,
 * read/write mix, etc).
 */
static int init_random_state(struct thread_data *td)
{
	int fd;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		td_verror(td, errno, "open");
		return 1;
	}

	if (read(fd, td->rand_seeds, sizeof(td->rand_seeds)) <
	    (int) sizeof(td->rand_seeds)) {
		td_verror(td, EIO, "read");
		close(fd);
		return 1;
	}

	close(fd);
	td_fill_rand_seeds(td);
	return 0;
}

/*
 * Adds a job to the list of things todo. Sanitizes the various options
 * to make sure we don't have conflicts, and initializes various
 * members of td.
 */
static int add_job(struct thread_data *td, const char *jobname, int job_add_num)
{
	const char *ddir_str[] = { NULL, "read", "write", "rw", NULL,
				   "randread", "randwrite", "randrw" };
	unsigned int i;
	const char *engine;
	char fname[PATH_MAX];
	int numjobs, file_alloced;

	/*
	 * the def_thread is just for options, it's not a real job
	 */
	if (td == &def_thread)
		return 0;

	/*
	 * if we are just dumping the output command line, don't add the job
	 */
	if (dump_cmdline) {
		put_job(td);
		return 0;
	}

	if (profile_td_init(td))
		goto err;

	engine = get_engine_name(td->o.ioengine);
	td->io_ops = load_ioengine(td, engine);
	if (!td->io_ops) {
		log_err("fio: failed to load engine %s\n", engine);
		goto err;
	}

	if (td->o.use_thread)
		nr_thread++;
	else
		nr_process++;

	if (td->o.odirect)
		td->io_ops->flags |= FIO_RAWIO;

	file_alloced = 0;
	if (!td->o.filename && !td->files_index && !td->o.read_iolog_file) {
		file_alloced = 1;

		if (td->o.nr_files == 1 && exists_and_not_file(jobname))
			add_file(td, jobname);
		else {
			for (i = 0; i < td->o.nr_files; i++) {
				sprintf(fname, "%s.%d.%d", jobname,
							td->thread_number, i);
				add_file(td, fname);
			}
		}
	}

	if (fixup_options(td))
		goto err;

	if (td->io_ops->flags & FIO_DISKLESSIO) {
		struct fio_file *f;

		for_each_file(td, f, i)
			f->real_file_size = -1ULL;
	}

	td->mutex = fio_mutex_init(0);

	td->ts.clat_percentiles = td->o.clat_percentiles;
	if (td->o.overwrite_plist)
		td->ts.percentile_list = td->o.percentile_list;
	else
		td->ts.percentile_list = NULL;

	td->ts.clat_stat[0].min_val = td->ts.clat_stat[1].min_val = ULONG_MAX;
	td->ts.slat_stat[0].min_val = td->ts.slat_stat[1].min_val = ULONG_MAX;
	td->ts.lat_stat[0].min_val = td->ts.lat_stat[1].min_val = ULONG_MAX;
	td->ts.bw_stat[0].min_val = td->ts.bw_stat[1].min_val = ULONG_MAX;
	td->ddir_seq_nr = td->o.ddir_seq_nr;

	if ((td->o.stonewall || td->o.new_group) && prev_group_jobs) {
		prev_group_jobs = 0;
		groupid++;
	}

	td->groupid = groupid;
	prev_group_jobs++;

	if (init_random_state(td))
		goto err;

	if (setup_rate(td))
		goto err;

	if (td->o.write_lat_log) {
		setup_log(&td->lat_log);
		setup_log(&td->slat_log);
		setup_log(&td->clat_log);
	}
	if (td->o.write_bw_log)
		setup_log(&td->bw_log);

	if (!td->o.name)
		td->o.name = strdup(jobname);

	if (!terse_output) {
		if (!job_add_num) {
			if (!strcmp(td->io_ops->name, "cpuio")) {
				log_info("%s: ioengine=cpu, cpuload=%u,"
					 " cpucycle=%u\n", td->o.name,
							td->o.cpuload,
							td->o.cpucycle);
			} else {
				char *c1, *c2, *c3, *c4;

				c1 = to_kmg(td->o.min_bs[DDIR_READ]);
				c2 = to_kmg(td->o.max_bs[DDIR_READ]);
				c3 = to_kmg(td->o.min_bs[DDIR_WRITE]);
				c4 = to_kmg(td->o.max_bs[DDIR_WRITE]);

				log_info("%s: (g=%d): rw=%s, bs=%s-%s/%s-%s,"
					 " ioengine=%s, iodepth=%u\n",
						td->o.name, td->groupid,
						ddir_str[td->o.td_ddir],
						c1, c2, c3, c4,
						td->io_ops->name,
						td->o.iodepth);

				free(c1);
				free(c2);
				free(c3);
				free(c4);
			}
		} else if (job_add_num == 1)
			log_info("...\n");
	}

	/*
	 * recurse add identical jobs, clear numjobs and stonewall options
	 * as they don't apply to sub-jobs
	 */
	numjobs = td->o.numjobs;
	while (--numjobs) {
		struct thread_data *td_new = get_new_job(0, td);

		if (!td_new)
			goto err;

		td_new->o.numjobs = 1;
		td_new->o.stonewall = 0;
		td_new->o.new_group = 0;

		if (file_alloced) {
			td_new->o.filename = NULL;
			td_new->files_index = 0;
			td_new->files_size = 0;
			td_new->files = NULL;
		}

		job_add_num = numjobs - 1;

		if (add_job(td_new, jobname, job_add_num))
			goto err;
	}

	return 0;
err:
	put_job(td);
	return -1;
}

/*
 * Parse as if 'o' was a command line
 */
void add_job_opts(const char **o)
{
	struct thread_data *td, *td_parent;
	int i, in_global = 1;
	char jobname[32];

	i = 0;
	td_parent = td = NULL;
	while (o[i]) {
		if (!strncmp(o[i], "name", 4)) {
			in_global = 0;
			if (td)
				add_job(td, jobname, 0);
			td = NULL;
			sprintf(jobname, "%s", o[i] + 5);
		}
		if (in_global && !td_parent)
			td_parent = get_new_job(1, &def_thread);
		else if (!in_global && !td) {
			if (!td_parent)
				td_parent = &def_thread;
			td = get_new_job(0, td_parent);
		}
		if (in_global)
			fio_options_parse(td_parent, (char **) &o[i], 1);
		else
			fio_options_parse(td, (char **) &o[i], 1);
		i++;
	}

	if (td)
		add_job(td, jobname, 0);
}

static int skip_this_section(const char *name)
{
	int i;

	if (!nr_job_sections)
		return 0;
	if (!strncmp(name, "global", 6))
		return 0;

	for (i = 0; i < nr_job_sections; i++)
		if (!strcmp(job_sections[i], name))
			return 0;

	return 1;
}

static int is_empty_or_comment(char *line)
{
	unsigned int i;

	for (i = 0; i < strlen(line); i++) {
		if (line[i] == ';')
			return 1;
		if (line[i] == '#')
			return 1;
		if (!isspace((int) line[i]) && !iscntrl((int) line[i]))
			return 0;
	}

	return 1;
}

/*
 * This is our [ini] type file parser.
 */
int parse_jobs_ini(char *file, int is_buf, int stonewall_flag)
{
	unsigned int global;
	struct thread_data *td;
	char *string, *name;
	FILE *f;
	char *p;
	int ret = 0, stonewall;
	int first_sect = 1;
	int skip_fgets = 0;
	int inside_skip = 0;
	char **opts;
	int i, alloc_opts, num_opts;

	if (is_buf)
		f = NULL;
	else {
		if (!strcmp(file, "-"))
			f = stdin;
		else
			f = fopen(file, "r");

		if (!f) {
			perror("fopen job file");
			return 1;
		}
	}

	string = malloc(4096);

	/*
	 * it's really 256 + small bit, 280 should suffice
	 */
	name = malloc(280);
	memset(name, 0, 280);

	alloc_opts = 8;
	opts = malloc(sizeof(char *) * alloc_opts);
	num_opts = 0;

	stonewall = stonewall_flag;
	do {
		/*
		 * if skip_fgets is set, we already have loaded a line we
		 * haven't handled.
		 */
		if (!skip_fgets) {
			if (is_buf)
				p = strsep(&file, "\n");
			else
				p = fgets(string, 4095, f);
			if (!p)
				break;
		}

		skip_fgets = 0;
		strip_blank_front(&p);
		strip_blank_end(p);

		if (is_empty_or_comment(p))
			continue;
		if (sscanf(p, "[%255[^\n]]", name) != 1) {
			if (inside_skip)
				continue;
			log_err("fio: option <%s> outside of [] job section\n",
									p);
			break;
		}

		name[strlen(name) - 1] = '\0';

		if (skip_this_section(name)) {
			inside_skip = 1;
			continue;
		} else
			inside_skip = 0;

		global = !strncmp(name, "global", 6);

		if (dump_cmdline) {
			if (first_sect)
				log_info("fio ");
			if (!global)
				log_info("--name=%s ", name);
			first_sect = 0;
		}

		td = get_new_job(global, &def_thread);
		if (!td) {
			ret = 1;
			break;
		}

		/*
		 * Seperate multiple job files by a stonewall
		 */
		if (!global && stonewall) {
			td->o.stonewall = stonewall;
			stonewall = 0;
		}

		num_opts = 0;
		memset(opts, 0, alloc_opts * sizeof(char *));

		while (1) {
			if (is_buf)
				p = strsep(&file, "\n");
			else
				p = fgets(string, 4096, f);
			if (!p)
				break;

			if (is_empty_or_comment(p))
				continue;

			strip_blank_front(&p);

			/*
			 * new section, break out and make sure we don't
			 * fgets() a new line at the top.
			 */
			if (p[0] == '[') {
				skip_fgets = 1;
				break;
			}

			strip_blank_end(p);

			if (num_opts == alloc_opts) {
				alloc_opts <<= 1;
				opts = realloc(opts,
						alloc_opts * sizeof(char *));
			}

			opts[num_opts] = strdup(p);
			num_opts++;
		}

		ret = fio_options_parse(td, opts, num_opts);
		if (!ret) {
			if (dump_cmdline)
				for (i = 0; i < num_opts; i++)
					log_info("--%s ", opts[i]);

			ret = add_job(td, name, 0);
		} else {
			log_err("fio: job %s dropped\n", name);
			put_job(td);
		}

		for (i = 0; i < num_opts; i++)
			free(opts[i]);
		num_opts = 0;
	} while (!ret);

	if (dump_cmdline)
		log_info("\n");

	for (i = 0; i < num_opts; i++)
		free(opts[i]);

	free(string);
	free(name);
	free(opts);
	if (!is_buf && f != stdin)
		fclose(f);
	return ret;
}

static int fill_def_thread(void)
{
	memset(&def_thread, 0, sizeof(def_thread));

	fio_getaffinity(getpid(), &def_thread.o.cpumask);

	/*
	 * fill default options
	 */
	fio_fill_default_options(&def_thread);
	return 0;
}

static void usage(const char *name)
{
	printf("%s\n", fio_version_string);
	printf("%s [options] [job options] <job file(s)>\n", name);
	printf("\t--debug=options\tEnable debug logging\n");
	printf("\t--output\tWrite output to file\n");
	printf("\t--timeout\tRuntime in seconds\n");
	printf("\t--latency-log\tGenerate per-job latency logs\n");
	printf("\t--bandwidth-log\tGenerate per-job bandwidth logs\n");
	printf("\t--minimal\tMinimal (terse) output\n");
	printf("\t--version\tPrint version info and exit\n");
	printf("\t--terse-version=x Terse version output format\n");
	printf("\t--help\t\tPrint this page\n");
	printf("\t--cmdhelp=cmd\tPrint command help, \"all\" for all of"
		" them\n");
	printf("\t--showcmd\tTurn a job file into command line options\n");
	printf("\t--eta=when\tWhen ETA estimate should be printed\n");
	printf("\t          \tMay be \"always\", \"never\" or \"auto\"\n");
	printf("\t--readonly\tTurn on safety read-only checks, preventing"
		" writes\n");
	printf("\t--section=name\tOnly run specified section in job file\n");
	printf("\t--alloc-size=kb\tSet smalloc pool to this size in kb"
		" (def 1024)\n");
	printf("\t--warnings-fatal Fio parser warnings are fatal\n");
	printf("\t--max-jobs\tMaximum number of threads/processes to support\n");
	printf("\t--server\tStart a backend fio server\n");
	printf("\t--client=hostname Talk to remove backend fio server at hostname\n");
	printf("\t--net-port=port\tUse specified port for client/server connection\n");
	printf("\nFio was written by Jens Axboe <jens.axboe@oracle.com>");
	printf("\n                   Jens Axboe <jaxboe@fusionio.com>\n");
}

#ifdef FIO_INC_DEBUG
struct debug_level debug_levels[] = {
	{ .name = "process",	.shift = FD_PROCESS, },
	{ .name = "file",	.shift = FD_FILE, },
	{ .name = "io",		.shift = FD_IO, },
	{ .name = "mem",	.shift = FD_MEM, },
	{ .name = "blktrace",	.shift = FD_BLKTRACE },
	{ .name = "verify",	.shift = FD_VERIFY },
	{ .name = "random",	.shift = FD_RANDOM },
	{ .name = "parse",	.shift = FD_PARSE },
	{ .name = "diskutil",	.shift = FD_DISKUTIL },
	{ .name = "job",	.shift = FD_JOB },
	{ .name = "mutex",	.shift = FD_MUTEX },
	{ .name	= "profile",	.shift = FD_PROFILE },
	{ .name = "time",	.shift = FD_TIME },
	{ .name = "net",	.shift = FD_NET },
	{ .name = NULL, },
};

static int set_debug(const char *string)
{
	struct debug_level *dl;
	char *p = (char *) string;
	char *opt;
	int i;

	if (!strcmp(string, "?") || !strcmp(string, "help")) {
		log_info("fio: dumping debug options:");
		for (i = 0; debug_levels[i].name; i++) {
			dl = &debug_levels[i];
			log_info("%s,", dl->name);
		}
		log_info("all\n");
		return 1;
	}

	while ((opt = strsep(&p, ",")) != NULL) {
		int found = 0;

		if (!strncmp(opt, "all", 3)) {
			log_info("fio: set all debug options\n");
			fio_debug = ~0UL;
			continue;
		}

		for (i = 0; debug_levels[i].name; i++) {
			dl = &debug_levels[i];
			found = !strncmp(opt, dl->name, strlen(dl->name));
			if (!found)
				continue;

			if (dl->shift == FD_JOB) {
				opt = strchr(opt, ':');
				if (!opt) {
					log_err("fio: missing job number\n");
					break;
				}
				opt++;
				fio_debug_jobno = atoi(opt);
				log_info("fio: set debug jobno %d\n",
							fio_debug_jobno);
			} else {
				log_info("fio: set debug option %s\n", opt);
				fio_debug |= (1UL << dl->shift);
			}
			break;
		}

		if (!found)
			log_err("fio: debug mask %s not found\n", opt);
	}
	return 0;
}
#else
static int set_debug(const char *string)
{
	log_err("fio: debug tracing not included in build\n");
	return 1;
}
#endif

static void fio_options_fill_optstring(void)
{
	char *ostr = cmd_optstr;
	int i, c;

	c = i = 0;
	while (l_opts[i].name) {
		ostr[c++] = l_opts[i].val;
		if (l_opts[i].has_arg == required_argument)
			ostr[c++] = ':';
		else if (l_opts[i].has_arg == optional_argument) {
			ostr[c++] = ':';
			ostr[c++] = ':';
		}
		i++;
	}
	ostr[c] = '\0';
}

int parse_cmd_line(int argc, char *argv[])
{
	struct thread_data *td = NULL;
	int c, ini_idx = 0, lidx, ret = 0, do_exit = 0, exit_val = 0;
	char *ostr = cmd_optstr;
	int daemonize_server = 0;
	char *cur_client = NULL;
	int backend = 0;

	while ((c = getopt_long_only(argc, argv, ostr, l_opts, &lidx)) != -1) {
		switch (c) {
		case 'a':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			smalloc_pool_size = atoi(optarg);
			break;
		case 't':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			def_thread.o.timeout = atoi(optarg);
			break;
		case 'l':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			write_lat_log = 1;
			break;
		case 'b':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			write_bw_log = 1;
			break;
		case 'o':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			f_out = fopen(optarg, "w+");
			if (!f_out) {
				perror("fopen output");
				exit(1);
			}
			f_err = f_out;
			break;
		case 'm':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			terse_output = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'c':
			exit(fio_show_option_help(optarg));
		case 's':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			dump_cmdline = 1;
			break;
		case 'r':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			read_only = 1;
			break;
		case 'v':
			log_info("%s\n", fio_version_string);
			exit(0);
		case 'V':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			terse_version = atoi(optarg);
			if (terse_version != 2) {
				log_err("fio: bad terse version format\n");
				exit_val = 1;
				do_exit++;
			}
			break;
		case 'e':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			if (!strcmp("always", optarg))
				eta_print = FIO_ETA_ALWAYS;
			else if (!strcmp("never", optarg))
				eta_print = FIO_ETA_NEVER;
			break;
		case 'd':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			if (set_debug(optarg))
				do_exit++;
			break;
		case 'x': {
			size_t new_size;

			fio_client_add_cmd_option(cur_client, argv[optind-1]);

			if (!strcmp(optarg, "global")) {
				log_err("fio: can't use global as only "
					"section\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			new_size = (nr_job_sections + 1) * sizeof(char *);
			job_sections = realloc(job_sections, new_size);
			job_sections[nr_job_sections] = strdup(optarg);
			nr_job_sections++;
			break;
			}
		case 'p':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			exec_profile = strdup(optarg);
			break;
		case FIO_GETOPT_JOB: {
			const char *opt = l_opts[lidx].name;
			char *val = optarg;

			fio_client_add_cmd_option(cur_client, argv[optind-1]);

			if (!strncmp(opt, "name", 4) && td) {
				ret = add_job(td, td->o.name ?: "fio", 0);
				if (ret)
					return 0;
				td = NULL;
			}
			if (!td) {
				int is_section = !strncmp(opt, "name", 4);
				int global = 0;

				if (!is_section || !strncmp(val, "global", 6))
					global = 1;

				if (is_section && skip_this_section(val))
					continue;

				td = get_new_job(global, &def_thread);
				if (!td)
					return 0;
			}

			ret = fio_cmd_option_parse(td, opt, val);
			break;
		}
		case 'w':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			warnings_fatal = 1;
			break;
		case 'j':
			fio_client_add_cmd_option(cur_client, argv[optind-1]);
			max_jobs = atoi(optarg);
			if (!max_jobs || max_jobs > REAL_MAX_JOBS) {
				log_err("fio: invalid max jobs: %d\n", max_jobs);
				do_exit++;
				exit_val = 1;
			}
			break;
		case 'S':
			if (nr_clients) {
				log_err("fio: can't be both client and server\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			is_backend = 1;
			backend = 1;
			break;
		case 'D':
			daemonize_server = 1;
			break;
		case 'P':
			fio_net_port = atoi(optarg);
			break;
		case 'C':
			if (is_backend) {
				log_err("fio: can't be both client and server\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			fio_client_add(optarg);
			cur_client = optarg;
			break;
		default:
			do_exit++;
			exit_val = 1;
			break;
		}
		if (do_exit)
			break;
	}

	if (do_exit)
		exit(exit_val);

	if (nr_clients && fio_clients_connect()) {
		do_exit++;
		exit_val = 1;
		return -1;
	}

	if (is_backend && backend)
		return fio_start_server(daemonize_server);

	if (td) {
		if (!ret)
			ret = add_job(td, td->o.name ?: "fio", 0);
	}

	while (optind < argc) {
		ini_idx++;
		ini_file = realloc(ini_file, ini_idx * sizeof(char *));
		ini_file[ini_idx - 1] = strdup(argv[optind]);
		optind++;
	}

	return ini_idx;
}

int parse_options(int argc, char *argv[])
{
	int job_files, i;

	f_out = stdout;
	f_err = stderr;

	fio_options_fill_optstring();
	fio_options_dup_and_init(l_opts);

	atexit(free_shm);

	if (fill_def_thread())
		return 1;

	job_files = parse_cmd_line(argc, argv);

	if (job_files > 0) {
		for (i = 0; i < job_files; i++) {
			if (fill_def_thread())
				return 1;
			if (nr_clients) {
				if (fio_clients_send_ini(ini_file[i]))
					return 1;
				free(ini_file[i]);
			} else if (!is_backend) {
				if (parse_jobs_ini(ini_file[i], 0, i))
					return 1;
				free(ini_file[i]);
			}
		}
	}

	free(ini_file);
	options_mem_free(&def_thread);

	if (!thread_number) {
		if (dump_cmdline)
			return 0;
		if (exec_profile)
			return 0;
		if (is_backend || nr_clients)
			return 0;

		if (job_files > 0) {
			log_err("No jobs(s) defined\n\n");
			usage(argv[0]);
		}
		return 1;
	}

	if (def_thread.o.gtod_offload) {
		fio_gtod_init();
		fio_gtod_offload = 1;
		fio_gtod_cpu = def_thread.o.gtod_cpu;
	}

	log_info("%s\n", fio_version_string);
	return 0;
}
