#define _GNU_SOURCE
#include <iostream>
#include <optional>
#include <array>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <netinet/in.h>

namespace sys {

inline void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    int  get()   const { return fd_; }
    bool valid() const { return fd_ != -1; }

    void reset(int newfd = -1) {
        if (fd_ != -1) ::close(fd_);
        fd_ = newfd;
    }

private:
    int fd_ = -1;
};

class Epoll {
public:
    Epoll() : ep_(::epoll_create1(EPOLL_CLOEXEC)) {}

    bool valid() const { return ep_.valid(); }
    int  fd()    const { return ep_.get(); }

    void add_in(int fd) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev);
    }

    void del(int fd) {
        ::epoll_ctl(ep_.get(), EPOLL_CTL_DEL, fd, nullptr);
    }

    int wait(epoll_event* out, int cap, int timeout_ms = -1) {
        return ::epoll_wait(ep_.get(), out, cap, timeout_ms);
    }

private:
    UniqueFd ep_;
};

class SignalFd {
public:
    explicit SignalFd(std::initializer_list<int> sigs) {
        ::sigemptyset(&mask_);
        for (int s : sigs) ::sigaddset(&mask_, s);

        ::sigprocmask(SIG_BLOCK, &mask_, nullptr);

        fd_.reset(::signalfd(-1, &mask_, SFD_CLOEXEC | SFD_NONBLOCK));
    }

    bool valid() const { return fd_.valid(); }
    int  fd()    const { return fd_.get(); }

    bool drain_and_handle(std::ostream& os) {
        bool stop = false;
        signalfd_siginfo si{};
        while (::read(fd_.get(), &si, sizeof(si)) == sizeof(si)) {
            if (si.ssi_signo == SIGHUP) {
                os << "[signal] SIGHUP\n";
            } else if (si.ssi_signo == SIGTERM) {
                os << "[signal] SIGTERM -> shutdown\n";
                stop = true;
            } else {
                os << "[signal] " << si.ssi_signo << "\n";
            }
        }
        return stop;
    }

private:
    sigset_t  mask_{};
    UniqueFd  fd_;
};

class Listener {
public:
    explicit Listener(unsigned short port) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return;

        int opt = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons(port);

        if (::bind(s, (sockaddr*)&a, sizeof(a)) < 0) { ::close(s); return; }
        if (::listen(s, SOMAXCONN) < 0) { ::close(s); return; }

        set_nonblock(s);
        fd_.reset(s);
    }

    bool valid() const { return fd_.valid(); }
    int  fd()    const { return fd_.get(); }

    int accept_one() const {
        return ::accept(fd_.get(), nullptr, nullptr);
    }

private:
    UniqueFd fd_;
};

class Client {
public:
    explicit Client(int fd) : fd_(fd) {
        set_nonblock(fd_.get());
    }

    int fd() const { return fd_.get(); }

    ssize_t recv_some(void* buf, size_t cap) {
        return ::recv(fd_.get(), buf, cap, 0);
    }

private:
    UniqueFd fd_;
};

} // namespace sys

int main(int argc, char* argv[]) {
    unsigned short port = 12345;
    if (argc > 1) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<unsigned short>(p);
    }

    sys::SignalFd sfd{SIGHUP, SIGTERM};
    sys::Listener lst(port);
    sys::Epoll ep;

    if (!sfd.valid() || !lst.valid() || !ep.valid()) return 1;

    ep.add_in(sfd.fd());
    ep.add_in(lst.fd());

    std::cout << "Listening on port " << port << "\n";

    std::optional<sys::Client> client;

    std::array<epoll_event, 8> events{};
    bool stop = false;

    while (!stop) {
        int n = ep.wait(events.data(), static_cast<int>(events.size()), -1);
        if (n <= 0) continue;

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == sfd.fd()) {
                stop |= sfd.drain_and_handle(std::cout);
                continue;
            }

            if (fd == lst.fd()) {
                for (;;) {
                    int nf = lst.accept_one();
                    if (nf < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    std::cout << "New connection\n";

                    if (!client.has_value()) {
                        client.emplace(nf);
                        ep.add_in(client->fd());
                        std::cout << "This connection is now the active client\n";
                    } else {
                        std::cout << "Active client already present, closing new\n";
                        ::close(nf);
                    }
                }
                continue;
            }

            if (client.has_value() && fd == client->fd()) {
                char buf[4096];

                for (;;) {
                    ssize_t r = client->recv_some(buf, sizeof(buf));

                    if (r > 0) {
                        std::cout << "Received " << r << " bytes\n";
                        continue;
                    }

                    if (r == 0) {
                        std::cout << "Client closed connection\n";
                        ep.del(client->fd());
                        client.reset();
                        break;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }

                    std::cout << "Client recv error\n";
                    ep.del(client->fd());
                    client.reset();
                    break;
                }

                continue;
            }
        }
    }

    std::cout << "Server stopped\n";
    return 0;
}
