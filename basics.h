#pragma once

#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include <sstream>

namespace chatllm
{
    class LogMessageFatal
    {
    public:
        LogMessageFatal(const char *file, int line) { oss_ << file << ':' << line << ' '; }
        ~LogMessageFatal() noexcept(false) { throw std::runtime_error(oss_.str()); }
        std::ostringstream &stream() { return oss_; }

    private:
        std::ostringstream oss_;
    };

#define CHATLLM_THROW ::chatllm::LogMessageFatal(__FILE__, __LINE__).stream()
#define CHATLLM_CHECK(cond) \
    if (!(cond))            \
    CHATLLM_THROW << "check failed (" #cond ") "

    template <class T> void ordering(const std::vector<T> &lst, std::vector<size_t> &order, bool descending = false)
    {
        order.resize(lst.size());
        for (size_t i = 0; i < lst.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&lst, descending](auto a, auto b)
        {
            return descending ? lst[a] > lst[b] : lst[a] < lst[b];
        });
    }
}

namespace base64
{
    std::string encode(const void *raw_data, int data_len, bool for_url);
    std::vector<uint8_t> decode(const char *encoded_string);
    void decode_to_utf8(const char *encoded_string, std::string &s);
    std::string encode_utf8(const std::string &s);
}