// SshShellHelpers.h
#pragma once

#include <libssh/libssh.h>
#include <stdexcept>

inline ssh_channel openPtyShell(ssh_session session,
                                int cols = 100,
                                int rows = 30,
                                const char *term = "xterm-color")
{
    if (!session) {
        throw std::runtime_error("openPtyShell: session is null");
    }

    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        throw std::runtime_error("openPtyShell: failed to create channel");
    }

    // 1) Session channel
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        throw std::runtime_error("openPtyShell: ssh_channel_open_session failed");
    }

    // 2) Request PTY
    if (ssh_channel_request_pty_size(channel, term, cols, rows) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        throw std::runtime_error("openPtyShell: ssh_channel_request_pty_size failed");
    }

    // 3) Request interactive shell
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        throw std::runtime_error("openPtyShell: ssh_channel_request_shell failed");
    }

    return channel; // you now have a real shell like ssh
}
