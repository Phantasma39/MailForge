#include "utils.h"
#include <algorithm>

// ============================================================================
//  去除字符串首尾的空白字符（空格、Tab、回车、换行）
//
//  实现原理（三步）：
//    1. find_first_not_of(" \t\n\r")
//       从字符串"开头"向后找，第一个"不属于这些空白字符"的位置 → first
//    2. find_last_not_of(" \t\n\r")
//       从字符串"末尾"向前找，最后一个"不属于这些空白字符"的位置 → last
//    3. substr(first, 长度) 截取 [first, last] 这一段，就是去掉首尾空白后的内容
//
//  边界情况：
//    如果整个字符串全部是空白，first 会等于 npos（字符串::npos 表示"找不到"），
//    此时直接返回空字符串 ""
// ============================================================================
std::string trim(const std::string& str) {
    // 找第一个非空白字符的位置
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";   // 全是空白 → 返回空串

    // 找最后一个非空白字符的位置
    size_t last = str.find_last_not_of(" \t\n\r");

    // (last - first + 1) 是这段内容的长度；
    // substr(起始位置, 长度) 截取出来
    return str.substr(first, (last - first + 1));
}
