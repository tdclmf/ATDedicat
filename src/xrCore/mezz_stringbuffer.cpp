#include "stdafx.h"

#include "mezz_stringbuffer.h"

MezzStringBuffer::MezzStringBuffer(uint32_t Size)
{
	StringBuffer = std::make_unique<char[]>(Size);
	BufferRaw = StringBuffer.get();

	BufferSize = Size;
}

char* MezzStringBuffer::GetBuffer() const
{
	return BufferRaw;
}

uint32_t MezzStringBuffer::GetSize() const
{
	return BufferSize;
}

MezzStringBuffer::operator char* () const
{
	return BufferRaw;
}

std::vector<std::string> splitStringMulti(const std::string& inputString, const std::string& separator, bool includeSeparators, bool trimStrings)
{
    std::vector<std::string> wordVector;
    if (inputString.empty()) return wordVector;

    const size_t sepLen = separator.length();
    if (sepLen == 0) {
        wordVector.push_back(inputString);
        return wordVector;
    }

    size_t prev = 0, pos = 0;
    wordVector.reserve(inputString.length() / 8); // Ёвристическое предварительное выделение пам€ти

    while ((pos = inputString.find(separator, prev)) != std::string::npos) {
        if (pos > prev) {
            wordVector.emplace_back(inputString.substr(prev, pos - prev));
            if (trimStrings) trim(wordVector.back());
        }

        if (includeSeparators && pos != std::string::npos) {
            wordVector.emplace_back(inputString.substr(pos, sepLen));
            if (trimStrings) trim(wordVector.back());
        }

        if (pos == std::string::npos) break;
        prev = pos + sepLen;
    }

    if (prev < inputString.length()) {
        wordVector.emplace_back(inputString.substr(prev));
        if (trimStrings) trim(wordVector.back());
    }

    return std::move(wordVector);
}

// ”лучшенна€ верси€ splitStringLimit
std::vector<std::string> splitStringLimit(const std::string& inputString, const std::string& separator, int limit,  bool trimStrings)
{
    std::vector<std::string> wordVector;
    if (inputString.empty() || limit == 0) return wordVector;

    const size_t sepLen = separator.length();
    if (sepLen == 0) {
        wordVector.push_back(inputString);
        return wordVector;
    }

    size_t prev = 0, pos = 0;
    wordVector.reserve(std::min(static_cast<size_t>(limit > 0 ? limit : 16),
        inputString.length() / 8));

    while ((pos = inputString.find(separator, prev)) != std::string::npos) {
        if (pos > prev) {
            wordVector.emplace_back(inputString.substr(prev, pos - prev));
            if (trimStrings) trim(wordVector.back());
        }

        prev = pos + sepLen;

        if (limit > 0 && wordVector.size() >= static_cast<size_t>(limit - 1)) {
            break;
        }

        if (pos == std::string::npos) break;
    }

    if (prev < inputString.length()) {
        wordVector.emplace_back(inputString.substr(prev));
        if (trimStrings) trim(wordVector.back());
    }

    return std::move(wordVector);
}

std::string getFilename(std::string& s) {
    size_t pos_backslash = s.rfind('\\');
    size_t pos_slash = s.rfind('/');

    size_t pos = std::string::npos;
    if (pos_backslash != std::string::npos) {
        pos = pos_backslash;
    }
    if (pos_slash != std::string::npos) {
        if (pos == std::string::npos || pos_slash > pos) {
            pos = pos_slash;
        }
    }

    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

void printIniItemLine(const CInifile::Item& s) {
	std::string fname = s.filename.c_str();
	Msg("%s = %s -> %s", s.first.c_str(), s.second.c_str(), fname.c_str());
}

void trim(std::string& s, const char* t) {
	s.erase(s.find_last_not_of(t) + 1);
	s.erase(0, s.find_first_not_of(t));
};
std::string trimCopy(std::string s, const char* t) {
	trim(s, t);
	return s;
}

void toLowerCase(std::string& s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return std::tolower(c);
	});
}
std::string toLowerCaseCopy(std::string s) {
	toLowerCase(s);
	return s;
}

void replaceAll(std::string& str, const std::string& from, const std::string& to) {
	if (from.empty())
		return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
	}
}
std::string replaceAllCopy(std::string str, const std::string& from, const std::string& to) {
	replaceAll(str, from, to);
	return str;
}
