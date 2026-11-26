#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace 
{
    constexpr int kDefaultPort = 8080;
    constexpr int kBacklog = 16;
    constexpr std::size_t kBufSize = 4096;
    volatile std::sig_atomic_t g_was_sighup = 0;
    volatile std::sig_atomic_t g_stop = 0; 
    extern "C" void sighup_handler(int /*signo*/)
    {
        g_was_sighup = 1;
    }
    extern "C" void term_handler(int /*signo*/)
    {
        g_stop = 1;
    }
    static bool set_socket_flags(int fd)
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1) return false;
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return false;
        flags = ::fcntl(fd, F_GETFD, 0);
        if (flags == -1) return false;
        if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) return false;
        return true;
    }
    class UniqueFd 
    {
    public:
        UniqueFd() = default;
        explicit UniqueFd(int fd) : fd_(fd) {}
        ~UniqueFd() { reset(); }
        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;
        UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this != &other) reset(other.release());
            return *this;
        }
        [[nodiscard]] int  get()   const { return fd_; }
        [[nodiscard]] bool valid() const { return fd_ != -1; }
        int release()
        {
            int tmp = fd_;
            fd_ = -1;
            return tmp;
        }
        void reset(int fd = -1)
        {
            if (fd_ != -1) ::close(fd_);
            fd_ = fd;
        }
    private:
        int fd_ = -1;
    };
    class SigMaskGuard 
    {
    public:
        SigMaskGuard() = default;
        ~SigMaskGuard()
        {
            if (active_) ::sigprocmask(SIG_SETMASK, &old_, nullptr);
        }
        SigMaskGuard(const SigMaskGuard&) = delete;
        SigMaskGuard& operator=(const SigMaskGuard&) = delete;
        bool block(const sigset_t& set)
        {
            if (::sigprocmask(SIG_BLOCK, &set, &old_) == -1) return false;
            active_ = true;
            return true;
        }

        [[nodiscard]] const sigset_t& original() const { return old_; }

    private:
        sigset_t old_{};
        bool active_ = false;
    };

    int run_server(int port)
    {
        SigMaskGuard mask_guard;
        UniqueFd server_fd;
        UniqueFd client_fd;
        server_fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (!server_fd.valid()) 
        {
            std::perror("socket");
            return EXIT_FAILURE;
        }
        if (server_fd.get() >= FD_SETSIZE) 
        {
            std::fprintf(stderr, "Server fd %d >= FD_SETSIZE\n", server_fd.get());
            return EXIT_FAILURE;
        }
        if (!set_socket_flags(server_fd.get())) 
        {
            std::perror("fcntl (server)");
            return EXIT_FAILURE;
        }
        int yes = 1;
        if (::setsockopt(server_fd.get(), SOL_SOCKET, SO_REUSEADDR,&yes, sizeof(yes)) == -1) 
        {
            std::perror("setsockopt");
            return EXIT_FAILURE;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::bind(server_fd.get(), reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) == -1) 
        {
            std::perror("bind");
            return EXIT_FAILURE;
        }
        if (::listen(server_fd.get(), kBacklog) == -1) 
        {
            std::perror("listen");
            return EXIT_FAILURE;
        }
        sigset_t blocked_mask{};
        ::sigemptyset(&blocked_mask);
        ::sigaddset(&blocked_mask, SIGHUP);
        ::sigaddset(&blocked_mask, SIGINT);  
        ::sigaddset(&blocked_mask, SIGTERM);
        if (!mask_guard.block(blocked_mask)) 
        {
            std::perror("sigprocmask");
            return EXIT_FAILURE;
        }
        struct sigaction sa_hup {}, sa_term{};
        sa_hup.sa_handler = sighup_handler;
        ::sigemptyset(&sa_hup.sa_mask);
        sa_hup.sa_flags = 0;
        if (::sigaction(SIGHUP, &sa_hup, nullptr) == -1) 
        {
            std::perror("sigaction (SIGHUP)");
            return EXIT_FAILURE;
        }
        sa_term.sa_handler = term_handler;
        ::sigemptyset(&sa_term.sa_mask);
        sa_term.sa_flags = 0;
        if (::sigaction(SIGINT, &sa_term, nullptr) == -1 ||::sigaction(SIGTERM, &sa_term, nullptr) == -1) 
        {
            std::perror("sigaction (TERM)");
            return EXIT_FAILURE;
        }
        sigset_t wait_mask = mask_guard.original();
        ::sigdelset(&wait_mask, SIGHUP);
        ::sigdelset(&wait_mask, SIGINT);
        ::sigdelset(&wait_mask, SIGTERM);
        while (!g_stop) 
        { 
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd.get(), &read_fds);
            int max_fd = server_fd.get();
            if (client_fd.valid()) 
            {
                FD_SET(client_fd.get(), &read_fds);
                if (client_fd.get() > max_fd) max_fd = client_fd.get();
            }
            const int ready = ::pselect(max_fd + 1, &read_fds, nullptr, nullptr,nullptr, &wait_mask);
            if (ready == -1) 
            {
                if (errno == EINTR) 
                {
                    if (g_was_sighup) 
                    {
                        g_was_sighup = 0;
                        std::printf("SIGHUP received\n");
                        std::fflush(stdout);
                    }
                    continue;
                }
                std::perror("pselect");
                return EXIT_FAILURE;
            }
            if (g_was_sighup) 
            {
                g_was_sighup = 0;
                std::printf("SIGHUP received\n");
                std::fflush(stdout);
            }
            if (FD_ISSET(server_fd.get(), &read_fds)) 
            {
                const int new_fd = ::accept(server_fd.get(), nullptr, nullptr);
                if (new_fd == -1) 
                {
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) 
                    {
                        std::perror("accept");
                    }
                }
                else if (new_fd >= FD_SETSIZE) 
                {
                    std::fprintf(stderr, "Too many descriptors: fd=%d >= FD_SETSIZE\n", new_fd);
                    ::close(new_fd);
                }
                else 
                {
                    if (!set_socket_flags(new_fd)) 
                    {
                        std::perror("fcntl (client)");
                        ::close(new_fd);
                    }
                    else 
                    {
                        std::printf("New connection accepted: fd=%d\n", new_fd);
                        std::fflush(stdout);
                        if (!client_fd.valid()) 
                        {
                            client_fd.reset(new_fd);
                            std::printf("Keeping connection: fd=%d\n", client_fd.get());
                        }
                        else 
                        {
                            std::printf("Closing extra connection: fd=%d\n", new_fd);
                            ::close(new_fd);
                        }
                        std::fflush(stdout);
                    }
                }
            }
            const int ready_client = client_fd.get();
            if (ready_client != -1 && FD_ISSET(ready_client, &read_fds)) 
            {
                char buffer[kBufSize];
                const ssize_t received = ::recv(ready_client, buffer, sizeof(buffer), 0);
                if (received > 0) 
                {
                    std::printf("Received %zd bytes\n", received);
                    std::fflush(stdout);
                }
                else if (received == 0) 
                {
                    std::printf("Client closed connection\n");
                    std::fflush(stdout);
                    client_fd.reset();
                }
                else 
                {
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) 
                    {
                        std::perror("recv");
                        client_fd.reset();
                    }
                }
            }
        }
        std::printf("Shutting down gracefully...\n");
        return EXIT_SUCCESS;
    }
} 

int main(int argc, char** argv)
{
    int port = kDefaultPort;
    if (argc > 1) 
    {
        errno = 0;
        char* endptr = nullptr;
        long val = std::strtol(argv[1], &endptr, 10);
        if (errno != 0 || endptr == argv[1] || *endptr != '\0' || val < 1 || val > 65535) 
        {
            std::fprintf(stderr, "Invalid port: %s. Must be 1..65535.\n", argv[1]);
            return EXIT_FAILURE;
        }
        port = static_cast<int>(val);
    }
    return run_server(port);
}