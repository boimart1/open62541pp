#pragma once

#include <functional>  // invoke
#include <future>
#include <tuple>
#include <type_traits>
#include <utility>  // forward, move

#include "open62541pp/types.hpp"  // StatusCode

namespace opcua {

/**
 * @defgroup Async Asynchronous operations
 * The asynchronous model is based on (Boost) Asio's universal model for asynchronous operations.
 * Each async function takes a `CompletionToken` as it's last parameter.
 * The completion token can be a callable with the signature `void(T)` or `void(T&)` where `T` is a
 * function-specific result type.
 *
 * @see https://think-async.com/asio/asio-1.28.0/doc/asio/overview/model/async_ops.html
 * @see https://think-async.com/asio/asio-1.28.0/doc/asio/overview/model/completion_tokens.html
 * @see https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3747.pdf
 * @{
 */

template <class CompletionToken, typename T>
struct AsyncResult {
    template <typename Initiation, typename CompletionHandler, typename... Args>
    static void initiate(Initiation&& initiation, CompletionHandler&& handler, Args&&... args) {
        static_assert(
            std::is_invocable_v<CompletionHandler, T> || std::is_invocable_v<CompletionHandler, T&>
        );
        std::invoke(
            std::forward<Initiation>(initiation),
            std::forward<CompletionHandler>(handler),
            std::forward<Args>(args)...
        );
    }
};

template <typename T, typename Initiation, typename CompletionToken, typename... Args>
auto asyncInitiate(Initiation&& initiation, CompletionToken&& token, Args&&... args) {
    return AsyncResult<std::decay_t<CompletionToken>, T>::initiate(
        std::forward<Initiation>(initiation),
        std::forward<CompletionToken>(token),
        std::forward<Args>(args)...
    );
}

/* ------------------------------------------- Future ------------------------------------------- */

/**
 * Future completion token type.
 * A completion token that causes an asynchronous operation to return a future.
 */
struct UseFutureToken {};

/**
 * Future completion token object.
 * @see UseFutureToken
 */
inline constexpr UseFutureToken useFuture;

template <typename T>
struct AsyncResult<UseFutureToken, T> {
    template <typename Initiation, typename... Args>
    static auto initiate(Initiation&& initiation, UseFutureToken /* unused */, Args&&... args) {
        std::promise<T> promise;
        auto future = promise.get_future();
        std::invoke(
            std::forward<Initiation>(initiation),
            [p = std::move(promise)](T& result) mutable { p.set_value(std::move(result)); },
            std::forward<Args>(args)...
        );
        return future;
    }
};

/* ------------------------------------------ Deferred ------------------------------------------ */

/**
 * Deferred completion token type.
 * The token is used to indicate that an asynchronous operation should return a function object to
 * lazily launch the operation.
 */
struct UseDeferredToken {};

/**
 * Deferred completion token object.
 * @see UseDeferredToken
 */
inline constexpr UseDeferredToken useDeferred;

template <typename T>
struct AsyncResult<UseDeferredToken, T> {
    template <typename Initiation, typename... Args>
    static auto initiate(Initiation&& initiation, UseDeferredToken /* unused */, Args&&... args) {
        return [initiation = std::forward<Initiation>(initiation),
                argsPack = std::make_tuple(std::forward<Args>(args)...)](auto&& token) mutable {
            return std::apply(
                [&](auto&&... argsInner) {
                    return AsyncResult<std::decay_t<decltype(token)>, T>::initiate(
                        std::move(initiation),
                        std::forward<decltype(token)>(token),
                        std::forward<decltype(argsInner)>(argsInner)...
                    );
                },
                std::move(argsPack)
            );
        };
    }
};

/* ------------------------------------------ Detached ------------------------------------------ */

/**
 * Detached completion token type.
 * The token is used to indicate that an asynchronous operation is detached. That is, there is no
 * completion handler waiting for the operation's result.
 */
struct UseDetachedToken {};

/**
 * Detached completion token object.
 * @see UseDetachedToken
 */
inline constexpr UseDetachedToken useDetached;

template <typename T>
struct AsyncResult<UseDetachedToken, T> {
    template <typename Initiation, typename... Args>
    static auto initiate(Initiation&& initiation, UseDetachedToken /* unused */, Args&&... args) {
        std::invoke(
            std::forward<Initiation>(initiation),
            [](auto&&...) {
                // ...
            },
            std::forward<Args>(args)...
        );
    }
};

/* ------------------------------------------ Defaults ------------------------------------------ */

/**
 * Default completion token for async operations.
 * @see UseFutureToken
 */
using DefaultCompletionToken = UseFutureToken;

/**
 * @}
 */
}  // namespace opcua

// ----------- Cancellation proof of concept -----------

// For UA_Client_cancelByRequestId(). Move this include to a .cpp file, hide the call
#include <open62541/client.h>

namespace opcua {

/* detail */
struct CancellationValues {
    UA_Client* client_ = nullptr;
    UA_UInt32 requestId_ = 0;
};

class CancellationSignal;

// Implementation-facing object to receive cancellation requests
class CancellationSlot {
public:
    CancellationSlot() = default;

    bool isConnected() const {
        return values_ptr_ != nullptr;
    }

    void emplace(UA_Client* client, UA_UInt32 requestId) {
        // precondition: isConnected()
        *values_ptr_ = {client, requestId};
    }

private:
    friend CancellationSignal;

    CancellationSlot(CancellationValues* values_ptr)
        : values_ptr_(values_ptr) {}

    CancellationValues* values_ptr_ = nullptr;
};

// Basic user-facing object to send cancellation requests
class CancellationSignal {
public:
    CancellationSignal() {}

    // Cannot be copied or moved. The underlying handler needs to have a stable address
    // so CancellationSlot can keep track of it.
    CancellationSignal(const CancellationSignal&) = delete;
    CancellationSignal& operator=(const CancellationSignal&) = delete;
    CancellationSignal(CancellationSignal&&) = delete;
    CancellationSignal& operator=(CancellationSignal&&) = delete;

    CancellationSlot slot() {
        return CancellationSlot(&values_);
    }

    void emit() {
        if (values_.client_) {
            UA_UInt32 cancelCount = 0;
            const auto statusCode = UA_Client_cancelByRequestId(
                values_.client_, values_.requestId_, &cancelCount
            );
            static_cast<void>(cancelCount);
            static_cast<void>(statusCode);
        }
    }

private:
    CancellationValues values_;
};

// Type trait. Do not call directly.
// Can be used to customize completion tokens and completion handlers.
template <typename T>
struct AssociatedCancellationSlot {
    // No association by default
    static CancellationSlot get(const T&) noexcept {
        return CancellationSlot();
    }
};

// User/implementation entrypoint to get cancellation slots
template <typename T>
[[nodiscard]] inline CancellationSlot getAssociatedCancellationSlot(const T& t) {
    return AssociatedCancellationSlot<T>::get(t);
}

// Basic slot binder object. Multi-purpose, can be used for both completion tokens
// and completion handlers.
template <typename Wrapped>
struct CancellationSlotBinder {
    CancellationSlot slot_;
    Wrapped wrapped_;

    template <typename... Args>
    std::invoke_result_t<Wrapped&, Args...> operator()(Args&&... args) & {
        return wrapped_(std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::invoke_result_t<const Wrapped&, Args...> operator()(Args&&... args) const& {
        return wrapped_(std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::invoke_result_t<Wrapped&&, Args...> operator()(Args&&... args) && {
        return std::move(wrapped_)(std::forward<Args>(args)...);
    }
};

// Basic slot binder adaptor function
template <typename Wrapped>
auto bindCancellationSlot(CancellationSlot slot, Wrapped&& wrapped) {
    return CancellationSlotBinder<std::decay_t<Wrapped>>{slot, std::forward<Wrapped>(wrapped)};
}

// basic slot binder association
template <typename T>
struct AssociatedCancellationSlot<CancellationSlotBinder<T>> {
    static CancellationSlot get(const CancellationSlotBinder<T>& binder) {
        return binder.slot_;
    }
};

// basic slot binder initiation forwarding
template <typename Wrapped, typename T>
struct AsyncResult<CancellationSlotBinder<Wrapped>, T> {
    template <typename Initiation, typename Binder, typename... Args>
    static auto initiate(Initiation&& initiation, Binder&& binder, Args&&... args) {
        // 1. get slot
        // 2. associate slot with inner completion handler
        auto slot = binder.slot_;
        return AsyncResult<Wrapped, T>::initiate(
            [slot](auto&& handler, auto&& innerInitiation, auto&&... innerArgs) {
                std::invoke(
                    std::forward<decltype(innerInitiation)>(innerInitiation),
                    (bindCancellationSlot)(slot, std::forward<decltype(handler)>(handler)),
                    std::forward<decltype(innerArgs)>(innerArgs)...
                );
            },
            std::forward<Binder>(binder).wrapped_,
            std::forward<Initiation>(initiation),
            std::forward<Args>(args)...
        );
    }
};

}  // namespace opcua
