#pragma once
#include "asio.hpp"
#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <functional>
#include <string>
#include <thread>

namespace tmc {
class ex_asio {
public:
#ifdef TMC_USE_BOOST_ASIO
  using ioc_t = boost::asio::io_context;
#else
  using ioc_t = asio::io_context;
#endif
  ioc_t ioc;
  std::jthread ioc_thread;
  tmc::detail::type_erased_executor type_erased_this;
  bool is_initialized;
  inline void init(int nthreads = 1) {
    if (is_initialized) {
      return;
    }
    is_initialized = true;
    if (ioc.stopped()) {
      ioc.restart();
    }
    // replaces need for an executor_work_guard
    ioc.get_executor().on_work_started();
    ioc_thread = std::jthread([this]() {
      init_thread_locals(0);
      ioc.run();
    });
  }
  inline void teardown() {
    if (!is_initialized) {
      return;
    }
    is_initialized = false;
    // replaces need for an executor_work_guard
    ioc.get_executor().on_work_finished();
    ioc.stop();
    ioc_thread.join();
  }

  inline ex_asio() : ioc(1), type_erased_this(*this), is_initialized(false) {}
  inline ex_asio(int nthreads)
      : ioc(nthreads), type_erased_this(*this), is_initialized(false) {
    init(nthreads);
  }
  inline ~ex_asio() { teardown(); }
  inline tmc::detail::type_erased_executor* type_erased() {
    return &type_erased_this;
  }
  inline void init_thread_locals(size_t slot) {
    detail::this_thread::executor = &type_erased_this;
    // detail::this_thread::this_task = {.prio = 0, .yield_priority =
    // &yield_priority[slot]};
    // use string concatenation to avoid needing add'l headers
    detail::this_thread::thread_name =
      std::string("i/o thread ") + std::to_string(slot);
  }

  inline void clear_thread_locals() {
    detail::this_thread::executor = nullptr;
    // detail::this_thread::this_task = {};
    detail::this_thread::thread_name.clear();
  }
  inline void graceful_stop() { ioc.stop(); }

  inline void post(work_item&& item, size_t priority) {
#ifdef TMC_USE_BOOST_ASIO
    boost::asio::post(ioc.get_executor(), item);
#else
    asio::post(ioc.get_executor(), item);
#endif
  }

  template <typename It>
  void post_bulk(It items, size_t priority, size_t count) {
    for (size_t i = 0; i < count; ++i) {
#ifdef TMC_USE_BOOST_ASIO
      boost::asio::post(ioc.get_executor(), *items);
#else
      asio::post(ioc.get_executor(), *items);
#endif
      ++items;
    }
  }

  // Make it possible to pass this directly to asio functions,
  // Whether you are using the default type-erased any_io_executor
  // or the specialized io_context executor
  using executor_type = ioc_t::executor_type;
  inline operator executor_type() { return ioc.get_executor(); }
  inline operator asio::any_io_executor() { return ioc.get_executor(); }

private:
  friend class aw_ex_scope_enter<ex_asio>;
  inline std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> outer, size_t prio) {
    if (detail::this_thread::executor == &type_erased_this) {
      return outer;
    } else {
      post(std::move(outer), prio);
      return std::noop_coroutine();
    }
  }
};

namespace detail {
inline ex_asio g_ex_asio;
} // namespace detail

/// Returns a reference to the global instance of `tmc::ex_asio`.
constexpr ex_asio& asio_executor() { return detail::g_ex_asio; }

} // namespace tmc
