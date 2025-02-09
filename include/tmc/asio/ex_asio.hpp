// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <asio/any_io_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <thread>

namespace tmc {
class ex_asio {
  struct InitParams {
    void (*thread_init_hook)(size_t) = nullptr;
  };
  InitParams* init_params = nullptr;

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

  /// Hook will be invoked at the startup of each thread owned by this executor,
  /// and passed the ordinal index (0..thread_count()-1) of the thread.
  inline ex_asio& set_thread_init_hook(void (*Hook)(size_t)) {
    assert(!is_initialized);
    if (init_params == nullptr) {
      init_params = new InitParams;
    }
    init_params->thread_init_hook = Hook;
    return *this;
  }

  inline void init([[maybe_unused]] int ThreadCount = 1) {
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

    if (init_params != nullptr) {
      delete init_params;
      init_params = nullptr;
    }
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

  inline ex_asio() : ioc(1), type_erased_this(this), is_initialized(false) {}
  inline ex_asio(int ThreadCount)
      : ioc(ThreadCount), type_erased_this(this), is_initialized(false) {
    init(ThreadCount);
  }
  inline ~ex_asio() { teardown(); }
  inline tmc::detail::type_erased_executor* type_erased() {
    return &type_erased_this;
  }
  inline void init_thread_locals(size_t Slot) {
    tmc::detail::this_thread::executor = &type_erased_this;
    // tmc::detail::this_thread::this_task = {.prio = 0, .yield_priority =
    // &yield_priority[slot]};
    if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
      init_params->thread_init_hook(Slot);
    }
  }

  inline void clear_thread_locals() {
    tmc::detail::this_thread::executor = nullptr;
    // tmc::detail::this_thread::this_task = {};
  }
  inline void graceful_stop() { ioc.stop(); }

  inline void post(work_item&& Item, [[maybe_unused]] size_t Priority) {
#ifdef TMC_USE_BOOST_ASIO
    boost::asio::post(ioc.get_executor(), item);
#else
    asio::post(ioc.get_executor(), std::move(Item));
#endif
  }

  template <typename It>
  void post_bulk(It Items, size_t Count, [[maybe_unused]] size_t Priority) {
    for (size_t i = 0; i < Count; ++i) {
#ifdef TMC_USE_BOOST_ASIO
      boost::asio::post(ioc.get_executor(), *Items);
#else
      asio::post(ioc.get_executor(), std::move(*Items));
#endif
      ++Items;
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
  friend tmc::detail::executor_traits<ex_asio>;
  inline std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
    if (tmc::detail::this_thread::exec_is(&type_erased_this)) {
      return Outer;
    } else {
      post(std::move(Outer), Priority);
      return std::noop_coroutine();
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_asio> {
  static inline void
  post(tmc::ex_asio& ex, tmc::work_item&& Item, size_t Priority) {
    ex.post(std::move(Item), Priority);
  }

  template <typename It>
  static inline void
  post_bulk(tmc::ex_asio& ex, It&& Items, size_t Count, size_t Priority) {
    ex.post_bulk(std::forward<It>(Items), Count, Priority);
  }

  static inline tmc::detail::type_erased_executor* type_erased(tmc::ex_asio& ex
  ) {
    return ex.type_erased();
  }

  static inline std::coroutine_handle<> task_enter_context(
    tmc::ex_asio& ex, std::coroutine_handle<> Outer, size_t Priority
  ) {
    return ex.task_enter_context(Outer, Priority);
  }
};

inline ex_asio g_ex_asio;
} // namespace detail

/// Returns a reference to the global instance of `tmc::ex_asio`.
constexpr ex_asio& asio_executor() { return tmc::detail::g_ex_asio; }

} // namespace tmc
