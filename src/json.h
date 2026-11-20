// A small JSON reader for GitHub's answers: values into a tree, \u escapes into UTF-8. Reading only.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sanctum {

struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json &operator[](const char *k) const;   // a missing key is null
    const Json &operator[](size_t i) const;
    std::string str() const { return type == STR ? s : std::string(); }
    long long num() const { return type == NUM ? (long long)n : 0; }
};

bool json_parse(const std::string &text, Json &out);

}  // namespace sanctum
