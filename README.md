## tmc-asio
tmc-asio provides [Asio](https://think-async.com/Asio/) integration functionality for the [TooManyCooks](https://github.com/tzcnt/TooManyCooks) async runtime.

See https://github.com/tzcnt/tmc-examples/tree/main/examples/asio for examples of how to use this.

This is a header-only library. It respects the following preprocessor directive configurations:
- `TMC_USE_BOOST_ASIO`: Integrate with boost::asio instead of standalone Asio.

Currently this provides only 2 files. Each file is standalone (does not depend on the other).

### aw_asio.hpp
Provides a completion token `tmc::aw_asio` that can be passed to any Asio async function to turn it into a TMC awaitable. The parameters that Asio would provide to a callback completion token will instead be returned as a tuple from the `co_await` expression.

This awaitable can be used from within any C++20 coroutine that can be converted to a std::coroutine_handle<>. If that coroutine is running on a thread attached to an executor that provides a specialization of `tmc::detail::executor_traits` (all of the TMC executors provide this), then when the awaitable completes, it will be resumed back on its original executor, at its original priority.

A typical use case would be to `co_await` an Asio operation from within the TMC CPU executor; after the Asio operation completes, the coroutine will resume back on the TMC CPU executor. If you prefer to have the coroutine resume inline on the Asio executor, you can specify that by calling `.resume_on(tmc::asio_executor())` on the awaitable before awaiting it.

For example:
```cpp
#include "asio.hpp" // Asio library header
#include "tmc/task.hpp"
#include "tmc/asio/aw_asio.hpp"

tmc::task<void> handler(asio::ip::tcp::socket sock) {
  char data[4096];
  auto buf = asio::buffer(data);
  auto [error, bytes_read] = co_await socket.async_read_some(buf, tmc::aw_asio);
  // Do something with buf...
}
```

### ex_asio.hpp
Provides a global Asio executor accessible via `tmc::asio_executor()`. This executor transparently wraps an `asio::io_context`, so that it can be provided directly to any Asio calls. It also functions as a TMC executor by providing a specialization of `tmc::detail::executor_traits`.

```cpp
#include "asio.hpp" // Asio library header
#include "tmc/ex_cpu.hpp"
#include "tmc/task.hpp"
#include "tmc/asio/aw_asio.hpp"
#include "tmc/asio/ex_asio.hpp"
int main() {
  tmc::asio_executor().init();
  return tmc::async_main([]() -> tmc::task<int> {
    asio::ip::tcp::acceptor acceptor(tmc::asio_executor(), {asio::ip::tcp::v4(), 55555});
    while (true) {
      auto [error, sock] = co_await acceptor.async_accept(tmc::aw_asio);
      if (error) {
        break;
      }
      // Spawn the handler function from the prior aw_asio example as a detached coroutine.
      tmc::spawn(handler(std::move(sock)));
    }

    co_return 0;
  }());
}
```

### Integrating with an existing Asio setup
Use of the `ex_asio.hpp` header and the global executor that it provides is not required. TMC can integrate with an existing standalone executor via `aw_asio.hpp` alone; however, under these conditions, you will be limited to the following functionality:
- From a coroutine running on the TMC executor, initiating an async operation on Asio and `co_await`ing it using the `tmc::aw_asio` completion token. Once the Asio operation completes, the coroutine will be posted back to the TMC executor queue to be resumed.


To fully integrate your existing Asio setup with TMC, you must provide a specialization of `tmc::detail::executor_traits`, and set the `tmc::detail::this_thread::executor` pointer on each of its threads (see `tmc::ex_asio.init_thread_locals()`). This unlocks the following additional functionality:
- `co_await`ing a `tmc::aw_asio` operation from a coroutine running on the Asio executor and having it resume inline on the Asio executor.
- `co_await`ing a `tmc::aw_asio` operation from a coroutine running on the TMC executor and modifying it via the `.resume_on()` function to have it resume inline on the Asio executor, rather than back on the TMC executor.
- Using the standalone `co_await tmc::resume_on()` function to switch a running coroutine onto the Asio executor at any time.
- Using the standalone `co_await tmc::enter()` function to switch a running coroutine onto the Asio executor at any time.
