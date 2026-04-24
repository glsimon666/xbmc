#pragma once

#include "IFile.h"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <list>
#include <unordered_map>

typedef void CURL_HANDLE;
typedef void CURLM;

namespace XFILE
{

class CStreamFastFile : public IFile
{
public:
  CStreamFastFile();
  ~CStreamFastFile() override;

  bool Open(const CURL& url) override;
  bool OpenForWrite(const CURL& url, bool bOverWrite = false) override;
  bool Exists(const CURL& url) override;
  int Stat(const CURL& url, struct __stat64* buffer) override;
  ssize_t Read(void* lpBuf, size_t uiBufSize) override;
  ssize_t Write(const void* lpBuf, size_t uiBufSize) override;
  int64_t Seek(int64_t iFilePosition, int iWhence = SEEK_SET) override;
  void Close() override;
  int64_t GetPosition() override;
  int64_t GetLength() override;
  int IoControl(EIoControl request, void* param) override;
  int GetChunkSize() override { return 1 * 1024 * 1024; }
  double GetDownloadSpeed() override;
  int Truncate(int64_t size) override;

  static void UpdateLRUSettings(size_t block_size, size_t total_size);
  bool IsIsoFile() const { return m_is_iso; }
  bool IsRangeSupported() const { return m_support_range; }

private:
  struct CacheContext
  {
    std::vector<uint8_t>* buffer;
    size_t offset;
    size_t limit;
  };

  struct LRUBlockKey
  {
    std::string url;
    int64_t block_num;
    bool operator==(const LRUBlockKey& o) const
    {
      return block_num == o.block_num && url == o.url;
    }
  };

  struct LRUBlockKeyHash
  {
    size_t operator()(const LRUBlockKey& k) const
    {
      size_t h1 = std::hash<std::string>{}(k.url);
      size_t h2 = std::hash<int64_t>{}(k.block_num);
      return h1 ^ (h2 * 2654435761ULL);
    }
  };

  struct LRUBlockCache
  {
    std::list<LRUBlockKey> lru_order;
    std::unordered_map<LRUBlockKey,
        std::pair<std::list<LRUBlockKey>::iterator, std::shared_ptr<std::vector<uint8_t>>>,
        LRUBlockKeyHash> blocks;

    std::shared_ptr<std::vector<uint8_t>> Get(const std::string& url, int64_t block_num);
    void Put(const std::string& url, int64_t block_num, const uint8_t* data, size_t size);
    void Clear();
  };

  static std::mutex s_lru_cache_mutex;
  static LRUBlockCache s_lru_cache;
  static size_t s_lru_block_size;
  static size_t s_lru_total_size;
  static size_t s_lru_max_blocks;

  static std::mutex s_pool_mutex;
  static std::vector<CURL_HANDLE*> s_curl_pool;

  static size_t s_instance_count;

  static CURL_HANDLE* GetCurlHandleFromPool();
  static void ReturnCurlHandleToPool(CURL_HANDLE* handle);

  void ParseUrlAndSetup(const CURL& url);
  bool OpenInternal(const CURL& url);
  bool StatInternal(const CURL& url);
  void CloseInternal();
  void ResetForReuse();
  void StartWorker();
  void WorkerThread();
  bool DownloadRange(CURL_HANDLE* curl, int64_t start, int64_t length, std::vector<uint8_t>& buffer);
  bool IsTransferAborted() const { return m_abort_transfer; }

  void SetupBaseCurlOptions(CURL_HANDLE* curl, const std::string& target_url);
  void SetupStatWebDavOptions(CURL_HANDLE* curl, const std::string& target_url, struct curl_slist** headers_out);
  void SetupStatHeadOptions(CURL_HANDLE* curl, const std::string& target_url);
  void SetupStatGetFallbackOptions(CURL_HANDLE* curl, const std::string& target_url);
  void SetupDownloadRangeOptions(CURL_HANDLE* curl, const std::string& target_url, int64_t start, int64_t length);
  void SetupWorkerDownloadOptions(CURL_HANDLE* curl, const std::string& target_url, int64_t start);
  void UpdateEffectiveUrlFromCurl(CURL_HANDLE* curl, const std::string& original_url, const char* context_name);

  static std::string GetFileExtensionFromUrl(const std::string& url);
  static std::string FixDavProtocol(const std::string& url);
  static std::string ExtractHost(const std::string& url);
  static std::string ExtractHostnameOnly(const std::string& host_and_port);
  static bool IsLocalHost(const std::string& hostname);
  static bool EqualsNoCase(const std::string& a, const std::string& b);

  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static size_t CacheWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static size_t StatWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static size_t HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static int WorkerProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

  size_t HandleWrite(void* contents, size_t size);

  std::string m_file_url;
  std::string m_effective_url;
  std::string m_original_url;
  std::string m_username;
  std::string m_password;
  std::string m_user_agent;
  bool m_is_iso = false;
  bool m_is_first_read = true;
  bool m_support_range = false;
  bool m_is_directory = false;

  int64_t m_total_size = 0;
  int64_t m_logical_position = 0;
  std::atomic<int64_t> m_download_position{0};

  std::atomic<bool> m_is_running{false};
  std::atomic<bool> m_is_eof{false};
  std::atomic<bool> m_has_error{false};
  std::atomic<bool> m_abort_transfer{false};
  std::atomic<bool> m_trigger_reset{false};
  std::atomic<int64_t> m_reset_target_pos{0};

  std::string m_redirect_url;

  std::thread m_worker_thread;

  std::vector<uint8_t> m_ring_buffer;
  size_t m_ring_buffer_size = 100 * 1024 * 1024;
  size_t m_ring_buffer_head = 0;
  size_t m_ring_buffer_tail = 0;
  size_t m_rb_bytes_available = 0;

  std::mutex m_ring_buffer_mutex;
  std::condition_variable m_cv_reader;
  std::condition_variable m_cv_writer;

  time_t m_mod_time = 0;
  time_t m_access_time = 0;

  long m_net_connect_timeout_sec = 10;
  long m_net_low_speed_time_sec = 15;
  long m_net_worker_low_speed_time_sec = 15;
  long m_net_read_timeout_sec = 20;
  long m_net_range_total_timeout_sec = 20;
  int m_net_max_retries = 5;
  bool m_fail_fast = false;
  bool m_enable_http2 = false;

  bool m_for_write = false;
  bool m_write_error = false;
  bool m_write_eof = false;
  CURL_HANDLE* m_write_curl = nullptr;
  CURLM* m_write_multi = nullptr;
  int m_write_still_running = 0;
  const uint8_t* m_write_buffer = nullptr;
  size_t m_write_buffer_size = 0;
  size_t m_write_buffer_pos = 0;
  bool m_write_paused = false;

  struct curl_slist* m_write_headers = nullptr;
};

} // namespace XFILE
