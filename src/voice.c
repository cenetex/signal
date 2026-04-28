/*
 * voice.c -- Local voicebox subprocess for TTS hailing.
 * Manages subprocess lifecycle, stdin pipe I/O, and event serialization.
 */

#include "voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <io.h>
#include <windows.h>
#define PIPE_READ 0
#define PIPE_WRITE 1
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

static struct {
    pid_t pid;
    int stdin_fd;
} g_voice = {-1, -1};

void voice_init(void) {
#ifdef _WIN32
    /* Windows: _pipe() + _spawnvp() */
    int pipefd[2];
    if (_pipe(pipefd, 1024, _O_BINARY) == -1) {
        perror("voice: _pipe failed");
        return;
    }

    const char *voicebox_path = "assets/voice/voicebox.exe";
    const char *argv[] = {
        voicebox_path,
        "--ship",
        "--persona-add", "nav7", "assets/voice/nav7.persona",
        "--persona-add", "prospect", "assets/voice/prospect.persona",
        "--persona-add", "kepler", "assets/voice/kepler.persona",
        "--persona-add", "helios", "assets/voice/helios.persona",
        "assets/voice/kokoro",
        NULL
    };

    intptr_t pid = _spawnvp(_P_NOWAIT, voicebox_path, argv);
    if (pid == -1) {
        perror("voice: _spawnvp failed");
        _close(pipefd[PIPE_READ]);
        _close(pipefd[PIPE_WRITE]);
        return;
    }

    _close(pipefd[PIPE_READ]);
    g_voice.pid = (pid_t)pid;
    g_voice.stdin_fd = pipefd[PIPE_WRITE];
#else
    /* POSIX: pipe() + fork() + execvp() */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("voice: pipe failed");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("voice: fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* Child process: set up stdin, exec voicebox */
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) == -1) {
            perror("voice: dup2 failed");
            exit(1);
        }
        close(pipefd[0]);

        const char *voicebox_path = "assets/voice/voicebox";
        char *argv[] = {
            (char *)voicebox_path,
            (char *)"--ship",
            (char *)"--persona-add", (char *)"nav7", (char *)"assets/voice/nav7.persona",
            (char *)"--persona-add", (char *)"prospect", (char *)"assets/voice/prospect.persona",
            (char *)"--persona-add", (char *)"kepler", (char *)"assets/voice/kepler.persona",
            (char *)"--persona-add", (char *)"helios", (char *)"assets/voice/helios.persona",
            (char *)"assets/voice/kokoro",
            NULL
        };

        execvp(voicebox_path, argv);
        perror("voice: execvp failed");
        exit(1);
    }

    /* Parent process: store PID and write-end of pipe */
    close(pipefd[0]);
    g_voice.pid = pid;
    g_voice.stdin_fd = pipefd[1];

    /* Set non-blocking so writes never stall */
    int flags = fcntl(pipefd[1], F_GETFL);
    if (flags != -1) {
        fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

void voice_event(const char *persona, const char *line) {
    if (g_voice.stdin_fd == -1 || !persona || !line) return;

    char buf[512];
    int len = snprintf(buf, sizeof(buf), "EVENT %s %s\n", persona, line);
    if (len < 0 || len >= (int)sizeof(buf)) return;

#ifdef _WIN32
    /* Windows: write() on a pipe */
    _write(g_voice.stdin_fd, buf, len);
#else
    /* POSIX: write() returns -1 on error; ignore EAGAIN for busy pipes */
    ssize_t written = write(g_voice.stdin_fd, buf, (size_t)len);
    (void)written;
#endif
}

void voice_state(const char *fields) {
    if (g_voice.stdin_fd == -1 || !fields) return;

    char buf[512];
    int len = snprintf(buf, sizeof(buf), "STATE %s\n", fields);
    if (len < 0 || len >= (int)sizeof(buf)) return;

#ifdef _WIN32
    _write(g_voice.stdin_fd, buf, len);
#else
    ssize_t written = write(g_voice.stdin_fd, buf, (size_t)len);
    (void)written;
#endif
}

void voice_ask(const char *persona, const char *directive) {
    if (g_voice.stdin_fd == -1 || !persona || !directive) return;

    char buf[512];
    int len = snprintf(buf, sizeof(buf), "ASK %s %s\n", persona, directive);
    if (len < 0 || len >= (int)sizeof(buf)) return;

#ifdef _WIN32
    _write(g_voice.stdin_fd, buf, len);
#else
    ssize_t written = write(g_voice.stdin_fd, buf, (size_t)len);
    (void)written;
#endif
}

void voice_quit(void) {
    if (g_voice.stdin_fd != -1) {
#ifdef _WIN32
        _write(g_voice.stdin_fd, "QUIT\n", 5);
        _close(g_voice.stdin_fd);
#else
        write(g_voice.stdin_fd, "QUIT\n", 5);
        close(g_voice.stdin_fd);
#endif
        g_voice.stdin_fd = -1;
    }

    if (g_voice.pid != -1) {
#ifdef _WIN32
        WaitForSingleObject((HANDLE)g_voice.pid, INFINITE);
        CloseHandle((HANDLE)g_voice.pid);
#else
        waitpid(g_voice.pid, NULL, 0);
#endif
        g_voice.pid = -1;
    }
}
