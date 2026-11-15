#pragma once

#include <QFuture>
#include <QPromise>
#include <QThreadPool>

#include <type_traits>
#include <utility>

namespace blink {

// Runs `work` on the global thread pool. The future is started at once, so result() waits for the work even before it
// begins; `.then(context, ...)` delivers to the context's thread, and not at all once the context is gone.
template<typename Work>
QFuture<std::invoke_result_t<Work>> runInPool(Work work)
{
    QPromise<std::invoke_result_t<Work>> promise;
    QFuture<std::invoke_result_t<Work>> future = promise.future();
    promise.start();
    QThreadPool::globalInstance()->start([work = std::move(work), promise = std::move(promise)] mutable {
        promise.addResult(work());
        promise.finish();
    });
    return future;
}

} // namespace blink
