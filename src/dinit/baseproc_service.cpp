#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "baseproc_sys.hpp"
#include "dinit.hpp"
#include "dinit/log.hpp"
#include "dinit/socket.hpp"
#include "proc_service.hpp"

/*
 * Base process implementation (base_process_service).
 *
 * See proc-service.h for interface documentation.
 */

void base_process_service::do_smooth_recovery() noexcept {
	if (!restart_ps_process()) {
		unrecoverable_stop();
		services->process_queues();
	}
}

bool base_process_service::bring_up() noexcept {
	if (restarting) {
		if (pid == -1) {
			return restart_ps_process();
		}
		return true;
	}
	if (!open_socket()) {
		return false;
	}

	restart_interval_count = 0;
	if (start_ps_process(exec_arg_parts, onstart_flags.starts_on_console ||
	                                         onstart_flags.shares_console)) {
		// start_ps_process updates last_start_time, use it also for
		// restart_interval_time:
		restart_interval_time = last_start_time;
		// Note: we don't set a start timeout for PROCESS services.
		if (start_timeout != time_val(0, 0) &&
		    get_type() != service_type_t::PROCESS) {
			restart_timer.arm_timer_rel(event_loop, start_timeout);
			stop_timer_armed = true;
		} else if (stop_timer_armed) {
			restart_timer.stop_timer(event_loop);
			stop_timer_armed = false;
		}
		return true;
	}
	restart_interval_time = last_start_time;
	return false;
}

bool base_process_service::start_ps_process(
    const std::vector<const char *> &cmd, bool on_console) noexcept {
	// In general, you can't tell whether fork/exec is successful. We use a pipe
	// to communicate success/failure from the child to the parent. The pipe is
	// set CLOEXEC so a successful exec closes the pipe, and the parent sees
	// EOF. If the exec is unsuccessful, the errno is written to the pipe, and
	// the parent can read it.

	event_loop.get_time(last_start_time, clock_type::MONOTONIC);

	int pipefd[2];
	if (bp_sys::pipe2(pipefd, O_CLOEXEC) != 0) {
		log(loglevel_t::ERROR, get_name(),
		    ": can't create status check pipe: ", strerror(errno));
		return false;
	}

	const char *logfile = this->logfile.c_str();
	if (*logfile == 0) {
		logfile = "/dev/null";
	}

	bool child_status_registered = false;
	control_conn_t *control_conn = nullptr;

	int control_socket[2] = {-1, -1};
	int notify_pipe[2] = {-1, -1};
	bool have_notify = !notification_var.empty() || force_notification_fd != -1;
	ready_notify_watcher *rwatcher =
	    have_notify ? get_ready_watcher() : nullptr;
	bool ready_watcher_registered = false;

	if (onstart_flags.pass_cs_fd) {
		if (dinit_socketpair(AF_UNIX, SOCK_STREAM, /* protocol */ 0,
		                     control_socket, SOCK_NONBLOCK) != 0) {
			log(loglevel_t::ERROR, get_name(),
			    ": can't create control socket: ", strerror(errno));
			goto out_p;
		}

		// Make the server side socket close-on-exec:
		int fdflags = bp_sys::fcntl(control_socket[0], F_GETFD);
		bp_sys::fcntl(control_socket[0], F_SETFD, fdflags | FD_CLOEXEC);

		try {
			control_conn =
			    new control_conn_t(event_loop, services, control_socket[0]);
		} catch (std::exception &exc) {
			log(loglevel_t::ERROR, get_name(),
			    ": can't launch process; out of memory");
			goto out_cs;
		}
	}

	if (have_notify) {
		// Create a notification pipe:
		if (bp_sys::pipe2(notify_pipe, 0 | O_CLOEXEC) != 0) {
			log(loglevel_t::ERROR, get_name(),
			    ": can't create notification pipe: ", strerror(errno));
			goto out_cs_h;
		}

		// Set the read side as close-on-exec:
		int fdflags = bp_sys::fcntl(notify_pipe[0], F_GETFD);
		bp_sys::fcntl(notify_pipe[0], F_SETFD, fdflags | FD_CLOEXEC);

		// add, but don't yet enable, readiness watcher:
		try {
			rwatcher->add_watch(event_loop, notify_pipe[0], dasynq::IN_EVENTS,
			                    false);
			ready_watcher_registered = true;
		} catch (std::exception &exc) {
			log(loglevel_t::ERROR, get_name(),
			    ": can't add notification watch: ", exc.what());
		}
	}

	// Set up complete, now fork and exec:

	pid_t forkpid;

	try {
		child_status_listener.add_watch(event_loop, pipefd[0],
		                                dasynq::IN_EVENTS);
		child_status_registered = true;

		// We specify a high priority (i.e. low priority value) so that process
		// termination is handled early. This means we have always recorded that
		// the process is terminated by the time that we handle events that
		// might otherwise cause us to signal the process, so we avoid sending a
		// signal to an invalid (and possibly recycled) process ID.
		forkpid = child_listener.fork(event_loop, reserved_child_watch,
		                              dasynq::DEFAULT_PRIORITY - 10);
		reserved_child_watch = true;
	} catch (std::exception &e) {
		log(loglevel_t::ERROR, get_name(), ": Could not fork: ", e.what());
		goto out_cs_h;
	}

	if (forkpid == 0) {
		const char *working_dir_c = nullptr;
		if (!working_dir.empty())
			working_dir_c = working_dir.c_str();
		after_fork(getpid());
		run_proc_params run_params{cmd.data(), working_dir_c, logfile,
		                           pipefd[1],  run_as_uid,    run_as_gid,
		                           rlimits};
		run_params.on_console = on_console;
		run_params.in_foreground = !onstart_flags.shares_console;
		run_params.csfd = control_socket[1];
		run_params.socket_fd = socket_fd;
		run_params.notify_fd = notify_pipe[1];
		run_params.force_notify_fd = force_notification_fd;
		run_params.notify_var = notification_var.c_str();
		run_params.env_file = env_file.c_str();
		run_child_proc(run_params);
	} else {
		// Parent process
		pid = forkpid;

		bp_sys::close(pipefd[1]); // close the 'other end' fd
		if (control_socket[1] != -1)
			bp_sys::close(control_socket[1]);
		if (notify_pipe[1] != -1)
			bp_sys::close(notify_pipe[1]);
		notification_fd = notify_pipe[0];
		waiting_for_execstat = true;
		return true;
	}

	// Failure exit:

out_cs_h:
	if (child_status_registered) {
		child_status_listener.deregister(event_loop);
	}

	if (notify_pipe[0] != -1)
		bp_sys::close(notify_pipe[0]);
	if (notify_pipe[1] != -1)
		bp_sys::close(notify_pipe[1]);
	if (ready_watcher_registered) {
		rwatcher->deregister(event_loop);
	}

	if (onstart_flags.pass_cs_fd) {
		delete control_conn;

	out_cs:
		bp_sys::close(control_socket[0]);
		bp_sys::close(control_socket[1]);
	}

out_p:
	bp_sys::close(pipefd[0]);
	bp_sys::close(pipefd[1]);

	return false;
}

base_process_service::base_process_service(
    service_set *sset, const string &name, service_type_t service_type_p,
    string &&command,
    const std::list<std::pair<unsigned, unsigned>> &command_offsets,
    const std::list<prelim_dep> &deplist_p)
    : service_record(sset, name, service_type_p, deplist_p),
      child_listener(this), child_status_listener(this), restart_timer(this) {
	program_name = std::move(command);
	exec_arg_parts = separate_args(program_name, command_offsets);

	restart_interval_count = 0;
	restart_interval_time = {0, 0};
	restart_timer.service = this;
	restart_timer.add_timer(event_loop);

	// By default, allow a maximum of 3 restarts within 10.0 seconds:
	restart_interval.seconds() = 10;
	restart_interval.nseconds() = 0;
	max_restart_interval_count = 3;

	waiting_restart_timer = false;
	reserved_child_watch = false;
	tracking_child = false;
	stop_timer_armed = false;
}

void base_process_service::do_restart() noexcept {
	waiting_restart_timer = false;
	restart_interval_count++;
	auto service_state = get_state();

	if (service_state == service_state_t::STARTING) {
		// for a smooth recovery, we want to check dependencies are available
		// before actually starting:
		if (!check_deps_started()) {
			waiting_for_deps = true;
			return;
		}
	}

	if (!start_ps_process(exec_arg_parts,
	                      have_console || onstart_flags.shares_console)) {
		restarting = false;
		if (service_state == service_state_t::STARTING) {
			failed_to_start();
		} else {
			unrecoverable_stop();
		}
		services->process_queues();
	}
}

bool base_process_service::restart_ps_process() noexcept {
	using time_val = dasynq::time_val;

	time_val current_time;
	event_loop.get_time(current_time, clock_type::MONOTONIC);

	if (max_restart_interval_count != 0) {
		// Check whether we're still in the most recent restart check interval:
		time_val int_diff = current_time - restart_interval_time;
		if (int_diff < restart_interval) {
			if (restart_interval_count >= max_restart_interval_count) {
				log(loglevel_t::ERROR, "Service ", get_name(),
				    " restarting too quickly; stopping.");
				return false;
			}
		} else {
			restart_interval_time = current_time;
			restart_interval_count = 0;
		}
	}

	// Check if enough time has lapsed since the previous restart. If not, start
	// a timer:
	time_val tdiff = current_time - last_start_time;
	if (restart_delay <= tdiff) {
		// > restart delay (normally 200ms)
		do_restart();
	} else {
		time_val timeout = restart_delay - tdiff;
		restart_timer.arm_timer_rel(event_loop, timeout);
		waiting_restart_timer = true;
	}
	return true;
}

bool base_process_service::interrupt_start() noexcept {
	if (waiting_restart_timer) {
		restart_timer.stop_timer(event_loop);
		waiting_restart_timer = false;
		return service_record::interrupt_start();
	}
	log(loglevel_t::WARN, "Interrupting start of service ", get_name(),
	    " with pid ", pid, " (with SIGINT).");
	kill_pg(SIGINT);

	if (stop_timeout != time_val(0, 0)) {
		restart_timer.arm_timer_rel(event_loop, stop_timeout);
		stop_timer_armed = true;
	} else if (stop_timer_armed) {
		restart_timer.stop_timer(event_loop);
		stop_timer_armed = false;
	}

	set_state(service_state_t::STOPPING);
	return false;
}

void base_process_service::kill_with_fire() noexcept {
	if (pid != -1) {
		log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid,
		    " exceeded allowed stop time; killing.");
		kill_pg(SIGKILL);
	}
}

void base_process_service::kill_pg(int signo) noexcept {
	if (onstart_flags.signal_process_only) {
		bp_sys::kill(pid, signo);
	} else {
		pid_t pgid = bp_sys::getpgid(pid);
		if (pgid == -1) {
			// On some OSes (eg OpenBSD) we aren't allowed to get the pgid of a
			// process in a different session. If the process is in a different
			// session, however, it must be a process group leader and the pgid
			// must equal the process id.
			pgid = pid;
		}
		bp_sys::kill(-pgid, signo);
	}
}

void base_process_service::timer_expired() noexcept {
	stop_timer_armed = false;

	// Timer expires if:
	// We are stopping, including after having startup cancelled (stop timeout,
	// state is STOPPING); We are starting (start timeout, state is STARTING);
	// We are waiting for restart timer before restarting, including smooth
	// recovery (restart timeout, state is STARTING or STARTED).
	if (get_state() == service_state_t::STOPPING) {
		kill_with_fire();
	} else if (pid != -1) {
		// Starting, start timed out.
		log(loglevel_t::WARN, "Service ", get_name(), " with pid ", pid,
		    " exceeded allowed start time; cancelling.");
		interrupt_start();
		stop_reason = stopped_reason_t::TIMEDOUT;
		failed_to_start(false, false);
	} else {
		// STARTING / STARTED, and we have no pid: must be restarting (smooth
		// recovery if STARTED)
		do_restart();
	}
}

void base_process_service::becoming_inactive() noexcept {
	if (socket_fd != -1) {
		close(socket_fd);
		socket_fd = -1;
	}
}

bool base_process_service::open_socket() noexcept {
	if (socket_path.empty() || socket_fd != -1) {
		// No socket, or already open
		return true;
	}

	const char *saddrname = socket_path.c_str();

	// Check the specified socket path
	struct stat stat_buf {};
	if (stat(saddrname, &stat_buf) == 0) {
		if ((stat_buf.st_mode & S_IFSOCK) == 0) {
			// Not a socket
			log(loglevel_t::ERROR, get_name(),
			    ": Activation socket file exists (and is not a socket)");
			return false;
		}
	} else if (errno != ENOENT) {
		// Other error
		log(loglevel_t::ERROR, get_name(),
		    ": Error checking activation socket: ", strerror(errno));
		return false;
	}

	// Remove stale socket file (if it exists).
	// We won't test the return from unlink - if it fails other than due to
	// ENOENT, we should get an error when we try to create the socket anyway.
	unlink(saddrname);

	uint sockaddr_size =
	    offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;
	auto *name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
	if (name == nullptr) {
		log(loglevel_t::ERROR, get_name(),
		    ": Opening activation socket: out of memory");
		return false;
	}

	name->sun_family = AF_UNIX;
	strcpy(name->sun_path, saddrname);

	int sockfd =
	    dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (sockfd == -1) {
		log(loglevel_t::ERROR, get_name(),
		    ": Error creating activation socket: ", strerror(errno));
		free(name);
		return false;
	}

	if (bind(sockfd, reinterpret_cast<struct sockaddr *>(name),
	         sockaddr_size) == -1) {
		log(loglevel_t::ERROR, get_name(),
		    ": Error binding activation socket: ", strerror(errno));
		close(sockfd);
		free(name);
		return false;
	}

	free(name);

	// POSIX (1003.1, 2013) says that fchown and fchmod don't necessarily work
	// on sockets. We have to use chown and chmod instead.
	if (chown(saddrname, socket_uid, socket_gid) != 0) {
		log(loglevel_t::ERROR, get_name(),
		    ": Error setting activation socket owner/group: ", strerror(errno));
		close(sockfd);
		return false;
	}

	if (chmod(saddrname, socket_perms) == -1) {
		log(loglevel_t::ERROR, get_name(),
		    ": Error setting activation socket permissions: ", strerror(errno));
		close(sockfd);
		return false;
	}

	if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
		log(loglevel_t::ERROR,
		    ": Error listening on activation socket: ", strerror(errno));
		close(sockfd);
		return false;
	}

	socket_fd = sockfd;
	return true;
}