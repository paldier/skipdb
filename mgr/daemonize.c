#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <ev.h>

int pid_daemon;
static char *lock_path;

int get_daemonize_pid()
{
	return pid_daemon;
}

static void
child_handler(int signum)
{
	int fd;
	int len;
	int sent;
	char sz[20];

	switch (signum) {

	case SIGALRM: /* timedout daemonizing */
		exit(1);
		break;

	case SIGUSR1: /* positive confirmation we daemonized well */
		/* Create the lock file as the current user */

		fd = open(lock_path, O_TRUNC | O_RDWR | O_CREAT, 0640);
		if (fd < 0) {
			fprintf(stderr,
			   "unable to create lock file %s, code=%d (%s)\n",
				lock_path, errno, strerror(errno));
			exit(1);
		}
		len = sprintf(sz, "%u", pid_daemon);
		sent = write(fd, sz, len);
		if (sent != len)
			fprintf(stderr,
			  "unable write pid to lock file %s, code=%d (%s)\n",
					     lock_path, errno, strerror(errno));

		close(fd);
		exit(!!(sent == len));

	case SIGCHLD: /* daemonization failed */
		exit(1);
		break;
	}
}

static void daemon_closing(int sigact)
{
	if (getpid() == pid_daemon)
		if (lock_path) {
			unlink(lock_path);
		}

	kill(getpid(), SIGKILL);
}

int skipd_daemonize(char *_lock_path)
{
	pid_t sid, parent;
	int fd;
	char buf[10];
	int n, ret;
	struct sigaction act;

	/* already a daemon */
	if (getppid() == 1)
		return 1;

	fd = open(_lock_path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n) {
			n = atoi(buf);
			ret = kill(n, 0);
			if (ret >= 0) {
				fprintf(stderr,
				     "Daemon already running from pid %d\n", n);
				exit(1);
			}
			fprintf(stderr,
			    "Removing stale lock file %s from dead pid %d\n",
								 _lock_path, n);
			unlink(_lock_path);
		}
	}

    lock_path = _lock_path;

	/* Trap signals that we expect to recieve */
	signal(SIGCHLD, child_handler);	/* died */
	signal(SIGUSR1, child_handler); /* was happy */
	signal(SIGALRM, child_handler); /* timeout daemonizing */

	/* Fork off the parent process */
	pid_daemon = fork();
	if (pid_daemon < 0) {
		fprintf(stderr, "unable to fork daemon, code=%d (%s)",
		    errno, strerror(errno));
		exit(1);
	}

	/* If we got a good PID, then we can exit the parent process. */
	if (pid_daemon > 0) {

		/*
		 * Wait for confirmation signal from the child via
		 * SIGCHILD / USR1, or for two seconds to elapse
		 * (SIGALRM).  pause() should not return.
		 */
		alarm(2);

		pause();
		/* should not be reachable */
		exit(1);
	}

	/* At this point we are executing as the child process */
	parent = getppid();
	pid_daemon = getpid();

	/* Cancel certain signals */
	signal(SIGCHLD, SIG_DFL); /* A child process dies */
	signal(SIGTSTP, SIG_IGN); /* Various TTY signals */
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGHUP, SIG_IGN); /* Ignore hangup signal */

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		fprintf(stderr,
			"unable to create a new session, code %d (%s)",
			errno, strerror(errno));
		exit(1);
	}

	/*
	 * Change the current working directory.  This prevents the current
	 * directory from being locked; hence not being able to remove it.
	 */
	if (chdir("/") < 0) {
		fprintf(stderr,
			"unable to change directory to %s, code %d (%s)",
			"/", errno, strerror(errno));
		exit(1);
	}

	/* Redirect standard files to /dev/null */
	if (!freopen("/dev/null", "r", stdin))
		fprintf(stderr, "unable to freopen() stdin, code %d (%s)",
						       errno, strerror(errno));

	if (!freopen("/dev/null", "w", stdout))
		fprintf(stderr, "unable to freopen() stdout, code %d (%s)",
						       errno, strerror(errno));

	if (!freopen("/dev/null", "w", stderr))
		fprintf(stderr, "unable to freopen() stderr, code %d (%s)",
						       errno, strerror(errno));

	/* Tell the parent process that we are A-okay */
	kill(parent, SIGUSR1);

	act.sa_handler = daemon_closing;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGTERM, &act, NULL);

	/* return to continue what is now "the daemon" */

	return 0;
}
