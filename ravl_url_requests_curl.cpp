// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ravl_url_requests.h"

#include <chrono>
#include <cstring>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>
#include <curl/urlapi.h>
#include <new>
#include <ratio>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

namespace ravl
{
  static bool initialized = false;
  static std::mutex mtx;

  static size_t body_write_fun(
    char* ptr, size_t size, size_t nmemb, void* userdata)
  {
    URLResponse* r = static_cast<URLResponse*>(userdata);
    size_t real_size = nmemb * size;
    r->body += std::string(ptr, real_size);
    return real_size;
  }

  static size_t header_write_fun(
    char* buffer, size_t size, size_t nitems, void* userdata)
  {
    URLResponse* r = static_cast<URLResponse*>(userdata);
    size_t real_size = nitems * size;
    std::string h = std::string(buffer, real_size);
    char* colon = std::strchr(buffer, ':');
    if (colon != NULL)
    {
      std::string key(buffer, colon - buffer);
      std::string value(colon + 2, real_size - (colon - buffer) - 1);
      r->headers.emplace(std::make_pair(key, value));
    }
    return real_size;
  }

  static CURL* easy_setup(
    CURL* curl,
    const std::string& url,
    const std::string& body,
    URLResponse& r,
    bool verbose)
  {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_write_fun);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write_fun);

    if (verbose)
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    if (!body.empty())
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());

    return curl;
  }

  static bool must_retry(CURL* curl, URLResponse& response, bool verbose)
  {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    if (response.status == 429)
    {
      long retry_after = 0;
      curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retry_after);
      if (verbose)
        printf("HTTP 429; RETRY after %lds\n", retry_after);
      std::this_thread::sleep_for(std::chrono::seconds(retry_after));
      response.body = "";
      response.headers.clear();
      response.status = 0;
      return true;
    }
    else
      return false;
  }

  URLResponse URLRequest::execute(bool verbose)
  {
    if (!initialized)
    {
      curl_global_init(CURL_GLOBAL_ALL);
      atexit(curl_global_cleanup);
      initialized = true;
    }

    CURL* curl = curl_easy_init();

    if (!curl)
      throw std::runtime_error("libcurl initialization failed");

    URLResponse response;

    // printf("Sync: %s\n", url.c_str());

    while (max_attempts > 0)
    {
      easy_setup(curl, url, body, response, verbose);

      CURLcode curl_code = curl_easy_perform(curl);

      if (curl_code != CURLE_OK)
      {
        curl_easy_cleanup(curl);
        throw std::runtime_error(fmt::format("curl error: {}", curl_code));
      }
      else
      {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

        if (must_retry(curl, response, verbose))
          max_attempts--;
        else
        {
          curl_easy_cleanup(curl);
          return response;
        }
      }
    }

    if (curl)
      curl_easy_cleanup(curl);

    throw std::runtime_error("maxmimum number of URL request retries exceeded");
  }

  std::vector<uint8_t> URLResponse::url_decode(const std::string& in)
  {
    int outsz = 0;
    char* decoded = curl_easy_unescape(NULL, in.c_str(), in.size(), &outsz);
    if (!decoded)
      throw std::bad_alloc();
    std::vector<uint8_t> r = {decoded, decoded + outsz};
    free(decoded);
    return r;
  }

  std::vector<uint8_t> URLResponse::get_header_data(
    const std::string& name, bool url_decoded) const
  {
    auto hit = headers.find(name);
    if (hit == headers.end())
      throw std::runtime_error("missing response header '" + name + "'");
    if (url_decoded)
      return url_decode(hit->second);
    else
      return {hit->second.data(), hit->second.data() + hit->second.size()};
  }

  AsynchronousURLRequestTracker::AsynchronousURLRequestTracker(bool verbose) :
    URLRequestTracker(verbose)
  {}

  class AsynchronousURLRequestTracker::MonitorThread
  {
  public:
    MonitorThread(
      AsynchronousURLRequestTracker* tracker,
      URLRequestSetId id,
      void* multi,
      std::function<void(URLResponses&&)> callback) :
      keep_going(true),
      tracker(tracker),
      id(id),
      multi(multi),
      callback(callback)
    {
      t = std::thread(&MonitorThread::run, this);
      t.detach();
    }

    virtual ~MonitorThread() {}

    void stop()
    {
      keep_going = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void run()
    {
      while (keep_going)
        keep_going &= tracker->poll(id, multi, callback);
    }

  protected:
    bool keep_going = true;
    std::thread t;

    AsynchronousURLRequestTracker* tracker;
    URLRequestSetId id;
    void* multi;
    std::function<void(URLResponses&&)> callback;
  };

  bool AsynchronousURLRequestTracker::poll(
    URLRequestSetId id,
    void* multi,
    std::function<void(URLResponses&&)>& callback)
  {
    auto consume_msgs = [this, id, multi]() {
      struct CURLMsg* m;
      do
      {
        int msgq = 0;
        m = curl_multi_info_read(multi, &msgq);
        if (m && m->msg == CURLMSG_DONE)
        {
          size_t i = 0;
          auto cc = curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &i);
          if (cc == CURLE_OK)
            complete(id, i, m->easy_handle);
        }
      } while (m);
    };

    if (!is_complete(id))
    {
      std::lock_guard<std::mutex> guard(mtx);
      int num_active_fds = 0;
      CURLMcode mc = curl_multi_poll(multi, NULL, 0, 100, &num_active_fds);
      if (mc != CURLM_OK)
        throw std::runtime_error("curl_multi_poll failed");
      consume_msgs();
      return true;
    }
    else if (callback)
    {
      std::lock_guard<std::mutex> guard(mtx);
      consume_msgs();
      auto rsps_it = responses.find(id);
      if (rsps_it == responses.end())
        throw std::runtime_error("could not find url responses");
      URLResponses rs;
      rs.swap(rsps_it->second);
      callback(std::move(rs));
      responses.erase(rsps_it);
      requests.erase(id);
    }

    curl_multi_cleanup(multi);

    return false;
  }

  URLRequestSetId AsynchronousURLRequestTracker::submit(
    URLRequests&& rs, std::function<void(URLResponses&&)>&& callback)
  {
    std::lock_guard<std::mutex> guard(mtx);

    if (!initialized)
    {
      curl_global_init(CURL_GLOBAL_ALL);
      atexit(curl_global_cleanup);
      initialized = true;
    }

    URLRequestSetId id = requests.size();
    auto [it, ok] = requests.emplace(id, TrackedRequests{std::move(rs)});
    if (!ok)
      throw std::bad_alloc();

    TrackedRequests& reqs = it->second;

    CURLM* multi = curl_multi_init();

    if (!multi)
      throw std::bad_alloc();

    reqs.handle = multi;

    auto [rsps_it, rsps_ok] =
      responses.emplace(id, URLResponses(reqs.requests.size()));
    if (!rsps_ok)
      throw std::bad_alloc();

    std::vector<void*> easies;

    for (size_t i = 0; i < reqs.requests.size(); i++)
    {
      auto& request = reqs.requests.at(i);
      // printf("Submit   %zu: %s\n", i, request.url.c_str());
      CURL* easy = curl_easy_init();
      if (!easy)
        throw std::bad_alloc();
      URLResponse& response = rsps_it->second[i];
      easy_setup(easy, request.url, request.body, response, verbose);
      curl_easy_setopt(easy, CURLOPT_PRIVATE, i);
      curl_multi_add_handle(multi, easy);
      easies.push_back(easy);
    }

    int running_handles = 0;
    CURLMcode curl_code = curl_multi_perform(multi, &running_handles);

    if (curl_code != CURLM_OK)
    {
      for (const auto& easy : easies)
      {
        curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);
      }
      curl_multi_cleanup(multi);
      throw std::runtime_error("curl_multi_perform unsuccessful");
    }

    monitor_threads[id] =
      std::make_shared<MonitorThread>(this, id, multi, callback);

    return id;
  }

  void AsynchronousURLRequestTracker::complete(
    size_t id, size_t i, void* handle)
  {
    // Lock held by the monitor thread

    auto rqit = requests.find(id);
    if (rqit == requests.end())
      return;

    auto multi = static_cast<CURLM*>(rqit->second.handle);
    auto easy = static_cast<CURL*>(handle);

    auto& req = rqit->second.requests.at(i);

    auto rsit = responses.find(id);
    if (rsit == responses.end())
      throw std::runtime_error("response set not found");

    if (rsit->second.size() != rqit->second.requests.size())
      rsit->second.resize(rqit->second.requests.size());

    if (i >= rsit->second.size())
      throw std::runtime_error("request index too large");

    URLResponse& response = rsit->second.at(i);

    curl_multi_remove_handle(multi, easy);
    if (must_retry(easy, response, true))
      curl_multi_add_handle(multi, easy);
    else
    {
      curl_easy_cleanup(easy);
      // printf(
      //   "Complete %zu: %u size %zu (req. %zu)\n",
      //   i,
      //   response.status,
      //   response.body.size(),
      //   id);
    }
  }

  bool AsynchronousURLRequestTracker::is_complete(
    const URLRequestSetId& id) const
  {
    Requests::const_iterator rit = requests.end();
    CURLMcode mc = CURLM_OK;
    int still_running = 0;
    CURLM* multi = NULL;

    {
      std::lock_guard<std::mutex> guard(mtx);
      rit = requests.find(id);
      if (rit == requests.end())
        throw std::runtime_error("no such request set");

      multi = static_cast<CURL*>(rit->second.handle);
      mc = curl_multi_perform(multi, &still_running);
      return mc != CURLM_OK || still_running == 0;
    }
  }
}