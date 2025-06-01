// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#ifdef TMC_USE_BOOST_ASIO
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#else
#include <asio/any_io_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#endif

#include <cassert>
#include <functional>
#include <thread>

namespace tmc {
class ex_asio {
  struct InitParams {
    std::function<void(size_t)> thread_init_hook = nullptr;
    std::function<void(size_t)> thread_teardown_hook = nullptr;
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
  tmc::ex_any type_erased_this;
  bool is_initialized;

  /// Hook will be invoked at the startup of each thread owned by this executor,
  /// and passed the ordinal index (0..thread_count()-1) of the thread.
  inline ex_asio& set_thread_init_hook(std::function<void(size_t)> Hook) {
    assert(!is_initialized);
    if (init_params == nullptr) {
      init_params = new InitParams;
    }
    init_params->thread_init_hook = std::move(Hook);
    return *this;
  }

  /// Hook will be invoked before destruction of each thread owned by this
  /// executor, and passed the ordinal index (0..thread_count()-1) of the
  /// thread.
  inline ex_asio& set_thread_teardown_hook(std::function<void(size_t)> Hook) {
    assert(!is_initialized);
    if (init_params == nullptr) {
      init_params = new InitParams;
    }
    init_params->thread_teardown_hook = std::move(Hook);
    return *this;
  }

private:
  inline void init_thread_locals() {
    tmc::detail::this_thread::executor = &type_erased_this;
  }

  inline void clear_thread_locals() {
    tmc::detail::this_thread::executor = nullptr;
  }

public:
  inline void init() {
    if (is_initialized) {
      return;
    }
    is_initialized = true;
    if (ioc.stopped()) {
      ioc.restart();
    }
    // replaces need for an executor_work_guard
    ioc.get_executor().on_work_started();

    InitParams params;
    if (init_params != nullptr) {
      params = *init_params;
    }

    ioc_thread = std::jthread([this, params]() {
      // Setup
      init_thread_locals();
      if (params.thread_init_hook != nullptr) {
        params.thread_init_hook(0);
      }

      // Run loop
      ioc.run();

      // Teardown
      if (params.thread_teardown_hook != nullptr) {
        params.thread_teardown_hook(0);
      }
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
  inline ~ex_asio() { teardown(); }

  /// Returns a pointer to the type erased `ex_any` version of this executor.
  /// This object shares a lifetime with this executor, and can be used for
  /// pointer-based equality comparison against the thread-local
  /// `tmc::current_executor()`.
  inline tmc::ex_any* type_erased() { return &type_erased_this; }

  inline void graceful_stop() { ioc.stop(); }

  inline void post(
    work_item&& Item, size_t Priority = 0,
    [[maybe_unused]] size_t ThreadHint = NO_HINT
  ) {
#ifdef TMC_USE_BOOST_ASIO
    boost::asio::post(
      ioc.get_executor(),
      [Priority, Item = std::move(Item)]() mutable -> void {
        tmc::detail::this_thread::this_task.prio = Priority;
        Item();
      }
    );
#else
    asio::post(
      ioc.get_executor(),
      [Priority, Item = std::move(Item)]() mutable -> void {
        tmc::detail::this_thread::this_task.prio = Priority;
        Item();
      }
    );
#endif
  }

  template <typename It>
  void post_bulk(
    It Items, size_t Count, [[maybe_unused]] size_t Priority = 0,
    [[maybe_unused]] size_t ThreadHint = NO_HINT
  ) {
    for (size_t i = 0; i < Count; ++i) {
#ifdef TMC_USE_BOOST_ASIO
      boost::asio::post(
        ioc.get_executor(),
        [Priority, Item = tmc::detail::into_work_item(std::move(*Items))](
        ) mutable -> void {
          tmc::detail::this_thread::this_task.prio = Priority;
          Item();
        }
      );
#else
      asio::post(
        ioc.get_executor(),
        [Priority, Item = tmc::detail::into_work_item(std::move(*Items))](
        ) mutable -> void {
          tmc::detail::this_thread::this_task.prio = Priority;
          Item();
        }
      );
#endif
      ++Items;
    }
  }

  // Make it possible to pass this directly to asio functions,
  // Whether you are using the default type-erased any_io_executor
  // or the specialized io_context executor
  using executor_type = ioc_t::executor_type;
  inline operator executor_type() { return ioc.get_executor(); }
#ifdef TMC_USE_BOOST_ASIO
  inline operator boost::asio::any_io_executor() { return ioc.get_executor(); }
#else
  inline operator asio::any_io_executor() { return ioc.get_executor(); }
#endif

private:
  friend class aw_ex_scope_enter<ex_asio>;
  friend tmc::detail::executor_traits<ex_asio>;
  inline std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
    if (tmc::detail::this_thread::exec_prio_is(&type_erased_this, Priority)) {
      return Outer;
    } else {
      post(std::move(Outer), Priority);
      return std::noop_coroutine();
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_asio> {
  static inline void post(
    tmc::ex_asio& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  ) {
    ex.post(std::move(Item), Priority, ThreadHint);
  }

  template <typename It>
  static inline void post_bulk(
    tmc::ex_asio& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(std::forward<It>(Items), Count, Priority, ThreadHint);
  }

  static inline tmc::ex_any* type_erased(tmc::ex_asio& ex) {
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
