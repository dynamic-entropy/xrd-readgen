#include "readgen/pushgateway_sink.hh"

#include "readgen/prom_encode.hh"

#include <curl/curl.h>

#include <cstdio>
#include <stdexcept>

namespace readgen {
namespace {

size_t DiscardWrite(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

std::string UrlEncodePathSegment(const std::string& s) {
    CURL* enc = curl_easy_init();
    if (!enc) return s;
    char* out = curl_easy_escape(enc, s.c_str(), static_cast<int>(s.size()));
    std::string r = out ? out : s;
    curl_free(out);
    curl_easy_cleanup(enc);
    return r;
}

std::string TrimTrailingSlash(std::string u) {
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

}  // namespace

PushgatewaySink::PushgatewaySink(std::string base_url, std::string push_job)
    : base_url_(TrimTrailingSlash(std::move(base_url))), push_job_(std::move(push_job)) {
    if (base_url_.empty()) throw std::runtime_error("pushgateway URL is empty");
    if (push_job_.empty()) push_job_ = "xrd-readgen";
}

std::string PushgatewaySink::GroupUrl(const std::string& instance) const {
    return base_url_ + "/metrics/job/" + UrlEncodePathSegment(push_job_) + "/instance/" +
           UrlEncodePathSegment(instance);
}

bool PushgatewaySink::HttpRequest(const char* method, const std::string& url, const std::string& body,
                                  long* http_code_out) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fprintf(stderr, "pushgateway: curl_easy_init failed\n");
        return false;
    }

    struct curl_slist* headers = nullptr;
    if (method[0] == 'P') {  // PUT
        headers = curl_slist_append(headers, "Content-Type: text/plain; version=0.0.4; charset=utf-8");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardWrite);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code_out) *http_code_out = code;

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

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
    last_instance_ = snap.job_id.empty() ? "local" : snap.job_id;
    const std::string url = GroupUrl(last_instance_);
    const std::string body = EncodePrometheusText(snap);
    long code = 0;
    const bool ok = HttpRequest("PUT", url, body, &code);
    if (ok) {
        // Quiet success on interval pushes; first success noted by caller if desired.
    }
    return ok;
}

void PushgatewaySink::Finish(const std::string& instance) {
    if (finished_) return;
    finished_ = true;
    const std::string inst = !instance.empty() ? instance : last_instance_;
    if (inst.empty()) return;
    long code = 0;
    (void)HttpRequest("DELETE", GroupUrl(inst), {}, &code);
}

}  // namespace readgen
