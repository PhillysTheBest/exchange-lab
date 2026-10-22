// app/run.hpp — start a program as a child that shares this terminal, and wait.
//
// Arguments go to the child as a vector, never through a shell, so a path with
// a space in it cannot be split or reinterpreted. The child inherits stdin,
// stdout and stderr, which is what lets /watch hand the whole screen to the
// viewer and take it back on q.
//
// While a child runs, the app ignores Ctrl-C and the child does not: Ctrl-C
// stops the sweep or the viewer, and leaves you at the prompt.
#pragma once

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace px::app {

#ifdef _WIN32
// The C runtime joins argv into one command line with spaces and no quoting,
// so a word with a space or a quote in it has to be quoted here, by the rules
// the child's runtime uses to split it again.
[[nodiscard]] inline std::string windows_quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    std::size_t slashes = 0;
    for (const char c : a) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        q.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, '\\');
    return q + "\"";
}
#endif

// The child's exit status; -1 if it could not be started at all; 128 plus the
// signal's number if a signal ended it, as a shell would say.
[[nodiscard]] inline int run_child(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    std::vector<std::string> words;
    for (const std::string& a : argv) words.push_back(windows_quote(a));
    std::string program = argv[0];
    for (char& c : program)
        if (c == '/') c = '\\';
    std::vector<const char*> args;
    for (const std::string& w : words) args.push_back(w.c_str());
    args.push_back(nullptr);
    auto* const old = std::signal(SIGINT, SIG_IGN);
    const intptr_t rc = _spawnvp(_P_WAIT, program.c_str(), args.data());
    std::signal(SIGINT, old);
    return rc == -1 ? -1 : static_cast<int>(rc);
#else
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    // A close-on-exec pipe tells "could not start" apart from a program that
    // happens to exit 127: it closes empty when the exec succeeds, and carries
    // errno when it does not.
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    struct sigaction ignore {}, old_int {}, old_quit {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGINT, &ignore, &old_int);
    sigaction(SIGQUIT, &ignore, &old_quit);

    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        execvp(args[0], args.data());
        const int err = errno;
        [[maybe_unused]] const ssize_t n = write(fds[1], &err, sizeof err);
        _exit(127);
    }
    close(fds[1]);
    int status = -1;
    if (pid > 0) {
        int err = 0;
        ssize_t n;
        do n = read(fds[0], &err, sizeof err);
        while (n < 0 && errno == EINTR);
        int raw = 0;
        pid_t w;
        do w = waitpid(pid, &raw, 0);
        while (w < 0 && errno == EINTR);
        if (n > 0 || w < 0) status = -1;
        else if (WIFEXITED(raw)) status = WEXITSTATUS(raw);
        else if (WIFSIGNALED(raw)) status = 128 + WTERMSIG(raw);
    }
    close(fds[0]);
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGQUIT, &old_quit, nullptr);
    return status;
#endif
}

} // namespace px::app
