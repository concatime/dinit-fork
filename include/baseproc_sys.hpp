#ifndef BASEPROC_SYS_HPP
#define BASEPROC_SYS_HPP

/*
 * This header implements a namespace, bp_sys, which wraps various system calls
 * used by baseproc-service.cc.
 *
 * When running tests, another header is substituted in place of this one. The
 * substitute provides mocks/stubs for the functions, to avoid calling the real
 * functions and thus allow for unit-level testing.
 */

#include <csignal> // kill
#include <fcntl.h>
#include <sys/uio.h>  // writev
#include <sys/wait.h> // waitpid
#include <unistd.h>

#include <dasynq/util.hpp> // for pipe2

namespace bp_sys {

// NOLINTNEXTLINE(misc-unused-using-decls)
using dasynq::pipe2;

// NOLINTNEXTLINE(misc-unused-using-decls)
using ::close;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::fcntl;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::getpgid;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::getpgrp;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::kill;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::open;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::read;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::tcsetpgrp;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::write;
// NOLINTNEXTLINE(misc-unused-using-decls)
using ::writev;

// Wrapper around a POSIX exit status
class exit_status {
	friend pid_t waitpid(pid_t /*p*/, exit_status * /*statusp*/, int /*flags*/);

	int status{0};

public:
	exit_status() noexcept {}
	explicit exit_status(int status_p) noexcept : status(status_p) {}

	bool did_exit() noexcept { return WIFEXITED(status); }

	bool did_exit_clean() const noexcept { return status == 0; }

	bool was_signalled() noexcept { return WIFSIGNALED(status); }

	int get_exit_status() noexcept { return WEXITSTATUS(status); }

	int get_term_sig() noexcept { return WTERMSIG(status); }

	int as_int() const noexcept { return status; }
};

inline pid_t waitpid(pid_t p, exit_status *statusp, int flags) {
	return ::waitpid(p, &statusp->status, flags);
}

} // namespace bp_sys

#endif /* BASEPROC_SYS_HPP */
