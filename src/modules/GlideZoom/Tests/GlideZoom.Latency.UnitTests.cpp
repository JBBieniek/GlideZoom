#include "PortableTestAdapter.h"

int Run_latency_native();

namespace GlideZoomTests
{
    TEST_CLASS(EngineTests)
    {
    public:
        TEST_METHOD(latency_native)
        {
            RunPortableSuite([] { return Run_latency_native(); });
        }

    };
}
