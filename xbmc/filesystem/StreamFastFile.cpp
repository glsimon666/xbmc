#include "StreamFastFile.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#define CURL CURL_HANDLE
#include <curl/curl.h>
#undef CURL

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#ifdef TARGET_POSIX
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using namespace XFILE;

// =========================================================================
// Static member initialization
// =========================================================================
size_t CStreamFastFile::s_lru_block_size = 1 * 1024 * 1024;
size_t CStreamFastFile::s_lru_total_size = 100 * 1024 * 1024;
size_t CStreamFastFile::s_lru_max_blocks = 100;
CStreamFastFile::LRUBlockCache CStreamFastFile::s_lru_cache;
std::mutex CStreamFastFile::s_lru_cache_mutex;

std::mutex CStreamFastFile::s_pool_mutex;
std::vector<CURL_HANDLE*> CStreamFastFile::s_curl_pool;

size_t CStreamFastFile::s_instance_count = 0;

// =========================================================================
// Helper functions
// =========================================================================

static bool IsFatalError(CURLcode res)
{
  static const std::vector<CURLcode> fatal_errors = {
    CURLE_URL_MALFORMAT,
    CURLE_COULDNT_RESOLVE_HOST,
    CURLE_COULDNT_CONNECT,
    CURLE_SSL_CONNECT_ERROR,
    CURLE_GOT_NOTHING
  };
  for (auto code : fatal_errors)
  {
    if (res == code) return true;
  }
  return false;
}

// =========================================================================
// LRUBlockCache implementation
// =========================================================================

std::shared_ptr<std::vector<uint8_t>> CStreamFastFile::LRUBlockCache::Get(
    const std::string& url, int64_t block_num)
{
  LRUBlockKey key{url, block_num};
  auto it = blocks.find(key);
  if (it == blocks.end()) return nullptr;
  lru_order.splice(lru_order.begin(), lru_order, it->second.first);
  return it->second.second;
}

void CStreamFastFile::LRUBlockCache::Put(
    const std::string& url, int64_t block_num, const uint8_t* data, size_t size)
{
  LRUBlockKey key{url, block_num};
  auto it = blocks.find(key);
  if (it != blocks.end())
  {
    it->second.second = std::make_shared<std::vector<uint8_t>>(data, data + size);
    lru_order.splice(lru_order.begin(), lru_order, it->second.first);
    return;
  }
  while (blocks.size() >= s_lru_max_blocks && !lru_order.empty())
  {
    blocks.erase(lru_order.back());
    lru_order.pop_back();
  }
  lru_order.push_front(key);
  blocks[key] = {lru_order.begin(),
                 std::make_shared<std::vector<uint8_t>>(data, data + size)};
}

void CStreamFastFile::LRUBlockCache::Clear()
{
  lru_order.clear();
  blocks.clear();
}

void CStreamFastFile::UpdateLRUSettings(size_t block_size, size_t total_size)
{
  std::lock_guard<std::mutex> lock(s_lru_cache_mutex);
  if (s_lru_block_size != block_size)
    s_lru_cache.Clear();
  s_lru_block_size = block_size;
  s_lru_total_size = total_size;
  if (s_lru_block_size == 0)
    s_lru_block_size = 1 * 1024 * 1024;
  s_lru_max_blocks = s_lru_total_size / s_lru_block_size;
  while (s_lru_cache.blocks.size() > s_lru_max_blocks && !s_lru_cache.lru_order.empty())
  {
    s_lru_cache.blocks.erase(s_lru_cache.lru_order.back());
    s_lru_cache.lru_order.pop_back();
  }
}

// =========================================================================
// URL helper functions
// =========================================================================

std::string CStreamFastFile::ExtractHost(const std::string& url)
{
  size_t protocol_pos = url.find("://");
  if (protocol_pos == std::string::npos) return "";
  size_t start = protocol_pos + 3;
  size_t at_pos = url.find('@', start);
  size_t slash_pos = url.find('/', start);
  if (at_pos != std::string::npos && (slash_pos == std::string::npos || at_pos < slash_pos))
    start = at_pos + 1;
  size_t end = url.find('/', start);
  if (end == std::string::npos) return url.substr(start);
  return url.substr(start, end - start);
}

std::string CStreamFastFile::ExtractHostnameOnly(const std::string& host_and_port)
{
  if (!host_and_port.empty() && host_and_port[0] == '[')
  {
    size_t bracket = host_and_port.find(']');
    if (bracket != std::string::npos)
      return host_and_port.substr(1, bracket - 1);
  }
  size_t colon = host_and_port.rfind(':');
  if (colon == std::string::npos) return host_and_port;
  return host_and_port.substr(0, colon);
}

bool CStreamFastFile::EqualsNoCase(const std::string& a, const std::string& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

bool CStreamFastFile::IsLocalHost(const std::string& hostname)
{
  if (hostname.empty()) return false;
  if (hostname.rfind("127.", 0) == 0) return true;
  if (hostname == "::1") return true;
  if (EqualsNoCase(hostname, "localhost")) return true;
  {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) == 0)
    {
      buf[sizeof(buf) - 1] = '\0';
      if (EqualsNoCase(hostname, std::string(buf)))
        return true;
    }
  }
#ifdef TARGET_POSIX
  {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0)
    {
      for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
      {
        if (!ifa->ifa_addr) continue;
        char ip[INET6_ADDRSTRLEN] = {};
        if (ifa->ifa_addr->sa_family == AF_INET)
          inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip));
        else if (ifa->ifa_addr->sa_family == AF_INET6)
          inet_ntop(AF_INET6, &((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr, ip, sizeof(ip));
        if (ip[0] && hostname == ip)
        {
          freeifaddrs(ifaddr);
          return true;
        }
      }
      freeifaddrs(ifaddr);
    }
  }
#endif
  return false;
}

std::string CStreamFastFile::FixDavProtocol(const std::string& url)
{
  if (url.rfind("dav://", 0) == 0 || url.rfind("DAV://", 0) == 0)
  {
    std::string result = url;
    result.replace(0, 6, "http://");
    return result;
  }
  if (url.rfind("davs://", 0) == 0 || url.rfind("DAVS://", 0) == 0)
  {
    std::string result = url;
    result.replace(0, 7, "https://");
    return result;
  }
  return url;
}

std::string CStreamFastFile::GetFileExtensionFromUrl(const std::string& url)
{
  std::string ext;
  CURLU* h = curl_url();
  if (!h) return "unknown";
  CURLUcode rc = curl_url_set(h, CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME);
  if (!rc)
  {
    char* path = nullptr;
    if (!curl_url_get(h, CURLUPART_PATH, &path, 0))
    {
      std::string spath(path);
      size_t dot = spath.rfind('.');
      if (dot != std::string::npos)
      {
        ext = spath.substr(dot);
        for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
      }
      curl_free(path);
    }
  }
  curl_url_cleanup(h);
  if (ext.empty()) return "unknown";
  return ext;
}

// =========================================================================
// CURL handle pool
// =========================================================================

CURL_HANDLE* CStreamFastFile::GetCurlHandleFromPool()
{
  std::lock_guard<std::mutex> lock(s_pool_mutex);
  if (!s_curl_pool.empty())
  {
    CURL_HANDLE* handle = s_curl_pool.back();
    s_curl_pool.pop_back();
    return handle;
  }
  return curl_easy_init();
}

void CStreamFastFile::ReturnCurlHandleToPool(CURL_HANDLE* handle)
{
  if (!handle) return;
  curl_easy_reset(handle);
  std::lock_guard<std::mutex> lock(s_pool_mutex);
  s_curl_pool.push_back(handle);
}

// =========================================================================
// Deferred Close Cache (ISO optimization)
// =========================================================================

static constexpr int CLOSE_DELAY_MS = 200;

void CStreamFastFile::EnsureCleanupThread()
{
  if (!s_cleanup_running)
  {
    s_cleanup_running = true;
    s_cleanup_thread = std::thread(CStreamFastFile::CleanupThreadFunc);
  }
}

void CStreamFastFile::CleanupThreadFunc()
{
  std::unique_lock<std::mutex> lock(s_cache_mutex);
  while (s_cleanup_running)
  {
    if (s_deferred_cache.empty())
    {
      s_cache_cv.wait(lock);
      continue;
    }
    auto earliest = std::min_element(s_deferred_cache.begin(), s_deferred_cache.end(),
        [](const auto& a, const auto& b) { return a.second.expire_at < b.second.expire_at; });
    s_cache_cv.wait_until(lock, earliest->second.expire_at);
    auto now = std::chrono::steady_clock::now();
    for (auto it = s_deferred_cache.begin(); it != s_deferred_cache.end();)
    {
      if (now >= it->second.expire_at)
      {
        CLog::Log(LOGDEBUG, "StreamFastFile: Deferred close expired, destroying session. URL: {}",
                  it->first);
        it->second.file->CloseInternal();
        delete it->second.file;
        it = s_deferred_cache.erase(it);
      }
      else
        ++it;
    }
  }
}

void CStreamFastFile::ShutdownDeferredClose()
{
  {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_cleanup_running = false;
    for (auto& [url, session] : s_deferred_cache)
    {
      CLog::Log(LOGDEBUG, "StreamFastFile: Closing residual deferred session: {}", url);
      session.file->CloseInternal();
      delete session.file;
    }
    s_deferred_cache.clear();
  }
  s_cache_cv.notify_all();
  if (s_cleanup_thread.joinable())
    s_cleanup_thread.join();
}

// =========================================================================
// Construction / Destruction
// =========================================================================

CStreamFastFile::CStreamFastFile()
{
  if (s_instance_count++ == 0)
  {
    std::string ua = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_userAgent;
    if (ua.empty())
      ua = "Kodi";
    m_user_agent = ua;
  }
}

CStreamFastFile::~CStreamFastFile()
{
  CloseInternal();
  --s_instance_count;
}

// =========================================================================
// IFile: Stat (static version - creates temp file for probe)
// =========================================================================

int CStreamFastFile::Stat(const CURL& url, struct __stat64* buffer)
{
  memset(buffer, 0, sizeof(struct __stat64));
  std::string ext = URIUtils::GetExtension(url);
  bool is_iso = (ext == ".iso" || ext == ".ISO" || ext == ".img" || ext == ".IMG");
  if (!is_iso)
    return -1;

  CStreamFastFile temp;
  if (temp.StatInternal(url))
  {
    buffer->st_size = temp.m_total_size;
    buffer->st_mode = _S_IFREG;
    if (temp.m_mod_time > 0)
      buffer->st_mtime = temp.m_mod_time;
    return 0;
  }
  return -1;
}

// =========================================================================
// Internal Stat (HEAD / PROPFIND / GET fallback)
// =========================================================================

size_t CStreamFastFile::StatWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  return size * nmemb;
}

size_t CStreamFastFile::HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t total = size * nmemb;
  std::string header(static_cast<const char*>(contents), total);
  int64_t* file_size = static_cast<int64_t*>(userp);

  if (header.rfind("Content-Length:", 0) == 0 || header.rfind("content-length:", 0) == 0)
  {
    size_t colon = header.find(':');
    if (colon != std::string::npos)
    {
      std::string val = header.substr(colon + 1);
      StringUtils::Trim(val);
      *file_size = std::atoll(val.c_str());
    }
  }
  return total;
}

bool CStreamFastFile::StatInternal(const CURL& url)
{
  CURL_HANDLE* curl = GetCurlHandleFromPool();
  if (!curl) return false;

  std::string target_url = url.Get();
  target_url = FixDavProtocol(target_url);
  std::string ext = GetFileExtensionFromUrl(target_url);

  bool ret = false;

  // Try WebDAV PROPFIND first if dav/davs protocol
  if (url.IsProtocol("dav") || url.IsProtocol("davs"))
  {
    struct curl_slist* headers = nullptr;
    SetupStatWebDavOptions(curl, target_url, &headers);

    std::string response_body;
    auto wcb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
      std::string* body = static_cast<std::string*>(userdata);
      body->append(ptr, size * nmemb);
      return size * nmemb;
    };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +wcb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res == CURLE_OK && response_code == 207)
    {
      // Parse PROPFIND response for file size
      // Look for getcontentlength in the XML
      size_t cl_pos = response_body.find("getcontentlength");
      if (cl_pos != std::string::npos)
      {
        size_t val_start = response_body.find('>', cl_pos);
        if (val_start != std::string::npos)
        {
          size_t val_end = response_body.find('<', val_start);
          if (val_end != std::string::npos)
          {
            std::string val_str = response_body.substr(val_start + 1, val_end - val_start - 1);
            StringUtils::Trim(val_str);
            m_total_size = std::atoll(val_str.c_str());
            ret = true;
          }
        }
      }
    }
    curl_slist_free_all(headers);
  }

  if (!ret)
  {
    // Try HEAD
    curl_easy_reset(curl);
    int64_t file_size = 0;
    SetupStatHeadOptions(curl, target_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StatWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &file_size);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res == CURLE_OK && response_code == 200)
    {
      // Check if range is supported
      struct curl_header* h = nullptr;
      if (curl_easy_header(curl, "Accept-Ranges", 0, CURLH_HEADER, -1, &h) == CURLHE_OK)
      {
        if (h && h->value && std::string(h->value).find("bytes") != std::string::npos)
          m_support_range = true;
      }
      if (file_size > 0)
      {
        m_total_size = file_size;
        ret = true;
      }

      // Modification time
      long filetime = 0;
      curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime);
      if (filetime >= 0)
        m_mod_time = static_cast<time_t>(filetime);
    }
  }

  if (!ret)
  {
    // Fallback: GET with Range: 0-1
    curl_easy_reset(curl);
    std::vector<uint8_t> dummy(2);
    auto fcb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
      std::vector<uint8_t>* buf = static_cast<std::vector<uint8_t>*>(userdata);
      size_t total = size * nmemb;
      size_t to_copy = std::min(total, buf->size());
      memcpy(buf->data(), ptr, to_copy);
      return total;
    };
    SetupStatGetFallbackOptions(curl, target_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +fcb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dummy);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res == CURLE_OK && response_code == 206)
    {
      m_support_range = true;
      double content_length = 0;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
      if (content_length > 0)
      {
        char* range_url = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &range_url);

        // Try HEAD again now that we know the effective URL
        CURL_HANDLE* hcurl = GetCurlHandleFromPool();
        if (hcurl)
        {
          int64_t hsize = 0;
          SetupStatHeadOptions(hcurl, range_url ? range_url : target_url);
          curl_easy_setopt(hcurl, CURLOPT_WRITEFUNCTION, StatWriteCallback);
          curl_easy_setopt(hcurl, CURLOPT_HEADERFUNCTION, HeaderCallback);
          curl_easy_setopt(hcurl, CURLOPT_HEADERDATA, &hsize);
          CURLcode hres = curl_easy_perform(hcurl);
          if (hres == CURLE_OK && hsize > 0)
          {
            m_total_size = hsize;
            ret = true;
          }
          ReturnCurlHandleToPool(hcurl);
        }
      }
    }
    else if (res == CURLE_OK && response_code == 200)
    {
      double content_length = 0;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
      if (content_length > 0)
      {
        m_total_size = static_cast<int64_t>(content_length);
        m_support_range = false;
        ret = true;
      }
    }
  }

  ReturnCurlHandleToPool(curl);
  return ret;
}

// =========================================================================
// IFile: Open
// =========================================================================

bool CStreamFastFile::Open(const CURL& url)
{
  std::string ext = URIUtils::GetExtension(url);
  m_is_iso = (ext == ".iso" || ext == ".ISO" || ext == ".img" || ext == ".IMG");

  CLog::Log(LOGDEBUG, "StreamFastFile: Open {} (ISO={})", url.GetRedacted(), m_is_iso);

  return OpenInternal(url);
}

bool CStreamFastFile::OpenInternal(const CURL& url)
{
  ParseUrlAndSetup(url);

  std::string target_url = FixDavProtocol(m_file_url);

  // Stat the file first
  CURL_HANDLE* curl = GetCurlHandleFromPool();
  if (!curl) return false;

  int64_t file_size = 0;

  // HEAD request
  SetupStatHeadOptions(curl, target_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StatWriteCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &file_size);

  CURLcode res = curl_easy_perform(curl);
  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  if (res == CURLE_OK && response_code == 200)
  {
    struct curl_header* h = nullptr;
    if (curl_easy_header(curl, "Accept-Ranges", 0, CURLH_HEADER, -1, &h) == CURLHE_OK)
    {
      if (h && h->value && std::string(h->value).find("bytes") != std::string::npos)
        m_support_range = true;
    }
    if (file_size > 0)
      m_total_size = file_size;

    char* eff_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
    if (eff_url)
      UpdateEffectiveUrlFromCurl(curl, m_file_url, "Open");

    long filetime = 0;
    curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime);
    if (filetime >= 0)
      m_mod_time = static_cast<time_t>(filetime);
  }
  else
  {
    // Fallback GET with Range: 0-1
    curl_easy_reset(curl);
    std::vector<uint8_t> dummy(2);
    auto fcb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
      std::vector<uint8_t>* buf = static_cast<std::vector<uint8_t>*>(userdata);
      size_t total = size * nmemb;
      size_t to_copy = std::min(total, buf->size());
      memcpy(buf->data(), ptr, to_copy);
      return total;
    };
    SetupStatGetFallbackOptions(curl, target_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +fcb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dummy);

    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res == CURLE_OK && response_code == 206)
    {
      m_support_range = true;
      double cl = 0;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
      if (cl >= 0)
        m_total_size = static_cast<int64_t>(cl) + 2;

      char* eff_url = nullptr;
      curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
      if (eff_url)
        UpdateEffectiveUrlFromCurl(curl, m_file_url, "Open");
    }
    else if (res == CURLE_OK && (response_code == 200 || response_code == 302))
    {
      double cl = 0;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
      if (cl > 0)
      {
        m_total_size = static_cast<int64_t>(cl);
        m_support_range = false;
      }
    }
  }

  ReturnCurlHandleToPool(curl);

  if (m_total_size <= 0)
  {
    CLog::Log(LOGWARNING, "StreamFastFile: Open failed - could not determine file size. URL: {}",
              url.GetRedacted());
    return false;
  }

  // Initialize ring buffer
  size_t actual_ring_size = m_ring_buffer_size;
  if (m_total_size > 0 && static_cast<uint64_t>(m_total_size) < actual_ring_size)
    actual_ring_size = static_cast<size_t>(m_total_size);
  m_ring_buffer_size = actual_ring_size;
  m_ring_buffer.resize(m_ring_buffer_size);
  m_ring_buffer_head = 0;
  m_ring_buffer_tail = 0;
  m_rb_bytes_available = 0;
  m_logical_position = 0;
  m_download_position = 0;
  m_is_first_read = true;
  m_is_eof = false;
  m_has_error = false;

  CLog::Log(LOGINFO, "StreamFastFile: Opened. Size={}MB, Range={}, Ring={}MB, LRU={}MB. URL: {}",
            m_total_size >> 20, m_support_range, m_ring_buffer_size >> 20,
            s_lru_total_size >> 20, url.GetRedacted());

  return true;
}

void CStreamFastFile::ParseUrlAndSetup(const CURL& url)
{
  m_file_url = url.Get();
  m_original_url = m_file_url;
  m_username = url.GetUserName();
  m_password = url.GetPassWord();

  std::string ua = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_userAgent;
  if (ua.empty()) ua = "Kodi";
  m_user_agent = ua;
}

// =========================================================================
// IFile: Close
// =========================================================================

void CStreamFastFile::Close()
{
  CLog::Log(LOGDEBUG, "StreamFastFile: Close. URL: {}", m_original_url);
  CloseInternal();
}

void CStreamFastFile::CloseInternal()
{
  if (!m_is_running) return;
  m_is_running = false;
  m_abort_transfer = true;
  m_trigger_reset = true;
  m_cv_writer.notify_all();
  m_cv_reader.notify_all();
  if (m_worker_thread.joinable())
    m_worker_thread.join();
  if (m_write_multi && m_write_curl)
  {
    curl_multi_remove_handle(m_write_multi, m_write_curl);
    curl_multi_cleanup(m_write_multi);
    curl_easy_cleanup(m_write_curl);
    m_write_multi = nullptr;
    m_write_curl = nullptr;
  }
  if (m_write_headers)
  {
    curl_slist_free_all(m_write_headers);
    m_write_headers = nullptr;
  }
  m_ring_buffer.clear();
  m_ring_buffer_size = 0;
  m_rb_bytes_available = 0;
}

void CStreamFastFile::ResetForReuse()
{
  m_logical_position = 0;
  m_is_first_read = true;
}

// =========================================================================
// IFile: OpenForWrite / Write
// =========================================================================

bool CStreamFastFile::OpenForWrite(const CURL& url, bool bOverWrite)
{
  m_for_write = true;
  m_write_error = false;
  m_write_eof = false;
  m_write_curl = nullptr;
  m_write_multi = nullptr;
  m_write_still_running = 0;

  ParseUrlAndSetup(url);
  std::string target_url = FixDavProtocol(m_file_url);

  m_write_curl = curl_easy_init();
  if (!m_write_curl) return false;

  SetupBaseCurlOptions(m_write_curl, target_url);
  curl_easy_setopt(m_write_curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(m_write_curl, CURLOPT_READFUNCTION, +[](char* buffer, size_t size, size_t nitems, void* userp) -> size_t {
    auto* self = static_cast<CStreamFastFile*>(userp);
    if (!self || self->m_write_eof) return 0;
    size_t to_copy = std::min(size * nitems, self->m_write_buffer_size - self->m_write_buffer_pos);
    if (to_copy == 0)
    {
      self->m_write_paused = true;
      return CURL_READFUNC_PAUSE;
    }
    memcpy(buffer, self->m_write_buffer + self->m_write_buffer_pos, to_copy);
    self->m_write_buffer_pos += to_copy;
    return to_copy;
  });
  curl_easy_setopt(m_write_curl, CURLOPT_READDATA, this);

  m_write_multi = curl_multi_init();
  if (!m_write_multi)
  {
    curl_easy_cleanup(m_write_curl);
    m_write_curl = nullptr;
    return false;
  }

  curl_multi_add_handle(m_write_multi, m_write_curl);
  m_write_paused = false;

  return true;
}

ssize_t CStreamFastFile::Write(const void* lpBuf, size_t uiBufSize)
{
  if (!m_for_write || !m_write_multi || !m_write_curl || m_write_error)
    return -1;

  m_write_buffer = static_cast<const uint8_t*>(lpBuf);
  m_write_buffer_size = uiBufSize;
  m_write_buffer_pos = 0;
  m_write_paused = false;

  curl_easy_pause(m_write_curl, CURLPAUSE_CONT);

  CURLMcode result = CURLM_OK;
  m_write_still_running = 1;

  while (m_write_still_running && !m_write_paused)
  {
    result = curl_multi_perform(m_write_multi, &m_write_still_running);
    if (!m_write_still_running) break;
    if (result != CURLM_OK)
    {
      long code = 0;
      curl_easy_getinfo(m_write_curl, CURLINFO_RESPONSE_CODE, &code);
      CLog::Log(LOGERROR, "StreamFastFile: Write failed. HTTP={}, MultiError={}", code, result);
      m_write_error = true;
      return -1;
    }
    if (!m_write_paused)
      curl_multi_poll(m_write_multi, nullptr, 0, 1000, nullptr);
  }

  if (!m_write_still_running && m_write_buffer_pos < m_write_buffer_size)
  {
    CURLMsg* msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(m_write_multi, &msgs_left)))
    {
      if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK)
      {
        long code = 0;
        curl_easy_getinfo(m_write_curl, CURLINFO_RESPONSE_CODE, &code);
        CLog::Log(LOGERROR, "StreamFastFile: Write transfer ended prematurely. HTTP={}, CurlCode={}",
                  code, msg->data.result);
        m_write_error = true;
        return -1;
      }
    }
  }

  ssize_t written = static_cast<ssize_t>(m_write_buffer_pos);
  m_logical_position += written;
  return written;
}

// =========================================================================
// IFile: Read
// =========================================================================

ssize_t CStreamFastFile::Read(void* lpBuf, size_t uiBufSize)
{
  uint8_t* buffer = static_cast<uint8_t*>(lpBuf);
  size_t size = uiBufSize;

  if (m_total_size > 0 && m_logical_position >= m_total_size)
    return 0;

  size_t total_read = 0;

  while (total_read < size)
  {
    if (!m_support_range)
    {
      // Sequential mode (no Range support) - read from ring buffer only
      if (!m_worker_thread.joinable())
        StartWorker();

      std::unique_lock<std::mutex> rb_lock(m_ring_buffer_mutex);

      while (m_rb_bytes_available == 0)
      {
        if (m_is_eof) return total_read;
        if (m_has_error) return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;
        if (!m_is_running) return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;

        if (m_cv_reader.wait_for(rb_lock, std::chrono::seconds(60)) == std::cv_status::timeout)
        {
          CLog::Log(LOGERROR, "StreamFastFile: Sequential read timeout (60s)");
          return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;
        }
      }

      size_t space_to_end = m_ring_buffer_size - m_ring_buffer_tail;
      size_t to_copy = std::min(size - total_read, std::min(m_rb_bytes_available, space_to_end));
      memcpy(buffer + total_read, m_ring_buffer.data() + m_ring_buffer_tail, to_copy);
      m_ring_buffer_tail = (m_ring_buffer_tail + to_copy) % m_ring_buffer_size;
      m_rb_bytes_available -= to_copy;
      total_read += to_copy;
      m_logical_position += to_copy;
      m_cv_writer.notify_one();
      continue;
    }

    // Range-supported mode: LRU + Ring Buffer
    int64_t current_pos = m_logical_position;
    if (m_total_size > 0 && current_pos >= m_total_size)
      break;

    int64_t block_num = current_pos / static_cast<int64_t>(s_lru_block_size);
    size_t block_offset = static_cast<size_t>(current_pos % s_lru_block_size);

    // 1. Check LRU cache
    {
      std::shared_ptr<std::vector<uint8_t>> block_ptr;
      {
        std::lock_guard<std::mutex> lru_lock(s_lru_cache_mutex);
        block_ptr = s_lru_cache.Get(m_file_url, block_num);
      }
      if (block_ptr)
      {
        size_t block_valid_size = block_ptr->size();
        if (block_offset < block_valid_size)
        {
          size_t avail = block_valid_size - block_offset;
          size_t to_copy = std::min(size - total_read, avail);
          memcpy(buffer + total_read, block_ptr->data() + block_offset, to_copy);
          total_read += to_copy;
          m_logical_position += to_copy;
          continue;
        }
        break;
      }
    }

    // 2. LRU miss - small file optimization
    if (m_total_size > 0 && m_total_size <= static_cast<int64_t>(s_lru_block_size))
    {
      std::vector<uint8_t> file_data(static_cast<size_t>(m_total_size));
      CURL_HANDLE* dl_curl = GetCurlHandleFromPool();
      bool ok = DownloadRange(dl_curl, 0, m_total_size, file_data);
      ReturnCurlHandleToPool(dl_curl);

      if (ok && !file_data.empty())
      {
        {
          std::lock_guard<std::mutex> lru_lock(s_lru_cache_mutex);
          s_lru_cache.Put(m_file_url, 0, file_data.data(), file_data.size());
        }
        size_t avail = file_data.size() - block_offset;
        size_t to_copy = std::min(size - total_read, avail);
        memcpy(buffer + total_read, file_data.data() + block_offset, to_copy);
        total_read += to_copy;
        m_logical_position += to_copy;
        continue;
      }
    }

    // 3. LRU miss - start worker from aligned position
    if (!m_worker_thread.joinable())
    {
      int64_t aligned_pos = (m_logical_position / static_cast<int64_t>(s_lru_block_size)) *
                            static_cast<int64_t>(s_lru_block_size);
      int64_t saved_pos = m_logical_position;
      m_logical_position = aligned_pos;
      StartWorker();
      m_logical_position = saved_pos;
    }

    // 4. ISO first-read optimization: prefetch last block
    if (m_is_first_read && m_is_iso && m_support_range &&
        m_total_size > static_cast<int64_t>(s_lru_block_size))
    {
      m_is_first_read = false;
      int64_t last_block_num = (m_total_size - 1) / static_cast<int64_t>(s_lru_block_size);
      if (last_block_num != block_num)
      {
        std::shared_ptr<std::vector<uint8_t>> tail_cached;
        {
          std::lock_guard<std::mutex> lru_lock(s_lru_cache_mutex);
          tail_cached = s_lru_cache.Get(m_file_url, last_block_num);
        }
        if (!tail_cached)
        {
          int64_t last_block_start = last_block_num * static_cast<int64_t>(s_lru_block_size);
          int64_t last_block_size = m_total_size - last_block_start;
          CLog::Log(LOGDEBUG, "StreamFastFile: ISO first-read prefetch tail block #{} (pos {}, size {})",
                    last_block_num, last_block_start, last_block_size);

          std::vector<uint8_t> tail_data(static_cast<size_t>(last_block_size));
          CURL_HANDLE* dl_curl = GetCurlHandleFromPool();
          bool ok = DownloadRange(dl_curl, last_block_start, last_block_size, tail_data);
          ReturnCurlHandleToPool(dl_curl);

          if (ok && !tail_data.empty())
          {
            std::lock_guard<std::mutex> lru_lock(s_lru_cache_mutex);
            s_lru_cache.Put(m_file_url, last_block_num, tail_data.data(), tail_data.size());
            CLog::Log(LOGDEBUG, "StreamFastFile: ISO tail block prefetch success");
          }
        }
      }
    }
    else
    {
      m_is_first_read = false;
    }

    // 5. Wait for block data from ring buffer
    int64_t block_start = block_num * static_cast<int64_t>(s_lru_block_size);
    int64_t block_end_ideal = block_start + static_cast<int64_t>(s_lru_block_size);
    if (m_total_size > 0)
      block_end_ideal = std::min(block_end_ideal, m_total_size);

    std::unique_lock<std::mutex> rb_lock(m_ring_buffer_mutex);

    bool block_populated = false;
    while (true)
    {
      int64_t buf_start = m_download_position - static_cast<int64_t>(m_rb_bytes_available);
      int64_t buf_end = m_download_position;

      int64_t block_end = block_end_ideal;
      if (m_is_eof && m_download_position < block_end)
        block_end = m_download_position;
      if (m_total_size > 0 && block_end > m_total_size)
        block_end = m_total_size;

      size_t block_size = 0;
      if (block_end > block_start)
        block_size = static_cast<size_t>(block_end - block_start);

      if (block_size > 0 && block_start >= buf_start && block_end <= buf_end)
      {
        std::vector<uint8_t> block_data(block_size);
        size_t offset_from_tail = static_cast<size_t>(block_start - buf_start);
        size_t read_ptr = (m_ring_buffer_tail + offset_from_tail) % m_ring_buffer_size;

        size_t copied = 0;
        while (copied < block_size)
        {
          size_t space_to_end = m_ring_buffer_size - read_ptr;
          size_t chunk = std::min(block_size - copied, space_to_end);
          memcpy(block_data.data() + copied, m_ring_buffer.data() + read_ptr, chunk);
          read_ptr = (read_ptr + chunk) % m_ring_buffer_size;
          copied += chunk;
        }

        // Lazy pruning
        {
          size_t bytes_to_drop = static_cast<size_t>(block_start - buf_start);
          if (bytes_to_drop > m_rb_bytes_available)
            bytes_to_drop = m_rb_bytes_available;
          if (bytes_to_drop > 0)
          {
            m_ring_buffer_tail = (m_ring_buffer_tail + bytes_to_drop) % m_ring_buffer_size;
            m_rb_bytes_available -= bytes_to_drop;
            m_cv_writer.notify_one();
          }
        }

        rb_lock.unlock();

        {
          std::lock_guard<std::mutex> lru_lock(s_lru_cache_mutex);
          s_lru_cache.Put(m_file_url, block_num, block_data.data(), block_size);
        }

        if (block_offset < block_size)
        {
          size_t avail = block_size - block_offset;
          size_t to_copy = std::min(size - total_read, avail);
          memcpy(buffer + total_read, block_data.data() + block_offset, to_copy);
          total_read += to_copy;
          m_logical_position += to_copy;
        }

        block_populated = true;
        break;
      }

      // Check if we need to reset
      bool need_reset = false;
      if (block_start < buf_start)
        need_reset = true;
      else if (block_start > (buf_end + static_cast<int64_t>(m_ring_buffer_size)) ||
               (block_start - buf_end) > (16 * 1024 * 1024))
        need_reset = true;

      if (need_reset)
      {
        m_reset_target_pos = block_start;
        m_trigger_reset = true;
        m_abort_transfer = true;
        m_cv_writer.notify_all();
        m_has_error = false;
        m_is_eof = false;
      }

      if (m_has_error && !need_reset)
        return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;
      if (m_is_eof && block_start >= m_download_position)
        return total_read;

      // Deadlock prevention: drop stale data
      {
        size_t bytes_to_drop = static_cast<size_t>(block_start - buf_start);
        if (bytes_to_drop > m_rb_bytes_available)
          bytes_to_drop = m_rb_bytes_available;
        if (bytes_to_drop > 0)
        {
          m_ring_buffer_tail = (m_ring_buffer_tail + bytes_to_drop) % m_ring_buffer_size;
          m_rb_bytes_available -= bytes_to_drop;
          m_cv_writer.notify_all();
        }
      }

      if (m_is_eof) return total_read;
      if (m_has_error) return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;

      if (m_cv_reader.wait_for(rb_lock, std::chrono::seconds(60)) == std::cv_status::timeout)
      {
        CLog::Log(LOGERROR, "StreamFastFile: Read timeout (60s) waiting for block");
        return -1;
      }

      if (m_has_error) return total_read > 0 ? static_cast<ssize_t>(total_read) : -1;
      if (m_is_eof && m_rb_bytes_available == 0) return total_read;
      if (!m_is_running) return -1;
    }

    if (!block_populated)
      break;
  }

  return total_read;
}

// =========================================================================
// IFile: Seek
// =========================================================================

int64_t CStreamFastFile::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_support_range)
  {
    if (!((iWhence == SEEK_SET && iFilePosition == 0) ||
          (iWhence == SEEK_CUR && iFilePosition == 0)))
      return -1;
  }

  std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);

  int64_t target_pos = 0;
  if (iWhence == SEEK_SET)
    target_pos = iFilePosition;
  else if (iWhence == SEEK_CUR)
    target_pos = m_logical_position + iFilePosition;
  else if (iWhence == SEEK_END)
    target_pos = m_total_size + iFilePosition;

  if (target_pos < 0)
    target_pos = 0;
  if (m_total_size > 0 && target_pos > m_total_size)
    target_pos = m_total_size;

  if (m_support_range)
  {
    m_logical_position = target_pos;
    int64_t block_start = (target_pos / static_cast<int64_t>(s_lru_block_size)) *
                           static_cast<int64_t>(s_lru_block_size);
    m_reset_target_pos = block_start;
    m_trigger_reset = true;
    m_abort_transfer = true;
    m_cv_writer.notify_all();
    m_has_error = false;
    m_is_eof = false;
  }
  else
  {
    // Sequential mode: can only seek within buffer
    int64_t buf_start = m_download_position - static_cast<int64_t>(m_rb_bytes_available);
    if (target_pos >= buf_start && target_pos <= m_download_position)
    {
      size_t offset = static_cast<size_t>(target_pos - buf_start);
      size_t new_tail = (m_ring_buffer_tail + offset) % m_ring_buffer_size;
      m_ring_buffer_tail = new_tail;
      m_rb_bytes_available -= offset;
      m_logical_position = target_pos;
    }
    else
    {
      // Can't seek outside buffer in sequential mode
      return -1;
    }
  }

  return target_pos;
}

// =========================================================================
// IFile: GetPosition / GetLength / IoControl / GetDownloadSpeed
// =========================================================================

int64_t CStreamFastFile::GetPosition()
{
  return m_logical_position;
}

int64_t CStreamFastFile::GetLength()
{
  return m_total_size;
}

int CStreamFastFile::IoControl(EIoControl request, void* param)
{
  if (request == IOCTRL_SEEK_POSSIBLE)
  {
    return m_support_range ? 1 : 0;
  }
  return -1;
}

double CStreamFastFile::GetDownloadSpeed()
{
  return 0.0;
}

bool CStreamFastFile::Exists(const CURL& url)
{
  struct __stat64 buffer;
  return Stat(url, &buffer) == 0;
}

// =========================================================================
// Worker thread management
// =========================================================================

void CStreamFastFile::StartWorker()
{
  if (m_worker_thread.joinable())
    return;

  m_is_running = true;
  m_ring_buffer_head = 0;
  m_ring_buffer_tail = 0;
  m_rb_bytes_available = 0;
  m_download_position = m_logical_position;

  m_worker_thread = std::thread(&CStreamFastFile::WorkerThread, this);

  std::stringstream ss;
  ss << m_worker_thread.get_id();
  CLog::Log(LOGDEBUG, "StreamFastFile: Worker thread started. TID: {}", ss.str());
}

// =========================================================================
// Worker progress callback
// =========================================================================

int CStreamFastFile::WorkerProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                                             curl_off_t ultotal, curl_off_t ulnow)
{
  auto* self = static_cast<CStreamFastFile*>(clientp);
  if (self && !self->m_is_running)
    return 1;
  if (self && self->IsTransferAborted())
    return 1;
  return 0;
}

// =========================================================================
// Worker thread (main download loop)
// =========================================================================

void CStreamFastFile::WorkerThread()
{
  CURL_HANDLE* curl = GetCurlHandleFromPool();
  if (!curl) return;

  int retries = 0;
  char errbuf[CURL_ERROR_SIZE];

  while (m_is_running)
  {
    // 1. Check reset signal
    if (m_trigger_reset)
    {
      {
        std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);
        m_ring_buffer_head = 0;
        m_ring_buffer_tail = 0;
        m_rb_bytes_available = 0;
        m_download_position = m_reset_target_pos.load();
        m_is_eof = false;
        m_has_error = false;
        retries = 0;
      }
      m_trigger_reset = false;
      m_abort_transfer = false;
    }

    // 2. Check EOF
    if (m_download_position >= m_total_size && m_total_size > 0)
    {
      if (!m_is_eof)
      {
        m_is_eof = true;
        m_cv_reader.notify_all();
      }
      std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);
      m_cv_writer.wait(lock, [this] { return m_trigger_reset || !m_is_running; });
      if (!m_is_running) break;
      continue;
    }

    // 3. Download
    curl_easy_reset(curl);
    errbuf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    std::string target_url = !m_effective_url.empty() ? m_effective_url : m_file_url;
    target_url = FixDavProtocol(target_url);

    SetupWorkerDownloadOptions(curl, target_url, m_download_position.load());

    CURLcode res = curl_easy_perform(curl);

    // Update redirect info
    {
      long resp_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp_code);
      if (resp_code > 0)
        UpdateEffectiveUrlFromCurl(curl, m_file_url, "Worker");
    }

    // Dynamic range detection
    if (res == CURLE_OK && !m_support_range)
    {
      long response_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
      bool supports_range = (response_code == 206);
      if (!supports_range)
      {
        struct curl_header* h = nullptr;
        if (curl_easy_header(curl, "Accept-Ranges", 0, CURLH_HEADER, -1, &h) == CURLHE_OK)
        {
          if (h && h->value && std::string(h->value).find("bytes") != std::string::npos)
            supports_range = true;
        }
      }
      if (supports_range)
      {
        m_support_range = true;
        CLog::Log(LOGINFO, "StreamFastFile: Dynamic range detection - server supports Range");
      }
    }

    // 4. Handle result
    if (res == CURLE_ABORTED_BY_CALLBACK && m_abort_transfer)
      continue;

    if (res == CURLE_OK)
    {
      retries = 0;
      if (m_total_size == 0 && m_download_position > 0)
      {
        m_total_size = m_download_position;
        CLog::Log(LOGINFO, "StreamFastFile: Dynamic file size correction: 0 -> {}", m_total_size);
      }
      m_is_eof = true;
      m_cv_reader.notify_all();
    }
    else if (res == CURLE_WRITE_ERROR)
    {
      // Stopped by HandleWrite
    }
    else
    {
      if (res == CURLE_OPERATION_TIMEDOUT)
        CLog::Log(LOGWARNING, "StreamFastFile: Worker timeout. Retry {}/{}", retries + 1, m_net_max_retries);
      else
        CLog::Log(LOGERROR, "StreamFastFile: Curl error: {}. Retry {}/{}", res, retries, m_net_max_retries);

      m_effective_url.clear();

      // If buffer is full, wait before reconnecting
      {
        std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);
        size_t wait_threshold = static_cast<size_t>(m_ring_buffer_size * 0.9);
        if (m_rb_bytes_available > wait_threshold)
        {
          m_cv_writer.wait(lock, [this, wait_threshold] {
            return m_rb_bytes_available < wait_threshold || m_trigger_reset || !m_is_running;
          });
        }
      }

      retries++;
      if (retries > m_net_max_retries)
      {
        m_has_error = true;
        m_cv_reader.notify_all();
        std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);
        m_cv_writer.wait(lock, [this] { return m_trigger_reset || !m_is_running; });
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
  }

  CLog::Log(LOGDEBUG, "StreamFastFile: Worker thread exiting.");
  ReturnCurlHandleToPool(curl);
}

// =========================================================================
// DownloadRange (cache fill helper)
// =========================================================================

size_t CStreamFastFile::CacheWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t realsize = size * nmemb;
  auto* ctx = static_cast<CacheContext*>(userp);
  if (ctx->offset + realsize > ctx->limit)
    realsize = ctx->limit - ctx->offset;
  if (realsize > 0)
  {
    memcpy(ctx->buffer->data() + ctx->offset, contents, realsize);
    ctx->offset += realsize;
  }
  return realsize;
}

bool CStreamFastFile::DownloadRange(CURL_HANDLE* curl, int64_t start, int64_t length,
                                     std::vector<uint8_t>& buffer)
{
  if (!curl) return false;

  int retries = 0;
  CURLcode res = CURLE_FAILED_INIT;
  long response_code = 0;
  CacheContext ctx;
  char errbuf[CURL_ERROR_SIZE];

  while (retries < m_net_max_retries)
  {
    curl_easy_reset(curl);
    errbuf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    std::string target_url = !m_effective_url.empty() ? m_effective_url : m_file_url;
    target_url = FixDavProtocol(target_url);

    SetupDownloadRangeOptions(curl, target_url, start, length);

    ctx.buffer = &buffer;
    ctx.offset = 0;
    ctx.limit = buffer.size();

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CacheWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res == CURLE_OK)
    {
      UpdateEffectiveUrlFromCurl(curl, m_file_url, "DownloadRange");
      if (response_code >= 200 && response_code < 300)
      {
        if (ctx.offset < buffer.size())
          buffer.resize(ctx.offset);
        return true;
      }
    }

    m_effective_url.clear();
    retries++;
    if (retries < m_net_max_retries)
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  return false;
}

// =========================================================================
// Write callback (worker thread receives data here)
// =========================================================================

size_t CStreamFastFile::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  auto* self = static_cast<CStreamFastFile*>(userp);
  return self->HandleWrite(contents, size * nmemb);
}

size_t CStreamFastFile::HandleWrite(void* contents, size_t size)
{
  if (!m_is_running) return 0;

  std::unique_lock<std::mutex> lock(m_ring_buffer_mutex);

  while (m_rb_bytes_available + size > m_ring_buffer_size)
  {
    if (m_abort_transfer || m_trigger_reset)
      return 0;
    m_cv_writer.wait(lock);
    if (!m_is_running) return 0;
    if (m_abort_transfer || m_trigger_reset)
      return 0;
  }

  size_t written = 0;
  while (written < size)
  {
    size_t space_at_end = m_ring_buffer_size - m_ring_buffer_head;
    size_t to_write = std::min(size - written, space_at_end);
    memcpy(m_ring_buffer.data() + m_ring_buffer_head,
           static_cast<uint8_t*>(contents) + written, to_write);
    m_ring_buffer_head = (m_ring_buffer_head + to_write) % m_ring_buffer_size;
    written += to_write;
  }

  m_rb_bytes_available += size;
  m_download_position += static_cast<int64_t>(size);
  m_cv_reader.notify_one();

  return size;
}

// =========================================================================
// URL redirect tracking
// =========================================================================

void CStreamFastFile::UpdateEffectiveUrlFromCurl(CURL_HANDLE* curl,
                                                  const std::string& original_url,
                                                  const char* context_name)
{
  char* eff_url = nullptr;
  if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url) == CURLE_OK && eff_url)
  {
    std::string effective(eff_url);
    if (effective != original_url && effective != m_effective_url)
    {
      std::string orig_redacted = original_url;
      {
        size_t at_pos = orig_redacted.find('@');
        if (at_pos != std::string::npos)
        {
          size_t proto = orig_redacted.find("://");
          if (proto != std::string::npos && proto + 3 < at_pos)
            orig_redacted = orig_redacted.substr(0, proto + 3) + "***" +
                           orig_redacted.substr(at_pos);
        }
      }
      CLog::Log(LOGDEBUG, "StreamFastFile: [{}] Redirect detected: {} -> {}", context_name,
                orig_redacted, effective);
      m_effective_url = effective;
    }
  }
}

// =========================================================================
// Curl options setup
// =========================================================================

void CStreamFastFile::SetupBaseCurlOptions(CURL_HANDLE* curl, const std::string& target_url)
{
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   m_enable_http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, m_user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(curl, CURLOPT_URL, target_url.c_str());

  // Auth
  bool should_send_auth = true;
  {
    std::string host_origin = ExtractHost(m_file_url);
    std::string host_target = ExtractHost(target_url);
    if (!host_origin.empty() && !host_target.empty() && host_origin != host_target)
      should_send_auth = false;
  }
  if (!m_username.empty() && should_send_auth)
  {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_password.c_str());
  }
  else
  {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
  }

  // SSL
  {
    static std::string s_ca_cert_file;
    static bool s_ca_cert_resolved = false;
    if (!s_ca_cert_resolved)
    {
      s_ca_cert_resolved = true;
      const char* env_cert = std::getenv("SSL_CERT_FILE");
      if (env_cert && env_cert[0] != '\0')
        s_ca_cert_file = env_cert;
      if (s_ca_cert_file.empty())
      {
        auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
        if (settings)
          s_ca_cert_file = settings->GetString(CSettings::SETTING_NETWORK_CACERT);
      }
    }
    if (!s_ca_cert_file.empty())
      curl_easy_setopt(curl, CURLOPT_CAINFO, s_ca_cert_file.c_str());
  }

  // Redirects
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

  // Timeouts and TCP
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_net_connect_timeout_sec);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 15L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
  curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_net_low_speed_time_sec);

  // Proxy - use Kodi proxy settings
  {
    std::shared_ptr<CSettings> s = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (s && s->GetBool(CSettings::SETTING_NETWORK_USEHTTPPROXY) &&
        !s->GetString(CSettings::SETTING_NETWORK_HTTPPROXYSERVER).empty() &&
        s->GetInt(CSettings::SETTING_NETWORK_HTTPPROXYPORT) > 0)
    {
      int proxy_type = s->GetInt(CSettings::SETTING_NETWORK_HTTPPROXYTYPE);
      curl_proxytype curl_ptype = CURLPROXY_HTTP;
      switch (proxy_type)
      {
        case 0: curl_ptype = CURLPROXY_HTTP; break;
        case 1: curl_ptype = CURLPROXY_SOCKS4; break;
        case 2: curl_ptype = CURLPROXY_SOCKS4A; break;
        case 3: curl_ptype = CURLPROXY_SOCKS5; break;
        case 4: curl_ptype = CURLPROXY_SOCKS5_HOSTNAME; break;
        default: curl_ptype = CURLPROXY_HTTP; break;
      }
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, static_cast<long>(curl_ptype));
      curl_easy_setopt(curl, CURLOPT_PROXY,
                       s->GetString(CSettings::SETTING_NETWORK_HTTPPROXYSERVER).c_str());
      curl_easy_setopt(curl, CURLOPT_PROXYPORT,
                       static_cast<long>(s->GetInt(CSettings::SETTING_NETWORK_HTTPPROXYPORT)));

      std::string proxy_user = s->GetString(CSettings::SETTING_NETWORK_HTTPPROXYUSERNAME);
      std::string proxy_pass = s->GetString(CSettings::SETTING_NETWORK_HTTPPROXYPASSWORD);
      if (!proxy_user.empty())
      {
        curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy_user.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy_pass.c_str());
      }
    }
  }
}

void CStreamFastFile::SetupStatWebDavOptions(CURL_HANDLE* curl, const std::string& target_url,
                                              struct curl_slist** headers)
{
  curl_easy_setopt(curl, CURLOPT_URL, target_url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, m_user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   m_enable_http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);

  if (!m_username.empty())
  {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_password.c_str());
  }

  const char* data = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<D:propfind xmlns:D=\"DAV:\">"
                      "<D:prop><D:getcontentlength/></D:prop>"
                      "</D:propfind>";
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(strlen(data)));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  *headers = curl_slist_append(*headers, "Content-Type: text/xml; charset=utf-8");
  *headers = curl_slist_append(*headers, "Depth: 0");
  *headers = curl_slist_append(*headers, "translate: f");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
}

void CStreamFastFile::SetupStatHeadOptions(CURL_HANDLE* curl, const std::string& target_url)
{
  curl_easy_setopt(curl, CURLOPT_URL, target_url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, m_user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   m_enable_http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

  if (!m_username.empty())
  {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_password.c_str());
  }
}

void CStreamFastFile::SetupStatGetFallbackOptions(CURL_HANDLE* curl,
                                                   const std::string& target_url)
{
  SetupStatHeadOptions(curl, target_url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
  curl_easy_setopt(curl, CURLOPT_RANGE, "0-1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
}

void CStreamFastFile::SetupWorkerDownloadOptions(CURL_HANDLE* curl,
                                                  const std::string& target_url,
                                                  int64_t position)
{
  SetupBaseCurlOptions(curl, target_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, WorkerProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_net_low_speed_time_sec);

  if (position > 0)
  {
    std::string range = "bytes=" + std::to_string(position) + "-";
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
  }

  if (m_fail_fast)
  {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
  }
}

void CStreamFastFile::SetupDownloadRangeOptions(CURL_HANDLE* curl,
                                                 const std::string& target_url,
                                                 int64_t start, int64_t length)
{
  SetupBaseCurlOptions(curl, target_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CacheWriteCallback);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  std::string range_end = (length > 0)
    ? "bytes=" + std::to_string(start) + "-" + std::to_string(start + length - 1)
    : "bytes=" + std::to_string(start) + "-";
  curl_easy_setopt(curl, CURLOPT_RANGE, range_end.c_str());
}

// =========================================================================
// IFile: Truncate
// =========================================================================

int CStreamFastFile::Truncate(int64_t size)
{
  if (size < 0) return -1;
  m_total_size = size;
  if (m_download_position > size)
    m_download_position = size;
  if (m_logical_position > size)
    m_logical_position = size;
  return 0;
}