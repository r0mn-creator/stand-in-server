/**
 * @file src/usb_forward.cpp
 * @brief Definitions for forwarding a client's USB devices to this host over USB/IP.
 */
// standard includes
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

// platform includes
#ifdef __linux__
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <sys/capability.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

// lib includes
#include <boost/endian/conversion.hpp>
#include <openssl/ssl.h>

// local includes
#include "logging.h"
#include "usb_forward.h"

using namespace std::literals;

namespace usb_forward {
  namespace endian = boost::endian;

  // USB/IP wire protocol, all fields big endian
  constexpr std::uint16_t usbip_version = 0x0111;
  constexpr std::uint16_t op_req_import = 0x8003;
  constexpr std::uint16_t op_rep_import = 0x0003;

#pragma pack(push, 1)
  struct op_common_t {
    std::uint16_t version;
    std::uint16_t code;
    std::uint32_t status;
  };

  struct usb_device_t {
    char path[256];
    char busid[32];
    std::uint32_t busnum;
    std::uint32_t devnum;
    std::uint32_t speed;
    std::uint16_t id_vendor;
    std::uint16_t id_product;
    std::uint16_t bcd_device;
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t configuration_value;
    std::uint8_t num_configurations;
    std::uint8_t num_interfaces;
  };
#pragma pack(pop)

  static_assert(sizeof(op_common_t) == 8);
  static_assert(sizeof(usb_device_t) == 312);

  static bool ssl_write_all(SSL *ssl, const void *data, std::size_t size) {
    auto p = static_cast<const char *>(data);
    while (size) {
      auto n = SSL_write(ssl, p, (int) size);
      if (n <= 0) {
        return false;
      }
      p += n;
      size -= n;
    }
    return true;
  }

  static bool ssl_read_all(SSL *ssl, void *data, std::size_t size) {
    auto p = static_cast<char *>(data);
    while (size) {
      auto n = SSL_read(ssl, p, (int) size);
      if (n <= 0) {
        return false;
      }
      p += n;
      size -= n;
    }
    return true;
  }

  /**
   * @brief Ask the client for the device and read its description.
   */
  static std::optional<usb_device_t> import_device(SSL *ssl, const std::string &busid) {
    struct {
      op_common_t common;
      char busid[32];
    } request {};
    request.common.version = endian::native_to_big(usbip_version);
    request.common.code = endian::native_to_big(op_req_import);
    std::strncpy(request.busid, busid.c_str(), sizeof(request.busid) - 1);

    if (!ssl_write_all(ssl, &request, sizeof(request))) {
      return std::nullopt;
    }

    op_common_t reply;
    if (!ssl_read_all(ssl, &reply, sizeof(reply))) {
      return std::nullopt;
    }
    if (endian::big_to_native(reply.code) != op_rep_import || reply.status) {
      BOOST_LOG(warning) << "USB forward: client refused to export ["sv << busid << "], status "sv << endian::big_to_native(reply.status);
      return std::nullopt;
    }

    usb_device_t device;
    if (!ssl_read_all(ssl, &device, sizeof(device))) {
      return std::nullopt;
    }
    device.busnum = endian::big_to_native(device.busnum);
    device.devnum = endian::big_to_native(device.devnum);
    device.speed = endian::big_to_native(device.speed);
    device.id_vendor = endian::big_to_native(device.id_vendor);
    device.id_product = endian::big_to_native(device.id_product);
    device.path[sizeof(device.path) - 1] = '\0';
    return device;
  }

#ifdef __linux__
  constexpr auto vhci_path = "/sys/devices/platform/vhci_hcd.0/";
  constexpr std::uint32_t usb_speed_super = 5;

  /**
   * @brief Holds CAP_DAC_OVERRIDE for its lifetime; the vhci attach files are root only.
   */
  class dac_override_t {
  public:
    dac_override_t() {
      caps = cap_get_proc();
      cap_value_t cap = CAP_DAC_OVERRIDE;
      raised = caps && !cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap, CAP_SET) && !cap_set_proc(caps);
    }

    ~dac_override_t() {
      if (raised) {
        cap_value_t cap = CAP_DAC_OVERRIDE;
        cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap, CAP_CLEAR);
        cap_set_proc(caps);
      }
      if (caps) {
        cap_free(caps);
      }
    }

    cap_t caps;
    bool raised;
  };

  static bool write_sysfs(const std::string &file, const std::string &value) {
    dac_override_t dac_override;
    std::ofstream out {vhci_path + file};
    out << value;
    out.flush();
    return (bool) out;
  }

  /**
   * @brief Find a free vhci port on the hub matching the device speed.
   */
  static std::optional<int> free_vhci_port(std::uint32_t speed) {
    std::ifstream status {vhci_path + "status"s};
    std::string line;
    std::getline(status, line);  // header

    auto wanted_hub = speed >= usb_speed_super ? "ss"sv : "hs"sv;
    while (std::getline(status, line)) {
      std::istringstream fields {line};
      std::string hub;
      int port, state;
      fields >> hub >> port >> state;
      // state 4 is VDEV_ST_NULL: nothing attached
      if (fields && hub == wanted_hub && state == 4) {
        return port;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Copy USB/IP traffic between the client's TLS stream and the kernel until either side closes.
   */
  static void pump(SSL *ssl, int tls_fd, int kernel_fd) {
    std::array<char, 64 * 1024> buffer;
    std::array<pollfd, 2> fds {{{tls_fd, POLLIN, 0}, {kernel_fd, POLLIN, 0}}};

    while (true) {
      if (!SSL_pending(ssl) && poll(fds.data(), fds.size(), -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }

      if (SSL_pending(ssl) || (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
        auto n = SSL_read(ssl, buffer.data(), buffer.size());
        if (n <= 0) {
          return;
        }
        for (auto p = buffer.data(); n > 0;) {
          auto written = write(kernel_fd, p, n);
          if (written <= 0) {
            return;
          }
          p += written;
          n -= written;
        }
      }

      if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
        auto n = read(kernel_fd, buffer.data(), buffer.size());
        if (n <= 0 || !ssl_write_all(ssl, buffer.data(), n)) {
          return;
        }
      }

      fds[0].revents = fds[1].revents = 0;
    }
  }

  /**
   * @brief A connected pair of loopback TCP sockets; vhci-hcd only takes TCP sockets.
   * @return 0 on success.
   */
  static int tcp_loopback_pair(int pair[2]) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
      return -1;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    int result = -1;
    if (!bind(listener, (sockaddr *) &addr, sizeof(addr)) && !listen(listener, 1) && !getsockname(listener, (sockaddr *) &addr, &len)) {
      pair[0] = socket(AF_INET, SOCK_STREAM, 0);
      if (pair[0] >= 0 && !connect(pair[0], (sockaddr *) &addr, sizeof(addr))) {
        pair[1] = accept(listener, nullptr, nullptr);
        if (pair[1] >= 0) {
          int one = 1;
          setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          setsockopt(pair[1], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          result = 0;
        }
      }
      if (result && pair[0] >= 0) {
        close(pair[0]);
      }
    }
    close(listener);
    return result;
  }

  static void attach_and_pump(SSL *ssl, int tls_fd, const usb_device_t &device, const std::string &label) {
    if (access(vhci_path, F_OK)) {
      BOOST_LOG(error) << "USB forward: the vhci-hcd kernel module isn't loaded"sv;
      return;
    }

    auto port = free_vhci_port(device.speed);
    if (!port) {
      BOOST_LOG(error) << "USB forward: no free virtual USB port for "sv << label;
      return;
    }

    // The kernel speaks USB/IP on one end of this pair, we relay the other end
    int pair[2];
    if (tcp_loopback_pair(pair)) {
      BOOST_LOG(error) << "USB forward: couldn't create a loopback connection: "sv << std::strerror(errno);
      return;
    }

    auto devid = (device.busnum << 16) | device.devnum;
    std::ostringstream attach;
    attach << *port << ' ' << pair[1] << ' ' << devid << ' ' << device.speed;
    auto attached = write_sysfs("attach", attach.str());

    // The kernel holds its own reference to the socket now
    close(pair[1]);

    if (!attached) {
      BOOST_LOG(error) << "USB forward: attaching "sv << label << " failed: "sv << std::strerror(errno)
                       << " (does the server have cap_dac_override?)"sv;
      close(pair[0]);
      return;
    }

    BOOST_LOG(info) << "USB forward: attached "sv << label << " on virtual port "sv << *port;
    pump(ssl, tls_fd, pair[0]);
    close(pair[0]);

    // Closing the socket usually detaches on its own; make sure
    write_sysfs("detach", std::to_string(*port));
    BOOST_LOG(info) << "USB forward: detached "sv << label;
  }
#else
  static void attach_and_pump(SSL *ssl, int tls_fd, const usb_device_t &device, const std::string &label) {
    BOOST_LOG(error) << "USB forward: not supported on this platform yet"sv;
  }
#endif

  void forward(std::shared_ptr<ssl_socket_t> socket, const std::string &busid, const std::string &device_name) {
    std::thread {[socket, busid, device_name]() {
      boost::system::error_code ec;
      socket->lowest_layer().non_blocking(false, ec);
      socket->lowest_layer().native_non_blocking(false, ec);

      auto ssl = socket->native_handle();
      auto tls_fd = socket->lowest_layer().native_handle();

      // Asio runs TLS through an in-memory BIO pair that only its own async
      // operations drain; point the session straight at the socket instead.
      // The client waits for our reply, so nothing is left buffered in Asio.
      SSL_set_fd(ssl, tls_fd);

      constexpr auto response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: stand-in-usb\r\nConnection: Upgrade\r\n\r\n"sv;
      if (!ssl_write_all(ssl, response.data(), response.size())) {
        return;
      }

      auto device = import_device(ssl, busid);
      if (!device) {
        BOOST_LOG(warning) << "USB forward: couldn't import ["sv << busid << "] from "sv << device_name;
        return;
      }

      char ids[10];
      std::snprintf(ids, sizeof(ids), "%04x:%04x", device->id_vendor, device->id_product);
      auto label = "["s + ids + "] from "s + device_name;

      attach_and_pump(ssl, tls_fd, *device, label);

      SSL_shutdown(ssl);
      socket->lowest_layer().close(ec);
    }}.detach();
  }
}  // namespace usb_forward
