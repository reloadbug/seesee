// seesee / 看看 — RAW 内嵌预览提取
#include "rawdecode.h"

#include <algorithm>
#include <cwctype>

namespace raw {
namespace {

// 常见单反/微单 RAW 扩展名
const wchar_t* const kRawExts[] = {
    L".cr2",  L".cr3",  L".crw",  L".nef",  L".nrw",  L".arw",  L".srf",  L".sr2",
    L".dng",  L".orf",  L".rw2",  L".raf",  L".pef",  L".srw",  L".rwl",  L".3fr",
    L".fff",  L".iiq",  L".mos",  L".mrw",  L".x3f",  L".dcr",  L".kdc",  L".mef",
    L".raw",  L".erf",  L".rwz",  L".gpr",  L".braw",
};

}  // namespace

bool IsRawExtension(const std::wstring& extWithDot) {
    std::wstring lower = extWithDot;
    for (auto& c : lower) c = (wchar_t)std::towlower(c);
    for (const wchar_t* e : kRawExts) {
        if (lower == e) return true;
    }
    return false;
}

std::vector<JpegRange> FindEmbeddedJpegs(const uint8_t* data, size_t size, size_t maxCount) {
    std::vector<JpegRange> found;
    if (!data || size < 4 || maxCount == 0) return found;

    size_t i = 0;
    while (i + 3 < size) {
        // JPEG 以 FF D8 FF 开头
        if (data[i] != 0xFF || data[i + 1] != 0xD8 || data[i + 2] != 0xFF) {
            ++i;
            continue;
        }
        // 从 SOI 往后找 EOI（FF D9）；熵编码数据里 FF 会被填充成 FF 00，所以第一个 FF D9 就是结尾
        size_t j = i + 3;
        size_t end = 0;
        bool closed = false;
        while (j + 1 < size) {
            if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                end = j + 2;
                closed = true;
                break;
            }
            if (data[j] == 0xFF && data[j + 1] == 0x00) {
                j += 2;  // 填充字节，跳过
                continue;
            }
            ++j;
        }
        if (!closed) break;  // 后面没有有效结尾，收摊
        JpegRange r;
        r.offset = i;
        r.length = end - i;
        if (r.length > 1024) found.push_back(r);  // 太小的多半是缩略图噪声
        i = end;
    }

    std::sort(found.begin(), found.end(),
              [](const JpegRange& a, const JpegRange& b) { return a.length > b.length; });
    if (found.size() > maxCount) found.resize(maxCount);
    return found;
}

}  // namespace raw
