//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_process.hpp>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// for srs-librtmp, @see https://github.com/ossrs/srs/issues/213
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std;

#include <srs_app_config.hpp>
#include <srs_app_utility.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_protocol_utility.hpp>

ISrsProcess::ISrsProcess()
{
}

ISrsProcess::~ISrsProcess()
{
}

SrsProcess::SrsProcess()
{
    is_started_ = false;
    fast_stopped_ = false;
    pid_ = -1;
}

SrsProcess::~SrsProcess()
{
}

// LCOV_EXCL_START
int SrsProcess::get_pid()
{
    return pid_;
}

bool SrsProcess::started()
{
    return is_started_;
}
// LCOV_EXCL_STOP

srs_error_t SrsProcess::initialize(string binary, vector<string> argv)
{
    srs_error_t err = srs_success;

    bin_ = binary;
    cli_ = "";
    actual_cli_ = "";
    params_.clear();

    for (int i = 0; i < (int)argv.size(); i++) {
        std::string ffp = argv[i];
        std::string nffp = (i < (int)argv.size() - 1) ? argv[i + 1] : "";
        std::string nnffp = (i < (int)argv.size() - 2) ? argv[i + 2] : "";

        // >file
        if (srs_strings_starts_with(ffp, ">")) {
            stdout_file_ = ffp.substr(1);
            continue;
        }

        // 1>file
        if (srs_strings_starts_with(ffp, "1>")) {
            stdout_file_ = ffp.substr(2);
            continue;
        }

        // 2>file
        if (srs_strings_starts_with(ffp, "2>")) {
            stderr_file_ = ffp.substr(2);
            continue;
        }

        // LCOV_EXCL_START
        // 1 >X
        if (ffp == "1" && srs_strings_starts_with(nffp, ">")) {
            if (nffp == ">") {
                // 1 > file
                if (!nnffp.empty()) {
                    stdout_file_ = nnffp;
                    i++;
                }
            } else {
                // 1 >file
                stdout_file_ = srs_strings_trim_start(nffp, ">");
            }
            // skip the >
            i++;
            continue;
        }

        // 2 >X
        if (ffp == "2" && srs_strings_starts_with(nffp, ">")) {
            if (nffp == ">") {
                // 2 > file
                if (!nnffp.empty()) {
                    stderr_file_ = nnffp;
                    i++;
                }
            } else {
                // 2 >file
                stderr_file_ = srs_strings_trim_start(nffp, ">");
            }
            // skip the >
            i++;
            continue;
        }
        // LCOV_EXCL_STOP

        params_.push_back(ffp);
    }

    actual_cli_ = srs_strings_join(params_, " ");
    cli_ = srs_strings_join(argv, " ");

    return err;
}

// LCOV_EXCL_START
srs_error_t srs_redirect_output(string from_file, int to_fd)
{
    srs_error_t err = srs_success;

    // use default output.
    if (from_file.empty()) {
        return err;
    }

    // redirect the fd to file.
    int fd = -1;
    int flags = O_CREAT | O_RDWR | O_APPEND;
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

    if ((fd = ::open(from_file.c_str(), flags, mode)) < 0) {
        return srs_error_new(ERROR_FORK_OPEN_LOG, "open process %d %s failed", to_fd, from_file.c_str());
    }

    int r0 = dup2(fd, to_fd);
    ::close(fd);

    if (r0 < 0) {
        return srs_error_new(ERROR_FORK_DUP2_LOG, "dup2 fd=%d, to=%d, file=%s failed, r0=%d",
                             fd, to_fd, from_file.c_str(), r0);
    }

    return err;
}

srs_error_t SrsProcess::redirect_io()
{
    srs_error_t err = srs_success;

    // for the stdout, ignore when not specified.
    // redirect stdout to file if possible.
    if ((err = srs_redirect_output(stdout_file_, STDOUT_FILENO)) != srs_success) {
        return srs_error_wrap(err, "redirect stdout");
    }

    // for the stderr, ignore when not specified.
    // redirect stderr to file if possible.
    if ((err = srs_redirect_output(stderr_file_, STDERR_FILENO)) != srs_success) {
        return srs_error_wrap(err, "redirect stderr");
    }

    // No stdin for process, @bug https://github.com/ossrs/srs/issues/1592
    if ((err = srs_redirect_output("/dev/null", STDIN_FILENO)) != srs_success) {
        return srs_error_wrap(err, "redirect /dev/null");
    }

    return err;
}

srs_error_t SrsProcess::start()
{
    srs_error_t err = srs_success;

    if (is_started_) {
        return err;
    }

    // generate the argv of process.
    srs_info("fork process: %s", cli_.c_str());

    // for log
    SrsContextId cid = _srs_context->get_id();
    int ppid = getpid();

    // TODO: fork or vfork?
    if ((pid_ = fork()) < 0) {
        return srs_error_new(ERROR_ENCODER_FORK, "vfork process failed, cli=%s", cli_.c_str());
    }

    // for osx(lldb) to debug the child process.
    // user can use "lldb -p <pid>" to resume the parent or child process.
    // kill(0, SIGSTOP);

    // child process: ffmpeg encoder engine.
    if (pid_ == 0) {
        // ignore the SIGINT and SIGTERM
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);

        // redirect standard I/O, if it failed, output error to stdout, and exit child process.
        if ((err = redirect_io()) != srs_success) {
            fprintf(stdout, "child process error, %s\n", srs_error_desc(err).c_str());
            exit(-1);
        }

        // should never close the fd 3+, for it myabe used.
        // for fd should close at exec, use fnctl to set it.

        // log basic info to stderr.
        if (true) {
            fprintf(stdout, "\n");
            fprintf(stdout, "process ppid=%d, cid=%s, pid=%d, in=%d, out=%d, err=%d\n",
                    ppid, cid.c_str(), getpid(), STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
            fprintf(stdout, "process binary=%s, cli: %s\n", bin_.c_str(), cli_.c_str());
            fprintf(stdout, "process actual cli: %s\n", actual_cli_.c_str());
        }

        // memory leak in child process, it's ok.
        char **argv = new char *[params_.size() + 1];
        for (int i = 0; i < (int)params_.size(); i++) {
            std::string &p = params_[i];

            // memory leak in child process, it's ok.
            char *v = new char[p.length() + 1];
            argv[i] = strcpy(v, p.data());
        }
        argv[params_.size()] = NULL;

        // use execv to start the program.
        int r0 = execv(bin_.c_str(), argv);
        if (r0 < 0) {
            fprintf(stderr, "fork process failed, errno=%d(%s)", errno, strerror(errno));
        }
        exit(r0);
    }

    // parent.
    if (pid_ > 0) {
        // Wait for a while for process to really started.
        // @see https://github.com/ossrs/srs/issues/1634#issuecomment-597568840
        srs_usleep(10 * SRS_UTIME_MILLISECONDS);

        is_started_ = true;
        srs_trace("forked process, pid=%d, bin=%s, stdout=%s, stderr=%s, argv=%s",
                  pid_, bin_.c_str(), stdout_file_.c_str(), stderr_file_.c_str(), actual_cli_.c_str());
        return err;
    }

    return err;
}

srs_error_t SrsProcess::cycle()
{
    srs_error_t err = srs_success;

    if (!is_started_) {
        return err;
    }

    // ffmpeg is prepare to stop, donot cycle.
    if (fast_stopped_) {
        return err;
    }

    int status = 0;
    pid_t p = waitpid(pid_, &status, WNOHANG);

    if (p < 0) {
        return srs_error_new(ERROR_SYSTEM_WAITPID, "process waitpid failed, pid=%d", pid_);
    }

    if (p == 0) {
        srs_info("process process pid=%d is running.", pid_);
        return err;
    }

    srs_trace("process pid=%d terminate, please restart it.", pid_);
    is_started_ = false;

    return err;
}

void SrsProcess::stop()
{
    if (!is_started_) {
        return;
    }

    // kill the ffmpeg,
    // when rewind, upstream will stop publish(unpublish),
    // unpublish event will stop all ffmpeg encoders,
    // then publish will start all ffmpeg encoders.
    SrsAppUtility utility;
    srs_error_t err = utility.kill(pid_);
    if (err != srs_success) {
        srs_warn("ignore kill the process failed, pid=%d. err=%s", pid_, srs_error_desc(err).c_str());
        srs_freep(err);
        return;
    }

    // terminated, set started to false to stop the cycle.
    is_started_ = false;
}

void SrsProcess::fast_stop()
{
    int ret = ERROR_SUCCESS;

    if (!is_started_) {
        return;
    }

    if (pid_ <= 0) {
        return;
    }

    if (kill(pid_, SIGTERM) < 0) {
        ret = ERROR_SYSTEM_KILL;
        srs_warn("ignore fast stop process failed, pid=%d. ret=%d", pid_, ret);
        return;
    }

    fast_stopped_ = true;
    return;
}

void SrsProcess::fast_kill()
{
    int ret = ERROR_SUCCESS;

    if (!is_started_) {
        return;
    }

    if (pid_ <= 0) {
        return;
    }

    if (kill(pid_, SIGKILL) < 0) {
        ret = ERROR_SYSTEM_KILL;
        srs_warn("ignore fast kill process failed, pid=%d. ret=%d", pid_, ret);
        return;
    }

    // Try to wait pid to avoid zombie FFMEPG.
    int status = 0;
    waitpid(pid_, &status, WNOHANG);

    return;
}
// LCOV_EXCL_STOP

