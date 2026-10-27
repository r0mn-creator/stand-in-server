/**
 * @file src/auto_bitrate.h
 * @brief Declarations for the automatic bitrate controller.
 *
 * The controller lowers the video bitrate when the client reports lost frames and
 * slowly raises it again while the stream stays clean, never exceeding the bitrate
 * the client asked for. The bitrate that held up is remembered per client and
 * network, so the next session starts there.
 *
 * It only listens to messages every Moonlight client already sends (IDR requests
 * and reference frame invalidations), so it works with stock clients too.
 */
#pragma once

// standard includes
#include <chrono>
#include <deque>
#include <optional>
#include <string>

// lib includes
#include <boost/asio/ip/address.hpp>

namespace auto_bitrate {
  using clock = std::chrono::steady_clock;

  /**
   * @brief Build the key a stable bitrate is remembered under.
   * @param unique_id The client's unique id.
   * @param address The client's address; only its network part is used.
   */
  std::string make_key(const std::string &unique_id, const boost::asio::ip::address &address);

  /**
   * @brief Bitrate to start a session at: the remembered stable bitrate, capped at the ceiling.
   */
  int starting_kbps(const std::string &key, int ceiling_kbps);

  class controller_t {
  public:
    controller_t(std::string key, int ceiling_kbps, int start_kbps, int framerate, clock::time_point now = clock::now());

    /**
     * @brief Record that the client lost frames.
     * @param frames Number of frames the client reported lost (1 for an IDR request).
     */
    void on_loss(std::int64_t frames, clock::time_point now = clock::now());

    /**
     * @brief Advance the controller.
     * @return The new bitrate when it should change.
     */
    std::optional<int> tick(clock::time_point now);

    /**
     * @brief Remember the stable bitrate for the next session.
     */
    void finish();

    int current_kbps() const {
      return current;
    }

  private:
    std::string key;
    int ceiling;
    int floor;
    int current;
    int framerate;

    // The lowest bitrate that lost frames recently; probing back past it is slower
    std::optional<int> failed_kbps;
    clock::time_point failed_at;

    // Last bitrate that ran clean for a while
    std::optional<int> stable_kbps;

    clock::time_point started_at;
    clock::time_point changed_at;
    clock::time_point last_loss_at;
    std::deque<std::pair<clock::time_point, std::int64_t>> losses;
  };
}  // namespace auto_bitrate
