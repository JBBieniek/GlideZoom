#include "PortableTestAdapter.h"

int Run_capture_native();

namespace GlideZoomTests
{
    TEST_CLASS(EngineTests)
    {
    public:
        TEST_METHOD(capture_native)
        {
            RunPortableSuite([] { return Run_capture_native(); });
        }

    };
}
