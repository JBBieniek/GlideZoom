// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include "CppUnitTest.h"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace GlideZoomTests
{
    // Keep the upstream exhaustive suites usable through Visual Studio Test
    // Explorer, with their assertion messages attached to the test result.
    template<typename Suite>
    void RunPortableSuite(Suite suite)
    {
        std::ostringstream output;
        int result = 1;
        std::string failure;
        {
            struct RedirectOutput
            {
                std::streambuf* previousOut;
                std::streambuf* previousError;

                explicit RedirectOutput(std::streambuf* target) :
                    previousOut(std::cout.rdbuf(target)),
                    previousError(std::cerr.rdbuf(target))
                {
                }

                ~RedirectOutput()
                {
                    std::cout.rdbuf(previousOut);
                    std::cerr.rdbuf(previousError);
                }
            } redirect(output.rdbuf());

            try
            {
                result = suite();
            }
            catch (const std::exception& error)
            {
                failure = error.what();
            }
        }

        Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(output.str().c_str());
        if (!failure.empty())
        {
            const std::wstring message(failure.begin(), failure.end());
            Microsoft::VisualStudio::CppUnitTestFramework::Assert::Fail(message.c_str());
        }
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual(0, result);
    }
}
