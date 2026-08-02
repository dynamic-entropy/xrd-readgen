#include "readgen/pushgateway_sink.hh"

#include "readgen/prom_encode.hh"

#include <cstdio>
#include <stdexcept>

namespace readgen {
namespace {

size_t DiscardWrite(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

std::string TrimTrailingSlash(std::string u) {
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

}  // namespace

PushgatewaySink::PushgatewaySink(std::string base_url, std::string push_job)
    : base_url_(TrimTrailingSlash(std::move(base_url))), push_job_(std::move(push_job)) {
    if (base_url_.empty()) throw std::runtime_error("pushgateway URL is empty");
    if (push_job_.empty()) push_job_ = "xrd-readgen";

    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("pushgateway: curl_easy_init failed");

    put_headers_ =
        curl_slist_append(nullptr, "Content-Type: text/plain; version=0.0.4; charset=utf-8");
    if (!put_headers_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
        throw std::runtime_error("pushgateway: curl_slist_append failed");
    }

    // Stable options for the life of the handle (connection reuse / keep-alive).
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, DiscardWrite);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);

    encoded_job_ = UrlEncode(push_job_);
}

PushgatewaySink::~PushgatewaySink() {
    if (put_headers_) {
        curl_slist_free_all(put_headers_);
        put_headers_ = nullptr;
    }
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

std::string PushgatewaySink::UrlEncode(const std::string& s) {
    char* out = curl_easy_escape(curl_, s.c_str(), static_cast<int>(s.size()));
    if (!out) return s;
    std::string r = out;
    curl_free(out);
    return r;
}

std::string PushgatewaySink::GroupUrl(const std::string& instance) {
    if (instance == last_instance_ && !cached_group_url_.empty()) return cached_group_url_;
    cached_group_url_ = base_url_ + "/metrics/job/" + encoded_job_ + "/instance/" + UrlEncode(instance);
    last_instance_ = instance;
    return cached_group_url_;
}

bool PushgatewaySink::HttpRequest(const char* method, const std::string& url, const std::string& body,
                                  long* http_code_out) {
    if (!curl_) {
        std::fprintf(stderr, "pushgateway: curl handle is null\n");
        return false;
    }

    const bool is_put = method[0] == 'P';  // PUT

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, is_put ? put_headers_ : nullptr);
    if (is_put && !body.empty()) {
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else {
        // Clear any previous PUT body so DELETE does not re-send it.
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L);
    }

    const CURLcode rc = curl_easy_perform(curl_);
    long code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;

    if (rc != CURLE_OK) {
        std::fprintf(stderr, "pushgateway: %s %s failed: %s\n", method, url.c_str(),
                     curl_easy_strerror(rc));
        return false;
    }
    if (code < 200 || code >= 300) {
        std::fprintf(stderr, "pushgateway: %s %s HTTP %ld\n", method, url.c_str(), code);
        return false;
    }
    return true;
}

bool PushgatewaySink::Push(const MetricsSnapshot& snap) {
    if (finished_) return false;
    const std::string instance = snap.job_id.empty() ? "local" : snap.job_id;
    const std::string url = GroupUrl(instance);
    const std::string body = EncodePrometheusText(snap);
    return HttpRequest("PUT", url, body, nullptr);
}

void PushgatewaySink::Finish(const std::string& instance) {
    if (finished_) return;
    finished_ = true;
    const std::string inst = !instance.empty() ? instance : last_instance_;
    if (inst.empty()) return;
    (void)HttpRequest("DELETE", GroupUrl(inst), {}, nullptr);
}

}  // namespace readgen
