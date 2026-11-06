// seesee / 看看 — 单反 RAW 支持
//
// 思路：WIC 默认不解 RAW（要么装厂商/微软 RAW 扩展）。但几乎所有 RAW（CR2/CR3/NEF/ARW/
// DNG/ORF/RW2/RAF/PEF/SRW…）里都内嵌了一张 JPEG 预览图（半尺寸到全尺寸）。把这段 JPEG
// 抠出来交给 WIC 解码，就能做到「不装任何插件也能看 RAW」，而且不用去马赛克、比真解 RAW 快
// 一个数量级 —— 这正是看图工具该走的路径。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace raw {

// 内嵌 JPEG 的位置
struct JpegRange {
    size_t offset = 0;
    size_t length = 0;
};

// 是否是我们认的 RAW 扩展名（传带点的小写扩展名，如 L".cr2"）
bool IsRawExtension(const std::wstring& extWithDot);

// 扫描整份文件，找出所有看起来完整的 JPEG 流（SOI…EOI），按长度从大到小返回。
// RAW 里的「无损 JPEG 压缩的 raw 数据」也会被扫到，所以调用方要逐个试着解码，
// 第一个能解出画面的就是预览图。
std::vector<JpegRange> FindEmbeddedJpegs(const uint8_t* data, size_t size, size_t maxCount = 8);

}  // namespace raw
