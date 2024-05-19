// Copyright (c) 2022 Klemens Morgenstern (klemens.morgenstern@gmx.net)
// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// inspired by
// https://github.com/boostorg/cobalt/blob/develop/include/boost/cobalt/op.hpp

#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <asio/async_result.hpp>
#include <coroutine>
#include <optional>
#include <tuple>
namespace tmc {

/// Base class used to implement TMC awaitables for Asio operations.
template <typename... ResultArgs> class aw_asio_base {
protected:
  std::optional<std::tuple<ResultArgs...>> result;
  std::coroutine_handle<> outer;
  detail::type_erased_executor* continuation_executor;
  size_t prio;

  struct callback {
    aw_asio_base* me;
    template <typename... ResultArgs_> void operator()(ResultArgs_&&... Args) {
      me->result.emplace(static_cast<ResultArgs_&&>(Args)...);
      auto exec = me->continuation_executor;
      if (exec == nullptr || detail::this_thread::exec_is(exec)) {
        me->outer.resume();
      } else {
        // post_checked is redundant with the prior check at the moment
        detail::post_checked(exec, std::move(me->outer), me->prio);
      }
    }
  };

  virtual void initiate_await(callback Callback) = 0;

  aw_asio_base()
      : continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio) {}

public:
  virtual ~aw_asio_base() = default;
  bool await_ready() { return false; }

  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    outer = Outer;
    initiate_await(callback{this});
  }

  auto await_resume() noexcept { return *std::move(result); }
};

struct aw_asio_t {
  constexpr aw_asio_t() {}

  // Adapts an executor to add the `aw_asio_t` completion token as the default.
  template <typename InnerExecutor>
  struct executor_with_default : InnerExecutor {
    typedef aw_asio_t default_completion_token_type;

    executor_with_default(const InnerExecutor& Executor) noexcept
        : InnerExecutor(Executor) {}

    template <typename InnerExecutor1>
    executor_with_default(
      const InnerExecutor1& Executor,
      typename std::enable_if<std::conditional<
        !std::is_same<InnerExecutor1, executor_with_default>::value,
        std::is_convertible<InnerExecutor1, InnerExecutor>,
        std::false_type>::type::value>::type = 0
    ) noexcept
        : InnerExecutor(Executor) {}
  };

  // Type alias to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename T>
  using as_default_on_t = typename T::template rebind_executor<
    executor_with_default<typename T::executor_type>>::other;

  // Function helper to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename AsioIoType>
  static typename std::decay_t<AsioIoType>::template rebind_executor<
    executor_with_default<typename std::decay_t<AsioIoType>::executor_type>>::
    other
    as_default_on(AsioIoType&& AsioIoObject) {
    return typename std::decay_t<AsioIoType>::template rebind_executor<
      executor_with_default<typename std::decay_t<AsioIoType>::executor_type>>::
      other(static_cast<AsioIoType&&>(AsioIoObject));
  }
};

// Static completion token object that tells asio to produce a TMC awaitable.
constexpr aw_asio_t aw_asio{};

} // namespace tmc

#ifdef TMC_USE_BOOST_ASIO
namespace boost::asio {
#else
namespace asio {
#endif

// Specialization of asio::async_result to produce a TMC awaitable
template <typename... ResultArgs>
struct async_result<tmc::aw_asio_t, void(ResultArgs...)> {
  /// TMC awaitable for an Asio operation
  template <typename Init, typename... InitArgs>
  class aw_asio final
      : public tmc::aw_asio_base<std::decay_t<ResultArgs>...>,
        public tmc::detail::resume_on_mixin<aw_asio<Init, InitArgs...>>,
        public tmc::detail::with_priority_mixin<aw_asio<Init, InitArgs...>> {
    friend async_result;
    friend class tmc::detail::resume_on_mixin<aw_asio<Init, InitArgs...>>;
    friend class tmc::detail::with_priority_mixin<aw_asio<Init, InitArgs...>>;

    Init initiation;
    std::tuple<InitArgs...> init_args;
    template <typename Init_, typename... InitArgs_>
    aw_asio(Init_&& Initiation, InitArgs_&&... Args)
        : initiation(static_cast<Init_&&>(Initiation)),
          init_args(static_cast<InitArgs_&&>(Args)...) {}

    void initiate_await(
      tmc::aw_asio_base<std::decay_t<ResultArgs>...>::callback Callback
    ) final override {
      std::apply(
        [&](InitArgs&&... Args) {
          std::move(initiation)(std::move(Callback), std::move(Args)...);
        },
        std::move(init_args)
      );
    }
  };

  // This doesn't actually initiate the operation, just returns the awaitable.
  // Initiation happens in aw_asio_base::await_suspend();
  template <typename Init, typename... InitArgs>
  static aw_asio<std::decay_t<Init>, std::decay_t<InitArgs>...>
  initiate(Init&& Initiation, tmc::aw_asio_t, InitArgs&&... Args) {
    return aw_asio<std::decay_t<Init>, std::decay_t<InitArgs>...>(
      static_cast<Init&&>(Initiation), static_cast<InitArgs&&>(Args)...
    );
  }
};

} // namespace asio
