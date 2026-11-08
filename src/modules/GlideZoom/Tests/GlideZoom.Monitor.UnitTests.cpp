#include "PortableTestAdapter.h"

int Run_monitor_native();

namespace GlideZoomTests
{
    TEST_CLASS(EngineTests)
    {
    public:
        TEST_METHOD(monitor_native)
        {
            RunPortableSuite([] { return Run_monitor_native(); });
        }

    };
}
