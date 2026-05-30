#include <cppjieba/Jieba.hpp>
#include <iostream>

using namespace std;

void print_words(const string& title, const vector<string>& words)
{
    cout << "[" << title << "] ";
    for (const auto& w : words) {
        cout << w << "/";
    }
    cout << endl;
}

int main()
{
    // 创建Jieba对象会读取/usr/local/dict目录下的词典文件，比较耗时
    // 最佳实践: 最好只创建一个Jieba对象
    cppjieba::Jieba tokenizer;

    string s = "金胖胖是一名杰出的计算机科学家，他今天来到了武汉天源迪科，让我们热烈欢迎金胖胖同学！";
    vector<string> words;

    // [MP]
    tokenizer.Cut(s, words, false); // HMM = false
    print_words("MP", words);
    // [HMM]
    tokenizer.CutHMM(s, words);
    print_words("HMM", words);
    // [MIX]
    // tokenizer.Cut(s, words, true);  // HMM = true
    tokenizer.Cut(s, words);
    print_words("MIX", words);
}
