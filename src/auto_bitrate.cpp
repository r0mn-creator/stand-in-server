/**
 * @file src/auto_bitrate.cpp
 * @brief Definitions for the automatic bitrate controller.
 */
// standard includes
#include <algorithm>
#include <filesystem>
#include <mutex>

// lib includes
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "auto_bitrate.h"
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace auto_bitrate {
  namespace pt = boost::property_tree;

  // Ignore losses while the decoder settles at the start and right after a change
  constexpr auto warmup = 3s;
  constexpr auto settle = 2s;

  // Losses within this window count towards one burst
  constexpr auto loss_window = 3s;
  constexpr int burst_events = 2;

  // Wait this long without losses before stepping up; climb back faster to a
  // bitrate that was already stable. A bitrate that lost frames stays off limits
  // (minus a margin) until it is forgotten, then it is probed once more.
  constexpr auto clean_before_raise = 10s;
  constexpr auto clean_before_recover = 3s;
  constexpr auto failure_memory = 5min;
  constexpr double failure_margin = 0.92;

  // Treat a bitrate as stable once it ran this long without losses
  constexpr auto clean_before_stable = 30s;

  constexpr double decrease_factor = 0.75;
  constexpr double increase_factor = 1.05;
  constexpr int minimum_kbps = 1500;

  static std::mutex store_mutex;

  static std::filesystem::path store_path() {
    return std::filesystem::path(config::nvhttp.file_state).parent_path() / "auto_bitrate.json";
  }

  static pt::ptree load_store() {
    pt::ptree tree;
    try {
      if (std::filesystem::exists(store_path())) {
        pt::read_json(store_path().string(), tree);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Auto bitrate: couldn't read "sv << store_path().string() << ": "sv << e.what();
    }
    return tree;
  }

  std::string make_key(const std::string &unique_id, const boost::asio::ip::address &address) {
    std::string network;
    if (address.is_v4() || (address.is_v6() && address.to_v6().is_v4_mapped())) {
      auto bytes = address.is_v4() ? address.to_v4().to_bytes() : boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).to_bytes();
      network = std::to_string(bytes[0]) + '.' + std::to_string(bytes[1]) + '.' + std::to_string(bytes[2]) + ".0/24";
    } else {
      auto bytes = address.to_v6().to_bytes();
      std::fill(bytes.begin() + 8, bytes.end(), 0);
      network = boost::asio::ip::address_v6(bytes).to_string() + "/64";
    }

    // Dots in the key would read as nested paths in the property tree
    std::replace(network.begin(), network.end(), '.', '_');
    return unique_id + '@' + network;
  }

  int starting_kbps(const std::string &key, int ceiling_kbps) {
    std::lock_guard lg {store_mutex};

    auto tree = load_store();
    auto remembered = tree.get_optional<int>(pt::ptree::path_type(key + "/kbps", '/'));
    if (!remembered) {
      BOOST_LOG(info) << "Auto bitrate: no stable bitrate remembered for this client and network, starting at "sv << ceiling_kbps << " Kbps"sv;
      return ceiling_kbps;
    }

    auto start = std::clamp(*remembered, std::min(minimum_kbps, ceiling_kbps), ceiling_kbps);
    BOOST_LOG(info) << "Auto bitrate: starting at remembered "sv << start << " Kbps (ceiling "sv << ceiling_kbps << " Kbps)"sv;
    return start;
  }

  controller_t::controller_t(std::string key, int ceiling_kbps, int start_kbps, int framerate, clock::time_point now):
      key {std::move(key)},
      ceiling {ceiling_kbps},
      floor {std::min(minimum_kbps, ceiling_kbps)},
      current {start_kbps},
      framerate {std::max(framerate, 1)} {
    started_at = now;
    changed_at = started_at;
    last_loss_at = started_at;
  }

  void controller_t::on_loss(std::int64_t frames, clock::time_point now) {
    if (now - started_at < warmup || now - changed_at < settle) {
      return;
    }

    losses.emplace_back(now, std::clamp<std::int64_t>(frames, 1, framerate));
    last_loss_at = now;
  }

  std::optional<int> controller_t::tick(clock::time_point now) {
    while (!losses.empty() && now - losses.front().first > loss_window) {
      losses.pop_front();
    }

    std::int64_t lost_frames = 0;
    for (auto &[_, frames] : losses) {
      lost_frames += frames;
    }

    // A burst: several separate losses, or a single loss that took out a tenth of a second
    bool burst = (int) losses.size() >= burst_events || lost_frames >= framerate / 10;
    if (burst) {
      losses.clear();

      if (current <= floor) {
        return std::nullopt;
      }

      failed_kbps = failed_kbps ? std::min(*failed_kbps, current) : current;
      failed_at = now;
      if (stable_kbps && *stable_kbps >= current) {
        // The network got worse; what was stable no longer is
        stable_kbps.reset();
      }

      auto next = std::max(floor, (int) (current * decrease_factor));
      BOOST_LOG(info) << "Auto bitrate: lost "sv << lost_frames << " frames, lowering "sv << current << " -> "sv << next << " Kbps"sv;
      current = next;
      changed_at = now;
      return current;
    }

    auto clean_for = now - std::max(changed_at, last_loss_at);

    if (clean_for >= clean_before_stable && (!stable_kbps || *stable_kbps < current)) {
      stable_kbps = current;
      BOOST_LOG(info) << "Auto bitrate: "sv << current << " Kbps is stable"sv;
    }

    if (current >= ceiling) {
      return std::nullopt;
    }

    if (failed_kbps && now - failed_at > failure_memory) {
      failed_kbps.reset();
    }

    auto limit = failed_kbps ? std::min(ceiling, (int) (*failed_kbps * failure_margin)) : ceiling;
    if (current >= limit) {
      return std::nullopt;
    }

    auto next = std::min(limit, std::max(current + 250, (int) (current * increase_factor)));
    auto wait = (stable_kbps && next <= *stable_kbps) ? clean_before_recover : clean_before_raise;
    if (clean_for < wait) {
      return std::nullopt;
    }

    BOOST_LOG(info) << "Auto bitrate: clean for "sv << std::chrono::duration_cast<std::chrono::seconds>(clean_for).count() << "s, raising "sv << current << " -> "sv << next << " Kbps"sv;
    current = next;
    changed_at = now;
    return current;
  }

  void controller_t::finish() {
    if (!stable_kbps) {
      // Too short a session to learn anything new
      return;
    }

    std::lock_guard lg {store_mutex};

    auto tree = load_store();
    pt::ptree entry;
    entry.put("kbps", *stable_kbps);
    entry.put("ceiling", ceiling);
    tree.put_child(pt::ptree::path_type(key, '/'), entry);

    try {
      pt::write_json(store_path().string(), tree);
      BOOST_LOG(info) << "Auto bitrate: remembered "sv << *stable_kbps << " Kbps as stable"sv;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Auto bitrate: couldn't write "sv << store_path().string() << ": "sv << e.what();
    }
  }
}  // namespace auto_bitrate
