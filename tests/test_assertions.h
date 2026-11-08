// Lightweight assertions for the standalone executable test suites.
#pragma once

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace glide_test
{
    inline std::string Message(const wchar_t* text)
    {
        std::string result;
        if (text)
            for (; *text; ++text)
                result += static_cast<unsigned int>(*text) <= 127 ? static_cast<char>(*text) : '?';
        return result.empty() ? "Standalone test assertion failed" : result;
    }
    struct Assert
    {
        static void IsTrue(bool value, const wchar_t* message = nullptr)
        {
            if (!value) throw std::runtime_error(Message(message));
        }
        static void IsFalse(bool value, const wchar_t* message = nullptr) { IsTrue(!value, message); }
        template<class Expected, class Actual>
        static void AreEqual(const Expected& expected, const Actual& actual, const wchar_t* message = nullptr)
        {
            IsTrue(expected == actual, message);
        }
        static void AreEqual(double expected, double actual, double tolerance, const wchar_t* message = nullptr)
        {
            IsTrue(std::isfinite(expected) && std::isfinite(actual) && std::abs(expected - actual) <= tolerance, message);
        }
    };
    struct Logger
    {
        static void WriteMessage(const char* value) { std::cout << value << '\n'; }
    };
}
