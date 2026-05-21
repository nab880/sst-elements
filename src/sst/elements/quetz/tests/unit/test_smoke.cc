#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

TEST_CASE("smoke") {
    CHECK(1 + 1 == 2);
}
