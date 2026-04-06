// Thread safety test for ConfigureLogging
// This test verifies that ConfigureLogging can be called safely while logging is in progress.

#include "ex_actor/api.h"
#include "ex_actor/internal/logging.h"
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>

using namespace ex_actor;

int main() {
  std::atomic<bool> stop {false};
  std::atomic<int> log_count {0};
  std::atomic<bool> error_occurred {false};

  // Start multiple threads that continuously log
  std::vector<std::jthread> logger_threads;
  for (int i = 0; i < 4; i++) {
    logger_threads.emplace_back([&stop, &log_count, &error_occurred, i]() {
      try {
        while (!stop.load()) {
          log::Info("Thread {}: Log message #{}", i, log_count.fetch_add(1));
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      } catch (const std::exception& e) {
        std::cerr << "Exception in thread " << i << ": " << e.what() << std::endl;
        error_occurred.store(true);
      }
    });
  }

  // Main thread repeatedly reconfigures logging
  for (int i = 0; i < 10; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Alternate log levels
    LogLevel level = (i % 2 == 0) ? LogLevel::kInfo : LogLevel::kWarn;
    ConfigureLogging({.level = level});

    std::cout << "Reconfigured logging to level " << static_cast<int>(level) << std::endl;
  }

  // Stop logging threads
  stop.store(true);
  for (auto& t : logger_threads) {
    t.join();
  }

  std::cout << "\n=== Test Results ===" << std::endl;
  std::cout << "Total log messages: " << log_count.load() << std::endl;

  if (error_occurred.load()) {
    std::cout << "FAILED: Error occurred during test" << std::endl;
    return 1;
  } else {
    std::cout << "PASSED: No errors during concurrent logging and reconfiguration" << std::endl;
    return 0;
  }
}
