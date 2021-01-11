#ifndef SERVICE_CONSTANTS_HPP
#define SERVICE_CONSTANTS_HPP

/* Service states */
enum class service_state_t {
	STOPPED,  // service is not running.
	STARTING, // service is starting, and will start (or fail to start) in time.
	STARTED,  // service is running,
	STOPPING  // service script is stopping and will stop.
};

/* Service types */
enum class service_type_t {
	DUMMY,     // Dummy service, used to detect cyclice dependencies
	PROCESS,   // Service runs as a process, and can be stopped by
	           // sending the process a signal (usually SIGTERM)
	BGPROCESS, // Service runs as a process which "daemonizes" to run in the
	           // "background".
	SCRIPTED,  // Service requires an external command to start,
	           // and a second command to stop
	INTERNAL   // Internal service, runs no external process
};

/* Service events */
enum class service_event_t {
	STARTED,     // Service was started (reached STARTED state)
	STOPPED,     // Service was stopped (reached STOPPED state)
	FAILEDSTART, // Service failed to start (possibly due to dependency failing)
	STARTCANCELLED, // Service was set to be started but a stop was requested
	STOPCANCELLED   // Service was set to be stopped but a start was requested
};

/* Shutdown types */
enum class shutdown_type_t {
	NONE,     // No explicit shutdown
	REMAIN,   // Continue running with no services
	HALT,     // Halt system without powering down
	POWEROFF, // Power off system
	REBOOT    // Reboot system
};

/* Reasons for why service stopped */
enum class stopped_reason_t {
	NORMAL,

	// Start failures:
	DEPFAILED,  // A dependency failed to start
	FAILED,     // failed to start (process terminated)
	EXECFAILED, // failed to start (couldn't launch process)
	TIMEDOUT,   // timed out when starting

	// Failure(?) after starting:
	TERMINATED // process terminated
};

inline bool did_finish(stopped_reason_t reason) {
	return reason == stopped_reason_t::TERMINATED;
}

enum class dependency_type {
	REGULAR,
	SOFT,      // dependency starts in parallel, failure/stop does not affect
	           // dependent
	WAITS_FOR, // as for SOFT, but dependent waits until dependency starts/fails
	           // before starting
	MILESTONE  // dependency must start successfully, but once started the
	           // dependency becomes soft
};

// Service set type identifiers:
constexpr int SSET_TYPE_NONE = 0;
constexpr int SSET_TYPE_DIRLOAD = 1;

#endif /* SERVICE_CONSTANTS_HPP */
