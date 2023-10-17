#pragma once
#include "asio/async_result.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <coroutine>
#include <functional>
#include <optional>
#include <string>
#include <thread>
namespace tmc {

template <typename... Args> struct aw_asio_base {
  std::optional<std::tuple<Args...>> result;
  std::coroutine_handle<> outer;
  detail::type_erased_executor* continuation_executor;
  size_t prio;

  struct callback {
    aw_asio_base* me;
    template <typename... Args_> void operator()(Args_&&... args) {
      me->result.emplace(std::move(args)...);
      if (me->continuation_executor == detail::this_thread::executor) {
        me->outer.resume();
      } else {
        me->continuation_executor->post(std::move(me->outer), me->prio);
      }
    }
  };

  virtual void initiate(callback cb) = 0;
  virtual ~aw_asio_base() = default;

  aw_asio_base()
      : continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio) {}
  aw_asio_base(aw_asio_base&& other) : result(std::move(other.result)) {}

  bool await_ready() { return false; }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    outer = h;
    initiate(callback{this});
  }

  auto await_resume() noexcept { return *std::move(result); }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_asio_base& resume_on(detail::type_erased_executor* executor) {
    continuation_executor = executor;
    return *this;
  }
  
  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_asio_base& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  
  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_asio_base& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_spawned_task& resume_with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

struct aw_asio_t {
  constexpr aw_asio_t() {}

  // Adapts an executor to add the `aw_asio_t` completion token as the default.
  template <typename InnerExecutor>
  struct executor_with_default : InnerExecutor {
    typedef aw_asio_t default_completion_token_type;

    executor_with_default(const InnerExecutor& ex) noexcept
        : InnerExecutor(ex) {}

    template <typename InnerExecutor1>
    executor_with_default(
      const InnerExecutor1& ex,
      typename std::enable_if<std::conditional<
        !std::is_same<InnerExecutor1, executor_with_default>::value,
        std::is_convertible<InnerExecutor1, InnerExecutor>,
        std::false_type>::type::value>::type = 0
    ) noexcept
        : InnerExecutor(ex) {}
  };

  // Type alias to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename T>
  using as_default_on_t = typename T::template rebind_executor<
    executor_with_default<typename T::executor_type>>::other;

  // Function helper to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename T>
  static typename std::decay_t<T>::template rebind_executor<
    executor_with_default<typename std::decay_t<T>::executor_type>>::other
  as_default_on(T&& object) {
    return
      typename std::decay_t<T>::template rebind_executor<executor_with_default<
        typename std::decay_t<T>::executor_type>>::other(std::forward<T>(object)
      );
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
template <typename... Args> struct async_result<tmc::aw_asio_t, void(Args...)> {
  template <typename Initiation, typename... InitArgs>
  struct aw_asio final : tmc::aw_asio_base<std::decay_t<Args>...> {
    Initiation initiation;
    std::tuple<InitArgs...> init_args;
    template <typename Initiation_, typename... InitArgs_>
    aw_asio(Initiation_&& initiation, InitArgs_&&... args)
        : initiation(std::forward<Initiation_>(initiation)),
          init_args(std::forward<InitArgs_>(args)...) {}

    void initiate(tmc::aw_asio_base<std::decay_t<Args>...>::callback cb
    ) final override {
      std::apply(
        [&](InitArgs&&... init_args) {
          std::move(initiation)(std::move(cb), std::move(init_args)...);
        },
        std::move(init_args)
      );
    }
  };

  // This doesn't actually initiate the operation, just returns the awaitable.
  // Initiation happens in aw_asio_base::await_suspend();
  template <typename Initiation, typename... InitArgs>
  static aw_asio<std::decay_t<Initiation>, std::decay_t<InitArgs>...>
  initiate(Initiation&& initiation, tmc::aw_asio_t, InitArgs&&... args) {
    return aw_asio<std::decay_t<Initiation>, std::decay_t<InitArgs>...>(
      std::forward<Initiation>(initiation), std::forward<InitArgs>(args)...
    );
  }
};

} // namespace asio
