#include "client.h"

#include <contracts>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

#include <lighter/http/bound_client.h>
#include <lighter/http/runtime.h>

namespace lighter::http {

namespace {

std::shared_ptr<detail::SharedResources> require_shared_resources() {
    auto resources = detail::make_shared_resources();
    if (!resources) {
        std::fprintf(stderr, "fatal: failed to initialize curl shared resources\n");
        std::abort();
    }
    return resources;
}

std::mutex &share_lock(curl_lock_data data, void *userptr) {
    auto *resources = static_cast<detail::SharedResources *>(userptr);
    contract_assert(resources != nullptr);
    const auto index = static_cast<usize>(data);
    contract_assert(index < resources->locks.size());
    return resources->locks[index];
}

void lock_share(CURL *, curl_lock_data data, curl_lock_access, void *userptr) { share_lock(data, userptr).lock(); }

void unlock_share(CURL *, curl_lock_data data, void *userptr) { share_lock(data, userptr).unlock(); }

} // namespace

std::shared_ptr<detail::SharedResources> detail::make_shared_resources() {
    if (auto code = detail::ensure_curl_runtime(); !curl::ok(code)) {
        return {};
    }

    auto resources = std::make_shared<detail::SharedResources>();
    resources->share = curl::ShareHandle::create();
    if (!resources->share) {
        return {};
    }

    // One Client's copies may drive transfers from several threads at once.
    if (!curl::ok(curl::share_setopt(resources->share.get(), CURLSHOPT_LOCKFUNC, &lock_share)) ||
        !curl::ok(curl::share_setopt(resources->share.get(), CURLSHOPT_UNLOCKFUNC, &unlock_share)) ||
        !curl::ok(curl::share_setopt(resources->share.get(), CURLSHOPT_USERDATA, static_cast<void *>(resources.get())))) {
        return {};
    }

    if (auto err = curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE); !curl::ok(err)) {
        return {};
    }

    if (auto err = curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS); !curl::ok(err)) {
        return {};
    }

    // SSL session sharing is best-effort: skip if curl was built without SSL.
    curl::share_setopt(resources->share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

    return resources;
}

Client::Client() : shared(require_shared_resources()) {}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;

Client &Client::operator=(Client &&) noexcept = default;

BoundClient Client::on(EventLoop &loop) & noexcept { return BoundClient(*this, loop); }

BoundClient Client::on(EventLoop &loop) && noexcept { return BoundClient(std::move(*this), loop); }

} // namespace lighter::http
