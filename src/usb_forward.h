/**
 * @file src/usb_forward.h
 * @brief Declarations for forwarding a client's USB devices to this host over USB/IP.
 *
 * A paired Stand-In client upgrades an HTTPS connection on the regular HTTPS port
 * (`Upgrade: stand-in-usb`) and then speaks USB/IP inside that TLS stream, acting
 * as the USB/IP server for one device. This host imports the device and attaches
 * it to the platform's virtual host controller, so it shows up as if it had been
 * plugged in locally: every axis and button, force feedback and vendor software.
 */
#pragma once

// standard includes
#include <memory>
#include <string>

// lib includes
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace usb_forward {
  using ssl_socket_t = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

  constexpr auto upgrade_protocol = "stand-in-usb";

  /**
   * @brief Take over an upgraded connection and forward the device behind it.
   * @param socket The client's TLS connection, no longer used by the HTTPS server.
   * @param busid The client's id for the device, as listed by the client.
   * @param device_name The paired client's name, for logging.
   */
  void forward(std::shared_ptr<ssl_socket_t> socket, const std::string &busid, const std::string &device_name);
}  // namespace usb_forward
