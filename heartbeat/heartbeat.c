const static char * _heartbeat_c_Id = "$Id: heartbeat.c,v 1.41 2000/04/08 21:33:35 horms Exp $";
/*
 *	Near term needs:
 *	- Logging of up/down status changes to a file... (or somewhere)
 */

/*
 *	Linux-HA heartbeat code
 *
 *	The basic facilities for round-robin (ring) and IP heartbeats are
 *	contained within.
 *
 *	There is a master configuration file which we open to tell us
 *	what to do.
 *
 *	It has lines like this in it:
 *
 *	serial	/dev/cua0, /dev/cua1
 *	udp	eth0
 *
 *	node	amykathy, kathyamy
 *	node	dralan
 *	keepalive 2
 *	deadtime  10
 *	hopfudge 2
 *	baud 19200
 *	udpport 1001
 *
 *	"Serial" lines tell us about our heartbeat configuration.
 *	If there is more than one serial port configured, we are in a "ring"
 *	configuration, every message not originated on our node is echoed
 *	to the other serial port(s)
 *
 *	"Node" lines tell us about the cluster configuration.
 *	We had better find our uname -n nodename here, or we won't start up.
 *
 *	We ought to complain if we find extra nodes in the stream that aren't in
 *	the master configuration file.
 *
 *	keepalive lines specify the keepalive interval
 *	deadtime lines specify how long we wait before declaring
 *		a node dead
 *	hopfudge says how much larger than #nodes we allow hopcount
 *		to grow before dropping the message
 *
 *	I need to separate things into a "global" configuration file,
 *	and a "local" configuration file, so I can double check
 *	the global against the cluster when changing configurations.
 *	Things like serial port assignments may be node-specific...
 *
 *	This has kind of happened over time.  Haresources and authkeys are
 *	decidely global, whereas ha.cf has remained more local.
 *
 */

/*
 *	Here's our process structure:
 *
 *
 *		Control process - starts off children and reads a fifo
 *			for commands to send to the cluster.  These
 *			commands are sent to write pipes, and status pipe
 *
 *		Status process - reads the status pipe
 *			and forks off child processes to perform actions
 *			associated with config status changes
 *			It also sends out the periodic keepalive messages.
 *
 *		hb channel read processes - each reads a hb channel, and
 *			copies messages to the status pipe.  The tty
 *			version of this cross-echos to the other ttys
 *			in the ring (ring passthrough)
 *
 *		hb channel write processes - one per hb channel, each reads its
 *			own pipe and send the result to its medium
 *
 *	The result of all this hoorah is that we have the following procs:
 *
 *	One Control process
 *	One Master Status process
 *		"n" hb channel read processes
 *		"n" hb channel write processes
 *
 *	For the usual 2 ttys in a ring configuration, this is 6 processes
 *
 *	For a system using only UDP for heartbeats this is 4 processes.
 *
 *	For a system using 2 ttys and UDP, this is 8 processes.
 *
 *	If every second, each node writes out 100 chars of status,
 *	and we have 8 nodes, and the data rate would be about 800 chars/sec.
 *	This would require about 8000 bps.
 *	This seems awfully close to 9600.  Better run faster than that
 *	for such a cluster...  With good UARTs and CTS/RTS, and good cables,
 *	you should be able to.
 *
 *
 *	Process/Pipe configuration:
 *
 *	Control process:
 *		Reads a control fifo for input
 *		Writes master status pipe
 *		Writes heartbeat channel pipes
 *
 *	Master Status process:
 *		Reads master status pipe
 *		forks children, etc. to deal with status changes
 *
 *	heartbeat read processes:
 *		Reads hb channel (tty, UDP, etc)
 *		copying to master status pipe
 *		For ttys ONLY:
 *			copying to tty write pipes (incrementing hop count and
 *				filtering out "ring wraparounds")
 *	Wish List:
 *
 *	Nearest Neighbor heartbeating (? maybe?)
 *		This should replace the current policy of full-ring heartbeats
 *		In this policy, each machine only heartbeats to it's nearest
 *		neighbors.  The nearest neighbors only forward on status CHANGES
 *		to their neighbors.  This means that the total ring traffic
 *		in the non-error case is reduced to the same as a 3-node
 *		cluster.  This is a huge improvement.  It probably means that
 *		19200 is fast enough for almost any size network.
 *		Non-heartbeat admin traffic is forwarded to all members of the
 *		ring as it was before.
 *
 *	IrDA heartbeats
 *		This is a near-exact replacement for ethernet with lower
 *		bandwidth, low costs and fewer points of failure.
 *		The role of an ethernet hub is replaced by a mirror, which
 *		is less likely to fail.  But if it does, it might mean
 *		seven years of bad luck :-)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <netdb.h>

#include <heartbeat.h>
#include <ha_msg.h>
#include <test.h>

#define OPTARGS		"dkrRsv"


int		verbose = 0;

const char *	cmdname = "heartbeat";
const char **	Argv = NULL;
int		Argc = -1;
int		debug = 0;
int		RestartRequested = 0;
int		WeAreRestarting = 0;
int             cluster_already_active = 0;
int             we_are_primary = 0;
int             send_starting_now = 1;
int             nice_failback = 0;
int		starting = 1;
int		killrunninghb = 0;
int		rpt_hb_status = 0;
int		childpid = -1;
char *		watchdogdev = NULL;
int		watchdogfd = -1;
time_t		starttime = 0L;
time_t		next_statsdump = 0L;
void		(*localdie)(void);


struct hb_media*	sysmedia[MAXMEDIA];
const struct hb_media_fns** HB_media;
extern const int	num_hb_media_types;
int			nummedia = 0;
int			status_pipe[2];	/* The Master status pipe */

const char *ha_log_priority[8] = {
	"emerg",
	"alert",
	"crit",
	"error",
	"warn",
	"notice",
	"info",
	"debug"
};


struct sys_config *	config = NULL;
struct node_info *	curnode = NULL;

volatile struct pstat_shm *	procinfo = NULL;
volatile struct process_info *	curproc = NULL;

int	setline(int fd);
void	cleanexit(int rc);
void	debug_sig(int sig);
void	signal_all(int sig);
void	parent_debug_sig(int sig);
void	reread_config_sig(int sig);
void	restart_heartbeat(void);
int	parse_config(const char * cfgfile);
int	parse_ha_resources(const char * cfgfile);
void	dump_config(void);
char *	ha_timestamp(void);
int	add_option(const char *	option, const char * value);
int	add_node(const char * value);
int   	parse_authfile(void);
void	init_watchdog(void);
void	tickle_watchdog(void);
void	usage(void);
int	init_config(const char * cfgfile);
void	init_procinfo(void);
int	initialize_heartbeat(void);
void	init_status_alarm(void);
void	ding(int sig);
void	AlarmUhOh(int sig);
void	dump_proc_stats(volatile struct process_info * proc);
void	dump_all_proc_stats(void);
void	check_node_timeouts(void);
void	request_msg_rexmit(struct node_info *, unsigned long lowseq, unsigned long hiseq);
void	check_rexmit_reqs(void);
void	mark_node_dead(struct node_info* hip);
void	notify_world(struct ha_msg * msg, const char * ostatus);
pid_t	get_running_hb_pid(void);
void	make_daemon(void);
void	heartbeat_monitor(struct ha_msg * msg);
void	send_to_all_media(char * smsg, int len);
void	init_monitor(void);
int	should_drop_message(struct node_info* node, const struct ha_msg* msg);
void	add2_xmit_hist (struct msg_xmit_hist * hist, struct ha_msg* msg
,		unsigned long seq);
void	init_xmit_hist (struct msg_xmit_hist * hist);
void	process_rexmit(struct msg_xmit_hist * hist, struct ha_msg* msg);
void	nak_rexmit(int seqno, const char * reason);
int	req_our_resources(void);
int	giveup_resources(void);

/* The biggies */
void control_process(FILE * f);
void read_child(struct hb_media* mp);
void write_child(struct hb_media* mp);
void master_status_process(void);		/* The real biggie */

#ifdef IRIX
	void setenv(const char *name, const char * value, int);
#endif

pid_t	processes[MAXPROCS];
int	num_procs = 0;
int	send_status_now = 1;	/* Send initial status immediately */
int	dump_stats_now = 0;
int	parse_only = 0;

#define	ADDPROC(pid)	{if (pid > 0 && pid != -1) {processes[num_procs] = (pid); ++num_procs;};}


void
init_procinfo()
{
	int	ipcid;
	char *	shm;
	(void)_heartbeat_c_Id;
	(void)_heartbeat_h_Id;
	(void)_ha_msg_h_Id;
	if ((ipcid = shmget(IPC_PRIVATE, sizeof(*procinfo), 0666)) < 0) {
		ha_perror("Cannot shmget for process status");
		return;
	}
	if (((long)(shm = shmat(ipcid, NULL, 0))) == -1) {
		ha_perror("Cannot shmat for process status");
		shm = NULL;
		return;
	}
	if (shm) {
		procinfo = (struct pstat_shm*) shm;
		memset(shm, 0, PAGE_SIZE);
	}

	/*
	 * Go ahead and "remove" our shared memory now...
	 *
	 * This is cool because the manual says:
	 *
	 *	IPC_RMID    is used to mark the segment as destroyed. It
	 *	will actually be destroyed after the last detach.
	 */
	if (shmctl(ipcid, IPC_RMID, NULL) < 0) {
		ha_perror("Cannot IPC_RMID proc status shared memory id");
	}
}



/* Look up the node in the configuration, returning the node info structure */
struct node_info *
lookup_node(const char * h)
{
	int			j;


	for (j=0; j < config->nodecount; ++j) {
		if (strcmp(h, config->nodes[j].nodename) == 0) {
			return(config->nodes+j);
		}
	}
	return(NULL);
}
char *
ha_timestamp(void)
{
	static char ts[64];
	struct tm*	ttm;
	time_t		now;

	time(&now);
	ttm = localtime(&now);

	sprintf(ts, "%04d/%02d/%02d_%02d:%02d:%02d"
	,	ttm->tm_year+1900, ttm->tm_mon+1, ttm->tm_mday
	,	ttm->tm_hour, ttm->tm_min, ttm->tm_sec);
	return(ts);
}

/* Very unsophisticated HA-error-logging function (deprecated) */
void
ha_error(const char *	msg)
{
	ha_log(LOG_ERR, "%s", msg);
}


/* HA-logging function */
void
ha_log(int priority, const char * fmt, ...)
{
	va_list ap;
	FILE *	fp = NULL;
	char *	fn = NULL;
	char buf[MAXLINE];

	va_start(ap, fmt);
	vsnprintf(buf, MAXLINE, fmt, ap);
	va_end(ap);

	if (config && config->log_facility >= 0) {
		syslog(priority, "%s", buf);
		return;
	}

	if (config) {
		if (priority == LOG_DEBUG) {
                        if (config->use_dbgfile) {
                                fn = config->dbgfile;
                        }
                        else {
                                return;
                        }
                }
                else {
                        if (config->use_logfile) {
                                fn = config->logfile;
                        }
                        else {
                                return;
                        }
                }
	}

	if (!config  || fn != NULL) {
		if (fn) {
			fp = fopen(fn, "a");
		}

		if (fp == NULL) {
			fp = stderr;
		}

		fprintf(fp, "heartbeat: %s %s: %s\n", ha_timestamp()
		,	ha_log_priority[LOG_PRI(priority)], buf);

		if (fp != stderr) {
			fclose(fp);
		}
	}
}

void
ha_perror(const char * fmt, ...)
{
	const char *	err;
	char	errornumber[16];
	extern int	sys_nerr;

	va_list ap;
	char buf[MAXLINE];

	if (errno < 0 || errno >= sys_nerr) {
		sprintf(errornumber, "error %d\n", errno);
		err = errornumber;
	}else{
		err = sys_errlist[errno];
	}
	va_start(ap, fmt);
	vsnprintf(buf, MAXLINE, fmt, ap);
	va_end(ap);

	ha_log(LOG_ERR, "%s: %s", buf, err);

}

/*
 *	This routine starts everything up and kicks off the heartbeat
 *	process.
 */
int
initialize_heartbeat()
{
/*
 *	Things we have to do:
 *
 *	Create all our pipes
 *	Open all our heartbeat channels
 *	fork all our children, and start the old ticker going...
 *
 *	Everything is forked from the parent process.  That's easier to
 *	monitor, and easier to shut down.
 */

	int		j;
	struct stat	buf;
	int		pid;
	FILE *		fifo;
	int		ourproc = 0;

	localdie = NULL;
	starttime = time(NULL);

	if (stat(FIFONAME, &buf) < 0 ||	!S_ISFIFO(buf.st_mode)) {
		ha_log(LOG_ERR, "Creating FIFO %s.", FIFONAME);
		unlink(FIFONAME);
		if (mkfifo(FIFONAME, FIFOMODE) < 0) {
			ha_perror("Cannot make fifo %s.", FIFONAME);
			return(HA_FAIL);
		}
	}

	if (stat(FIFONAME, &buf) < 0) {
		ha_log(LOG_ERR, "FIFO %s does not exist", FIFONAME);
		return(HA_FAIL);
	}else if (!S_ISFIFO(buf.st_mode)) {
		ha_log(LOG_ERR, "%s is not a FIFO", FIFONAME);
		return(HA_FAIL);
	}

	if (pipe(status_pipe) < 0) {
		ha_perror("cannot create status pipe");
		return(HA_FAIL);
	}

	/* Open all our heartbeat channels */

	for (j=0; j < nummedia; ++j) {
		struct hb_media* smj = sysmedia[j];

		if (pipe(smj->wpipe) < 0) {
			ha_perror("cannot create hb channel pipe");
			return(HA_FAIL);
		}
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "opening %s %s", smj->vf->type
			,	smj->name);
		}
		if (smj->vf->open(smj) != HA_OK) {
			ha_log(LOG_ERR, "cannot open %s %s"
			,	smj->vf->type
			,	smj->name);
			return(HA_FAIL);
		}
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "%s channel %s now open..."
			,	smj->vf->type, smj->name);
		}
	}
	ADDPROC(getpid());

	ourproc = procinfo->nprocs;
	procinfo->nprocs++;
	curproc = &procinfo->info[ourproc];
	curproc->type = PROC_CONTROL;
	curproc->pid = getpid();


	/* Now the fun begins... */
/*
 *	Optimal starting order:
 *		master_status_process();
 *		read_child();
 *		write_child();
 *		control_process(FILE * f);
 *
 */
	ourproc = procinfo->nprocs;
	procinfo->nprocs++;
	switch ((pid=fork())) {
		case -1:	ha_perror("Can't fork master status process!");
				return(HA_FAIL);
				break;

		case 0:		/* Child */
				curproc = &procinfo->info[ourproc];
				curproc->type = PROC_MST_STATUS;
				curproc->pid = getpid();
				master_status_process();
				ha_perror("master status process exiting");
				cleanexit(1);
	}
	ADDPROC(pid);
	if (ANYDEBUG) {
		ha_log(LOG_DEBUG, "master status process pid: %d\n", pid);
	}

	for (j=0; j < nummedia; ++j) {
		struct hb_media* mp = sysmedia[j];

		ourproc = procinfo->nprocs;
		procinfo->nprocs++;

		switch ((pid=fork())) {
			case -1:	ha_perror("Can't fork write process");
					return(HA_FAIL);
					break;

			case 0:		/* Child */
					curproc = &procinfo->info[ourproc];
					curproc->type = PROC_HBWRITE;
					curproc->pid = getpid();
					write_child(mp);
					ha_perror("write process exiting");
					cleanexit(1);
		}
		ADDPROC(pid);
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "write process pid: %d\n", pid);
		}
		ourproc = procinfo->nprocs;
		procinfo->nprocs++;

		switch ((pid=fork())) {
			case -1:	ha_perror("Can't fork read process");
					return(HA_FAIL);
					break;

			case 0:		/* Child */
					curproc = &procinfo->info[ourproc];
					curproc->type = PROC_HBREAD;
					curproc->pid = getpid();
					read_child(mp);
					ha_perror("read child process exiting");
					cleanexit(1);
		}
		ADDPROC(pid);
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "read child process pid: %d\n", pid);
		}
	}


	fifo = fopen(FIFONAME, "r");
	if (fifo == NULL) {
		ha_perror("FIFO open failed.");
	}
	(void)open(FIFONAME, O_WRONLY);	/* Keep reads from failing */
	control_process(fifo);
	ha_log(LOG_ERR, "control_process exiting");
	cleanexit(1);
	/*NOTREACHED*/
	return(HA_OK);
}



void
read_child(struct hb_media* mp)
{
	int	msglen;
	int	rc;
	int	statusfd = status_pipe[P_WRITEFD];
	for (;;) {
		struct	ha_msg*	m = mp->vf->read(mp);
		char *		sm;

		if (m == NULL) {
			continue;
		}

                sm = msg2string(m);
		if (sm != NULL) {
			msglen = strlen(sm);
			if (DEBUGPKT) {
				ha_log(LOG_DEBUG
				, "Writing %d bytes/%d fields to status pipe"
				,	msglen, m->nfields);
			}
			if (DEBUGPKTCONT) {
				ha_log(LOG_DEBUG, sm);
			}

			if ((rc=write(statusfd, sm, msglen)) != msglen)  {
				/* Try one extra time if we got EINTR */
				if (errno != EINTR
				||	(rc=write(statusfd, sm, msglen))
				!=	msglen)  {
					ha_perror("Write failure [%d/%d] %s"
					,	rc
					,	errno
					,	"to status pipe");
				}
			}
			ha_free(sm);
		}
		ha_msg_del(m);
	}
}


void
write_child(struct hb_media* mp)
{
	int	ourpipe =	mp->wpipe[P_READFD];
	FILE *	ourfp		= fdopen(ourpipe, "r");

	siginterrupt(SIGALRM, 1);
	for (;;) {
		struct ha_msg * msgp = msgfromstream(ourfp);
		if (msgp == NULL) {
			continue;
		}
		if (mp->vf->write(mp, msgp) != HA_OK) {
			ha_perror("write failure on %s %s."
			,	mp->vf->type, mp->name);
		}
		ha_msg_del(msgp);
	}
}


/* The master control process -- reads control fifo, sends msgs to cluster */
/* Not much to this one, eh? */
void
control_process(FILE * fp)
{
	int	statusfd = status_pipe[P_WRITEFD];
	struct msg_xmit_hist	msghist;
	init_xmit_hist (&msghist);

	/* Catch and propagate debugging level signals... */
	signal(SIGUSR1, parent_debug_sig);
	signal(SIGUSR2, parent_debug_sig);
	siginterrupt(SIGALRM, 1);

	for(;;) {
		struct ha_msg *	msg = controlfifo2msg(fp);
		char *		smsg;
		const char *	type;
		int		len;
		const char *	cseq;
		unsigned long	seqno = -1;
		const  char *	to;
		int		IsToUs;

		if (msg == NULL) {
			ha_log(LOG_ERR, "control_process: NULL message");
			continue;
		}
		if ((type = ha_msg_value(msg, F_TYPE)) == NULL) {
			ha_log(LOG_ERR, "control_process: no type in msg.");
			ha_msg_del(msg);
			continue;
		}
		if ((cseq = ha_msg_value(msg, F_SEQ)) != NULL) {
			if (sscanf(cseq, "%lx", &seqno) != 1
			||	seqno <= 0) {
				ha_log(LOG_ERR, "control_process: "
				"bad sequence number");
				smsg = NULL;
				ha_msg_del(msg);
				continue;
			}
		}

		to = ha_msg_value(msg, F_TO);
		IsToUs = (to != NULL) && (strcmp(to, curnode->nodename) == 0);

		if (strcasecmp(type, T_REXMIT) == 0
		&&	IsToUs) {
			process_rexmit(&msghist, msg);
			ha_msg_del(msg);
			continue;
		}
		/* Convert it to a string */
		smsg = msg2string(msg);

		/* If it didn't convert, throw original message away */
		if (smsg == NULL) {
			ha_msg_del(msg);
			continue;
		}
		/* Remember Messages with sequence numbers */
		if (cseq != NULL) {
			add2_xmit_hist (&msghist, msg, seqno);
		}

		len = strlen(smsg);

		/* Copy the message to the status process */
		write(statusfd, smsg, len);

		send_to_all_media(smsg, len);
		ha_free(smsg);

		/*  Throw away "msg" here if it's not saved above */
		if (cseq == NULL) {
			ha_msg_del(msg);
		}
	}
	/* That's All Folks... */
}

void
send_to_all_media(char * smsg, int len)
{
	int	j;

	/* Throw away some packets if testing is enabled */
	if (TESTSEND) {
		if (TestRand(send_loss_prob)) {
			return;
		}
	}

	/* Send the message to all our heartbeat interfaces */
	for (j=0; j < nummedia; ++j) {
		int	wrc;
		alarm(2);
		wrc=write(sysmedia[j]->wpipe[P_WRITEFD], smsg, len);
		if (wrc < 0) {
			ha_perror("Cannot write to media pipe %d"
			,	j);
			ha_log(LOG_ERR, "Shutting down.");
			signal_all(SIGTERM);
		}else if (wrc != len) {
			ha_log(LOG_ERR
			,	"Short write on media %d [%d vs %d]"
			,	j, wrc, len);
		}
		alarm(0);
	}
}

/* The master status process */
void
master_status_process(void)
{
	struct node_info *	thisnode;
	FILE *			f = fdopen(status_pipe[P_READFD], "r");
	struct ha_msg *		msg = NULL;
	int			resources_requested_yet = 0;
	time_t			lastnow = 0L;
	int 			received_starting = 0;

	init_status_alarm();
	init_watchdog();

	clearerr(f);

	for (;; (msg != NULL) && (ha_msg_del(msg),msg=NULL, 1)) {
		time_t		msgtime;
		time_t		now = time(NULL);
		const char *	from;
		const char *	ts;
		const char *	type;

		if (send_status_now) {
			send_status_now = 0;
			send_local_status();
		}

                if ((send_starting_now && nice_failback) && starting) {
			send_starting_now = 0;
			ha_log(LOG_DEBUG, "Sending starting msg");
			send_local_starting();
		}

		if (dump_stats_now) {
			dump_stats_now = 0;
			dump_all_proc_stats();
		}

		/* Scan nodes to see if any have timed out */
		check_node_timeouts();

		/* Check to see we need to resend any rexmit requests... */
		check_rexmit_reqs();

		/* Check for clock jumps */
		if (now < lastnow) {
			ha_log(LOG_INFO, "Clock jumped backwards. Compensating.");
			send_local_status();
			init_status_alarm();
		}
		lastnow = now;

		msg = msgfromstream(f);

		/* This may be caused by SIGALRM */
		if (msg == NULL) {
			continue;
		}
		now = time(NULL);

		/* Extract message type, originator, timestamp, auth*/
		type = ha_msg_value(msg, F_TYPE);
		from = ha_msg_value(msg, F_ORIG);
		ts = ha_msg_value(msg, F_TIME);

		if (from == NULL || ts == NULL || type == NULL) {
			ha_log(LOG_ERR
			,	"master_status_process: missing from/ts/type");
			continue;
		}

		if (!isauthentic(msg)) {
			ha_log(LOG_DEBUG
			,       "master_status_process: node [%s]"
			" failed authentication", from);
			if (DEBUGPKT) {
				ha_log_message(msg);
			}
			continue;
		}else if(ANYDEBUG) {
			ha_log(LOG_DEBUG
			,       "master_status_process: node [%s] auth  ok"
			,	from);
		}
		/* If a node isn't in the configfile but */

		thisnode = lookup_node(from);
		if (thisnode == NULL) {
#if defined(MITJA)
			ha_log(LOG_WARN
			,   "master_status_process: new node [%s] in message"
			,	from);
			add_node(from);
#else
			ha_log(LOG_ERR
			,   "master_status_process: bad node [%s] in message"
			,	from);
			ha_log_message(msg);
			continue;
#endif
		}

		/* Throw away some incoming packets if testing is enabled */
		if (TESTRCV) {
			if (thisnode != curnode && TestRand(rcv_loss_prob)) {
				continue;
			}
		}



		/* If we're starting and a "starting" message came from another
		 *  node, the primary may take its role. Else act as secondary 
		 *  (of course, if nice_failback is on)
		*/
		
		if (!strcasecmp(type,NOSEQ_PREFIX T_STARTING) 
		&& thisnode != curnode && (starting && nice_failback)) {
                        nice_failback = 0;
			cluster_already_active = 0;
			received_starting = 1;
			starting = 0;
			ha_log(LOG_DEBUG,"Received starting msg from %s"
					,from);
			send_local_starting();
			continue;
		}
				
		/*
		 * Request our resources after a (PPP-induced) delay.
		 * If we have PPP as our only link this delay might have
		 * to be 7 or 8 seconds.  Otherwise the needed delay is
		 * small.  We go ahead if we have any pkt from elsewhere, or
		 * or 10 seconds have elapsed.  If we have a packet that came
		 * in from somewhere else, then cluster comm is working...
		 *
		 */

                if (!WeAreRestarting && !resources_requested_yet
		&&	(thisnode != curnode && (now-starttime) > RQSTDELAY)) {
			if (nice_failback && !received_starting) {
				ha_log(LOG_DEBUG,
					"The cluster is already active");
				cluster_already_active = 1;
			} else {
				if (nice_failback && received_starting) {
					ha_log(LOG_DEBUG,
						"Everybody is starting now");
				}
			}
			resources_requested_yet=1;
			starting = 0;
			req_our_resources();
		}

                if (!strcasecmp(type,NOSEQ_PREFIX T_STARTING)) {
			continue;
		}

                /* Is this message a duplicate, or destined for someone else? */
                if (should_drop_message(thisnode, msg)) {
                        continue;
                }

		/* Is this a status update message? */
		if (strcasecmp(type, T_STATUS) == 0) {
			const char *	status;
			const char *	cseq;
			long		seqno;


			sscanf(ts, "%lx", &msgtime);
			status = ha_msg_value(msg, F_STATUS);
			if (status == NULL)  {
				ha_log(LOG_ERR, "master_status_process: "
				"status update without "
				F_STATUS " field");
				continue;
			}
			if ((cseq = ha_msg_value(msg, F_SEQ)) != NULL) {
				if (sscanf(cseq, "%lx", &seqno) != 1
				||	seqno <= 0) {
					continue;
				}
			}

                        /* Do we already have a newer status? */
                        if (msgtime < thisnode->rmt_lastupdate 
			&&	seqno < thisnode->status_seqno) {
				continue;
			}

			heartbeat_monitor(msg);

			thisnode->rmt_lastupdate = msgtime;
                        thisnode->local_lastupdate = times(NULL);
			thisnode->status_seqno = seqno;

			/* Is the status the same? */
			if (strcasecmp(thisnode->status, status) != 0) {
				ha_log(LOG_INFO
				,	"node %s: status %s"
				,	thisnode->nodename
				,	status);
				notify_world(msg, thisnode->status);
				strcpy(thisnode->status, status);
			}

			/* Did we get a status update on ourselves? */
			if (thisnode == curnode) {
				tickle_watchdog();
			}
		}else if (strcasecmp(type, T_REXMIT) == 0) {
			if (thisnode != curnode) {
				/* Forward to control process */
				send_cluster_msg(msg);
			}
		}else{
			notify_world(msg, thisnode->status);
		}
	}
}

void
check_auth_change(struct sys_config *conf)
{
	if (conf->rereadauth) {
		if (parse_authfile() != HA_OK) {
			/* OOPS.  Sayonara. */
			ha_log(LOG_ERR
			,	"Authentication reparsing error, exiting.");
			signal_all(SIGTERM);
			cleanexit(1);
		}
		conf->rereadauth = 0;
	}
}

/* Function called to set up status alarms */
void
init_status_alarm(void)
{
	siginterrupt(SIGALRM, 1);
	signal(SIGALRM, ding);
	alarm(1);
}


/* Notify the (external) world of an HA event */
void
notify_world(struct ha_msg * msg, const char * ostatus)
{
/*
 *	We invoke our "rc" script with the following arguments:
 *
 *	0:	RC_ARG0	(always the same)
 *	1:	lowercase version of command ("type" field)
 *
 *	All message fields get put into environment variables
 *
 *	The rc script, in turn, runs the scripts it finds in the rc.d
 *	directory (or whatever we call it... ) with the same arguments.
 *
 *	We set the following environment variables for the RC script:
 *	HA_CURHOST:	the node name we're running on
 *	HA_OSTATUS:	Status of node (before this change)
 *
 */
	char		command[STATUSLENG];
	const char *	argv[MAXFIELDS+3];
	const char *	fp;
	char *		tp;
	int		pid, status;

	tp = command;

	fp  = ha_msg_value(msg, F_TYPE);
	ASSERT(fp != NULL && strlen(fp) < STATUSLENG);

	if (fp == NULL || strlen(fp) > STATUSLENG)  {
		return;
	}

	while (*fp) {
		if (isupper(*fp)) {
			*tp = tolower(*fp);
		}else{
			*tp = *fp;
		}
		++fp; ++tp;
	}
	*tp = EOS;
	argv[0] = RC_ARG0;
	argv[1] = command;
	argv[2] = NULL;

	switch ((pid=fork())) {

		case -1:	ha_perror("Can't fork to notify world!");
				break;


		case 0:	{	/* Child */
				int	j;
				for (j=0; j < msg->nfields; ++j) {
					char ename[64];
					sprintf(ename, "HA_%s", msg->names[j]);
					setenv(ename, msg->values[j], 1);
				}
				setenv(OLDSTATUS, ostatus, 1);
				execv(RCSCRIPT, (char **)argv);

				ha_log(LOG_ERR, "cannot exec %s", RCSCRIPT);
				cleanexit(1);
				/*NOTREACHED*/
				break;
			}


		default:	/* Parent */
#if WAITFORCOMMANDS
				waitpid(pid, &status, 0);
#else
				(void)status;
#endif
	}
}

void
debug_sig(int sig)
{
	switch(sig) {
		case SIGUSR1:
			++debug;
			break;

		case SIGUSR2:
			if (debug > 0) {
				--debug;
			}else{
				debug=0;
			}
			break;
	}
	ha_log(LOG_DEBUG, "debug now set to %d [pid %d]", debug, getpid());
	dump_proc_stats(curproc);
}

void
dump_proc_stats(volatile struct process_info * proc)
{
	const char *	ct;
	unsigned long	curralloc;

	if (!proc) {
		return;
	}

	switch(proc->type) {
		case PROC_UNDEF:	ct = "UNDEF";		break;
		case PROC_CONTROL:	ct = "CONTROL";		break;
		case PROC_MST_STATUS:	ct = "MST_STATUS";	break;
		case PROC_HBREAD:	ct = "HBREAD";		break;
		case PROC_HBWRITE:	ct = "HBWRITE";		break;
		case PROC_PPP:		ct = "PPP";		break;
		default:		ct = "huh?";		break;
	}

	ha_log(LOG_INFO, "MSG stats: %ld/%ld age %ld [pid%d/%s]"
	,	proc->allocmsgs, proc->totalmsgs
	,	time(NULL) - proc->lastmsg, proc->pid, ct);

	if (proc->numalloc > proc->numfree) {
		curralloc = proc->numalloc - proc->numfree;
	}else{
		curralloc = 0;
	}

	ha_log(LOG_INFO, "ha_malloc stats: %lu/%lu  %lu/%lu [pid%d/%s]"
	,	curralloc, proc->numalloc
	,	proc->nbytes_alloc, proc->nbytes_req, proc->pid, ct);

	ha_log(LOG_INFO, "RealMalloc stats: %lu total malloc bytes."
	" pid %d/%s]", proc->mallocbytes, proc->pid, ct);
}
void
dump_all_proc_stats()
{
	int	j;

	for (j=0; j < procinfo->nprocs; ++j) {
		dump_proc_stats(procinfo->info+j);
	}
}


void
parent_debug_sig(int sig)
{
	debug_sig(sig);
	signal_all(sig);
}

void
restart_heartbeat(void)
{
	struct	timeval		tv;
	struct	timeval		newtv;
	struct	timezone	tz;
	long			usecs;
	int			j;
	pid_t			curpid = getpid();
	struct rlimit		oflimits;

	/*
	 * We need to do these things:
	 *
	 *	Wait until a propitious time
	 *
	 *	Kill our child processes
	 *
	 *	close most files...
	 *
	 *	re-exec ourselves with the -R option
	 */
	ha_log(LOG_INFO, "Restarting heartbeat.");

	
	getrlimit(RLIMIT_NOFILE, &oflimits);
	alarm(0);
	sleep(1);

	gettimeofday(&tv, &tz);

	usecs = tv.tv_usec;
	usecs += 200*1000;	/* 200 msec */
	if (usecs > 1000*1000) {
		tv.tv_sec++;
		tv.tv_usec = usecs % 1000000;
	}else{
		tv.tv_usec = usecs;
	}

	/* Pause a bit... */

	do {
		gettimeofday(&newtv, &tz);
	}while (newtv.tv_sec < tv.tv_sec && newtv.tv_usec < tv.tv_usec);


	/* Kill our child processes */
	for (j=0; j < procinfo->nprocs; ++j) {
		pid_t	pid = procinfo->info[j].pid;
		if (pid != curpid) {
			ha_log(LOG_INFO, "Killing process %d", pid);
			kill(pid, SIGKILL);
		}
	}


	for (j=3; j < oflimits.rlim_cur; ++j) {
		close(j);
	}

	ha_log(LOG_INFO, "Performing heartbeat restart exec.");
	execl(HALIB "/heartbeat", "heartbeat", "-R", NULL);
	ha_log(LOG_ERR, "Could not exec " HALIB "/heartbeat -R");
	ha_log(LOG_ERR, "Shutting down...");
	kill(curpid, SIGTERM);
}
void
reread_config_sig(int sig)
{
	int	j;

	signal(sig, reread_config_sig);

	/* If we're the control process, tell our children */
	if (curproc->type == PROC_CONTROL) {
		struct	stat	buf;
		if (stat(CONFIG_NAME, &buf) < 0) {
			ha_perror("Cannot stat " CONFIG_NAME);
			return;
		}
		if (buf.st_mtime != config->cfg_time) {
			restart_heartbeat();
			/*NOTREACHED*/
		}
		if (stat(KEYFILE, &buf) < 0) {
			ha_perror("Cannot stat " KEYFILE);
		}else if (buf.st_mtime != config->auth_time) {
			config->rereadauth = 1;
			ha_log(LOG_INFO, "Rereading authentication file.");
			for (j=0; j < procinfo->nprocs; ++j) {
				if (procinfo->info+j != curproc) {
					kill(procinfo->info[j].pid, sig);
				}
			}
		}else{
			ha_log(LOG_INFO, "Configuration unchanged.");
		}
	}
	ParseTestOpts();
}

#define	ONEDAY	(24*60*60)

/* Ding!  Activated once per second in the status process */
void
ding(int sig)
{
	static int	dingtime = 1;
	time_t		now = time(NULL);
	signal(SIGALRM, ding);

	if (debug) {
		ha_log(LOG_DEBUG, "Ding!");
	}

	dingtime --;
	if (dingtime <= 0) {
		dingtime = config->heartbeat_interval;
		/* Note that it's time to send out our status update */
		send_status_now = 1;
	}
	if (now > next_statsdump) {
		if (next_statsdump != 0L) {
			dump_stats_now = 1;
		}
		next_statsdump = now + ONEDAY;
	}
	alarm(1);
}

void
AlarmUhOh(int sig)
{
	signal(SIGALRM, AlarmUhOh);
	if (ANYDEBUG) {
		ha_log(LOG_ERR, "Unexpected alarm in process %d", getpid());
	}
}

/* See if any nodes have timed out */
void
check_node_timeouts(void)
{
	clock_t	now = times(NULL);
	struct node_info *	hip;
	clock_t dead_ticks = (CLK_TCK * config->deadtime_interval);
	clock_t TooOld = now - dead_ticks;
	int	j;

	/* We need to be careful to handle clock_t wrapround carefully */
	if (now < dead_ticks) {
		return; /* Ignore timeouts during wraparound */
			/* This doubles our timeout at this time */
			/* Sorry. */
	}

	for (j=0; j < config->nodecount; ++j) {
		hip= &config->nodes[j];
		if (hip->local_lastupdate > now) {
			/* This means wraparound has occurred */
			/* Fudge it to make comparisons work */
			hip->local_lastupdate = 0L;
		}
		/* If it's recently updated, or already dead, ignore it */
		if (hip->local_lastupdate >= TooOld
		||	strcmp(hip->status, DEADSTATUS) == 0 ) {
			continue;
		}
		mark_node_dead(hip);
	}

}

/* Set our local status to the given value, and send it out*/
int
set_local_status(const char * newstatus)
{
	if (strcmp(newstatus, curnode->status) != 0
	&&	strlen(newstatus) > 1 && strlen(newstatus) < STATUSLENG) {
		strcpy(curnode->status, newstatus);
		send_local_status();
		return(HA_OK);
	}
	return(HA_FAIL);
}

int
send_cluster_msg(struct ha_msg* msg)
{
	char *	smsg;
	const char *	type;

	if (msg == NULL || (type = ha_msg_value(msg, F_TYPE)) == NULL) {
		ha_perror("Invalid message in send_cluster_msg");
		return(HA_FAIL);
	}

	if ((smsg = msg2string(msg)) == NULL) {
		ha_log(LOG_ERR, "out of memory in send_cluster_msg");
		return(HA_FAIL);
	}

	{
	        int     ffd = open(FIFONAME, O_WRONLY);
		int	length;

		if (ffd < 0) {
			ha_free(smsg);
			return(HA_FAIL);
		}

		length=strlen(smsg);
		write(ffd, smsg, length);
		close(ffd);
	}
	ha_free(smsg);

	return(HA_OK);
}

/* Send the starting msg out to the cluster */
int
send_local_starting(void)
{
        struct ha_msg * m;
        int             rc;
        char            timestamp[16];

        sprintf(timestamp, "%lx", time(NULL));

        /* if (debug){ */
                ha_log(LOG_DEBUG, "Sending local starting msg");
        /* } */
        if ((m=ha_msg_new(0)) == NULL) {
                ha_log(LOG_ERR, "Cannot send local starting msg");
                return(HA_FAIL);
        }
        if ((ha_msg_add(m, F_TYPE, NOSEQ_PREFIX T_STARTING) == HA_FAIL) 
        &&  (ha_msg_add(m, F_ORIG, curnode->nodename) == HA_FAIL)
        &&  (ha_msg_add(m, F_TIME, timestamp) == HA_FAIL)) {
                ha_log(LOG_ERR, "send_local_starting: "
                "Cannot create local starting msg");
                rc = HA_FAIL;
        }else{
                rc = send_cluster_msg(m);
        }

        ha_msg_del(m);
        return(rc);
}

/* Send our local status out to the cluster */
int
send_local_status(void)
{
	struct ha_msg *	m;
	int		rc;


	if (debug){
		ha_log(LOG_DEBUG, "Sending local status");
	}
	if ((m=ha_msg_new(0)) == NULL) {
		ha_log(LOG_ERR, "Cannot send local status");
		return(HA_FAIL);
	}
	if (ha_msg_add(m, F_TYPE, T_STATUS) == HA_FAIL
	||	ha_msg_add(m, F_STATUS, curnode->status) == HA_FAIL) {
		ha_log(LOG_ERR, "send_local_status: "
		"Cannot create local status msg");
		rc = HA_FAIL;
	}else{
		rc = send_cluster_msg(m);
	}

	ha_msg_del(m);
	return(rc);
}

/* Mark the given node dead */
void
mark_node_dead(struct node_info *hip)
{
	struct ha_msg *	hmsg;
	char		timestamp[16];

	if ((hmsg = ha_msg_new(6)) == NULL) {
		ha_log(LOG_ERR, "no memory to mark node dead");
		return;
	}

	sprintf(timestamp, "%lx", time(NULL));

	if (	ha_msg_add(hmsg, F_TYPE, T_STATUS) == HA_FAIL
	||	ha_msg_add(hmsg, F_SEQ, "1") == HA_FAIL
	||	ha_msg_add(hmsg, F_TIME, timestamp) == HA_FAIL
	||	ha_msg_add(hmsg, F_ORIG, hip->nodename) == HA_FAIL
	||	ha_msg_add(hmsg, F_STATUS, "dead") == HA_FAIL
	||	ha_msg_add(hmsg, F_COMMENT, "timeout") == HA_FAIL) {
		ha_log(LOG_ERR, "no memory to mark node dead");
		ha_msg_del(hmsg);
		return;
	}
	ha_log(LOG_WARNING, "node %s: is dead", hip->nodename);

	heartbeat_monitor(hmsg);
	
	if (starting && nice_failback && hip != curnode) {
		ha_log(LOG_DEBUG, "I'm alone... ");
		/* This is one of the  place to put the
		 * SIT_AND_CRY stuff */
		nice_failback = 0;
		req_our_resources();
	}
			
	notify_world(hmsg, hip->status);
	strcpy(hip->status, "dead");
	if (hip == curnode) {
		/* Uh, oh... we're dead! */
		ha_log(LOG_ERR, "No local heartbeat. Forcing shutdown.");
		kill(procinfo->info[0].pid, SIGTERM);
	} else {
                if (we_are_primary && nice_failback) {
                        ha_log(LOG_DEBUG,"%s",  "We are primary again!");
                        we_are_primary = 0;
                        req_our_resources();
                }
        }

	ha_msg_del(hmsg);
}

#define	MONFILE "/proc/ha/.control"
struct fieldname_map {
	const char *	from;
	const char *	to;
};
struct fieldname_map fmap [] = {
	{F_SEQ,		NULL},		/* Drop sequence number */
	{F_TTL,		NULL},
	{F_TYPE,	NULL},
	{F_ORIG,	"node"},
	{F_COMMENT,	"reason"},
	{F_STATUS,	"status"},
	{F_TIME,	"nodetime"},
	{F_LOAD,	"loadavg"},
	{F_AUTH,	NULL},
};

static int	monfd = -1;

#define		RETRYINTERVAL	(3600*24)	/* Once A Day... */
#define	IGNORESIG(s)	((void)signal((s), SIG_IGN))

void init_monitor()
{
	static time_t	lasttry = 0;
	int		j;
	time_t	now;

	if (monfd >= 0) {
		return;
	}
	now = time(NULL);

	if ((now - lasttry) < RETRYINTERVAL) {
		return;
	}

	if ((monfd = open(MONFILE, O_WRONLY)) < 0) {
		ha_error("Cannot open " MONFILE);
		lasttry = now;
		return;
	}

	for (j=0; j < config->nodecount; ++j) {
		char		mon[MAXLINE];
		sprintf(mon, "add=?\ntype=node\nnode=%s\n"
		,	config->nodes[j].nodename);
		write(monfd, mon, strlen(mon));
	}
}

void
heartbeat_monitor(struct ha_msg * msg)
{
#if 0
	char		mon[MAXLINE];
	char *		outptr;
	int		j;
	int		k;
	const char *	last = mon + MAXLINE-1;
	int		rc, size;

	return;
	/*NOTREACHED*/

	init_monitor();
	if (monfd < 0) {
		return;
	}

	sprintf(mon, "hb=?\nhbtime=%lx\n", time(NULL));
	outptr = mon + strlen(mon);

	for (j=0; j < msg->nfields; ++j) {
		const char *	name = msg->names[j];
		const char *	value = msg->values[j];
		int	namelen, vallen;

		if (name == NULL || value == NULL) {
			continue;
		}
		for (k=0; k < DIMOF(fmap); ++k) {
			if (strcmp(name, fmap[k].from) == 0) {
				name = fmap[k].to;
				break;
			}
		}
		namelen = strlen(name);
		vallen = strlen(value);
		if (outptr + (namelen+vallen+2) >= last) {
			ha_log(LOG_ERR, "monitor message too long");
			return;
		}
		strcat(outptr, name);
		outptr += namelen;
		strcat(outptr, "=");
		outptr += 1;
		strcat(outptr, value);
		outptr += vallen;
		strcat(outptr, "\n");
		outptr += 1;
	}


	size = outptr - mon;
	errno = 0;
	if ((rc=write(monfd, mon, size)) != size) {
		ha_perror("cannot write monitor message");
		close(monfd);
		monfd = -1;
	}
#endif
}

int
req_our_resources()
{
	FILE *	rkeys;
	char	cmd[MAXLINE];
	char	getcmd[MAXLINE];
	char	buf[MAXLINE];
	int	finalrc = HA_OK;
	int	rc;
	int	rsc_count = 0;

	
	ha_log(LOG_INFO, "Requesting our resources.");
	sprintf(cmd, HALIB "/ResourceManager listkeys %s", curnode->nodename);

	if ((rkeys = popen(cmd, "r")) == NULL) {
		ha_log(LOG_ERR, "Cannot run command %s", cmd);
		return(HA_FAIL);
	}


	for (;;) {
		errno = 0;
		if (fgets(buf, MAXLINE, rkeys) == NULL) {
			if (ferror(rkeys)) {
				if (errno == EINTR) {
					/* Ding!  -- our alarm went off... */
					clearerr(rkeys);
					continue;
				}
				ha_perror("req_our_resources: fgets failure");
			}
			break;
		}
		++rsc_count;

		if (buf[strlen(buf)-1] == '\n') {
			buf[strlen(buf)-1] = EOS;
		}

                /* If the cluster is already active, act as standby. */
                if (cluster_already_active && nice_failback) {
                        ha_log(LOG_DEBUG,
                        "Acting as standby for resource %s",buf);
                } else {
			sprintf(getcmd, HALIB "/req_resource %s &", buf);
			if ((rc=system(getcmd)) != 0) {
				ha_perror("%s returned %d", getcmd, rc);
				finalrc=HA_FAIL;
			}
		}
	}

        if (rsc_count && nice_failback) {
                cluster_already_active = 0;
                we_are_primary = 1;
        }

	rc=pclose(rkeys);
	if (rc < 0 && errno != ECHILD) {
		ha_perror("pclose(%s) returned %d", cmd, rc);
	}else if (rc > 0) {
		ha_log(LOG_ERR, "[%s] exited with 0x%x", cmd, rc);
	}
	if (rsc_count == 0) {
		ha_log(LOG_INFO, "No local resources [%s]", cmd);
	}else if (ANYDEBUG) {
		ha_log(LOG_INFO, "%d local resources from [%s]"
		,	rsc_count, cmd);
	}
	return(finalrc);
}

int
giveup_resources()
{
	FILE *	rkeys;
	char	cmd[MAXLINE];
	char	buf[MAXLINE];
	int	finalrc = HA_OK;
	int	rc;

	
	ha_log(LOG_INFO, "Giving up all HA resources.");
	/*
	 *	We could do this ourselves fairly easily...
	 */

	sprintf(cmd, HALIB "/ResourceManager listkeys '.*'");

	if ((rkeys = popen(cmd, "r")) == NULL) {
		ha_log(LOG_ERR, "Cannot run command %s", cmd);
		return(HA_FAIL);
	}

	while (fgets(buf, MAXLINE, rkeys) != NULL) {
		if (buf[strlen(buf)-1] == '\n') {
			buf[strlen(buf)-1] = EOS;
		}
		sprintf(cmd, HALIB "/ResourceManager givegroup %s", buf);
		if ((rc=system(cmd)) != 0) {
			ha_log(LOG_ERR, "%s returned %d", cmd, rc);
			finalrc=HA_FAIL;
		}
	}
	pclose(rkeys);
	ha_log(LOG_INFO, "All HA resources relinquished.");
	return(finalrc);
}

/*  usage statement */
void
usage(void)
{
	const char *	optionargs = OPTARGS;
	const char *	thislet;

	fprintf(stderr, "\nUsage: %s [-", cmdname);
	for (thislet=optionargs; *thislet; ++thislet) {
		if (thislet[0] != ':' &&  thislet[1] != ':') {
			fputc(*thislet, stderr);
		}
	}
	fputc(']', stderr);
	for (thislet=optionargs; *thislet; ++thislet) {
		if (thislet[1] == ':') {
			const char *	desc = "unknown-flag-argument";

			/* Put a switch statement here eventually... */

			fprintf(stderr, " [-%c %s]", *thislet, desc);
		}
	}
	fprintf(stderr, "\n");
	cleanexit(1);
}


int
main(int argc, const char ** argv)
{
	int	flag;
	int	argerrs = 0;
	int	j;
	extern int	optind;
	pid_t	running_hb_pid = get_running_hb_pid();

	Argc = argc;
	Argv = argv;


	while ((flag = getopt(argc, (char **)argv, OPTARGS)) != EOF) {

		switch(flag) {

			case 'd':
				++debug;
				break;
			case 'k':
				++killrunninghb;
				break;
			case 'r':
				++RestartRequested;
				break;
			case 'R':
				++WeAreRestarting;
				break;
			case 's':
				++rpt_hb_status;
				break;

			case 'v':
				++verbose;
				break;

			default:
				++argerrs;
				break;
		}
	}

	if (optind > argc) {
		++argerrs;
	}
	if (argerrs) {
		usage();
	}


	setenv(HADIRENV, HA_D, 1);
	setenv(DATEFMT, HA_DATEFMT, 1);
	setenv(HAFUNCENV, HA_FUNCS, 1);

	init_procinfo();

	/* Perform static initialization for all our heartbeat medium types */
	for (j=0; j < num_hb_media_types; ++j) {
		if (HB_media[j]->init() != HA_OK) {
			ha_log(LOG_ERR
			,	"Initialization failure for %s channel"
			,	HB_media[j]->type);
			return(HA_FAIL);
		}
	}

	/*
	 *	We've been asked to shut down the currently running heartbeat
	 *	process
	 */

	if (killrunninghb) {

		if (running_hb_pid < 0) {
			fprintf(stderr
			,	"ERROR: Heartbeat not currently running.\n");
			cleanexit(1);
		}
			
		if (kill(running_hb_pid, SIGTERM) >= 0) {
			/* Wait for the running heartbeat to die */
			alarm(0);
			do {
				sleep(1);
			}while (kill(running_hb_pid, 0) >= 0);
			cleanexit(0);
		}
		fprintf(stderr, "ERROR: Could not kill pid %d",running_hb_pid);
		perror(" ");
		cleanexit(1);
	}

	/*
	 *	Report status of heartbeat processes, etc.
	 *	We report in both Red Hat and SuSE formats...
	 */
	if (rpt_hb_status) {

		if (running_hb_pid < 0) {
			printf("%s is stopped. No process\n", cmdname);
		}else{
			printf("%s OK [pid %d et al] is running...\n"
			,	cmdname, running_hb_pid);
		}
		cleanexit(0);
	}

	/*
	 *	We should perform an "exec" of ourselves to restart.
	 */

	if (WeAreRestarting) {

		if (running_hb_pid < 0) {
			fprintf(stderr, "ERROR: %s is not running.\n", cmdname);
			cleanexit(1);
		}
		if (running_hb_pid != getpid()) {
			fprintf(stderr
			,	"ERROR: Heartbeat already running [pid %d].\n"
			,	running_hb_pid);
			cleanexit(1);
		}
	}

	/*
	 *	We've been asked to restart currently running heartbeat process
	 *	(or at least get it to reread it's configuration files)
	 */

	if (RestartRequested) {
		if (running_hb_pid < 0) {
			fprintf(stderr
			,	"ERROR: Heartbeat not currently running.\n");
			cleanexit(1);
		}

		if (init_config(CONFIG_NAME)&&parse_ha_resources(RESOURCE_CFG)){
			ha_log(LOG_INFO
			,	"Signalling heartbeat pid %d to reread"
			" config files", running_hb_pid);
			if (kill(running_hb_pid, SIGHUP) >= 0) {
				cleanexit(0);
			}
			ha_perror("Unable to send SIGHUP to pid %d"
			,	running_hb_pid);
		}else{
			ha_log(LOG_INFO
			,	"Config errors: Heartbeat pid %d NOT restarted"
			,	running_hb_pid);
		}
		cleanexit(1);
	}

	if (init_config(CONFIG_NAME) && parse_ha_resources(RESOURCE_CFG)) {
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG
			,	"HA configuration OK.  Heartbeat started.\n");
		}
		if (verbose) {
			dump_config();
		}
		make_daemon();
		setenv(LOGFENV, config->logfile, 1);
		setenv(DEBUGFENV, config->dbgfile, 1);
		if (config->log_facility >= 0) {
			char	facility[40];
			sprintf(facility, "%d", config->log_facility);
			setenv(LOGFACILITY, facility, 1);
		}
		ParseTestOpts();
		initialize_heartbeat();
	}else{
		ha_log(LOG_ERR, "Configuration error, heartbeat not started.");
		cleanexit(1);
	}
	cleanexit(0);

	/*NOTREACHED*/
	return(HA_FAIL);
}

void
cleanexit(rc)
	int	rc;
{
	if (ANYDEBUG) {
		ha_log(LOG_DEBUG, "Exiting from pid %d [%d]"
		,	getpid(), rc);
	}
	if(config && config->log_facility >= 0) {
		closelog();
	}
	exit(rc);
}

void
signal_all(int sig)
{
	int us = getpid();
	int j;
	for (j=0; j < num_procs; ++j) {
		if (processes[j] != us) {
			if (ANYDEBUG) {
				ha_log(LOG_DEBUG,
				       "%d: Signalling process %d [%d]"
				       ,	getpid(), processes[j], sig);
			}
			kill(processes[j], sig);
		}
	}
	switch (sig) {
		case SIGTERM:
			if (localdie) {
				(*localdie)();
			}
			if (curproc && curproc->type == PROC_CONTROL) {
				ha_log(LOG_INFO, "Heartbeat shutdown in progress.");
				giveup_resources();
				ha_log(LOG_INFO, "Heartbeat shutdown complete.");
				unlink(PIDFILE);
			}
			cleanexit(sig);
			break;
	}
}


pid_t
get_running_hb_pid()
{
	pid_t	pid;
	FILE *	lockfd;
	if ((lockfd = fopen(PIDFILE, "r")) != NULL
	&&	fscanf(lockfd, "%d", &pid) == 1 && pid > 0) {
		if (kill(pid, 0) >= 0 || errno != ESRCH) {
			fclose(lockfd);
			return(pid);
		}
	}
	if (lockfd != NULL) {
		fclose(lockfd);
	}
	return(-1);
}


void
make_daemon(void)
{
	pid_t		pid;
	FILE *		lockfd;
	sigset_t	sighup;

	extern pid_t getsid(pid_t);


	/* See if heartbeat is already running... */

	if ((pid=get_running_hb_pid()) > 0 && pid != getpid()) {
		ha_log(LOG_ERR, "%s: already running [pid %d].\n"
		,	cmdname, pid);
		fprintf(stderr, "%s: already running [pid %d].\n"
		,	cmdname, pid);
		exit(HA_FAILEXIT);
	}

	/* Guess not. Go ahead and start things up */

	if (!WeAreRestarting) {
		pid = fork();
		if (pid < 0) {
			fprintf(stderr, "%s: could not start daemon\n"
			,	cmdname);
			perror("fork");
			exit(HA_FAILEXIT);
		}else if (pid > 0) {
			exit(HA_OKEXIT);
		}
	}
	pid = getpid();
	lockfd = fopen(PIDFILE, "w");
	if (lockfd != NULL) {
		fprintf(lockfd, "%d\n", pid);
		fclose(lockfd);
	}
	if (getsid(0) != pid) {
		if (setsid() < 0) {
			fprintf(stderr, "%s: setsid() failure.", cmdname);
			perror("setsid");
		}
	}

	sigemptyset(&sighup);
	sigaddset(&sighup, SIGHUP);
	if (sigprocmask(SIG_UNBLOCK, &sighup, NULL) < 0) {
		fprintf(stderr, "%s: could not unblock SIGHUP signal\n"
		,	cmdname);
	}

#ifdef	SIGTTOU
	IGNORESIG(SIGTTOU);
#endif
#ifdef	SIGTTIN
	IGNORESIG(SIGTTIN);
#endif

	/* Maybe we shouldn't do this on Linux */
#ifdef	SIGCHLD
	IGNORESIG(SIGCHLD);
#endif

#ifdef	SIGQUIT
	IGNORESIG(SIGQUIT);
#endif
#ifdef	SIGSTP
	IGNORESIG(SIGSTP);
#endif
	(void)signal(SIGUSR1, debug_sig);
	(void)signal(SIGUSR2, debug_sig);
	(void)signal(SIGHUP, reread_config_sig);
	(void)signal(SIGALRM, AlarmUhOh);

	(void)signal(SIGTERM, signal_all);
	umask(022);
	close(FD_STDIN);
	close(FD_STDOUT);
	if (!debug) {
		close(FD_STDERR);
	}
	chdir(HA_D);
}

void
init_watchdog(void)
{
	if (watchdogfd < 0 && watchdogdev != NULL) {
		watchdogfd = open(watchdogdev, O_WRONLY);
		if (watchdogfd >= 0) {
			ha_log(LOG_NOTICE, "Using watchdog device: %s"
			       , watchdogdev);
			tickle_watchdog();
		}else{
			ha_log(LOG_ERR, "Cannot open watchdog device: %s"
			,	watchdogdev);
		}
	}
}

void
tickle_watchdog(void)
{
	if (watchdogfd >= 0) {
		if (write(watchdogfd, "", 1) != 1) {
			close(watchdogfd);
			watchdogfd=-1;
			ha_perror("Watchdog write failure: closing %s!\n"
			,	watchdogdev);
		}
	}
}

void
ha_assert(const char * assertion, int line, const char * file)
{
	ha_log(LOG_ERR, "Assertion \"%s\" failed on line %d in file \"%s\""
	,	assertion, line, file);
	cleanexit(1);
}

/*
 *	Check to see if we should copy this packet further into the ring
 */
int
should_ring_copy_msg(struct ha_msg *m)
{
	const char *	us = curnode->nodename;
	const char *	from;	/* Originating Node name */
	const char *	ttl;	/* Time to live */

	/* Get originator and time to live field values */
	if ((from = ha_msg_value(m, F_ORIG)) == NULL
	||	(ttl = ha_msg_value(m, F_TTL)) == NULL) {
			ha_log(LOG_ERR, "bad packet in should_copy_ring_pkt");
			return(0);
	}
	/* Is this message from us? */
	if (strcmp(from, us) == 0 || ttl == NULL || atoi(ttl) <= 0) {
		/* Avoid infinite loops... Ignore this message */
		return(0);
	}

	/* Must be OK */
	return(1);
}


/*
 *	Right now, this is a little too simple.  There is no provision for
 *	sequence number wraparounds.  But, it will take a very long
 *	time to wrap around (~ 100 years)
 *
 *	I suspect that there are better ways to do this, but this will
 *	do for now...
 */
#define	SEQGAP	100	/* A heuristic number */
#define KEEPIT  0
#define DROPIT  1

/*
 *	Should we ignore this packet, or pay attention to it?
 */
int
should_drop_message(struct node_info * thisnode, const struct ha_msg *msg)
{
	struct seqtrack *	t = &thisnode->track;
	const char *		cseq = ha_msg_value(msg, F_SEQ);
	const char *		to = ha_msg_value(msg, F_TO);
	const char *		type = ha_msg_value(msg, F_TYPE);
	unsigned long		seq;
	int			IsToUs;
	int			j;

	/* Some packet types shouldn't have sequence numbers */
	if (type != NULL && strncmp(type, NOSEQ_PREFIX, sizeof(NOSEQ_PREFIX)-1) 
	==	0) {
		return(KEEPIT);
	}

	if (cseq  == NULL || sscanf(cseq, "%lx", &seq) != 1 ||	seq <= 0) {
		ha_log(LOG_ERR, "should_drop_message: bad sequence number");
		ha_log_message(msg);
		return(DROPIT);
	}

	/*
	 * We need to do sequence number processing on every
	 * packet, even those that aren't sent to us.
	 */
	IsToUs = (to == NULL) || (strcmp(to, curnode->nodename) == 0);

	/* Is this packet in sequence? */
	if (t->last_seq == NOSEQUENCE || seq == (t->last_seq+1)) {
		t->last_seq = seq;
		return(IsToUs ? KEEPIT : DROPIT);
	}else if (seq == t->last_seq) {
		/* Same as last-seen packet -- very common case */
		if (DEBUGPKT) {
			ha_log(LOG_DEBUG,
			       "should_drop_message: Duplicate packet(1)");
		}
		return(DROPIT);
	}

	/* Not in sequence... Hmmm... */

	/* Is it newer than the last packet we got? */

	if (seq > t->last_seq) {

		/* Yes.  Record the missing packets */
		unsigned long	k;
		unsigned long	nlost;
		nlost = ((unsigned long)(seq - (t->last_seq+1)));
		ha_log(LOG_ERR, "%lu lost packet(s) for [%s] [%lu:%lu]"
		,	nlost, thisnode->nodename, t->last_seq, seq);

		if (nlost > SEQGAP) {
			/* Something bad happened.  Start over */
			/* This keeps the loop below from going a long time */
			t->nmissing = 0;
			t->last_seq = seq;
			ha_log(LOG_ERR, "lost a lot of packets!");
			return(IsToUs ? KEEPIT : DROPIT);
		}else{
			request_msg_rexmit(thisnode, t->last_seq+1L, seq-1L);
		}

		/* Try and Record each of the missing sequence numbers */
		for(k = t->last_seq+1; k < seq; ++k) {
			if (t->nmissing < MAXMISSING-1) {
				t->seqmissing[t->nmissing] = k;
				++t->nmissing;
			}else{
				int		minmatch = -1;
				unsigned long	minseq = INT_MAX;
				/*
				 * Replace the lowest numbered missing seqno
				 * with this one
				 */
				for (j=0; j < MAXMISSING; ++j) {
					if (t->seqmissing[j] == NOSEQUENCE) {
						minmatch = j;
						break;
					}
					if (minmatch < 0
					|| t->seqmissing[j] < minseq) {
						minmatch = j;
						minseq = t->seqmissing[j];
					}
				}
				t->seqmissing[minmatch] = k;
			}
		}
		t->last_seq = seq;
		return(IsToUs ? KEEPIT : DROPIT);
	}
	/*
	 * This packet appears to be older than the last one we got.
	 */

	/*
	 * Is it a (recorded) missing packet?
	 */
	for (j=0; j < t->nmissing; ++j) {
		/* Is this one of our missing packets? */
		if (seq == t->seqmissing[j]) {
			/* Yes.  Delete it from the list */
			t->seqmissing[j] = NOSEQUENCE;
			/* Did we delete the last one on the list */
			if (j == (t->nmissing-1)) {
				t->nmissing --;
			}

			/* Swallow up found packets */
			while (t->nmissing > 0
			&&	t->seqmissing[t->nmissing-1] == NOSEQUENCE) {	
				t->nmissing --;
			}
			if (t->nmissing == 0) {
				ha_log(LOG_INFO, "No pkts missing from %s!"
				,	thisnode->nodename);
			}
			return(IsToUs ? KEEPIT : DROPIT);
		}
	}
	/*
	 * Is it a the result of a restart?
	 *
	 * We say it's the result of a restart
	 *	IF the sequence number is a small or a lot smaller than
	 *		the last known sequence number
	 *	AND the timestamp on the packet is newer than the
	 *		last known timestamp for that node.
	 */

	/* Does this look like a restart? */
	if (seq < SEQGAP || ((seq+SEQGAP) < t->last_seq)) {
		const char *	sts;
		time_t	newts = 0L;
		if ((sts = ha_msg_value(msg, F_TIME)) == NULL
		||	sscanf(sts, "%lx", &newts) != 1 || newts == 0L) {
			/* Toss it.  No valid timestamp */
			ha_log(LOG_ERR, "should_drop_message: bad timestamp");
			return(DROPIT);
		}
		/* Is the timestamp newer, and the sequence number smaller? */
		if (newts > thisnode->rmt_lastupdate) {
			/* Yes.  Looks like a software restart to me... */
			thisnode->rmt_lastupdate = newts;
			ha_log(LOG_NOTICE  /* or just INFO ? */
			,	"node %s seq restart %ld vs %ld"
			,	thisnode->nodename
			,	seq, t->last_seq);
			t->nmissing = 0;
			t->last_seq = seq;
			t->last_rexmit_req = 0L;
			return(IsToUs ? KEEPIT : DROPIT);
		}
	}
	/* This is a duplicate packet (or a really old one we lost track of) */
	if (DEBUGPKT) {
		ha_log(LOG_DEBUG, "should_drop_message: Duplicate packet");
		ha_log_message(msg);
	}
	return(DROPIT);

}

void
request_msg_rexmit(struct node_info *node, unsigned long lowseq, unsigned long hiseq)
{
	struct ha_msg*	hmsg;
	char	low[16];
	char	high[16];
	if ((hmsg = ha_msg_new(6)) == NULL) {
		ha_log(LOG_ERR, "no memory for " T_REXMIT);
	}

	sprintf(low, "%lu", lowseq);
	sprintf(high, "%lu", hiseq);


	if (	hmsg != NULL
	&& 	ha_msg_add(hmsg, F_TYPE, T_REXMIT) == HA_OK
	&&	ha_msg_add(hmsg, F_TO, node->nodename)==HA_OK
	&&	ha_msg_add(hmsg, F_FIRSTSEQ, low) == HA_OK
	&&	ha_msg_add(hmsg, F_LASTSEQ, high) == HA_OK) {
		/* Send a re-transmit request */
		if (send_cluster_msg(hmsg) == HA_FAIL) {
			ha_log(LOG_ERR, "cannot send " T_REXMIT
			" request to %s", node->nodename);
		}
		node->track.last_rexmit_req = times(NULL);
	}else{
		ha_log(LOG_ERR, "no memory for " T_REXMIT);
	}
	ha_msg_del(hmsg);
}
void
check_rexmit_reqs(void)
{
	clock_t	now = 0L;
	int	j;

	for (j=0; j < config->nodecount; ++j) {
		struct node_info *	hip = &config->nodes[j];
		struct seqtrack *	t = &hip->track;
		int			seqidx;

		if (t->nmissing <= 0 ) {
			continue;
		}
		/* We rarely reach this code, so avoid the extra system call */
		if (now == 0L) {
			now = times(NULL);
		}
		/* Allow for lbolt wraparound here */
		if ((now - t->last_rexmit_req) <= CLK_TCK && now >= t->last_rexmit_req) {
			continue;
		}
		/* Time to ask for some packets again ... */
		for (seqidx = 0; seqidx < t->nmissing; ++seqidx) {
			if (t->seqmissing[seqidx] != NOSEQUENCE) {
				/*
				 * The code for asking for these by groups here is
				 * complicated.  This code is not.
				 */
				request_msg_rexmit(hip, t->seqmissing[seqidx]
				,	t->seqmissing[seqidx]);
			}
		}
	}
}

/* Initialize the transmit history */
void
init_xmit_hist (struct msg_xmit_hist * hist)
{
	int	j;

	hist->lastmsg = MAXMSGHIST-1;
	hist->hiseq = hist->lowseq = 0;
	for (j=0; j< MAXMSGHIST; ++j) {
		hist->msgq[j] = NULL;
		hist->seqnos[j] = 0;
		hist->lastrexmit[j] = 0L;
	}
}

/* Add a packet to a channel's transmit history */
void
add2_xmit_hist (struct msg_xmit_hist * hist, struct ha_msg* msg
,	unsigned long seq)
{
	int	slot;

	/* Figure out which slot to put the message in */
	slot = hist->lastmsg+1;
	if (slot >= MAXMSGHIST) {
		slot = 0;
	}
	hist->hiseq = seq;
	if (hist->lowseq == 0) {
		hist->lowseq = seq;
	}
	/* Throw away old packet in this slot */
	if (hist->msgq[slot] != NULL) {
		/* Lowseq is less than the lowest recorded seqno */
		hist->lowseq = hist->seqnos[slot];
		ha_msg_del(hist->msgq[slot]);
	}
	hist->msgq[slot] = msg;
	hist->seqnos[slot] = seq;
	hist->lastrexmit[slot] = 0L;
	hist->lastmsg = slot;
}

void
process_rexmit (struct msg_xmit_hist * hist, struct ha_msg* msg)
{
	const char *	cfseq;
	const char *	clseq;
	int		fseq = 0;
	int		lseq = 0;
	int		thisseq;
	int		firstslot = hist->lastmsg-1;

	if ((cfseq = ha_msg_value(msg, F_FIRSTSEQ)) == NULL
	||	(clseq = ha_msg_value(msg, F_LASTSEQ)) == NULL
	||	(fseq=atoi(cfseq)) <= 0 || (lseq=atoi(clseq)) <= 0
	||	fseq > lseq) {
		ha_log(LOG_ERR, "Invalid rexmit seqnos");
		ha_log_message(msg);
	}

	for (thisseq = lseq; thisseq >= fseq; --thisseq) {
		int	msgslot;
		int	foundit = 0;
		if (thisseq <= hist->lowseq) {
			/* Lowseq is less than the lowest recorded seqno */
			nak_rexmit(thisseq, "seqno too low");
			continue;
		}
		if (thisseq > hist->hiseq) {
			nak_rexmit(thisseq, "seqno too high");
			continue;
		}

		for (msgslot = firstslot
		;	!foundit && msgslot != (firstslot+1); --msgslot) {
			char *	smsg;
			int	len;
			clock_t	now = times(NULL);
			clock_t	last_rexmit;
			if (msgslot < 0) {
				msgslot = MAXMSGHIST;
			}
			if (hist->msgq[msgslot] == NULL) {
				continue;
			}
			if (hist->seqnos[msgslot] != thisseq) {
				continue;
			}

			/*
			 * We resend a packet unless it has been re-sent in
			 * the last second.  We treat lbolt wraparound as though
			 * the packet needs resending
			 */
			last_rexmit = hist->lastrexmit[msgslot];
			if (last_rexmit != 0L && now > last_rexmit
			&&	(now - last_rexmit) < CLK_TCK) {
				/* Continue to outer loop */
				goto NextReXmit;
			}
			/* Found it!	Let's send it again! */
			firstslot = msgslot -1;
			foundit=1;
			ha_log(LOG_INFO, "Retransmitting pkt %d", thisseq);
			if (DEBUGPKT) {
				ha_log_message(hist->msgq[msgslot]);
			}
			smsg = msg2string(hist->msgq[msgslot]);

			/* If it didn't convert, throw original message away */
			if (smsg != NULL) {
				len = strlen(smsg);
				hist->lastrexmit[msgslot] = now;
				send_to_all_media(smsg, len);
			}

		}
		if (!foundit) {
			nak_rexmit(thisseq, "seqno not found");
		}
NextReXmit:
	}
}
void
nak_rexmit(int seqno, const char * reason)
{
	struct ha_msg*	msg;
	char	sseqno[32];
	char *	smsg;

	sprintf(sseqno, "%d", seqno);
	ha_log(LOG_ERR, "Cannot rexmit pkt %d: %s", seqno, reason);

	if ((msg = ha_msg_new(6)) == NULL) {
		ha_log(LOG_ERR, "no memory for " T_NAKREXMIT);
		return;
	}

	if (ha_msg_add(msg, F_TYPE, T_NAKREXMIT) != HA_OK
	||	ha_msg_add(msg, F_FIRSTSEQ, sseqno) != HA_OK
	||	ha_msg_add(msg, F_COMMENT, reason) != HA_OK) {
		ha_log(LOG_ERR, "cannot create " T_NAKREXMIT, " msg.");
		ha_msg_del(msg);
		return;
	}


	if ((smsg = msg2string(msg)) == NULL) {
		ha_log(LOG_ERR, "cannot create " T_NAKREXMIT, " msg.");
	}else{
		send_to_all_media(smsg, strlen(smsg));
		ha_free(smsg);
	}
	ha_msg_del(msg);
}


void
ParseTestOpts()
{
	const char *	openpath = HA_D "/OnlyForTesting";
	FILE *	fp;
	static struct TestParms p;
	char	name[64];
	char	value[64];

	if ((fp = fopen(openpath, "r")) == NULL) {
		if (TestOpts) {
			ha_log(LOG_INFO, "Test Code Now disabled.");
		}
		TestOpts = NULL;
		return;
	}
	TestOpts = &p;

	memset(&p, 0, sizeof(p));
	p.send_loss_prob = 0;
	p.rcv_loss_prob = 0;

	ha_log(LOG_INFO, "WARNING: Enabling Test Code");

	while((fscanf(fp, "%[a-zA-Z_]=%s\n", name, value) == 2)) {
		if (strcmp(name, "rcvloss") == 0) {
			p.rcv_loss_prob = atof(value);
			p.enable_rcv_pkt_loss = 1;
			ha_log(LOG_INFO, "Receive loss probability = %.3f"
			,	p.rcv_loss_prob);
		}else if (strcmp(name, "xmitloss") == 0) {
			p.send_loss_prob = atof(value);
			p.enable_send_pkt_loss = 1;
			ha_log(LOG_INFO, "Xmit loss probability = %.3f"
			,	p.send_loss_prob);
		}else{
			ha_log(LOG_INFO, "Cannot recognize test param [%s]"
			,	name);
		}
	}
	ha_log(LOG_INFO, "WARNING: Above Options Now Enabled.");
}

#ifdef IRIX
void
setenv(const char *name, const char * value, int why)
{
	char * envp = xmalloc(strlen(name)+strlen(value)+2);
	sprintf(envp, "%s=%s", name, value);
	putenv(envp);
}
#endif
/*
 * $Log: heartbeat.c,v $
 * Revision 1.41  2000/04/08 21:33:35  horms
 * readding logfile cleanup
 *
 * Revision 1.40  2000/04/05 13:40:28  lclaudio
 *   + Added the nice_failback feature. If the cluster is running when
 *         the primary starts it acts as a secondary.
 *
 * Revision 1.39  2000/04/03 08:26:29  horms
 *
 *
 * Tidied up the output from heartbeat.sh (/etc/rc.d/init.d/heartbeat)
 * on Redhat 6.2
 *
 * Loging to syslog if a facility is specified in ha.cf is instead of
 * rather than as well as file logging as per instructions in ha.cf
 *
 * Fixed a small bug in shellfunctions that caused logs to syslog
 * to be garbled.
 *
 * Revision 1.38  1999/12/25 19:00:48  alan
 * I now send local status unconditionally every time the clock jumps backwards.
 *
 * Revision 1.37  1999/12/25 08:44:17  alan
 * Updated to new version stamp
 * Added Lars Marowsky-Bree's suggestion to make the code almost completely
 * immune from difficulties inherent in jumping the clock around.
 *
 * Revision 1.36  1999/11/27 16:00:02  alan
 * Fixed a minor bug about where a continue should go...
 *
 * Revision 1.35  1999/11/26 07:19:17  alan
 * Changed heartbeat.c so that it doesn't say "seqno not found" for a
 * packet which has been retransmitted recently.
 * The code continued to the next iteration of the inner loop.  It needed
 * to continue to the next iteration of the outer loop.  lOOPS!
 *
 * Revision 1.34  1999/11/25 20:13:15  alan
 * Minor retransmit updates.  Need to add another source file to CVS, too...
 * These updates were to allow us to simulate lots of packet losses.
 *
 * Revision 1.33  1999/11/23 08:50:01  alan
 * Put in the complete basis for the "reliable" packet transport for heartbeat.
 * This include throttling the packet retransmission on both sides, both
 * from the requestor not asking too often, and from the resender, who won't
 * retransmit a packet any more often than once a second.
 * I think this looks pretty good at this point (famous last words :-)).
 *
 * Revision 1.32  1999/11/22 20:39:49  alan
 * Removed references to the now-obsolete monitoring code...
 *
 * Revision 1.31  1999/11/22 20:28:23  alan
 * First pass of putting real packet retransmission.
 * Still need to request missing packets from time to time
 * in case retransmit requests get lost.
 *
 * Revision 1.30  1999/11/14 08:23:44  alan
 * Fixed bug in serial code where turning on flow control caused
 * heartbeat to hang.  Also now detect hangs and shutdown automatically.
 *
 * Revision 1.29  1999/11/11 04:58:04  alan
 * Fixed a problem in the Makefile which caused resources to not be
 * taken over when we start up.
 * Added RTSCTS to the serial port.
 * Added lots of error checking to the resource takeover code.
 *
 * Revision 1.28  1999/11/09 07:34:54  alan
 * *Correctly* fixed the problem Thomas Hepper reported.
 *
 * Revision 1.27  1999/11/09 06:13:02  alan
 * Put in Thomas Hepper's bug fix for the alarm occurring when waiting for
 * resources to be listed during initial startup.
 * Also, minor changes to make config work without a linker warning...
 *
 * Revision 1.26  1999/11/08 02:07:59  alan
 * Minor changes for reasons I can no longer recall :-(
 *
 * Revision 1.25  1999/11/06 03:41:15  alan
 * Fixed some bugs regarding logging
 * Also added some printout for initially taking over resources
 *
 * Revision 1.24  1999/10/25 15:35:03  alan
 * Added code to move a little ways along the path to having error recovery
 * in the heartbeat protocol.
 * Changed the code for serial.c and ppp-udp.c so that they reauthenticate
 * packets they change the ttl on (before forwarding them).
 *
 * Revision 1.23  1999/10/19 13:55:36  alan
 * Changed comments about being red hat compatible
 * Also, changed heartbeat.c to be both SuSE and Red Hat compatible in it's -s
 * output
 *
 * Revision 1.22  1999/10/19 01:55:54  alan
 * Put in code to make the -k option loop until the killed heartbeat stops running.
 *
 * Revision 1.21  1999/10/11 14:29:15  alanr
 * Minor malloc tweaks
 *
 * Revision 1.20  1999/10/11 05:18:07  alanr
 * Minor tweaks in mem stats, etc
 *
 * Revision 1.19  1999/10/11 04:50:31  alanr
 * Alan Cox's suggested signal changes
 *
 * Revision 1.18  1999/10/10 22:22:47  alanr
 * New malloc scheme + send initial status immediately
 *
 * Revision 1.17  1999/10/10 20:12:08  alanr
 * New malloc/free (untested)
 *
 * Revision 1.16  1999/10/05 18:47:52  alanr
 * restart code (-r flag) now works as I think it should
 *
 * Revision 1.15  1999/10/05 16:11:49  alanr
 * First attempt at restarting everything with -R/-r flags
 *
 * Revision 1.14  1999/10/05 06:17:06  alanr
 * Fixed various uninitialized variables
 *
 * Revision 1.13  1999/10/05 05:17:34  alanr
 * Added -s (status) option to heartbeat, and used it in heartbeat.sh...
 *
 * Revision 1.12  1999/10/05 04:35:10  alanr
 * Changed it to use the new heartbeat -k option to shut donw heartbeat.
 *
 * Revision 1.11  1999/10/05 04:09:45  alanr
 * Fixed a problem reported by Thomas Hepper where heartbeat won't start if a regular
 * file by the same name as the FIFO exists.  Now I just remove it...
 *
 * Revision 1.10  1999/10/05 04:03:42  alanr
 * added code to implement the -r (restart already running heartbeat process) option.
 * It seems to work and everything!
 *
 * Revision 1.9  1999/10/04 03:12:20  alanr
 * Shutdown code now runs from heartbeat.
 * Logging should be in pretty good shape now, too.
 *
 * Revision 1.8  1999/10/03 03:13:47  alanr
 * Moved resource acquisition to 'heartbeat', also no longer attempt to make the FIFO, it's now done in heartbeat.  It should now be possible to start it up more readily...
 *
 * Revision 1.7  1999/10/02 18:12:08  alanr
 * Create fifo in heartbeat.c and change ha_perror() to  a var args thing...
 *
 * Revision 1.6  1999/09/30 05:40:37  alanr
 * Thomas Hepper's fixes
 *
 * Revision 1.5  1999/09/29 03:22:09  alanr
 * Added the ability to reread auth config file on SIGHUP
 *
 * Revision 1.4  1999/09/27 04:14:42  alanr
 * We now allow multiple strings, and the code for logging seems to also be working...  Thanks Guyscd ..
 *
 * Revision 1.3  1999/09/26 22:00:02  alanr
 * Allow multiple auth strings in auth file... (I hope?)
 *
 * Revision 1.2  1999/09/26 14:01:05  alanr
 * Added Mijta's code for authentication and Guenther Thomsen's code for serial locking and syslog reform
 *
 * Revision 1.1.1.1  1999/09/23 15:31:24  alanr
 * High-Availability Linux
 *
 * Revision 1.34  1999/09/16 05:50:20  alanr
 * Getting ready for 0.4.3...
 *
 * Revision 1.33  1999/09/15 17:47:13  alanr
 * removed the floating point load average calculation.  We didn't use it for anything anyway...
 *
 * Revision 1.32  1999/09/14 22:35:00  alanr
 * Added shared memory for tracking memory usage...
 *
 * Revision 1.31  1999/08/28 21:08:07  alanr
 * added code to handle SIGUSR1 and SIGUSR2 to diddle debug levels and
 * added code to not start heartbeat up if it's already running...
 *
 * Revision 1.30  1999/08/25 06:34:26  alanr
 * Added code to log outgoing messages in a FIFO...
 *
 * Revision 1.29  1999/08/18 04:27:31  alanr
 * #ifdefed out setting signal handler for SIGCHLD to SIG_IGN
 *
 * Revision 1.28  1999/08/17 03:48:11  alanr
 * added log entry...
 *
 */
