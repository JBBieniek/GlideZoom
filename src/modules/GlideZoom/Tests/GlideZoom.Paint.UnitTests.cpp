#include "PortableTestAdapter.h"

int Run_paint_native();

namespace GlideZoomTests
{
    TEST_CLASS(EngineTests)
    {
    public:
        TEST_METHOD(paint_native)
        {
            RunPortableSuite([] { return Run_paint_native(); });
        }

    };
}
