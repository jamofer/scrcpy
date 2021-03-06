#include "server.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <SDL2/SDL_timer.h>

#include "config.h"
#include "command.h"
#include "util/log.h"
#include "util/net.h"

#define SOCKET_NAME "scrcpy"
#define SERVER_FILENAME "scrcpy-server"

#define DEFAULT_SERVER_PATH PREFIX "/share/scrcpy/" SERVER_FILENAME
#define DEVICE_SERVER_PATH "/data/local/tmp/scrcpy-server.jar"
#define SSH_SERVER_PATH "/tmp/" SERVER_FILENAME

#define RANDOM_PORT 0

static const char *
get_server_path(void) {
    const char *server_path_env = getenv("SCRCPY_SERVER_PATH");
    if (server_path_env) {
        LOGD("Using SCRCPY_SERVER_PATH: %s", server_path_env);
        // if the envvar is set, use it
        return server_path_env;
    }

#ifndef PORTABLE
    LOGD("Using server: " DEFAULT_SERVER_PATH);
    // the absolute path is hardcoded
    return DEFAULT_SERVER_PATH;
#else
    // use scrcpy-server in the same directory as the executable
    char *executable_path = get_executable_path();
    if (!executable_path) {
        LOGE("Could not get executable path, "
             "using " SERVER_FILENAME " from current directory");
        // not found, use current directory
        return SERVER_FILENAME;
    }
    char *dir = dirname(executable_path);
    size_t dirlen = strlen(dir);

    // sizeof(SERVER_FILENAME) gives statically the size including the null byte
    size_t len = dirlen + 1 + sizeof(SERVER_FILENAME);
    char *server_path = SDL_malloc(len);
    if (!server_path) {
        LOGE("Could not alloc server path string, "
             "using " SERVER_FILENAME " from current directory");
        SDL_free(executable_path);
        return SERVER_FILENAME;
    }

    memcpy(server_path, dir, dirlen);
    server_path[dirlen] = PATH_SEPARATOR;
    memcpy(&server_path[dirlen + 1], SERVER_FILENAME, sizeof(SERVER_FILENAME));
    // the final null byte has been copied with SERVER_FILENAME

    SDL_free(executable_path);

    LOGD("Using server (portable): %s", server_path);
    return server_path;
#endif
}


static void copy_to_host(const char *ssh_uri, const char *origin, const char *target)
{
    process_t process;
    char ssh_target[128];

    sprintf(ssh_target, "%s:%s", ssh_uri, target);

    const char *cmd_args[] = {
        "scp",
        origin,
        ssh_target,
        NULL
    };

    printf("%s %s %s\n", cmd_args[0], cmd_args[1], cmd_args[2]);

    cmd_execute(cmd_args, &process);
    process_check_success(process, "scp server");
}


static bool
push_server(struct server *server) {
    const char *server_path = get_server_path();
    if (!is_regular_file(server_path)) {
        LOGE("'%s' does not exist or is not a regular file\n", server_path);
        return false;
    }

    if (server->ssh_uri) {
        copy_to_host(server->ssh_uri, server_path, SSH_SERVER_PATH);
    }
    process_t process = adb_push(server->ssh_uri, server->serial, SSH_SERVER_PATH, DEVICE_SERVER_PATH);
    return process_check_success(process, "adb push");
}


static bool
enable_tunnel(struct server *server) {
    process_t process = adb_reverse(server->ssh_uri, server->serial, SOCKET_NAME, server->local_port);
    return process_check_success(process, "adb reverse");
}

static bool
disable_tunnel(struct server *server) {
    process_t process = adb_reverse_remove(server->ssh_uri, server->serial, SOCKET_NAME);
    return process_check_success(process, "adb reverse --remove");
}

static process_t
execute_server(struct server *server, const struct server_params *params) {
    char max_size_string[6];
    char bit_rate_string[11];
    char max_fps_string[6];
    sprintf(max_size_string, "%"PRIu16, params->max_size);
    sprintf(bit_rate_string, "%"PRIu32, params->bit_rate);
    sprintf(max_fps_string, "%"PRIu16, params->max_fps);
    const char *const cmd[] = {
        "shell",
        "CLASSPATH=" DEVICE_SERVER_PATH,
        "app_process",
#ifdef SERVER_DEBUGGER
# define SERVER_DEBUGGER_PORT "5005"
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
            SERVER_DEBUGGER_PORT,
#endif
        "/", // unused
        "com.genymobile.scrcpy.Server",
        SCRCPY_VERSION,
        max_size_string,
        bit_rate_string,
        max_fps_string,
        server->tunnel_forward ? "true" : "false",
        params->crop ? params->crop : "-",
        "true", // always send frame meta (packet boundaries + timestamp)
        params->control ? "true" : "false",
    };
#ifdef SERVER_DEBUGGER
    LOGI("Server debugger waiting for a client on device port "
         SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif
    return adb_execute(server->ssh_uri, server->serial, cmd, sizeof(cmd) / sizeof(cmd[0]));
}

#define IPV4_LOCALHOST 0x7F000001

static socket_t
listen_on_port(uint16_t port) {
    return net_listen(IPV4_LOCALHOST, port, 1);
}

static socket_t
connect_and_read_byte(uint16_t port) {
    socket_t socket = net_connect(IPV4_LOCALHOST, port);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    char byte;
    // the connection may succeed even if the server behind the "adb tunnel"
    // is not listening, so read one byte to detect a working connection
    if (net_recv(socket, &byte, 1) != 1) {
        // the server is not listening yet behind the adb tunnel
        net_close(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

static socket_t
connect_to_server(uint16_t port, uint32_t attempts, uint32_t delay) {
    do {
        LOGD("Remaining connection attempts: %d", (int) attempts);
        socket_t socket = connect_and_read_byte(port);
        if (socket != INVALID_SOCKET) {
            // it worked!
            return socket;
        }
        if (attempts) {
            SDL_Delay(delay);
        }
    } while (--attempts > 0);
    return INVALID_SOCKET;
}

static void
close_socket(socket_t *socket) {
    assert(*socket != INVALID_SOCKET);
    net_shutdown(*socket, SHUT_RDWR);
    if (!net_close(*socket)) {
        LOGW("Could not close socket");
        return;
    }
    *socket = INVALID_SOCKET;
}

void
server_init(struct server *server) {
    *server = (struct server) SERVER_INITIALIZER;
}

static void enable_ssh_tunnel(const char *ssh_uri, uint16_t port)
{
    const char *cmd_args[6];
    char remote_forward[64];
    process_t process;

    sprintf(remote_forward, "%d:localhost:%d", port, port);

    cmd_args[0] = "ssh";
    cmd_args[1] = "-R";
    cmd_args[2] = remote_forward;
    cmd_args[3] = ssh_uri;
    cmd_args[4] = "exit";
    cmd_args[5] = NULL;

    printf("%s %s %s %s\n", cmd_args[0], cmd_args[1], cmd_args[2], cmd_args[3]);

    cmd_execute(cmd_args, &process);
}

static void disable_ssh_tunnel(const char *ssh_uri, uint16_t port)
{
    const char *cmd_args[7];
    char remote_forward[64];
    process_t process;

    sprintf(remote_forward, "%d:localhost:%d", port, port);

    cmd_args[0] = "ssh";
    cmd_args[1] = "-O";
    cmd_args[2] = "cancel";
    cmd_args[3] = "-R";
    cmd_args[4] = remote_forward;
    cmd_args[5] = ssh_uri;
    cmd_args[6] = NULL;

    printf("%s %s %s %s %s %s\n", cmd_args[0], cmd_args[1], cmd_args[2], cmd_args[3], cmd_args[4], cmd_args[5]);

    cmd_execute(cmd_args, &process);
}

bool
server_start(struct server *server, const char *serial, const char *ssh_uri, const struct server_params *params) {
    server->local_port = params->local_port;

    if (serial) {
        server->serial = SDL_strdup(serial);
        if (!server->serial) {
            return false;
        }
    }

    if (ssh_uri) {
        server->ssh_uri = SDL_strdup(ssh_uri);
    }

    if (!push_server(server)) {
        SDL_free(server->serial);
        return false;
    }

    server->server_socket = listen_on_port(params->local_port);
    if (server->local_port == RANDOM_PORT) {
        server->local_port = net_listening_port(server->server_socket);
    }

    if (server->server_socket == INVALID_SOCKET) {
        LOGE("Could not listen on port %" PRIu16, server->local_port);
        disable_tunnel(server);
        SDL_free(server->serial);
        return false;
    }

    enable_ssh_tunnel(ssh_uri, server->local_port);
    if (!enable_tunnel(server)) {
        SDL_free(server->serial);
        return false;
    }

    server->process = execute_server(server, params);

    if (server->process == PROCESS_NONE) {
        close_socket(&server->server_socket);
        disable_tunnel(server);
        SDL_free(server->serial);
        return false;
    }

    server->tunnel_enabled = true;

    return true;
}

bool
server_connect_to(struct server *server) {
    if (!server->tunnel_forward) {
        server->video_socket = net_accept(server->server_socket);
        if (server->video_socket == INVALID_SOCKET) {
            return false;
        }

        server->control_socket = net_accept(server->server_socket);
        if (server->control_socket == INVALID_SOCKET) {
            // the video_socket will be cleaned up on destroy
            return false;
        }

        // we don't need the server socket anymore
        close_socket(&server->server_socket);
    } else {
        uint32_t attempts = 100;
        uint32_t delay = 100; // ms
        server->video_socket =
            connect_to_server(server->local_port, attempts, delay);
        if (server->video_socket == INVALID_SOCKET) {
            return false;
        }

        // we know that the device is listening, we don't need several attempts
        server->control_socket =
            net_connect(IPV4_LOCALHOST, server->local_port);
        if (server->control_socket == INVALID_SOCKET) {
            return false;
        }
    }

    // we don't need the adb tunnel anymore
    disable_tunnel(server); // ignore failure
    server->tunnel_enabled = false;

    return true;
}

void
server_stop(struct server *server) {
    disable_ssh_tunnel(server->ssh_uri, server->local_port);
    SDL_free(server->ssh_uri);

    if (server->server_socket != INVALID_SOCKET) {
        close_socket(&server->server_socket);
    }
    if (server->video_socket != INVALID_SOCKET) {
        close_socket(&server->video_socket);
    }
    if (server->control_socket != INVALID_SOCKET) {
        close_socket(&server->control_socket);
    }

    assert(server->process != PROCESS_NONE);

    if (!cmd_terminate(server->process)) {
        LOGW("Could not terminate server");
    }

    cmd_simple_wait(server->process, NULL); // ignore exit code
    LOGD("Server terminated");

    if (server->tunnel_enabled) {
        // ignore failure
        disable_tunnel(server);
    }
}

void
server_destroy(struct server *server) {
    SDL_free(server->serial);
}
